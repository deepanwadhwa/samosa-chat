#ifndef SAMOSA_HTTP_H
#define SAMOSA_HTTP_H

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SAMOSA_HTTP_MAX_HEADER (64u << 10)
#define SAMOSA_HTTP_MAX_BODY (4u << 20)

typedef struct {
    char method[8];
    char path[256];
    char *body;
    size_t body_len;
} SamosaHttpRequest;

struct SamosaHttpServer;
typedef int (*SamosaHttpHandler)(struct SamosaHttpServer *, int,
                                 const SamosaHttpRequest *, void *);

typedef struct SamosaHttpServer {
    int listener;
    int port;
    atomic_int stopping;
    pthread_mutex_t connection_mu;
    pthread_cond_t connection_cv;
    int active_connections;
    SamosaHttpHandler handler;
    void *handler_ctx;
} SamosaHttpServer;

static int samosa_send_all(int fd, const void *data, size_t size) {
    const char *cursor=(const char *)data;
    while (size) {
#ifdef MSG_NOSIGNAL
        ssize_t n=send(fd,cursor,size,MSG_NOSIGNAL);
#else
        ssize_t n=send(fd,cursor,size,0);
#endif
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return 0;
        cursor+=n; size-=(size_t)n;
    }
    return 1;
}

static const char *samosa_http_reason(int status) {
    switch (status) {
        case 200: return "OK"; case 204: return "No Content";
        case 400: return "Bad Request"; case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict"; case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

static int samosa_http_headers(int fd, int status, const char *content_type,
                               size_t content_length, const char *extra) {
    char header[2048];
    int n=snprintf(header,sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n%s\r\n",
        status,samosa_http_reason(status),content_type,content_length,
        extra?extra:"");
    return n>0 && (size_t)n<sizeof(header) &&
           samosa_send_all(fd,header,(size_t)n);
}

static int samosa_http_response(int fd, int status, const char *content_type,
                                const char *body, const char *extra) {
    size_t length=body?strlen(body):0;
    return samosa_http_headers(fd,status,content_type,length,extra) &&
           (!length || samosa_send_all(fd,body,length));
}

static int samosa_http_json_error(int fd, int status, const char *code,
                                  const char *message) {
    char body[1024];
    /* Internal callers use fixed technical messages without JSON metacharacters. */
    snprintf(body,sizeof(body),
        "{\"error\":{\"message\":\"%s\",\"type\":\"invalid_request_error\","
        "\"code\":\"%s\"}}",message,code);
    return samosa_http_response(fd,status,"application/json",body,
                                status==429?"Retry-After: 1\r\n":NULL);
}

static int samosa_http_stream_headers(int fd) {
    const char *header=
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "X-Accel-Buffering: no\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n";
    return samosa_send_all(fd,header,strlen(header));
}

static int samosa_http_read_request(int fd, SamosaHttpRequest *request,
                                    int *error_status) {
    memset(request,0,sizeof(*request));
    *error_status=400;
    char *buffer=malloc(SAMOSA_HTTP_MAX_HEADER+1);
    if (!buffer) return 0;
    size_t used=0, header_bytes=0;
    while (used<SAMOSA_HTTP_MAX_HEADER) {
        ssize_t n=recv(fd,buffer+used,SAMOSA_HTTP_MAX_HEADER-used,0);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { free(buffer); return 0; }
        used+=(size_t)n; buffer[used]=0;
        char *end=strstr(buffer,"\r\n\r\n");
        if (end) { header_bytes=(size_t)(end-buffer)+4; break; }
    }
    if (!header_bytes) { free(buffer); *error_status=413; return 0; }
    char *line_end=strstr(buffer,"\r\n");
    if (!line_end) { free(buffer); return 0; }
    *line_end=0;
    if (sscanf(buffer,"%7s %255s",request->method,request->path)!=2) {
        free(buffer); return 0;
    }
    char *query=strchr(request->path,'?'); if(query)*query=0;
    size_t content_length=0;
    char *cursor=line_end+2;
    while (cursor<buffer+header_bytes-2) {
        char *next=strstr(cursor,"\r\n"); if(!next)break;
        *next=0;
        if (!strncasecmp(cursor,"Content-Length:",15)) {
            char *value=cursor+15; while(*value==' '||*value=='\t')value++;
            char *tail=NULL; unsigned long long parsed=strtoull(value,&tail,10);
            if (tail==value || (*tail && *tail!=' ' && *tail!='\t') ||
                parsed>SAMOSA_HTTP_MAX_BODY) {
                free(buffer); *error_status=413; return 0;
            }
            content_length=(size_t)parsed;
        } else if (!strncasecmp(cursor,"Transfer-Encoding:",18) &&
                   strcasestr(cursor+18,"chunked")) {
            free(buffer); return 0;
        }
        cursor=next+2;
    }
    request->body=malloc(content_length+1);
    if (!request->body) { free(buffer); *error_status=500; return 0; }
    size_t present=used-header_bytes;
    if (present>content_length) present=content_length;
    memcpy(request->body,buffer+header_bytes,present);
    free(buffer);
    while (present<content_length) {
        ssize_t n=recv(fd,request->body+present,content_length-present,0);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { free(request->body); request->body=NULL; return 0; }
        present+=(size_t)n;
    }
    request->body[content_length]=0; request->body_len=content_length;
    return 1;
}

typedef struct { SamosaHttpServer *server; int fd; } SamosaHttpConnection;

static void *samosa_http_connection_main(void *opaque) {
    SamosaHttpConnection *connection=(SamosaHttpConnection *)opaque;
    SamosaHttpServer *server=connection->server; int fd=connection->fd;
    free(connection);
    struct timeval timeout={.tv_sec=15,.tv_usec=0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    SamosaHttpRequest request; int error_status=400;
    if (!samosa_http_read_request(fd,&request,&error_status))
        samosa_http_json_error(fd,error_status,"invalid_http_request",
                               "Invalid or oversized HTTP request.");
    else {
        server->handler(server,fd,&request,server->handler_ctx);
        free(request.body);
    }
    close(fd);
    pthread_mutex_lock(&server->connection_mu);
    server->active_connections--;
    pthread_cond_broadcast(&server->connection_cv);
    pthread_mutex_unlock(&server->connection_mu);
    return NULL;
}

static int samosa_http_server_init(SamosaHttpServer *server, int port,
                                   SamosaHttpHandler handler, void *ctx) {
    memset(server,0,sizeof(*server)); server->listener=-1; server->port=port;
    server->handler=handler; server->handler_ctx=ctx; atomic_init(&server->stopping,0);
    pthread_mutex_init(&server->connection_mu,NULL);
    pthread_cond_init(&server->connection_cv,NULL);
    int listener=socket(AF_INET,SOCK_STREAM,0); if(listener<0)return 0;
    int one=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_port=htons((uint16_t)port);
    const char *bind_env = getenv("SAMOSA_BIND");
    if (!bind_env || !*bind_env) {
        bind_env = getenv("SAMOSA_HOST");
    }
    if (bind_env && *bind_env) {
        unsigned long addr = inet_addr(bind_env);
        if (addr != INADDR_NONE) {
            address.sin_addr.s_addr = addr;
            if (addr != htonl(INADDR_LOOPBACK)) {
                fprintf(stderr, "[serve] custom bind: %s\n", bind_env);
                fflush(stderr);
            }
        } else {
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
    } else {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if(bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,16)) {
        close(listener); return 0;
    }
    if(port==0){ socklen_t size=sizeof(address); getsockname(listener,(struct sockaddr *)&address,&size);
        server->port=ntohs(address.sin_port); }
    server->listener=listener; return 1;
}

static void samosa_http_server_stop(SamosaHttpServer *server) {
    if (!atomic_exchange(&server->stopping,1) && server->listener>=0){
        int listener=server->listener;server->listener=-1;
        shutdown(listener,SHUT_RDWR);close(listener);
    }
}

static int samosa_http_server_run(SamosaHttpServer *server) {
    while (!atomic_load(&server->stopping)) {
        int fd=accept(server->listener,NULL,NULL);
        if(fd<0){ if(errno==EINTR)continue; if(atomic_load(&server->stopping))break; return 0; }
        SamosaHttpConnection *connection=malloc(sizeof(*connection));
        if(!connection){ close(fd); continue; }
        connection->server=server; connection->fd=fd;
        pthread_mutex_lock(&server->connection_mu); server->active_connections++;
        pthread_mutex_unlock(&server->connection_mu);
        pthread_t thread;
        if(pthread_create(&thread,NULL,samosa_http_connection_main,connection)){
            close(fd); free(connection); pthread_mutex_lock(&server->connection_mu);
            server->active_connections--; pthread_mutex_unlock(&server->connection_mu); continue;
        }
        pthread_detach(thread);
    }
    pthread_mutex_lock(&server->connection_mu);
    while(server->active_connections>0)
        pthread_cond_wait(&server->connection_cv,&server->connection_mu);
    pthread_mutex_unlock(&server->connection_mu);
    return 1;
}

static void samosa_http_server_destroy(SamosaHttpServer *server) {
    if(server->listener>=0)close(server->listener);
    pthread_cond_destroy(&server->connection_cv);
    pthread_mutex_destroy(&server->connection_mu);
}

#endif
