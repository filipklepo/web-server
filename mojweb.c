#include <stdio.h>
#include "mrepro.h"
#include <err.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define HTTP_PORT "80"
#define BACKLOG 10
#define RCV_BUF_LEN 1024
#define SND_BUF_LEN 1024
#define ERR_BUF_LEN 1024

#define REQ_TYPE "GET"
#define ERR_400_MSG "Bad Request"
#define ERR_404_MSG "Not Found"
#define ERR_405_MSG "Method Not Allowed"
#define ERR_500_MSG "Internal Error"

int daemon_fl;

int http_parse_req(char *req, char **method, char **path, char **proto) {
	char *cr;
	*method = req;
	
	*path = strchr(req, ' ');
	if(*path == NULL) {
		return 0;
	}
	**path = '\0';
	*path += 1;
	
	*proto = strchr(*path, ' ');
	if(*proto == NULL) {
		return 0;
	}
	**proto = '\0';
	*proto += 1;
	
	cr = strchr(*proto, '\r');
	if(cr == NULL) {
		return 0;
	}
	*cr = '\0';

	return 1;
}

void http_send_header(int conn_fd, char *proto,  int status, char *msg, 
		      char *mime_type, int *con_len) {
	
	char *buf = (char *)malloc(SND_BUF_LEN * sizeof(char));
	sprintf(buf, "%s %d %s\r\n", proto != NULL ? proto : "HTTP/1.1", status, msg);

	if(mime_type) {
		sprintf(buf + strlen(buf), "Content-Type: %s\r\n", mime_type);
	}
	if(con_len) {
		sprintf(buf + strlen(buf), "Content-Length: %d\r\n", *con_len);
	}

	sprintf(buf + strlen(buf), "\r\n");
	writen(conn_fd, buf, strlen(buf));
}

void http_send_err(int conn_fd, char *proto, int status, char *msg) {
	int len;
	char *buf = (char *)malloc(ERR_BUF_LEN * sizeof(char));
	sprintf(buf, "<html><body><h2>%d - %s</h2></body></html>", status, msg);

	len = strlen(buf);
	http_send_header(conn_fd, proto, status, msg, "text/html", &len);
	writen(conn_fd, buf, strlen(buf));
}

int create_main_page(int conn_fd, char **buf) {
	struct dirent *dirent;
	struct stat st;
	
	sprintf(*buf, "<html><body><ul>");
	
	DIR *dir = opendir(".");
	if(dir == NULL) {
		http_send_err(conn_fd, NULL, 500, ERR_500_MSG);
		return 0;
	}

	while((dirent = readdir(dir)) != NULL) {
		if(dirent->d_type == 8) {
			stat(dirent->d_name, &st);
			
			sprintf(*buf + strlen(*buf), 
				"<li><a href=\"%s\">%s</a> (%lld) </li>",
				dirent->d_name,
				dirent->d_name,
				st.st_size);
		}
	}
	
	sprintf(*buf + strlen(*buf), "</ul></body></html>");
	closedir(dir);
	return 1;
}

void http_send_resp(int conn_fd, char *buf, char *proto, char *mime_type) {
	int len = strlen(buf);
	
	http_send_header(conn_fd, proto, 200, "OK", mime_type, &len);
	writen(conn_fd, buf, len);
}

void http_send_file(int conn_fd, FILE *file, char *proto, char *mime_type) {
	int len, nread;
	char buf[1024];
	
	fseek(file, 0L, SEEK_END);
	len = ftell(file);
	http_send_header(conn_fd, proto, 200, "OK", mime_type, &len);
	
	fseek(file, 0L, SEEK_SET);
	while((nread = fread(&buf[0], 1, 1024, file)) > 0) {
		writen(conn_fd, &buf[0], nread);
	}
}

char *get_mime_type(char *f_name) {
	char *ext = strchr(f_name, '.');
	if(!ext) {
		return "application/octet-stream";
	}	
	ext++;	

	if(!strcmp(ext, "html")) {
		return "text/html";
	}
	if(!strcmp(ext, "txt")) {
		return "text/plain";
	}
	if(!strcmp(ext, "gif")) {
		return "image/gif";
	}
	if(!strcmp(ext, "jpg")) {
		return "image/jpeg";
	}
	if(!strcmp(ext, "pdf")) {
		return "application/pdf";
	}
	return "application/octet-stream";	
}

void serve(int conn_fd) {
	char *req, *method = NULL, *path, *proto = NULL, *buf, *f_name;
	FILE *file;
	int res;
	
	req = (char *)malloc(RCV_BUF_LEN * sizeof(char));
	read_http_req(conn_fd, (void **)&req, RCV_BUF_LEN);

	res = http_parse_req(req, &method, &path, &proto); 
	if(strcmp(method, "GET")) {
		http_send_err(conn_fd, proto, 405, ERR_405_MSG);
		return;
	}
	if(!res || *path != '/' 
		|| (strcmp(proto, "HTTP/1.0") && strcmp(proto, "HTTP/1.1"))) {

		http_send_err(conn_fd, proto, 400, ERR_400_MSG);
		return;
	}	
	if(!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		buf = (char *)malloc(SND_BUF_LEN * sizeof(char));
		if(!create_main_page(conn_fd, &buf)) {
			return;
		}
		
		http_send_resp(conn_fd, buf, proto, "text/html");
		return;
	}	

	f_name = (char *)malloc(strlen(path) + 2);
	*f_name = '.';
	strcpy(f_name + 1, path);

	char *mime_type = get_mime_type(path);	
	file = fopen(f_name, "r");
	
	if(!file) {
		http_send_err(conn_fd, proto, 404, ERR_404_MSG);
		return;
	}
	
	http_send_file(conn_fd, file, proto, mime_type);
	fclose(file);
}

void run(int listen_fd) {
	int conn_fd, fork_res;

	while(1) {
		conn_fd = Accept(listen_fd, NULL, NULL);

		if(fork() == 0) {
			close(listen_fd);
			serve(conn_fd);
			close(conn_fd);
			exit(0);
		} else {
			close(conn_fd);
		}
	}
}

int main(int argc, char *argv[]) {
	char *port = HTTP_PORT;
	int listen_fd;

	if(argc > 1) {
		if(argc != 2) {
			errx(1, "Usage: ./mojweb [tcp_port]");
		}
	
		port = argv[1];
	}

	listen_fd = Socket(PF_INET, SOCK_STREAM, 0);
	bind_service(listen_fd, port, SOCK_STREAM);
	Listen(listen_fd, BACKLOG);
	run(listen_fd);

	return 0;
}
