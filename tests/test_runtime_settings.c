/* Pure persistence/resolution coverage for Advanced runtime settings.  The
 * HTTP integration lives in test_settings_compact_proxy.sh; this test stays
 * useful in sandboxes that cannot bind a loopback port. */
#define main samosa_gateway_program_main
#include "../src/samosa_gateway.c"
#undef main

#include <assert.h>

int main(void) {
    char temporary[] = "/tmp/samosa-runtime-settings-XXXXXX";
    assert(mkdtemp(temporary));
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
