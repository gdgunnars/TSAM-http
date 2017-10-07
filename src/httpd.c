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

/* ----- GLOBAL VARIABLES ----- */
FILE *logfile = NULL;
int sockfd;

/* ----- TYPEDEFS ----- */
/*typedef enum {
	GET,
	HEAD,
	POST,
	PUT,
	DELETE,
	CONNECT,
	OPTIONS,
	TRACE
} Method;*/

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
	// TODO: maybe we need this?
	/*
	bool connection_close;
	GHashTable* headers;
	*/
} Request;
/*
typedef enum {
    GET,
    POST,
    HEAD,
    PATH,
    QUERY,
    VERSION,
    HOST,
    USER_AGENT,
    CONTENT_TYPE,
    CONTENT_LENGTH,
    ACCEPT_LANGUAGE,
    ACCEPT_ENCODING,
    CONNECTION,
    BODY
} TokenCode;

typedef struct {
    GString lexeme;
    TokenCode t_code;
} Token;

Token next_token(gchar *string) {
    Token token;
    gchar **token = g_strsplit(string, ": ", 2);

    if(g_ascii_strcasecmp(token[0], g_string_new("Host")) == 0) {

    }

}
*/

bool l33t_strcmp(char* s1, char* s2, size_t n) {
    printf("%s %s\n", s1, s2);
    for(size_t i = 0; i < n; i++) {
        
        if(tolower(s1[i]) != tolower(s2[i])) {
            return false;
        }
    }
    return true;
}

void parse_header(gchar *line, Request *request) {
    // Split line into token and info
    gchar **split = g_strsplit(line, ":", 2);
    gchar *token = g_strstrip(split[0]);
    gchar *info = g_strstrip(split[1]);

    printf("Token: %s\n", token);
    printf("Info: %s\n", info);

    if (g_ascii_strcasecmp(token, "host") == 0) {
        request->host = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "user-agent") == 0) {
        request->user_agent = g_string_new(info);
        printf("--> %s\n", info);
    }
    else if (g_ascii_strcasecmp(token, "Content-Type") == 0) {
        request->content_type = g_string_new(info);
    }
    else if (g_ascii_strcasecmp(token, "Content-Length") == 0) {
        request->user_agent = g_string_new(info);
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
    request->msg_body = g_string_new(header_and_body[1]);

    fprintf(stdout, "Msg body: %s\n", request->msg_body->str);

    // Check if body is empty
    if (request->msg_body->len == 0) {
        fprintf(stdout, "<- string is empty ->\n");
    } 
    
    // Check the method of the message
    if (g_str_has_prefix(message->str, "GET")) {
        request->method = g_string_new("GET");
    }
    else if (g_str_has_prefix(message->str, "HEAD")) {
        request->method = g_string_new("HEAD");
    }
    else if (g_str_has_prefix(message->str, "POST")) {
        request->method = g_string_new("POST");
    }
    else {
        // TODO: Unknown prefix, should probably return immediatly with some perror!
    }
    
    
    gchar **first_line_and_the_rest = g_strsplit(header_and_body[0], "\n", 2);
    // Split the message on a newline to simplify extracting headers
    // header_1[0] = method, [1] = path,  [2] = version
    gchar **header_1 = g_strsplit(first_line_and_the_rest[0], " ", 3);
    fprintf(stdout, "Method: %s\n", header_1[0]);
    
    //check if we have a query in our path. 
    if(str_contains_query(header_1[1])) {
        // Since we have a query, we split the string on "?".
        gchar **path_and_query = g_strsplit(header_1[1], "?", 2);

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
    g_strfreev(header_1);
    
    
    // Split the header into separate lines and parse each line one at a time.
    
    gchar **lines = g_strsplit(first_line_and_the_rest[1], "\n", -1);

    for (guint i = 0; i < g_strv_length(lines); i++) {
        // do stuff
        parse_header(lines[i], request);
    }

    /*
    // get host
    gchar **split_host_from_message = g_strsplit(first_line_and_the_rest[1], "\n", 2);
    g_strfreev(first_line_and_the_rest);
    gchar **host_split = g_strsplit(split_host_from_message[0], ": ", 2);
    request->host = g_string_new(host_split[1]);
    g_strfreev(host_split);

    gchar **user_agent_from_message = g_strsplit(split_host_from_message[1], "\n", 2);
    g_strfreev(split_host_from_message);
    gchar **user_agent_split = g_strsplit(user_agent_from_message[0], ": ", 2);
    request->user_agent = g_string_new(user_agent_split[1]);
    g_strfreev(user_agent_split);

    */
    printf ("Path: %s\n", request->path->str);
    printf ("query: %s\n", request->query->str);
    printf("version: %s\n", request->http_version->str);
    printf("Host: %s\n", request->host->str);
    printf("User Agent: %s\n", request->user_agent->str);
    
    //
    
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
        printf("----------------------\n");
        printf("%s\n", message->str);
        printf("----------------------\n");
        
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
