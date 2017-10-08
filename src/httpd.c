/* A TCP echo server.
 *
 * Receive a message on port 32000, turn it into upper case and return
 * it to the sender.
 *
 * Copyright (c) 2016, Marcel Kyas
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Reykjavik University nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MARCEL
 * KYAS NOR REYKJAVIK UNIVERSITY BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <regex.h>
#include <arpa/inet.h>

/* ----- GLOBAL VARIABLES ----- */
const ssize_t BUFFER_SIZE = 1024;
const int TIMEOUT = 30;

FILE *logfile = NULL;
int sockfd;
GQueue *queue;
int r;
struct sockaddr_in server, client;

typedef struct {
	GString *method;
	GString *path;
    GString *query;
    GString *http_version;
    GString *host;
	GString *user_agent;
	GString *content_type;
    GString *content_length;
    GString *accept;
	GString *accept_language;
	GString *accept_encoding;
	GString *connection;
    GString *msg_body;
    GString *status_code;
} Request;

typedef struct {
    int connfd;
    GTimer *timer;
    struct sockaddr_in client;
    int request_count;
} Connection;

void handle_timeout(Connection *connection);
void add_client(Connection *connection, struct sockaddr_in *client, int connfd);
void serve_next_client(Connection *connection);
void close_connection(Connection *connection);

/* Takes in a status code number ast str. 
    and gets returned appropriate header status code. */
char *get_status_code(char *status_code);

/* Generates the response to send back. 
    Header & body (when needed). */
GString *generate_response(Request *request, GString *html);

/* Generate the in memory html response */
GString *generate_html(Request *request, char *ip, uint16_t port);

/* Should be passed one line that contains 1 header field.
    Header field name is extracted and it's content appended
    appropriately in the request struct. */
void parse_header(gchar *line, Request *request);

/* Checks if the passed string includes a ?,
    if so, the string(path) includes a query. */
bool str_contains_query(const char* strv);

/* Parse the request header and keep track of needed info in request struct. */
bool fill_request(GString *message, Request *request);

/* Initializes the request struct with NULL. */
void init_request(Request *req);

/* free's the strings inside the request struct. */
void reset_request(Request *req);

/* Writes to the logfile defined as global variable. */
void write_to_log(Request *request, char *ip, uint16_t port);

