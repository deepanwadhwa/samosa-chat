#define _GNU_SOURCE
#define main samosa_embedded_engine_main
#include "../src/qwen36b.c"
#undef main

#include <assert.h>

static void *run_server_thread(void *opaque){
    SamosaHttpServer *server=(SamosaHttpServer *)opaque;
    assert(samosa_http_server_run(server));
    return NULL;
}

static int connect_local(int port){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_in address={0};address.sin_family=AF_INET;
    address.sin_port=htons((uint16_t)port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!connect(fd,(struct sockaddr *)&address,sizeof(address)));return fd;
}

static char *request(int port,const char *wire){
    int fd=connect_local(port);assert(samosa_send_all(fd,wire,strlen(wire)));
    shutdown(fd,SHUT_WR);ServeBuffer response={0};char chunk[1024];ssize_t n;
    while((n=recv(fd,chunk,sizeof(chunk),0))>0)assert(serve_buffer_append(&response,chunk,(size_t)n));
    close(fd);return response.data;
}

int main(void){
    assert(context_fits(0,16384,8192));
    assert(context_fits(24000,500,76));
    assert(!context_fits(24000,500,77));
    assert(!context_fits(24576,1,1));

    assert(piece_ends_sentence("2003.",5));
    assert(piece_ends_sentence(".\n\n",3));
    assert(piece_ends_sentence(" done!)\n",8));
    assert(piece_ends_sentence("?\"",2));
    assert(piece_ends_sentence("cost.**",7));
    assert(!piece_ends_sentence(" trains",7));
    assert(!piece_ends_sentence("\n\n",2));
    assert(!piece_ends_sentence("list:\n",6));
    assert(!piece_ends_sentence(")",1));

    ServeBuffer escaped={0};
    assert(serve_json_escape(&escaped,"a\n\"b",4));
    assert(!strcmp(escaped.data,"a\\n\\\"b"));free(escaped.data);

    ServeScheduler scheduler;serve_scheduler_init(&scheduler,0);
    scheduler.active=1;assert(serve_scheduler_acquire(&scheduler,NULL)==0);
    scheduler.active=0;pthread_cond_destroy(&scheduler.cv);pthread_mutex_destroy(&scheduler.mu);

    atomic_int sink_cancel;atomic_init(&sink_cancel,0);
    ServeTokenSink special_sink={.thinking_open=1,.close_token=12,.eos_token=10,
        .eot_token=11,.cancel=&sink_cancel};
    assert(serve_token_sink(10,&special_sink)==1);
    assert(serve_token_sink(11,&special_sink)==1);
    assert(serve_token_sink(12,&special_sink)==0&&!special_sink.thinking_open);

    SamosaServeContext context={.started=now_s()};
    snprintf(context.app_html_path,sizeof(context.app_html_path),"assets/app.html");
    snprintf(context.app_logo_path,sizeof(context.app_logo_path),"assets/samosa-chat.png");
    atomic_init(&context.cancel,0);pthread_mutex_init(&context.stats_mu,NULL);
    serve_scheduler_init(&context.scheduler,2);
    SamosaHttpServer server;assert(samosa_http_server_init(&server,0,samosa_serve_handler,&context));
    pthread_t thread;assert(!pthread_create(&thread,NULL,run_server_thread,&server));

    char *health=request(server.port,"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(health,"HTTP/1.1 200 OK"));assert(strstr(health,"\"status\":\"ok\""));
    assert(strstr(health,"\"context_limit_tokens\":24576"));free(health);
    double warm_rss=rss_gb();
    for(int i=0;i<20;i++){
        health=request(server.port,"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
        assert(strstr(health,"HTTP/1.1 200 OK"));free(health);
    }
    assert(rss_gb()-warm_rss<0.01);

    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    GenStats terminal={.prompt=3,.generated=2,.model_stopped=1};
    assert(serve_send_stream_end(pair[0],&terminal));
    char terminal_wire[2048]={0};ssize_t terminal_n=recv(pair[1],terminal_wire,sizeof(terminal_wire)-1,0);
    assert(terminal_n>0);assert(strstr(terminal_wire,"\"session_saved\":null"));
    terminal.session_save_requested=1;terminal.session_save_failed=1;
    assert(serve_send_stream_end(pair[0],&terminal));
    terminal_n=recv(pair[1],terminal_wire,sizeof(terminal_wire)-1,0);
    assert(terminal_n>0);terminal_wire[terminal_n]=0;
    assert(strstr(terminal_wire,"\"session_saved\":false"));
    close(pair[0]);close(pair[1]);

    char *models=request(server.port,"GET /v1/models HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(models,"qwen3.6-35b-a3b"));free(models);
    char *root=request(server.port,"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(root,"Your model. Your machine."));
    assert(strstr(root,"/v1/chat/completions"));
    assert(strstr(root,"Content-Security-Policy: default-src 'self'"));free(root);
    char *logo=request(server.port,"GET /assets/samosa-chat.png HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(logo,"Content-Type: image/png"));free(logo);
    char *cancel=request(server.port,"POST /v1/cancel HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    assert(strstr(cancel,"\"cancelled\":true"));assert(atomic_load(&context.cancel));free(cancel);
    char *missing=request(server.port,"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(missing,"HTTP/1.1 404 Not Found"));free(missing);
    char *stop=request(server.port,"POST /v1/shutdown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    assert(strstr(stop,"\"shutting_down\":true"));free(stop);
    pthread_join(thread,NULL);samosa_http_server_destroy(&server);
    pthread_mutex_destroy(&context.stats_mu);pthread_cond_destroy(&context.scheduler.cv);
    pthread_mutex_destroy(&context.scheduler.mu);
    puts("samosa serve components: ok");return 0;
}
