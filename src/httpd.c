/* $Id: httpd.c,v 1.0 2017/09/10 04:30:00 hpa Exp $ */
/* ----------------------------------------------------------------------- *
 *   
 *   Copyright 2017 
 *		Guðjón Steinar Sverrisson
 *		Gunnar Davíð Gunnarsson
 *		Hlynur Stefánsson
  * - All Rights Reserved
 *
 *   This program is free software available under the same license
 *   as the "OpenBSD" operating system, distributed at
 *   http://www.openbsd.org/.
 *
 * ----------------------------------------------------------------------- */

/*
 * httpd.c
 *
 * A Simple HTTP Server that accepts GET, POST and HEAD and returns an in memory
 * generated HTML5 page that contains the requested URL, client ip and port and
 * the body of the request (if the request was POST).
 *                                                               d(-_-)b
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <errno.h>
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
int r, i, j, len;
struct sockaddr_in server, client;
struct pollfd fds[200];
int nfds = 1;
int new_sd = -1;
int current_size = 0;
bool close_conn = FALSE;
bool compress_array = FALSE;
char poll_buffer[80];

GHashTable *connections;

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

void array_compression(bool compress_array);
void handle_timeout(int connfd, GTimer *timer);
void serve_next_client(int connfd);

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

    int on = 1;
    connections = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, (GDestroyNotify) g_timer_destroy);

    // Allow socket descriptor to be reuseable  
    r = setsockopt(sockfd, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
    if (r < 0)
    {
        perror("setsockopt() failed");
        close(sockfd);
        exit(-1);
    }

    // Set socket to be nonblocking. All of the sockets for the incoming connections 
    // will also be nonblocking since they will inherit that state from the listening socket.
    r = ioctl(sockfd, FIONBIO, (char *)&on);
    if (r < 0)
    {
      perror("ioctl() failed");
      close(sockfd);
      exit(-1);
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
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Before the server can accept messages, it has to listen to the
    // welcome port. A backlog of one connection is allowed.
    r = listen(sockfd, 32);
    if (r == -1) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
	}
	fprintf(stdout, "Listening on port %d...\n", port);
	
    // Initialize the pollfd structure  
    memset(fds, 0, sizeof(fds));

    // Set up the initial listening socket  
    fds[0].fd = sockfd;
    fds[0].events = POLLIN;

    bool server_is_running = TRUE;

    while (server_is_running) {

        printf("\n###########################################################\n");

        printf("Waiting on poll()...\n");
        r = poll(fds, nfds, 1000*60);
        // Check if poll() failed
        if (r < 0) {
            perror("  poll() failed. Stopping server.");
            break;
        }
        // Check if poll() timed out
        if (r == 0) {
            printf("  poll() timed out, retrying...\n");
            continue;
        }

        // One or more descriptors are readable. Need to determine which ones they are. 
        current_size = nfds;
        for (i = 0; i < current_size; i++) {
            // Loop through to find the descriptors that returned POLLIN and determine whether 
            // it's the listening or the active connection.
            if(fds[i].revents == 0) {
                continue;
            }

            // If revents is not POLLIN, it's an unexpected result, log and end the server.
            if(fds[i].revents != POLLIN) {
                printf("  Error! revents = %d\n", fds[i].revents);
                server_is_running = FALSE;
                break;
            }

            // There is data to read
            if (fds[i].fd == sockfd) {
                // Listening descriptor is readable.
                printf("  Listening socket is readable\n");

                // Accept all incoming connections that are queued up on the listening socket 
                // before we loop back and call poll again.
                do {
                    socklen_t socklen = (socklen_t) sizeof(client);
                    // Accept each incoming connection. If accept fails with EWOULDBLOCK, then we have 
                    // accepted all of them. Any other failure on accept will cause us to end the server.
                    new_sd = accept(sockfd, (struct sockaddr *) &client, &socklen);
                    if (new_sd < 0) {
                        // Check if we have accepted all of the connections
                        if (errno != EWOULDBLOCK) {
                            perror("  accept() failed");
                            server_is_running = FALSE;
                        }
                        break;
                    }
                    
                    printf("New connection from %s:%d on socket %d\n", 
                        inet_ntoa(client.sin_addr), 
                        ntohs(client.sin_port), 
                        new_sd);
                    

                    // Add the new incoming connection to the pollfd structure.
                    fds[nfds].fd = new_sd;
                    fds[nfds].events = POLLIN;
                    // Add connection to hash table with timer
                    g_hash_table_insert(connections, &fds[nfds].fd, g_timer_new());
                    nfds++;
                    // Loop back up and accept another incoming connection

                } while (new_sd != -1);
            }
                
            // This is not the listening socket, therefore an existing connection must be readable.
            else {
                printf("  Descriptor %d is readable\n", fds[i].fd);
                close_conn = FALSE;

                // Receive all incoming data on this socket before we loop back and call poll again.
                serve_next_client(fds[i].fd);

                // If the close_conn flag was turned on, we need to clean up this active connection. 
                // This clean up process includes removing the descriptor.
                if (close_conn) {
                    printf("CLOSING THE MOTHER F-ING CONNECTION YO\n");
                    close(fds[i].fd);
                    g_hash_table_remove(connections, &fds[i].fd);
                    fds[i].fd = -1;
                    compress_array = TRUE;
                }


            }  /* End of existing connection is readable             */
        } /* End of loop through pollable descriptors              */

        // If the compress_array flag was turned on, we need to squeeze together the array and decrement 
        // the number of file descriptors. We do not need to move back the events and revents fields 
        // because the events will always be POLLIN in this case, and revents is output.
        array_compression(compress_array);

        g_hash_table_foreach(connections, (GHFunc)handle_timeout, NULL);

    }   // End of server running

    // Clean up all of the sockets that are open
    for (i = 0; i < nfds; i++) {
        if(fds[i].fd >= 0)
        close(fds[i].fd);
    }
}

