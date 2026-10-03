/* Pure persistence/resolution coverage for Advanced runtime settings.  The
 * HTTP integration lives in test_settings_compact_proxy.sh; this test stays
 * useful in sandboxes that cannot bind a loopback port. */
#define main samosa_gateway_program_main
#include "../src/samosa_gateway.c"
#undef main

#include <assert.h>

static void check_first_find_without_saved_jobs(const char *temporary) {
    Gateway *gateway = calloc(1, sizeof(*gateway));
    assert(gateway);
    pthread_mutex_init(&gateway->mu, NULL);
    assert(path_copy(gateway->home, sizeof(gateway->home), temporary));
    assert(path_join(gateway->jobs_root, sizeof(gateway->jobs_root), temporary, "new-jobs"));
    assert(path_join(gateway->samosa_decision, sizeof(gateway->samosa_decision), temporary, "decision-helper"));
    assert(write_small_file(gateway->samosa_decision,
        "#!/bin/sh\n"
        "while [ \"$1\" != --request ]; do shift; done\n"
        "shift\n"
        "read -r request <\"$1\"\n"
        "[ \"$request\" = '{\"goal\":\"find Person X\"}' ] || exit 1\n"
        "printf '%s\\n' '{\"ok\":true,\"action\":\"find\"}'\n"));
    assert(chmod(gateway->samosa_decision, 0700) == 0);
    assert(access(gateway->jobs_root, F_OK) != 0);
    char error[256] = {0};
    char *reply = jobs_decision_invoke(gateway, "route", "{\"goal\":\"find Person X\"}",
                                     NULL, NULL, error, sizeof(error));
    assert(reply && strstr(reply, "\"action\":\"find\""));
    free(reply);
    /* No temporary request remains after successful first-use routing. */
    assert(rmdir(gateway->jobs_root) == 0);
    assert(unlink(gateway->samosa_decision) == 0);
    pthread_mutex_destroy(&gateway->mu);
    free(gateway);
}

int main(void) {
    char temporary[] = "/tmp/samosa-runtime-settings-XXXXXX";
    assert(mkdtemp(temporary));
    check_first_find_without_saved_jobs(temporary);
    Gateway gateway = {0};
    assert(path_copy(gateway.home, sizeof(gateway.home), temporary));
    assert(path_copy(gateway.backend, sizeof(gateway.backend), "qwen"));

    char config_path[PATH_MAX];
    assert(path_join(config_path, sizeof(config_path), gateway.home, "config.json"));
    assert(write_small_file(config_path,
        "{\"search\":{\"provider\":\"fixture\",\"providers\":{\"fixture\":"
        "{\"api_key\":\"keep-me\"}}},\"unrelated\":{\"keep\":true}}\n"));

    RuntimeConfig config;
    runtime_config_load(&gateway, gateway.backend, &config);
    assert(config.cpu_auto && config.context_auto);
    assert(config.auto_compact && config.compact_threshold_percent == 80);

    config.cpu_auto = 0; config.cpu_threads = 1;
    config.context_auto = 0; config.context_tokens = 4096;
    config.auto_compact = 0; config.compact_threshold_percent = 75;
    assert(runtime_config_save(&gateway, gateway.backend, &config));

    char *raw = read_file_limit(config_path, 1 << 20);
    assert(raw && strstr(raw, "\"api_key\":\"keep-me\""));
    assert(strstr(raw, "\"unrelated\":{\"keep\":true}"));
    assert(strstr(raw, "\"cpu_threads\":1"));
    assert(strstr(raw, "\"context_tokens\":4096"));
    free(raw);

    RuntimeConfig reloaded;
    runtime_config_load(&gateway, gateway.backend, &reloaded);
    assert(!reloaded.cpu_auto && reloaded.cpu_threads == 1);
    assert(!reloaded.context_auto && reloaded.context_tokens == 4096);
    assert(!reloaded.auto_compact && reloaded.compact_threshold_percent == 75);

    unsetenv("OMP_NUM_THREADS"); unsetenv("SAMOSA_CONTEXT_TOKENS");
    RuntimeEffective effective;
    runtime_effective(&gateway, gateway.backend, &reloaded, &effective);
    assert(effective.cpu_effective == 1 && !effective.cpu_locked);
    assert(effective.context_effective == 4096 && !effective.context_locked);

    assert(unlink(config_path) == 0);
    assert(rmdir(temporary) == 0);
    puts("test_runtime_settings: PASS");
    return 0;
}
