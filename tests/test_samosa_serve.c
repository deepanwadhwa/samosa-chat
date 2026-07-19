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
    Cfg loaded={0};
    load_cfg(&loaded,"tests/fixtures/context_config");
    assert(loaded.max_position_embeddings==65536);
    assert(loaded.n_layers==2&&loaded.layer_type[0]==0&&loaded.layer_type[1]==1);
    free(loaded.layer_type);

    Model legacy_context={.context_limit=24576};
    assert(context_fits(&legacy_context,0,16384,8192));
    assert(context_fits(&legacy_context,24000,500,76));
    assert(!context_fits(&legacy_context,24000,500,77));
    assert(!context_fits(&legacy_context,24576,1,1));
    int layer_types[]={1,0,1};
    Model context_model={.c={.n_layers=3,.n_kv_heads=2,.head_dim=128,
                              .layer_type=layer_types,.max_position_embeddings=262144}};
    assert(model_configure_context_limit(&context_model,"131072"));
    assert(context_model.model_context_limit==262144);
    assert(context_model.context_limit==131072);
    assert(context_model.kv_bytes_per_token==4096);
    assert(!model_configure_context_limit(&context_model,"262145"));
    assert(context_auto_limit(UINT64_C(16)*1024*1024*1024,262144)==24576);
    assert(context_auto_limit(UINT64_C(64)*1024*1024*1024,262144)==131072);
    assert(compaction_should_run(70,5,5,100,80));
    assert(!compaction_should_run(69,5,5,100,80));
    assert(!compaction_should_run(20,60,0,100,80));
    int boundary_tokens[]={7,1,2,7,3,4,5,6,7,9,10};
    assert(compaction_tail_start(boundary_tokens,11,7,4)==8);
    assert(compaction_tail_start(boundary_tokens,11,99,4)==11);

    char sealed_dir[]="/tmp/samosa-compaction-session-XXXXXX";
    assert(mkdtemp(sealed_dir));
    char sealed_path[PATH_MAX];snprintf(sealed_path,sizeof(sealed_path),
        "%s/session.qws",sealed_dir);
    int sealed_layer_types[]={1};
    Model sealed={.c={.hidden=4,.n_layers=1,.n_kv_heads=1,.head_dim=1,
        .vocab=32,.layer_type=sealed_layer_types},.context_limit=128,.max_t=6};
    sealed.K=calloc(1,sizeof(float *));sealed.V=calloc(1,sizeof(float *));
    sealed.conv_state=calloc(1,sizeof(float *));sealed.recurrent_state=calloc(1,sizeof(float *));
    sealed.K[0]=calloc(6,sizeof(float));sealed.V[0]=calloc(6,sizeof(float));
    int sealed_tokens[]={7,1,2,7,3,9};
    assert(!session_save(&sealed,sealed_tokens,6,sealed_path));
    int *read_tokens=NULL,read_count=0;
    assert(session_read_tokens(&sealed,sealed_path,&read_tokens,&read_count));
    assert(read_count==6&&!memcmp(read_tokens,sealed_tokens,sizeof(sealed_tokens)));
    free(read_tokens);
    FILE *tampered=fopen(sealed_path,"r+b");assert(tampered);
    assert(!fseek(tampered,8+9*4+4+4+1,SEEK_SET));
    int byte=fgetc(tampered);assert(byte!=EOF);
    assert(!fseek(tampered,-1,SEEK_CUR));assert(fputc(byte^1,tampered)!=EOF);
    assert(!fclose(tampered));
    assert(!session_read_tokens(&sealed,sealed_path,&read_tokens,&read_count));
    teacher_state_end(&sealed);
    assert(!remove(sealed_path));assert(!rmdir(sealed_dir));

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

    char chats_dir[]="/tmp/samosa-compaction-api-XXXXXX";assert(mkdtemp(chats_dir));
    SamosaServeContext context={.model=&context_model,.started=now_s(),
        .auto_compact=1,.compact_threshold_percent=80};
    snprintf(context.chats_dir,sizeof(context.chats_dir),"%s",chats_dir);
    snprintf(context.app_html_path,sizeof(context.app_html_path),"assets/app.html");
    snprintf(context.app_logo_path,sizeof(context.app_logo_path),"assets/samosa-chat.png");
    atomic_init(&context.cancel,0);pthread_mutex_init(&context.stats_mu,NULL);
    serve_scheduler_init(&context.scheduler,2);
    SamosaHttpServer server;assert(samosa_http_server_init(&server,0,samosa_serve_handler,&context));
    pthread_t thread;assert(!pthread_create(&thread,NULL,run_server_thread,&server));

    char *health=request(server.port,"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(health,"HTTP/1.1 200 OK"));assert(strstr(health,"\"status\":\"ok\""));
    assert(strstr(health,"\"model_context_limit_tokens\":262144"));
    assert(strstr(health,"\"context_limit_tokens\":131072"));
    assert(strstr(health,"\"compaction\":{\"auto\":true,\"threshold_percent\":80}"));free(health);
    const char *settings_body="{\"context_tokens\":\"auto\",\"auto_compact\":false,\"compact_threshold_percent\":75}";
    char settings_wire[256];snprintf(settings_wire,sizeof(settings_wire),
        "POST /v1/settings HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        strlen(settings_body),settings_body);
    char *settings=request(server.port,settings_wire);
    assert(strstr(settings,"HTTP/1.1 200 OK"));assert(strstr(settings,"\"context_limit_mode\":\"auto\""));
    assert(strstr(settings,"\"auto_compact\":false"));assert(strstr(settings,"\"compact_threshold_percent\":75"));free(settings);
    settings_body="{\"context_tokens\":65536}";
    snprintf(settings_wire,sizeof(settings_wire),
        "POST /v1/settings HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        strlen(settings_body),settings_body);
    settings=request(server.port,settings_wire);
    assert(strstr(settings,"\"context_limit_tokens\":65536"));free(settings);
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
    terminal.compacted=1;terminal.compacted_from_tokens=100;terminal.compacted_to_tokens=20;
    assert(serve_send_stream_end(pair[0],&terminal));
    terminal_n=recv(pair[1],terminal_wire,sizeof(terminal_wire)-1,0);
    assert(terminal_n>0);terminal_wire[terminal_n]=0;
    assert(strstr(terminal_wire,"\"compacted\":true"));
    assert(strstr(terminal_wire,"\"compacted_from_tokens\":100"));
    close(pair[0]);close(pair[1]);

    char *models=request(server.port,"GET /v1/models HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(models,"qwen3.6-35b-a3b"));free(models);
    char *root=request(server.port,"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(root,"Your model. Your machine."));
    assert(strstr(root,"/v1/chat/completions"));
    assert(strstr(root,"Total context capacity"));
    assert(strstr(root,"Auto-compact conversations"));
    assert(strstr(root,"Compact this conversation now"));
    assert(strstr(root,"/v1/compact"));
    assert(strstr(root,"/v1/settings"));
    assert(strstr(root,"Content-Security-Policy: default-src 'self'"));free(root);
    char *logo=request(server.port,"GET /assets/samosa-chat.png HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(logo,"Content-Type: image/png"));free(logo);
    char *cancel=request(server.port,"POST /v1/cancel HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    assert(strstr(cancel,"\"cancelled\":true"));assert(atomic_load(&context.cancel));free(cancel);
    char *missing=request(server.port,"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(strstr(missing,"HTTP/1.1 404 Not Found"));free(missing);
    const char *compact_body="{\"conversation_id\":\"chat-missing\"}";
    char compact_wire[256];snprintf(compact_wire,sizeof(compact_wire),
        "POST /v1/compact HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        strlen(compact_body),compact_body);
    char *compact=request(server.port,compact_wire);
    assert(strstr(compact,"HTTP/1.1 404 Not Found"));assert(strstr(compact,"session_not_found"));free(compact);
    char *stop=request(server.port,"POST /v1/shutdown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    assert(strstr(stop,"\"shutting_down\":true"));free(stop);
    pthread_join(thread,NULL);samosa_http_server_destroy(&server);
    pthread_mutex_destroy(&context.stats_mu);pthread_cond_destroy(&context.scheduler.cv);
    pthread_mutex_destroy(&context.scheduler.mu);
    assert(!rmdir(chats_dir));
    puts("samosa serve components: ok");return 0;
}
