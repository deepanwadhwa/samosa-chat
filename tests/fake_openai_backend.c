#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

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
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/health"))
        return samosa_http_response(fd, 200, "application/json", "{\"status\":\"ok\"}", NULL);
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
        strstr(request->body, "Extract structured data"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"merchant\\\":\\\"Cafe\\\",\\\"total\\\":4.5}\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"compiled reply\"}}]}", NULL);
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
            if (n > 0) (void)write(pidfd, text, (size_t)n);
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
