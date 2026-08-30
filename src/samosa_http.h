#ifndef SAMOSA_HTTP_H
#define SAMOSA_HTTP_H

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#define SAMOSA_HTTP_MAX_HEADER (64u << 10)
#define SAMOSA_HTTP_MAX_BODY (4u << 20)
#define SAMOSA_HTTP_MAX_ATTACHMENT_BODY (4ULL << 30)

typedef struct {
    char method[8];
    char path[256];
    char query[256]; /* raw (still percent-encoded) query string after '?', empty if absent */
    char *body;
    size_t body_len;
    /* Oversized attachment uploads are streamed to this private temporary
       file instead of allocating Content-Length bytes. The connection owner
       unlinks it after the handler returns. */
    char body_file[PATH_MAX];
    int is_background;
    char range[128]; /* raw `Range:` header value, empty if absent */
    char ui_token[80]; /* raw `X-Samosa-Token:` header value, empty if absent */
    char host[256]; /* raw `Host:` value, used for same-origin validation */
    char cookie[512]; /* raw `Cookie:` value, used by LAN browser sessions */
    int remote_is_loopback; /* derived from the accepted socket, never a header */
    /* Correlates the microphone, STT, model, TTS, and playback stages of one
       explicitly traced Voice turn. It is opaque diagnostic metadata and is
       never forwarded to a model backend. */
    char voice_turn_id[64]; /* raw `X-Samosa-Voice-Turn:` value, empty if absent */
    char origin[256]; /* raw `Origin:` header value, empty if absent */
    char attachment_media_type[128]; /* raw `X-Samosa-Media-Type:` header value, empty if absent */
    char attachment_filename_b64[600]; /* raw `X-Samosa-Filename-B64:` header value, empty if absent */
} SamosaHttpRequest;

struct SamosaHttpServer;
typedef int (*SamosaHttpHandler)(struct SamosaHttpServer *, int,
                                 const SamosaHttpRequest *, void *);

