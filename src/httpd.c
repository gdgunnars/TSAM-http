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

/* ----- GLOBAL VARIABLES ----- */
FILE *logfile = NULL;
int sockfd;

/* ----- TYPEDEFS ----- */
typedef enum Method {
	GET,
	HEAD,
	POST,
	PUT,
	DELETE,
	CONNECT,
	OPTIONS,
	TRACE
} Method;

const char* methods[] = {
	"GET",
	"HEAD",
	"POST",
	"PUT",
	"DELETE",
	"CONNECT",
	"OPTIONS",
	"TRACE"
};


typedef struct Request {
	Method method;
	GString *path;
    GString *query;
    GString *http_version;
	GString *user_agent;
	GString *host;
	GString *content_type;
	GString *content_length;
	GString *accept_language;
	GString *accept_encoding;
	GString *connection;
	GString *msg_body;
	// TODO: maybe we need this?
	/*
	bool connection_close;
	GHashTable* headers;
	*/
} Request;

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
    request->msg_body = g_string_new(header_and_body[1]);

    fprintf(stdout, "Msg body: %s\n", request->msg_body->str);

    // Check if body is empty
    if (request->msg_body->len == 0) {
        fprintf(stdout, "string is empty\n");
    } 
    
    // Check the method of the message
    if (g_str_has_prefix(message->str, "GET")) {
        request->method = GET;
    }
    else if (g_str_has_prefix(message->str, "HEAD")) {
        request->method = HEAD;
    }
    else if (g_str_has_prefix(message->str, "POST")) {
        request->method = POST;
    }
    else if (g_str_has_prefix(message->str, "PUT")) {
        request->method = PUT;
    }
    else if (g_str_has_prefix(message->str, "DELETE")) {
        request->method = DELETE;
    }
    else if (g_str_has_prefix(message->str, "CONNECT")) {
        request->method = CONNECT;
    }
    else if (g_str_has_prefix(message->str, "OPTIONS")) {
        request->method = OPTIONS;
    }
    else if (g_str_has_prefix(message->str, "TRACE")) {
        request->method = TRACE;
    }
    else {
        // TODO: Unknown prefix, should probably return immediatly with some perror!
    }
    
     
    
    
    gchar **split_message = g_strsplit(header_and_body[0], "\n", 2);
    // Split the message on a newline to simplify extracting headers
    // header_1[0] = method, [1] = path,  [2] = version
    gchar **header_1 = g_strsplit(split_message[0], " ", 3);
    fprintf(stdout, "Method: %s\n", header_1[0]);

    // This is stupid and asks for " const char * const* "
    /*if (g_strv_contains (header_1[1], "ab")) {
        printf("----- Mathcing is true ------");
    }*/
    //Will do dirty hax until we find out how to use it.
    gchar **path_and_query = NULL;
    if(str_contains_query(header_1[1])) {
        // Since we have a query, we split the string on ?
        path_and_query = g_strsplit(header_1[1], "?", 2);
        // Set the request values correctly.
        request->path = g_string_new(path_and_query[0]);
        request->query = g_string_new(path_and_query[1]);

        
        printf ("query: %s\n", request->query->str);

        //TODO: Do we need to split the fragment from the query ? (fragment => comes after # )
    } else {
        request->path = g_string_new(header_1[1]);
        printf ("Path: %s\n", request->path->str);
    }
    
    
    

    fprintf(stdout, "version: %s\n", header_1[2]);
    
    // TODO: Parse rest of query into the proper variablez

    // TODO: We need to remember to use g_strfreev()  to free the memory.
    g_strfreev(header_and_body) ;
    return true;
}

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
	
    int r;
    struct sockaddr_in server, client;
    GString *message = g_string_sized_new(1024);

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
	

    while (1337) {
        // We first have to accept a TCP connection, connfd is a fresh
        // handle dedicated to this connection.
        socklen_t len = (socklen_t) sizeof(client);
        int connfd = accept(sockfd, (struct sockaddr *) &client, &len);
        if (connfd == -1) {
            perror("accept");
            exit(EXIT_FAILURE);
        }

        // Empty message.
        g_string_truncate (message, 0); 

        // Receive from connfd, not sockfd.
        ssize_t n = recv(connfd, message->str, message->allocated_len - 1, 0);
        if (n == -1) {
            perror("recv");
            exit(EXIT_FAILURE);
        }
        
        // Create a Request and fill into the various fields, using the message received
        Request request;
        fill_request(message, &request);

        // Print the complete message on screen.
        //printf("%s\n", message->str);

        // Send the message back.
        r = send(connfd, message->str, (size_t) n, 0);
        if (r == -1) {
            perror("send");
            exit(EXIT_FAILURE);
        }

        // Close the connection.
        r = shutdown(connfd, SHUT_RDWR);
        if (r == -1) {
            perror("shutdown");
            exit(EXIT_FAILURE);
        }
        
        r  = close(connfd);
        if (r == -1) {
            perror("close");
            exit(EXIT_FAILURE);
        }
    }
}