void serve_next_client(int connfd) {

    // Reset the timer of this client
    g_hash_table_replace (connections, &connfd, g_timer_new());

    socklen_t addrlen = (socklen_t) sizeof(client);
    getpeername(connfd, (struct sockaddr*) &client, &addrlen);

    printf("\n---------------------------------\n");
    printf("Now serving %s:%d on socket %d\n", 
        inet_ntoa(client.sin_addr), 
        ntohs(client.sin_port), 
        connfd);


    GString *message = g_string_sized_new(BUFFER_SIZE);
    char buffer[BUFFER_SIZE];
    g_string_truncate (message, 0); // empty provided GString variable
    ssize_t n;

    // Receive data on this connection until the recv fails with EWOULDBLOCK. 
    // If any other failure occurs, we will close the connection.
    do {
        // Receive from connfd, not sockfd.
        n = recv(connfd, buffer, BUFFER_SIZE, 0);
        if (n < 0) {
            if (errno != EWOULDBLOCK) {
                perror("  recv() failed");
                close_conn = TRUE;
            }
            break;
        }

        // Check to see if the connection has been closed by the client
        if (n == 0) {
            printf("  Connection closed\n");
            close_conn = TRUE;
            return;
        }
        // Data was received

        g_string_append_len(message, buffer, n);
    } while(n >= BUFFER_SIZE);
    
    printf("Length of message: %zd\n", message->len);
    
    printf("\nRECIEVED MESSAGE:\n%s\n", message->str);
    
    // Create a Request and fill into the various fields, using the message received
    Request request;
    init_request(&request);
    fill_request(message, &request);

    // Close connection if connection is not keep alive
    if (request.connection->len > 0 && g_ascii_strcasecmp(request.connection->str, "keep-alive") != 0) {
        close_conn = TRUE;
    }

    // Generate the response html for GET and POST
    GString *html = generate_html(&request, inet_ntoa(client.sin_addr), ntohs(client.sin_port));
    GString *response = generate_response(&request, html);

    // Adding to log file timestamp, ip, port, requested URL
    write_to_log(&request, inet_ntoa(client.sin_addr), ntohs(client.sin_port));
   
    // Send the message back.
    r = send(connfd, response->str, (size_t) response->len, 0);
    if (r == -1) {
        perror("send");
        exit(EXIT_FAILURE);
    }

    reset_request(&request);
}

void handle_timeout(int connfd, GTimer *timer) {
    gdouble time_elapsed = g_timer_elapsed(timer, NULL);
    //printf("Checking timeout! Elapsed: %f\n", time_elapsed);
    if (time_elapsed >= TIMEOUT) {
        printf("\tConnection on socket %d timed out!\n", connfd);
        for (int z = 0; z < nfds; z++) {
            if (fds[z].fd == connfd) {
                close(fds[z].fd);
                g_hash_table_remove(connections, &fds[z].fd);
                fds[z].fd = -1;
                array_compression(TRUE);
                break;
            }
        }
    }
}

void array_compression(bool compress_array) {
    if (compress_array) {
        compress_array = FALSE;
        for (i = 0; i < nfds; i++) {
            if (fds[i].fd == -1) {
                for(j = i; j < nfds; j++) {
                    fds[j].fd = fds[j+1].fd;
                }
                nfds--;
            }
        }
    }
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

    int content_length = html->len;
    if (g_ascii_strcasecmp(status, "200 OK") != 0 || g_ascii_strcasecmp(request->method->str, "HEAD") == 0) {
        content_length = 0;
    }

    g_string_printf(response, "%s %s\r\n"
                            "Date: %s\r\n"
                            "Server: S00ber 1337 S3rv3r\r\n"
                            "Content-Length: %d\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n",
                            http_version->str, status, date_time, content_length);
    if (strcmp(request->status_code->str, "405") == 0) {
        g_string_append_printf(response, "Allow: GET, POST, HEAD\r\n");
    }
    if (close_conn) {
        g_string_append(response, "Connection: close\r\n");
    }
    else {
        g_string_append(response, "Connection: keep-alive\r\n");
		g_string_append_printf(response, "Keep-Alive: timeout=%d, max=100\r\n", TIMEOUT);
    }


    if (strcmp(request->method->str, "GET") == 0 || strcmp(request->method->str, "POST") == 0 ) {
        g_string_append_printf(response, "\r\n%s", html->str);
    }
    else {
        g_string_append(response, "\r\n");
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
    
    // Split the message on a newline to simplify extracting headers
    gchar **first_line_and_the_rest = g_strsplit(header_and_body[0], "\r\n", 2);
    
    // header_1[0] = method, [1] = path,  [2] = version
    gchar **header_1 = g_strsplit(first_line_and_the_rest[0], " ", 3);

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
        // Since we have a query, we split the string on "?".
        gchar **path_and_query = g_strsplit(header_1[1], "?", 2);

        // Set the request values correctly.
        request->path = g_string_new(path_and_query[0]);
        request->query = g_string_new(path_and_query[1]);

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
    
    // Split the header into separate lines and parse each line one at a time.
    gchar **lines = g_strsplit(first_line_and_the_rest[1], "\r\n", -1);

    for (guint i = 0; i < g_strv_length(lines); i++) {
        parse_header(lines[i], request);
    }

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