typedef struct SamosaHttpServer {
    int listener;
    int port;
    char bind_address[INET_ADDRSTRLEN];
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
        case 206: return "Partial Content";
        case 303: return "See Other";
        case 400: return "Bad Request"; case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict"; case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
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

/* Escapes `value` into `out` as JSON string contents (no surrounding quotes),
   stopping cleanly at a character boundary rather than truncating mid-escape.
   Returns the number of bytes written. */
static size_t samosa_json_escape(char *out, size_t cap, const char *value) {
    size_t used=0;
    for (const unsigned char *p=(const unsigned char *)value; *p; ++p) {
        char unit[8]; size_t n;
        switch (*p) {
            case '"':  memcpy(unit,"\\\"",2); n=2; break;
            case '\\': memcpy(unit,"\\\\",2); n=2; break;
            case '\n': memcpy(unit,"\\n",2);  n=2; break;
            case '\r': memcpy(unit,"\\r",2);  n=2; break;
            case '\t': memcpy(unit,"\\t",2);  n=2; break;
            default:
                if (*p<0x20) { n=(size_t)snprintf(unit,sizeof(unit),"\\u%04x",*p); }
                else { unit[0]=(char)*p; n=1; }
        }
        if (used+n+1>cap) break;
        memcpy(out+used,unit,n); used+=n;
    }
    if (cap) out[used]=0;
    return used;
}

/* Phase W (docs/TASKS_WEB_SEARCH.md): `message` used to be interpolated raw,
   on the assumption -- stated in a comment here -- that every caller passes a
   fixed technical string with no JSON metacharacters. Phase W broke that: its
   errors quote configuration keys and provider responses, so an unescaped
   quote produced malformed JSON and gave remote content a way into the error
   body. Escaping here fixes it for every caller rather than requiring each new
   one to remember the old rule. */
static int samosa_http_json_error(int fd, int status, const char *code,
                                  const char *message) {
    char body[2048], safe_message[1024], safe_code[128];
    samosa_json_escape(safe_message,sizeof(safe_message),message?message:"");
    samosa_json_escape(safe_code,sizeof(safe_code),code?code:"error");
    snprintf(body,sizeof(body),
        "{\"error\":{\"message\":\"%s\",\"type\":\"invalid_request_error\","
        "\"code\":\"%s\"}}",safe_message,safe_code);
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
    char *buffer=(char*)malloc(SAMOSA_HTTP_MAX_HEADER+1);
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
    char *query=strchr(request->path,'?');
    if (query) {
        *query=0;
        size_t qn=strlen(query+1);
        if (qn>=sizeof(request->query)) qn=sizeof(request->query)-1;
        memcpy(request->query,query+1,qn); request->query[qn]=0;
    }
    size_t content_length=0;
    char *cursor=line_end+2;
    while (cursor<buffer+header_bytes-2) {
        char *next=strstr(cursor,"\r\n"); if(!next)break;
        *next=0;
        if (!strncasecmp(cursor,"Content-Length:",15)) {
            char *value=cursor+15; while(*value==' '||*value=='\t')value++;
            char *tail=NULL; unsigned long long parsed=strtoull(value,&tail,10);
            unsigned long long limit = (!strcmp(request->method,"POST") &&
                                         !strcmp(request->path,"/v1/attachments"))
                                       ? SAMOSA_HTTP_MAX_ATTACHMENT_BODY
                                       : SAMOSA_HTTP_MAX_BODY;
            if (tail==value || (*tail && *tail!=' ' && *tail!='\t') || parsed>limit ||
                parsed>(unsigned long long)SIZE_MAX) {
                free(buffer); *error_status=413; return 0;
            }
            content_length=(size_t)parsed;
        } else if (!strncasecmp(cursor,"X-Samosa-Priority:",18)) {
            char *value=cursor+18; while(*value==' '||*value=='\t')value++;
            if (!strncasecmp(value,"background",10)) {
                request->is_background = 1;
            }
        } else if (!strncasecmp(cursor,"Range:",6)) {
            char *value=cursor+6; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->range)) n=sizeof(request->range)-1;
            memcpy(request->range,value,n); request->range[n]=0;
        } else if (!strncasecmp(cursor,"X-Samosa-Token:",15)) {
            char *value=cursor+15; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->ui_token)) n=sizeof(request->ui_token)-1;
            memcpy(request->ui_token,value,n); request->ui_token[n]=0;
        } else if (!strncasecmp(cursor,"Host:",5)) {
            char *value=cursor+5; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->host)) n=sizeof(request->host)-1;
            memcpy(request->host,value,n); request->host[n]=0;
        } else if (!strncasecmp(cursor,"Cookie:",7)) {
            char *value=cursor+7; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->cookie)) n=sizeof(request->cookie)-1;
            memcpy(request->cookie,value,n); request->cookie[n]=0;
        } else if (!strncasecmp(cursor,"X-Samosa-Voice-Turn:",20)) {
            char *value=cursor+20; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->voice_turn_id)) n=sizeof(request->voice_turn_id)-1;
            memcpy(request->voice_turn_id,value,n); request->voice_turn_id[n]=0;
        } else if (!strncasecmp(cursor,"Origin:",7)) {
            char *value=cursor+7; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->origin)) n=sizeof(request->origin)-1;
            memcpy(request->origin,value,n); request->origin[n]=0;
        } else if (!strncasecmp(cursor,"X-Samosa-Media-Type:",20)) {
            char *value=cursor+20; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->attachment_media_type)) n=sizeof(request->attachment_media_type)-1;
            memcpy(request->attachment_media_type,value,n); request->attachment_media_type[n]=0;
        } else if (!strncasecmp(cursor,"X-Samosa-Filename-B64:",22)) {
            char *value=cursor+22; while(*value==' '||*value=='\t')value++;
            size_t n=strlen(value);
            if (n>=sizeof(request->attachment_filename_b64)) n=sizeof(request->attachment_filename_b64)-1;
            memcpy(request->attachment_filename_b64,value,n); request->attachment_filename_b64[n]=0;
        } else if (!strncasecmp(cursor,"Transfer-Encoding:",18) &&
                   strcasestr(cursor+18,"chunked")) {
            free(buffer); return 0;
        }
        cursor=next+2;
    }
    size_t present=used-header_bytes;
    if (present>content_length) present=content_length;
    int stream_attachment = content_length>SAMOSA_HTTP_MAX_BODY &&
                            !strcmp(request->method,"POST") &&
                            !strcmp(request->path,"/v1/attachments");
    if (stream_attachment) {
        const char *tmp_root=getenv("TMPDIR");
        if (!tmp_root || tmp_root[0]!='/') tmp_root="/tmp";
        if (snprintf(request->body_file,sizeof(request->body_file),
                     "%s/samosa-upload-XXXXXX",tmp_root)>=(int)sizeof(request->body_file)) {
            free(buffer); *error_status=500; return 0;
        }
        int upload=mkstemp(request->body_file);
        if (upload<0) { free(buffer); *error_status=500; return 0; }
        fchmod(upload,0600);
        size_t written=0;
        while (written<present) {
            ssize_t n=write(upload,buffer+header_bytes+written,present-written);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { close(upload); unlink(request->body_file); request->body_file[0]=0; free(buffer); return 0; }
            written+=(size_t)n;
        }
        free(buffer);
        char chunk[65536];
        while (present<content_length) {
            size_t want=content_length-present<sizeof(chunk)?content_length-present:sizeof(chunk);
            ssize_t n=recv(fd,chunk,want,0);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { close(upload); unlink(request->body_file); request->body_file[0]=0; return 0; }
            size_t off=0;
            while (off<(size_t)n) {
                ssize_t w=write(upload,chunk+off,(size_t)n-off);
                if (w<0 && errno==EINTR) continue;
                if (w<=0) { close(upload); unlink(request->body_file); request->body_file[0]=0; return 0; }
                off+=(size_t)w;
            }
            present+=(size_t)n;
        }
        if (fsync(upload)!=0 || lseek(upload,0,SEEK_SET)<0) {
            close(upload); unlink(request->body_file); request->body_file[0]=0; return 0;
        }
        close(upload);
        request->body=NULL; request->body_len=content_length;
        return 1;
    }
    request->body=(char*)malloc(content_length+1);
    if (!request->body) { free(buffer); *error_status=500; return 0; }
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
        struct sockaddr_storage peer={0}; socklen_t peer_size=sizeof(peer);
        if (getpeername(fd,(struct sockaddr *)&peer,&peer_size)==0) {
            if (peer.ss_family==AF_INET) {
                uint32_t address=ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr);
                request.remote_is_loopback=(address>>24)==127;
            }
