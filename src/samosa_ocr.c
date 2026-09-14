/* Native Tesseract adapter. No custom inference kernels or Python runtime.
 * read/detect/recognize retain the document sidecar's JSON interface.
 * pdf renders and recognizes one page at a time, reusing one Tesseract API.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <tesseract/capi.h>
#include <allheaders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "json.h"

static char pdf_temp_dir[4096], pdf_temp_image[4096];
static volatile sig_atomic_t renderer_pid;
static void cleanup_pdf(void) {
    if (*pdf_temp_image) unlink(pdf_temp_image);
    if (*pdf_temp_dir) rmdir(pdf_temp_dir);
}
static void interrupted(int signal_number) {
    if (renderer_pid > 0) kill(renderer_pid, SIGTERM);
    cleanup_pdf();
    _exit(128 + signal_number);
}

static const char *data_path(void) {
    const char *override = getenv("SAMOSA_TESSDATA");
    if (override && *override) return override;
    override = getenv("TESSDATA_PREFIX");
    if (override && *override) return override;
    static char path[4096]; char exe[4096];
#ifdef __APPLE__
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size)) return NULL;
#else
    ssize_t size = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (size < 0) return NULL;
    exe[size] = 0;
#endif
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = 0;
    int n = snprintf(path, sizeof path, "%s/../share/tessdata", exe);
    struct stat st;
    return n > 0 && (size_t)n < sizeof path && !stat(path, &st) && S_ISDIR(st.st_mode) ? path : NULL;
}

static int error(const char *code) {
    printf("{\"ok\":false,\"error\":\"%s\"}\n", code);
    return 65;
}
static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void quoted(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static PIX *load_image(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 256 * 1024 * 1024) {
        close(fd); return NULL;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) { close(fd); return NULL; }
    PIX *pix = pixReadStream(file, 0);
    fclose(file);
    if (pix && (int64_t)pixGetWidth(pix) * pixGetHeight(pix) > 40000000) pixDestroy(&pix);
    return pix;
}
static TessBaseAPI *init_api(void) {
    const char *data = data_path();
    const char *lang = getenv("SAMOSA_OCR_LANG");
    TessBaseAPI *api = TessBaseAPICreate();
    if (!api) return NULL;
    if (TessBaseAPIInit3(api, data && *data ? data : NULL, lang && *lang ? lang : "eng")) {
        TessBaseAPIDelete(api); return NULL;
    }
    TessBaseAPISetPageSegMode(api, PSM_AUTO);
    return api;
}
static int recognize(TessBaseAPI *api, PIX *pix, const int *box) {
    TessBaseAPISetImage2(api, pix);
    if (box) {
        TessBaseAPISetPageSegMode(api, PSM_SINGLE_BLOCK);
        TessBaseAPISetRectangle(api, box[0], box[1], box[2] - box[0], box[3] - box[1]);
    } else TessBaseAPISetPageSegMode(api, PSM_AUTO);
    return TessBaseAPIRecognize(api, NULL);
}
static int read_image(TessBaseAPI *api, const char *path, const char *cmd,
                      const int *box, const char *crops, float below) {
    double start = now();
    PIX *pix = load_image(path);
    if (!pix) return error("image_invalid");
    if (box && (box[0] < 0 || box[1] < 0 || box[2] > pixGetWidth(pix) ||
                box[3] > pixGetHeight(pix) || box[2] <= box[0] || box[3] <= box[1])) {
        pixDestroy(&pix); return error("image_invalid");
    }
    if (recognize(api, pix, box)) { pixDestroy(&pix); return error("ocr_process_failed"); }
    if (!strcmp(cmd, "recognize")) {
        char *text = TessBaseAPIGetUTF8Text(api);
        if (!text) { pixDestroy(&pix); return error("ocr_process_failed"); }
        printf("{\"ok\":true,\"text\":"); quoted(text);
        printf(",\"conf\":%.4f}\n", TessBaseAPIMeanTextConf(api) / 100.0);
        TessDeleteText(text);
    } else {
        int detect = !strcmp(cmd, "detect"), count = 0, emitted = 0;
        printf("{\"ok\":true,\"page_width\":%d,\"page_height\":%d,\"%s\":[",
               pixGetWidth(pix), pixGetHeight(pix), detect ? "boxes" : "lines");
        TessResultIterator *it = TessBaseAPIGetIterator(api);
        if (it) do {
            char *text = TessResultIteratorGetUTF8Text(it, RIL_TEXTLINE);
            int x0, y0, x1, y1;
            if (!text || !TessPageIteratorBoundingBox(TessResultIteratorGetPageIterator(it),
                                                      RIL_TEXTLINE, &x0, &y0, &x1, &y1)) {
                TessDeleteText(text); continue;
            }
            size_t n = strlen(text);
            while (n && isspace((unsigned char)text[n-1])) text[--n] = 0;
            if (!n) { TessDeleteText(text); continue; }
            float conf = TessResultIteratorConfidence(it, RIL_TEXTLINE) / 100.0f;
            printf("%s{\"bbox\":[%d,%d,%d,%d],", count++ ? "," : "", x0, y0, x1, y1);
            if (detect) printf("\"det_score\":%.4f}", conf);
            else {
                printf("\"text\":"); quoted(text);
                printf(",\"conf\":%.4f,\"script\":\"uncertain\",\"reader\":\"tesseract\"}", conf);
            }
            if (crops && conf < below) {
                char name[4096];
                int len = snprintf(name, sizeof name, "%s/crop_%03d.ppm", crops, count - 1);
                if (len > 0 && (size_t)len < sizeof name) {
                    BOX *region = boxCreate(x0, y0, x1 - x0, y1 - y0);
                    PIX *crop = pixClipRectangle(pix, region, NULL);
                    int fd = crop ? open(name, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
                    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
                    if (file) { if (!pixWriteStreamPnm(file, crop)) emitted++; fclose(file); }
                    else if (fd >= 0) close(fd);
                    pixDestroy(&crop); boxDestroy(&region);
                }
            }
            TessDeleteText(text);
        } while (TessResultIteratorNext(it, RIL_TEXTLINE));
        if (it) TessResultIteratorDelete(it);
        printf("],\"emitted_crops\":%d}\n", emitted);
    }
    pixDestroy(&pix);
    if (getenv("SAMOSA_OCR_PROFILE")) fprintf(stderr, "ocr_profile engine=tesseract total=%.3fs\n", now() - start);
    return 0;
}

/* Only the PDF renderer is a child process. Paths are argv, never shell code. */
static int extract(char *const argv[], FILE *out) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) {
        if (dup2(fileno(out), STDOUT_FILENO) < 0) _exit(126);
        execv(argv[0], argv); _exit(127);
    }
    renderer_pid = pid;
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { renderer_pid = 0; return -1; }
    renderer_pid = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static int read_pdf(TessBaseAPI *api, const char *pdf, const char *renderer) {
    FILE *inspection = tmpfile();
    if (!inspection) return error("render_failed");
    char *inspect_args[] = {(char *)renderer, "--json-pages", (char *)pdf, "1", "1", NULL};
    int failed = extract(inspect_args, inspection);
    long length = ftell(inspection);
    if (failed || length < 1 || length > 16 * 1024 * 1024) { fclose(inspection); return error("render_failed"); }
    rewind(inspection);
    char *raw = calloc((size_t)length + 1, 1);
    if (!raw || fread(raw, 1, length, inspection) != (size_t)length) {
        free(raw); fclose(inspection); return error("render_failed");
    }
    fclose(inspection);
    char *arena = NULL; jval *info = json_parse(raw, &arena);
    jval *pages = info ? json_get(info, "page_count") : NULL;
    int count = pages && pages->t == J_NUM && pages->num >= 1 && pages->num <= 10000 ? (int)pages->num : 0;
    json_free(info); free(arena); free(raw);
    if (!count) return error("render_failed");
    snprintf(pdf_temp_dir, sizeof pdf_temp_dir, "/tmp/samosa-ocr.XXXXXX");
    if (!mkdtemp(pdf_temp_dir)) { *pdf_temp_dir = 0; return error("render_failed"); }
    snprintf(pdf_temp_image, sizeof pdf_temp_image, "%s/page.ppm", pdf_temp_dir);
    char *ppm = pdf_temp_image;
    printf("PDF: %s\nPAGES: %d\n", pdf, count); fflush(stdout);
    int result = 0;
    for (int page = 1; page <= count; page++) {
        char number[16]; snprintf(number, sizeof number, "%d", page);
        char *args[] = {(char *)renderer, "--render-ocr-ppm", (char *)pdf, number, ppm, NULL};
        FILE *sink = tmpfile();
        if (!sink) { result = 65; break; }
        failed = extract(args, sink); fclose(sink);
        PIX *pix = failed ? NULL : load_image(ppm);
        unlink(ppm);
        if (!pix || recognize(api, pix, NULL)) {
            pixDestroy(&pix); fprintf(stderr, "OCR failed on PDF page %d\n", page); result = 65; break;
        }
        char *text = TessBaseAPIGetUTF8Text(api);
        if (!text) { pixDestroy(&pix); result = 65; break; }
        printf("\n--- PAGE %d ---\n%s\n", page, text); fflush(stdout);
        TessDeleteText(text); TessBaseAPIClear(api); pixDestroy(&pix);
    }
    cleanup_pdf(); *pdf_temp_dir = *pdf_temp_image = 0;
    return result;
}
int main(int argc, char **argv) {
    atexit(cleanup_pdf);
    struct sigaction action = {0};
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        printf("samosa-ocr 2 (tesseract-%s;reader-v2)\n", TessVersion()); return 0;
    }
    if (argc < 2) return 64;
    int data = !strcmp(argv[1], "--data-path");
    int check = !strcmp(argv[1], "--check") || data;
    int pdf = !strcmp(argv[1], "pdf");
    if (!check && (argc < 3 || (strcmp(argv[1], "read") && strcmp(argv[1], "recognize") &&
                              strcmp(argv[1], "detect") && !pdf))) return 64;
    if (pdf && argc != 4) return 64;
    TessBaseAPI *api = init_api();
    if (!api) return error("ocr_unavailable");
    int result;
    if (data) { puts(TessBaseAPIGetDatapath(api)); result = 0; }
    else if (check) { puts("{\"ok\":true,\"engine\":\"tesseract\"}"); result = 0; }
    else if (pdf) result = read_pdf(api, argv[2], argv[3]);
    else {
        int box[4], *region = NULL; const char *crops = NULL; float below = 0.84f;
        for (int i = 3; i < argc; i++) {
            if (++i >= argc) { TessBaseAPIDelete(api); return 64; }
            if (!strcmp(argv[i-1], "--box")) {
                if (sscanf(argv[i], "%d,%d,%d,%d", box, box+1, box+2, box+3) != 4) {
                    TessBaseAPIDelete(api); return 64;
                }
                region = box;
            } else if (!strcmp(argv[i-1], "--emit-crops")) crops = argv[i];
            else if (!strcmp(argv[i-1], "--below")) below = strtof(argv[i], NULL);
            else { TessBaseAPIDelete(api); return 64; }
        }
        result = read_image(api, argv[2], argv[1], region, crops, below);
    }
    TessBaseAPIDelete(api);
    return result;
}