int main(int argc, char **argv)
{
	// Check if number of arguments are correct
    if(argc != 2) {
		fprintf(stderr, "Usage: %s <port>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	// Get the port number form command line
	int port = atoi(argv[1]);

	// Open the log file
	logfile = fopen("httpd.log","a");
	if (logfile == NULL) {
		perror("Failed to open/create log file");
		exit(EXIT_FAILURE);
	}

    // Create and bind a TCP socket.
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Network functions need arguments in network byte order instead of
    // host byte order. The macros htonl, htons convert the values.
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(port);
    r = bind(sockfd, (struct sockaddr *) &server, (socklen_t) sizeof(server));
    if (r == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Before the server can accept messages, it has to listen to the
    // welcome port. A backlog of one connection is allowed.
    r = listen(sockfd, 1);
    if (r == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
	}
	fprintf(stdout, "Listening on port %d...\n", port);
	
    queue = g_queue_new();

    while (1337) {

        printf("Current Size of Queue: %d\n", g_queue_get_length(queue));

        // We first have to accept a TCP connection, connfd is a fresh
        // handle dedicated to this connection.
        Connection *connection = g_new0(Connection, 1);

        socklen_t len = (socklen_t) sizeof(client);
        int connfd = accept(sockfd, (struct sockaddr *) &client, &len);

        add_client(connection, &client, connfd);

        //printf("Master socket is: %d\n", sockfd);
        printf("New connection from %s:%d on socket %d\n", 
            inet_ntoa(connection->client.sin_addr), 
            ntohs(connection->client.sin_port), 
            connection->connfd);
        
        serve_next_client(g_queue_peek_head(queue));

        g_queue_foreach(queue, (GFunc) handle_timeout, NULL);
    }
}

void add_client(Connection *connection, struct sockaddr_in *client, int connfd) {
    
    connection->connfd = connfd;

    if (connection->connfd == -1) {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    connection->timer = g_timer_new();
    connection->client = *client;
    connection->request_count = 1;

    g_queue_push_tail(queue, connection);
}

void serve_next_client(Connection *connection) {
    GString *message = g_string_sized_new(BUFFER_SIZE);
    char buffer[BUFFER_SIZE];
    g_string_truncate (message, 0); // empty provided GString variable
    ssize_t n;

    // Recieve message from connection
    do {
        // Receive from connfd, not sockfd.
        n = recv(connection->connfd, buffer, BUFFER_SIZE, 0);
        if (n == -1) {
            perror("recv");
            exit(EXIT_FAILURE);
        }
        if (n == 0) {
            break;
        }
        g_string_append_len(message, buffer, n);
    } while(n >= BUFFER_SIZE);
    printf("Length of message: %zd\n", message->len);
    
    printf("\nRECIEVED MESSAGE:\n%s\n", message->str);
    
    // Create a Request and fill into the various fields, using the message received
    Request request;
    init_request(&request);
    fill_request(message, &request);
    // Generate the response html for GET and POST
    GString *html = generate_html(&request, inet_ntoa(connection->client.sin_addr), ntohs(connection->client.sin_port));
    GString *response = generate_response(&request, html);

    // Adding to log file timestamp, ip, port, requested URL
    write_to_log(&request, inet_ntoa(connection->client.sin_addr), ntohs(connection->client.sin_port));
   
    // Send the message back.
    r = send(connection->connfd, response->str, (size_t) response->len, 0);
    if (r == -1) {
        perror("send");
        exit(EXIT_FAILURE);
    }

    // Close connection if connection is not keep alive
    if (request.connection->len > 0 && g_ascii_strcasecmp(request.connection->str, "keep-alive") != 0) {
        close_connection(connection);
    }

    reset_request(&request);
}

void handle_timeout(Connection *connection) {
    gdouble time_elapsed = g_timer_elapsed(connection->timer, NULL);
    printf("Checking timeout! Elapsed: %p %f\n", (void*)&connection, time_elapsed);
    if (time_elapsed >= TIMEOUT) {
        printf("TRYING TO REMOVE CONNECTION!\n");
        close_connection(connection);
    }
}

void close_connection(Connection *connection) {
    printf("Closing connection from %s:%d on socket %d\n", 
        inet_ntoa(connection->client.sin_addr), 
        ntohs(connection->client.sin_port), 
        connection->connfd);

    // Close the connection.
    r = shutdown(connection->connfd, SHUT_RDWR);
    if (r == -1) {
        perror("shutdown");
        exit(EXIT_FAILURE);
    }

    r  = close(connection->connfd);
    if (r == -1) {
        perror("close");
        exit(EXIT_FAILURE);
    }

    g_timer_destroy(connection->timer);

    // Remove connection from the queue
    g_queue_remove(queue, connection);

    //g_free(connection);
}

char *get_status_code(char *status_code) {
    if (strcmp(status_code, "200") == 0) {
        return "200 OK";
    }
    else if (strcmp(status_code, "405") == 0) {
        return "405 Method Not Allowed";
    }
    else if (strcmp(status_code, "501") == 0) {
        return "501 Not Implemented";
    }
    else if (strcmp(status_code, "505") == 0) {
        return "505 HTTP Version not supported";
    }
    else if (strcmp(status_code, "500") == 0) {
        return "500 Internal Server Error";
    }
    else if (strcmp(status_code, "415") == 0) {
        return "415 Unsupported Media Type";
    }
    else if (strcmp(status_code, "408") == 0) {
        return "408 Request Timeout";
    }
    else if (strcmp(status_code, "417") == 0) {
        return "417 Expectation Failed";
    }
    return "200 OK";
}

GString *generate_response(Request *request, GString *html) {
    GDateTime *time = g_date_time_new_now_local();
    gchar *date_time = g_date_time_format(time, "%a, %m %b %Y %H:%M:%S %Z");
    GString *response = g_string_new(NULL);
    char *status;
    if (request->status_code->len > 0) {
         status = get_status_code(request->status_code->str);
    }
    else {
        status = "200 OK";
    }
    
    GString *http_version = g_string_new(request->http_version->str);

    g_string_printf(response, "%s %s\r\n"
                            "Date: %s\r\n"
                            "Server: S00ber 1337 S3rv3r\r\n"
                            "Content-Length: %zd\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n",
                            http_version->str, status, date_time, html->len);
    if (strcmp(request->status_code->str, "405") == 0) {
        g_string_append_printf(response, "Allow: GET, POST, HEAD\r\n");
    }
    if (strcmp(request->method->str, "GET") == 0 || strcmp(request->method->str, "POST") == 0 ) {
        g_string_append_printf(response, "\r\n%s", html->str);
    }

    g_date_time_unref(time);

    return response;
}

GString *generate_html(Request *request, char *ip, uint16_t port) {
    GString *html = g_string_new(NULL);
    GString *path_and_query = g_string_new(NULL);
    
    g_string_printf(path_and_query, "%s?%s", request->path->str, request->query->str);
    if (request->query->len < 1) {
        g_string_printf(path_and_query, "%s", request->path->str);
    }
    g_string_printf(html, "<!DOCTYPE html>\n<html>\n<head>\r\n\t"
                        "<title>S00b3r 1337 r3sp0ns3 p4g3</title>\n</head>\n<body>\n"
                        "\thttp://%s%s %s:%d\n"
                        "\t%s\n"
                        "</body>\n</html>",
                        request->host->str, path_and_query->str, ip, port, request->msg_body->str);
    g_string_free(path_and_query, TRUE);
    return html;
}

void parse_header(gchar *line, Request *request) {
    // Split line into token and info
    gchar **split = g_strsplit(line, ":", 2);
    gchar *token = g_strstrip(split[0]);
    gchar *info = g_strstrip(split[1]);

    if (g_ascii_strcasecmp(token, "Host") == 0) {
        request->host = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "User-Agent") == 0) {
        request->user_agent = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Content-Type") == 0) {
        request->content_type = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Content-Length") == 0) {
        request->content_length = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Accept") == 0) {
        request->accept = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Accept-Language") == 0) {
        request->accept_language = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Accept-Encoding") == 0) {
        request->accept_encoding = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Connection") == 0) {
        request->connection = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Expect") == 0) {
        // Expectation failed
        request->status_code = g_string_new("417");
    }
    else {
        printf("%s\n", "parse error");
        // TODO: parse error!
    }

    g_strfreev(split);
}

bool str_contains_query(const char* strv) {
    for(size_t i = 0; i < sizeof(strv); i++) {
        if (strv[i] == '?') {
            return TRUE;
        }
    }
    return FALSE;
}

bool fill_request(GString *message, Request *request)
{   
    // Split message into header and body
    gchar **header_and_body = g_strsplit(message->str, "\r\n\r\n", 2);
    // Immediatly assign body to request

    g_string_truncate(request->msg_body, 0);
    request->msg_body = g_string_new(header_and_body[1]);
    
    printf("before firstline split\n");
    // Split the message on a newline to simplify extracting headers
    gchar **first_line_and_the_rest = g_strsplit(header_and_body[0], "\r\n", 2);
    printf("after firstline split\n");
    
    printf("before header1 split\n");
    // header_1[0] = method, [1] = path,  [2] = version
    gchar **header_1 = g_strsplit(first_line_and_the_rest[0], " ", 3);
    printf("after header1 split\n");

    if (g_ascii_strcasecmp(header_1[0], "GET") == 0) {
        request->method = g_string_new("GET");
    }
    else if (g_ascii_strcasecmp(header_1[0], "HEAD") == 0) {
        request->method = g_string_new("HEAD");
    }
    else if (g_ascii_strcasecmp(header_1[0], "POST") == 0) {
        request->method = g_string_new("POST");
    }
    else {
        // Method not implemented
        request->method = g_string_new(header_1[0]);
        request->status_code = g_string_new("501");
    }

    
    //check if we have a query in our path. 
    if(str_contains_query(header_1[1])) {
        printf("before path_query split\n");
        // Since we have a query, we split the string on "?".
        gchar **path_and_query = g_strsplit(header_1[1], "?", 2);
        printf("after path_query split\n");

        // Set the request values correctly.
        request->path = g_string_new(path_and_query[0]);
        request->query = g_string_new(path_and_query[1]);

        //TODO: Do we need to split the fragment from the query ? (fragment => comes after # )
    } else {
        request->path = g_string_new(header_1[1]);
        request->query = g_string_new("");
    }
    
    // Assign the http version.
    request->http_version = g_string_new(header_1[2]);
    if (g_ascii_strcasecmp(request->http_version->str, "HTTP/1.1") != 0 && g_ascii_strcasecmp(request->http_version->str, "HTTP/1.0") != 0) {
        // HTTP version not supported
        request->status_code = g_string_new("505");
    }

    if (g_ascii_strcasecmp(request->http_version->str, "HTTP/1.1") == 0) {
        request->connection = g_string_new("Keep-Alive");
    }
    
    printf("before lines split\n");
    // Split the header into separate lines and parse each line one at a time.
    gchar **lines = g_strsplit(first_line_and_the_rest[1], "\r\n", -1);
    printf("after lines split\n");

    for (guint i = 0; i < g_strv_length(lines); i++) {
        parse_header(lines[i], request);
    }

    /*
    printf("Method: %s\n", request->method->str);
    printf("Path: %s\n", request->path->str);
    printf("query: %s\n", request->query->str);
    printf("version: %s\n", request->http_version->str);
    printf("Host: %s\n", request->host->str);
    printf("User Agent: %s\n", request->user_agent->str);
    printf("Content length: %s\n", request->content_length->str);
    printf("Content type: %s\n", request->content_type->str);
    */

    // TODO: We need to remember to use g_strfreev()  to free the array's.
    g_strfreev(header_and_body);
    g_strfreev(first_line_and_the_rest);
    g_strfreev(header_1);
    g_strfreev(lines);
    return true;
}

void init_request(Request *req) {
    req->method = g_string_new(NULL);
    req->path = g_string_new(NULL);
    req->query = g_string_new(NULL);
    req->http_version = g_string_new(NULL);
    req->host = g_string_new(NULL);
    req->user_agent = g_string_new(NULL);
    req->content_type = g_string_new(NULL);
    req->content_length = g_string_new(NULL);
    req->accept = g_string_new(NULL);
    req->accept_language = g_string_new(NULL);
    req->accept_encoding = g_string_new(NULL);
    req->connection = g_string_new(NULL);
    req->msg_body = g_string_new(NULL);
    req->status_code = g_string_new(NULL);
}

void reset_request(Request *req) {
    g_string_free(req->method, TRUE);
    g_string_free(req->path, TRUE);
    g_string_free(req->query, TRUE);
    g_string_free(req->http_version, TRUE);
    g_string_free(req->host, TRUE);
    g_string_free(req->user_agent, TRUE);
    g_string_free(req->content_type, TRUE);
    g_string_free(req->content_length, TRUE);
    g_string_free(req->accept, TRUE);
    g_string_free(req->accept_language, TRUE);
    g_string_free(req->accept_encoding, TRUE);
    g_string_free(req->connection, TRUE);
    g_string_free(req->msg_body, TRUE);
    g_string_free(req->status_code, TRUE);
}

void write_to_log(Request *request, char *ip, uint16_t port) {
    GDateTime *time = g_date_time_new_now_local();
    gchar *date_time = g_date_time_format(time, "%Y-%m-%dT%H:%M:%SZ");
    if (request->status_code->len == 0) {
        request->status_code = g_string_new("200");
    }
    fprintf(logfile, "%s : %s:%d %s %s : %s\n", date_time, ip, port, request->method->str, request->path->str, request->status_code->str);
    fflush(logfile);
    g_date_time_unref(time);
    
}