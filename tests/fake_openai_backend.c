#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "samosa_http.h"

static SamosaHttpServer *active_server;

static void sleep_ms(long ms) {
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) && errno == EINTR) {}
}

/* The compiled-gateway crash tests need to stop at a durable boundary, not
   merely race an arbitrary model response.  A marker is atomically claimed
   once, so the restarted gateway receives the normal response. */
static int claim_marker(const char *name) {
    const char *path = getenv(name);
    if (!path || !*path) return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void stop_server(int number) {
    (void)number;
    if (active_server) samosa_http_server_stop(active_server);
}

static int handler(SamosaHttpServer *server, int fd,
                   const SamosaHttpRequest *request, void *opaque) {
    (void)opaque;
    /* /healthz (not just /health) so this binary can also stand in for
       SAMOSA_QWEN_ENGINE in tests -- backend_probe() in src/samosa_gateway.c
       probes /healthz specifically for the "qwen" backend name. */
    if (!strcmp(request->method, "GET") && (!strcmp(request->path, "/health") || !strcmp(request->path, "/healthz"))) {
        /* T2.3 (docs/TASKS_UI_CHUTNI.md) readiness-safe model activation:
           gives a test a deterministic window to act (e.g. swap the model
           file to force a fingerprint mismatch) before the switch's
           watchdog ever observes a successful probe. Zero by default --
           nothing but a test sets this. */
        const char *delay = !strcmp(request->path, "/health")
            ? getenv("SAMOSA_FAKE_BONSAI_HEALTH_DELAY_MS") : NULL;
        if (!delay || !*delay) delay = getenv("SAMOSA_FAKE_HEALTH_DELAY_MS");
        if (delay && *delay) sleep_ms(atol(delay));
        return samosa_http_response(fd, 200, "application/json", "{\"status\":\"ok\"}", NULL);
    }
    /* T0.3 (docs/TASKS_UI_CHUTNI.md): real-extractor regression for the PDF
       page-batch-cap fix. Placed first so its specific goal text always
       shadows the generic "triaging filenames"/"classifying skimmed files"
       fallbacks further down. Drives doc.read against a real >5-page PDF
       through the real samosa-extract, then finishes on text that only
       appears on page 7 -- provable only if every batch was actually read. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "triaging filenames") && strstr(request->body, "find pdf paging probe"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"conf\\\":\\\"high\\\",\\\"why\\\":\\\"only candidate\\\"}]\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "classifying skimmed files") && strstr(request->body, "find pdf paging probe"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"v\\\":\\\"match\\\",\\\"why\\\":\\\"content fits\\\"}]\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find pdf paging probe") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_pdf_paging\",\"type\":\"function\",\"function\":{"
            "\"name\":\"doc.read\",\"arguments\":\"{\\\"path\\\":\\\"multipage_7pages.pdf\\\",\\\"detail\\\":\\\"text\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find pdf paging probe") && strstr(request->body, "\"role\":\"tool\"")) {
        if (!strstr(request->body, "Page 7 of 7"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"Page 7 was not in the tool result -- pagination is broken.\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_pdf_paging\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"multipage_7pages.pdf\\\","
            "\\\"evidence\\\":\\\"Page 7 of 7\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":0,\\\"notes\\\":\\\"Read all seven pages.\\\"}\"}}]}}]}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "slow interactive probe")) {
        sleep_ms(1200);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"slow interactive reply\"}}]}", NULL);
    }
    /* JI.2 process-crash seam: return one full triage batch, then hold the
       second.  The test SIGKILLs the gateway while this request is in flight;
       its restart must retain the first 16 durable verdicts exactly once. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "triaging filenames") && strstr(request->body, "find triage crash fixtures")) {
        if (!getenv("SAMOSA_FAKE_TRIAGE_FIRST") || claim_marker("SAMOSA_FAKE_TRIAGE_FIRST"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"items\\\":[{\\\"i\\\":1,\\\"c\\\":\\\"h\\\"}]}\"}}]}", NULL);
        if (claim_marker("SAMOSA_FAKE_TRIAGE_DELAY")) sleep_ms(5000);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"items\\\":[{\\\"i\\\":1,\\\"c\\\":\\\"m\\\"}]}\"}}]}", NULL);
    }
    /* JI.6 process-crash seam: the read result has already been appended to
       convo.json before this held verification turn.  On restart the saved
       conversation must produce the finish without a duplicate tool event. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find verify crash fixture") && strstr(request->body, "\"role\":\"tool\"")) {
        if (claim_marker("SAMOSA_FAKE_VERIFY_DELAY")) sleep_ms(5000);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_crash\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"crash-probe.txt\\\","
            "\\\"evidence\\\":\\\"durable crash probe content\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":0,\\\"notes\\\":\\\"Recovered the crash probe.\\\"}\"}}]}}]}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find verify crash fixture") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_crash\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"crash-probe.txt\\\"}\"}}]}}]}", NULL);
    /* JI.0 fallback: an implicit finding request reaches the model classifier
       and is routed to the same read-only find pipeline. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Classify this local-files request") && strstr(request->body, "my university diploma"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"kind\\\":\\\"find\\\"}\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "my university diploma") && !strstr(request->body, "Classify this local-files request") &&
        !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_diploma\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"diploma_bsc_2020.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "my university diploma") && strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_diploma\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"diploma_bsc_2020.txt\\\",\\\"evidence\\\":\\\"Bachelor of Science in Computer Science, 2020\\\",\\\"confidence\\\":\\\"high\\\"}],\\\"rejected_count\\\":2,\\\"notes\\\":\\\"Found the diploma.\\\"}\"}}]}}]}", NULL);
    /* ---- JI.8 scenario (d): education certificates (RC2 lock — no "pet") ----
       Triage: the education goal gets confidences for its 3-file folder.
       The handler MUST NOT mention "pet" anywhere — asserted by the test. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "triaging filenames") && strstr(request->body, "find my education certificates"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"conf\\\":\\\"high\\\",\\\"why\\\":\\\"name suggests education\\\"},"
            "{\\\"i\\\":2,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"uninformative\\\"},"
            "{\\\"i\\\":3,\\\"conf\\\":\\\"low\\\",\\\"why\\\":\\\"unrelated\\\"}]\"}}]}", NULL);
    /* Classify for education: match the diploma, maybe the anonymous one. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "classifying skimmed files") && strstr(request->body, "find my education certificates"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"v\\\":\\\"match\\\",\\\"why\\\":\\\"diploma content\\\"},"
            "{\\\"i\\\":2,\\\"v\\\":\\\"no\\\",\\\"why\\\":\\\"unrelated\\\"},"
            "{\\\"i\\\":3,\\\"v\\\":\\\"no\\\",\\\"why\\\":\\\"unrelated\\\"}]\"}}]}", NULL);
    /* Verify for education: read the diploma, then finish with one match. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find my education certificates") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_edu\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"diploma_bsc_2020.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\"") && strstr(request->body, "find my education certificates"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_edu\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"diploma_bsc_2020.txt\\\","
            "\\\"evidence\\\":\\\"Bachelor of Science in Computer Science, 2020\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":2,\\\"notes\\\":\\\"Found the diploma.\\\"}\"}}]}}]}", NULL);

    /* ---- JI.8 scenario (f): sweep contract (two matches + unreadable) ----
       Triage: 5 files — two targets high, junk medium, image file medium. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "triaging filenames") && strstr(request->body, "find all vet records"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"conf\\\":\\\"high\\\",\\\"why\\\":\\\"vet record name\\\"},"
            "{\\\"i\\\":2,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"anonymous\\\"},"
            "{\\\"i\\\":3,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"uninformative\\\"},"
            "{\\\"i\\\":4,\\\"conf\\\":\\\"high\\\",\\\"why\\\":\\\"vaccination name\\\"},"
            "{\\\"i\\\":5,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"image file\\\"}]\"}}]}", NULL);
    /* Classify for sweep: both targets match, junk no, image parked. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "classifying skimmed files") && strstr(request->body, "find all vet records"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"v\\\":\\\"match\\\",\\\"why\\\":\\\"vet record\\\"},"
            "{\\\"i\\\":2,\\\"v\\\":\\\"no\\\",\\\"why\\\":\\\"unrelated\\\"},"
            "{\\\"i\\\":3,\\\"v\\\":\\\"match\\\",\\\"why\\\":\\\"vaccination record\\\"}]\"}}]}", NULL);
    /* Verify for sweep: read both targets, then finish with both matches
       + the unreadable image in unreadable[]. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find all vet records") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_vet1\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"miso_vet_checkup.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\"") && strstr(request->body, "find all vet records") &&
        !strstr(request->body, "call_finish_sweep"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_sweep\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"miso_vet_checkup.txt\\\","
            "\\\"evidence\\\":\\\"Miso annual checkup 2026\\\",\\\"confidence\\\":\\\"high\\\"},"
            "{\\\"path\\\":\\\"titli_vaccination_2023.txt\\\","
            "\\\"evidence\\\":\\\"Titli rabies booster 2023\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":2,"
            "\\\"unreadable\\\":[{\\\"path\\\":\\\"scan_unknown.png\\\",\\\"reason\\\":\\\"ocr_unavailable\\\"}],"
            "\\\"notes\\\":\\\"Found both vet records. One image could not be read.\\\"}\"}}]}}]}", NULL);

    /* Phase A triage (JI.2, confidence): the system prompt asks for a JSON array
       of per-file confidence. Index 1 (sorted) is high; the rest medium — so
       every file is a survivor and nothing is dropped (the E-JI1 lesson). */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "triaging filenames"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"conf\\\":\\\"high\\\",\\\"why\\\":\\\"name matches\\\"},"
            "{\\\"i\\\":2,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"uninformative\\\"},"
            "{\\\"i\\\":3,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"uninformative\\\"},"
            "{\\\"i\\\":4,\\\"conf\\\":\\\"medium\\\",\\\"why\\\":\\\"uninformative\\\"}]\"}}]}", NULL);
    /* Phase C classify (JI.4): the skim rows come back match/maybe so every
       readable survivor stays on the shortlist for the verify loop. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "classifying skimmed files"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"[{\\\"i\\\":1,\\\"v\\\":\\\"match\\\",\\\"why\\\":\\\"content fits\\\"},"
            "{\\\"i\\\":2,\\\"v\\\":\\\"maybe\\\",\\\"why\\\":\\\"unclear\\\"},"
            "{\\\"i\\\":3,\\\"v\\\":\\\"maybe\\\",\\\"why\\\":\\\"unclear\\\"},"
            "{\\\"i\\\":4,\\\"v\\\":\\\"maybe\\\",\\\"why\\\":\\\"unclear\\\"}]\"}}]}", NULL);
    /* Answer-resume finish (JI.6): only fires when BOTH the user's answer
       ("the cafe one") and the run-1 read result ("Cafe total") are in the
       conversation — a direct lock on RC4 (the run-1 tool result must survive). */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "the cafe one") && strstr(request->body, "Cafe total"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_receipt\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"receipt-b.txt\\\","
            "\\\"evidence\\\":\\\"Cafe total 4.50\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":1,\\\"notes\\\":\\\"Found the cafe receipt.\\\"}\"}}]}}]}", NULL);
    /* Run-1 receipt sweep: after reading receipt-b.txt, ask which receipt.
       Keyed on the goal, not on content, because the skim index now puts every
       file's first lines (incl. "Cafe total") into every verify turn. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\"") && strstr(request->body, "find my receipt"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_ask_receipt\",\"type\":\"function\",\"function\":{"
            "\"name\":\"ask_user\",\"arguments\":\"{\\\"question\\\":\\\"Which receipt: the cafe or the coffee shop?\\\"}\"}}]}}]}", NULL);
    /* Cat-medical verify: after reading cat-medical-note.txt, finish (JI.5). */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\"") && strstr(request->body, "cat medical"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_finish_cat\",\"type\":\"function\",\"function\":{"
            "\"name\":\"finish\",\"arguments\":\"{\\\"matches\\\":[{\\\"path\\\":\\\"cat-medical-note.txt\\\","
            "\\\"evidence\\\":\\\"Titli vaccination record\\\",\\\"confidence\\\":\\\"high\\\"}],"
            "\\\"rejected_count\\\":3,\\\"notes\\\":\\\"Found Titli's vaccination record.\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find cat image document with doc.read") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_doc_read\",\"type\":\"function\",\"function\":{"
            "\"name\":\"doc.read\",\"arguments\":\"{\\\"path\\\":\\\"cat-medical-note.png\\\",\\\"detail\\\":\\\"lines\\\"}\"}}]}}]}", NULL);
    /* Cat-medical verify (first turn): read the plain-text record. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "cat medical") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_cat\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"cat-medical-note.txt\\\"}\"}}]}}]}", NULL);
    /* Receipt verify (first turn): read the cafe receipt. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find my receipt") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_read_receipt\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"receipt-b.txt\\\"}\"}}]}}]}", NULL);
    /* Generic tool-result fallback (the shared doc.read tests round 2): a prose
       reply, which the JI loop nudges once and then ends as model_no_finish. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"Found the matching record at cat-medical-note.txt. It contains Titli's vaccination record.\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Extract structured data")) {
        if (strstr(request->body, "Interlock definition probe")) {
            sleep_ms(800);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"Interlock\\\",\\\"total\\\":7}\"}}]}", NULL);
        }
        if (strstr(request->body, "Image definition probe")) {
            if (!strstr(request->body, "\"content\":[") ||
                !strstr(request->body, "\"type\":\"image_url\"") ||
                !strstr(request->body, "\"url\":\"data:image/png;base64,"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"people\\\":2}\"}}]}", NULL);
        }
        if (strstr(request->body, "PDF first-final page probe")) {
            if (!strstr(request->body, "Page 1:") ||
                !strstr(request->body, "FIRST PAGE TITLE") ||
                !strstr(request->body, "Final page:") ||
                !strstr(request->body, "FINAL AFFILIATION") ||
                strstr(request->body, "MIDDLE PAGE BODY"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"PdfPages\\\",\\\"total\\\":12}\"}}]}", NULL);
        }
        if (strstr(request->body, "Require budget probe")) {
            if (!strstr(request->body, "\"max_tokens\":1536") ||
                !strstr(request->body, "\"chat_template_kwargs\":{\"enable_thinking\":false}") ||
                !strstr(request->body, "\"response_format\":{\"type\":\"json_object\"}"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"Budget\\\",\\\"total\\\":10}\"}}]}", NULL);
        }
        if (strstr(request->body, "Fenced JSON probe")) {
            /* Reproduce Qwen vision's habit of wrapping the object in a ```json
               markdown fence; the gateway must still recover the object. */
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"```json\\n{\\\"merchant\\\":\\\"Fenced\\\",\\\"total\\\":3}\\n```\"}}]}", NULL);
        }
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Describe this image factually"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"caption\\\":\\\"A small repository OCR fixture containing printed text.\\\"}\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Summarize this file in two or three factual sentences")) {
        if (strstr(request->body, "Page 7 of 7") ||
            strstr(request->body, "TAIL_CONTENT_LEAK"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"summary\\\":\\\"ERROR: content beyond the summary budget leaked into the prompt.\\\"}\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"summary\\\":\\\"Repository fixture summary retained as portable Chutni memory.\\\"}\"}}]}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Extract structured data"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"merchant\\\":\\\"Cafe\\\",\\\"total\\\":4.5}\"}}]}", NULL);
    /* T3.2 (docs/TASKS_UI_CHUTNI.md sec5.8): proves samosa_gateway.c's
       attachment_ids resolution actually rewrote the outgoing request body
       (base64 image data URI / doc.read-extracted text) before it reached
       this stand-in backend, rather than forwarding the client's plain
       attachment_ids field through unmodified. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "attachment image probe")) {
        if (!strstr(request->body, "\"type\":\"image_url\"") ||
            !strstr(request->body, "\"url\":\"data:image/png;base64,"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":\"missing image attachment\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"saw the image attachment\"}}]}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "attachment document probe")) {
        if (!strstr(request->body, "Attached document") || !strstr(request->body, "Hello PDFium"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":\"missing document attachment\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"saw the document attachment\"}}]}", NULL);
    }
    /* Explicit Web research plans a dynamic number of focused queries locally
       before any public request. The contextual case only succeeds when the
       gateway supplied the prior assistant answer; without it the fixture
       deliberately echoes the useless referential instruction. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Plan public-web research for this chat turn")) {
        if (!strstr(request->body, "\"chat_template_kwargs\":{\"enable_thinking\":false}") ||
            !strstr(request->body, "\"response_format\":{\"type\":\"json_object\"}"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"planner controls missing\\\","
                "\\\"queries\\\":[\\\"planner reasoning controls missing\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "Acme Zephyr 7 uses sodium ion batteries"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
        if (strstr(request->body, "WAL2 is now the default journal mode"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"What changed in recent SQLite releases and is WAL2 the current default?\\\","
                "\\\"queries\\\":[\\\"SQLite 3.49 release notes\\\","
                "\\\"SQLite current stable release\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "accuracy of the above"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"check the accuracy of the above latest\\\","
                "\\\"queries\\\":[\\\"check the accuracy of the above latest\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "Rewrite this sentence for clarity no public facts are needed"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"resolved_question\\\":\\\"Rewrite this sentence for clarity\\\",\\\"queries\\\":[]}\"}}]}", NULL);
        if (strstr(request->body, "check accuracy above information provide latest thanks"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"check accuracy above information provide latest thanks\\\","
                "\\\"queries\\\":[\\\"check accuracy above information provide latest thanks\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "web tool probe readable insufficient"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"How many cyclosporiasis cases are in South Carolina in 2026?\\\","
                "\\\"queries\\\":[\\\"2026 cyclosporiasis cases South Carolina\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "web tool probe state count followup"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"How many cyclosporiasis cases are in South Carolina in 2026 and what safety guidance applies?\\\","
                "\\\"queries\\\":[\\\"2026 cyclosporiasis cases South Carolina safety guidance\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "web tool probe eight articles"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"What do eight current article fixtures report?\\\","
                "\\\"queries\\\":[\\\"eight current article fixtures\\\"]}\"}}]}", NULL);
        if (strstr(request->body, "check the internet dumbass"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"resolved_question\\\":\\\"check the internet dumbass\\\","
                "\\\"queries\\\":[\\\"check the internet dumbass\\\"]}\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"resolved_question\\\":\\\"Which museum exhibitions and events are available in New York this week?\\\","
            "\\\"queries\\\":[\\\"museum exhibitions New York\\\","
            "\\\"New York museum events this week\\\","
            "\\\"New York museum visitor guide\\\"]}\"}}]}", NULL);
    }
    /* Result selection is an LLM judgement. Fixtures force the failure modes
       that a hostname score could not handle: an unreadable first result and
       a directly relevant state page below a generic CDC result. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Rank web search results for the exact research question")) {
        const char *content = strstr(request->body, "Readable generic national fixture") ?
            "{\\\"indices\\\":[1,2]}" :
            strstr(request->body, "South Carolina reports 30 cyclosporiasis cases") ?
            "{\\\"indices\\\":[2,1]}" :
            strstr(request->body, "web tool probe hostile") ?
            "{\\\"indices\\\":[1,2]}" : "{\\\"indices\\\":[1]}";
        char response[512];
        snprintf(response, sizeof(response),
                 "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                 "\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}]}", content);
        return samosa_http_response(fd, 200, "application/json", response, NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Decide whether the fetched article digests contain enough evidence")) {
        int insufficient = strstr(request->body, "Readable generic national fixture") != NULL;
        return samosa_http_response(fd, 200, "application/json", insufficient ?
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"sufficient\\\":false,\\\"raw_indices\\\":[1,2]}\"}}]}" :
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"sufficient\\\":true,\\\"raw_indices\\\":[]}\"}}]}", NULL);
    }
    /* Phase W (docs/TASKS_WEB_SEARCH.md W5): the model-decided web tool loop.
       Planner rounds are recognised by their system prompt; the second round
       is told apart from the first by the accumulated findings block, which
       is what proves the loop actually fed a tool result back before asking
       again rather than looping on an unchanged prompt. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "deciding whether this turn needs the public web")) {
        /* WK6: a planner that asks for the identical URL every round, however
           clearly the findings say it already failed. This is not a synthetic
           worst case -- it is what Ornith 9B did on the first real-model run
           (2026-07-28), burning its last tool call re-requesting a page that
           had just returned 403. Checked before the findings case so it keeps
           repeating across rounds, exactly as the real model did. */
        if (strstr(request->body, "web tool probe repeat"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"tool\\\":\\\"open_url\\\",\\\"url\\\":\\\"http://example.com/jobs\\\"}\"}}]}", NULL);
        if (strstr(request->body, "Findings so far"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"tool\\\":\\\"none\\\"}\"}}]}", NULL);
        if (strstr(request->body, "web tool probe open"))
            /* Deliberately wrapped in a reasoning span and a code fence: a
               reasoning model does this, and the gateway must recover the
               object rather than fail the turn. */
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"<think>{\\\"tool\\\":\\\"open_url\\\",\\\"url\\\":\\\"http://decoy.example/\\\"}</think>"
                "Sure.\\n```json\\n{\\\"tool\\\":\\\"open_url\\\",\\\"url\\\":\\\"http://example.com/jobs\\\"}\\n```\"}}]}", NULL);
        if (strstr(request->body, "web tool probe search"))
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"tool\\\":\\\"web_search\\\",\\\"query\\\":\\\"careers page\\\"}\"}}]}", NULL);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"tool\\\":\\\"none\\\"}\"}}]}", NULL);
    }
    /* The answering turn. Reports whether the web evidence actually arrived
       spliced into the user message, so a silently dropped splice fails the
       test instead of looking like a pass. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "web tool probe hostile"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "--- Fetched article digest") &&
            strstr(request->body, "HIGH-STAKES TURN:") &&
            strstr(request->body, "Do not claim that you cannot browse") &&
            !strstr(request->body, "I cannot browse the internet") &&
            !strstr(request->body, "999,999 alleged deaths from an unverified snippet")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"saw verified authority\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"unsafe web grounding\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "web tool probe state count followup"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "--- Fetched article digest") &&
            strstr(request->body, "South Carolina DPH confirms 30 cases") &&
            strstr(request->body, "South Carolina reports 30 cyclosporiasis cases")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"used directly relevant state evidence\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"selected wrong evidence\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "web tool probe readable insufficient"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "Generic CDC background page text") &&
            strstr(request->body, "South Carolina DPH confirms 30 cases")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"continued past readable insufficient page\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"stopped at readable insufficient page\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "web tool probe"))
        return samosa_http_response(fd, 200, "application/json",
            (strstr(request->body, "--- Web page:") || strstr(request->body, "--- Web search results")) &&
            !strstr(request->body, "\"content\":[{\"type\":\"text\"")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"saw web evidence\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"missing web evidence or incompatible content shape\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"seed\":424242"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "--- Selected folder memory inventory") &&
            strstr(request->body, "Selected folder display name: Research") &&
            strstr(request->body, "Chutni is the feature name, not the folder name") &&
            strstr(request->body, "[File: notes.md]") &&
            strstr(request->body, "[File: report.txt]")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"saw Research inventory\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"missing Chutni inventory\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"seed\":424243"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "--- Chutni local memory status") &&
            strstr(request->body, "no current indexed passage matched")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"saw honest no-match status\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"missing no-match status\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "chutni memory probe"))
        return samosa_http_response(fd, 200, "application/json",
            strstr(request->body, "--- Chutni local memory") &&
            strstr(request->body, "[Source: report.txt]") &&
            strstr(request->body, "renewal date June")
                ? "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"saw Chutni memory\"}}]}"
                : "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                  "\"message\":{\"role\":\"assistant\",\"content\":\"missing Chutni memory\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"compiled reply\"}}]}", NULL);
    /* T3.2: stands in for qwen36b.c's own real /v1/settings and /v1/compact
       handlers when this fixture is wired up as SAMOSA_QWEN_ENGINE -- proves
       samosa_gateway.c's generic proxy_request() reaches them (method, path,
       and body all forwarded verbatim) without needing the real 24 GB model. */
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/settings"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"context_limit_tokens\":24576,\"context_limit_mode\":\"configured\","
            "\"model_context_limit_tokens\":32768,\"auto_compact\":true,\"compact_threshold_percent\":80}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/compact"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"before_tokens\":1000,\"after_tokens\":400,\"retained_recent_tokens\":400}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/shutdown")) {
        samosa_http_response(fd, 200, "application/json", "{}", NULL);
        samosa_http_server_stop(server); return 1;
    }
    return samosa_http_json_error(fd, 404, "not_found", "Not found.");
}

int main(int argc, char **argv) {
    int port = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
    if (port < 1) return 2;
    const char *pid_file = getenv("SAMOSA_FAKE_PID_FILE");
    if (pid_file && *pid_file) {
        int pidfd = open(pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (pidfd >= 0) {
            char text[32]; int n = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
            if (n > 0) {
                ssize_t written = write(pidfd, text, (size_t)n);
                (void)written;
            }
            close(pidfd);
        }
    }
    SamosaHttpServer server;
    if (!samosa_http_server_init(&server, port, handler, NULL)) return 2;
    active_server = &server;
    signal(SIGINT, stop_server); signal(SIGTERM, stop_server);
    int ok = samosa_http_server_run(&server);
    samosa_http_server_destroy(&server);
    active_server = NULL;
    return ok ? 0 : 2;
}