#ifdef AF_INET6
            else if (peer.ss_family==AF_INET6) {
                request.remote_is_loopback=IN6_IS_ADDR_LOOPBACK(
                    &((struct sockaddr_in6 *)&peer)->sin6_addr);
            }
#endif
        }
        server->handler(server,fd,&request,server->handler_ctx);
        free(request.body);
        if (request.body_file[0]) unlink(request.body_file);
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
    /* Without this, every forked child (a qwen/bonsai/ornith backend engine,
       or T2.2's curl download subprocess) inherits this listening socket
       across its execve() and keeps it open for as long as that child lives
       -- found for real via a T2.4 test that kills and restarts the gateway
       mid-download: the new process's bind() failed with EADDRINUSE because
       the orphaned curl child was still holding the old listener fd open,
       not because anything was actually still listening on purpose. */
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    int one=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_port=htons((uint16_t)port);
    const char *bind_env = getenv("SAMOSA_BIND");
    if (!bind_env || !*bind_env) {
        bind_env = getenv("SAMOSA_HOST");
    }
    if (bind_env && *bind_env) {
        struct in_addr parsed={0};
        if (inet_pton(AF_INET,bind_env,&parsed)==1) {
            address.sin_addr=parsed;
            if (parsed.s_addr != htonl(INADDR_LOOPBACK)) {
                fprintf(stderr, "[serve] custom bind: %s\n", bind_env);
                fflush(stderr);
            }
        } else {
            close(listener); errno=EINVAL; return 0;
        }
    } else {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if(bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,16)) {
        close(listener); return 0;
    }
    if(port==0){ socklen_t size=sizeof(address); getsockname(listener,(struct sockaddr *)&address,&size);
        server->port=ntohs(address.sin_port); }
    if (!inet_ntop(AF_INET,&address.sin_addr,server->bind_address,
                   sizeof(server->bind_address))) {
        close(listener); return 0;
    }
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
        if(fd<0){
            if(errno==EINTR)continue;
            if(atomic_load(&server->stopping))break;
            fprintf(stderr, "samosa_http_server_run: accept failed: %s\n", strerror(errno)); fflush(stderr);
            return 0;
        }
        /* A request handler may restart a local model before it returns (for
           example, after an on-demand visual specialist has produced the
           answer). Without close-on-exec, that model child inherits this
           accepted client socket and keeps an otherwise completed streaming
           response open until the model exits. The listener already has the
           same protection above; accepted sockets need it independently. */
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(fd);
            continue;
        }
        SamosaHttpConnection *connection=(SamosaHttpConnection*)malloc(sizeof(*connection));
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
