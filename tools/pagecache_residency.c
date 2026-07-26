/*
 * Report how much of a file the OS currently has resident in its page cache.
 *
 * This intentionally never reads from the mapping.  mincore(2) queries page
 * residency only, so it is suitable for E-X3's before/after measurements
 * without itself warming experts.bin.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr, "usage: %s [--json] FILE\n", program);
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if (*p >= 0x20) putchar(*p);
        else printf("\\u%04x", *p);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    int json = 0;
    const char *path = NULL;
    if (argc == 2) path = argv[1];
    else if (argc == 3 && strcmp(argv[1], "--json") == 0) {
        json = 1;
        path = argv[2];
    } else {
        usage(argv[0]);
        return 2;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        fprintf(stderr, "%s: expected a non-empty regular file\n", path);
        close(fd);
        return 2;
    }

    const long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        close(fd);
        return 1;
    }
    const size_t page_size = (size_t)page_size_long;
    const uintmax_t file_bytes = (uintmax_t)st.st_size;
    if (file_bytes > SIZE_MAX || file_bytes > SIZE_MAX - (page_size - 1)) {
        fprintf(stderr, "%s: file is too large to map in this process\n", path);
        close(fd);
        return 1;
    }
    const size_t map_bytes = ((size_t)file_bytes + page_size - 1) / page_size * page_size;
    const size_t page_count = map_bytes / page_size;
    char *residency = calloc(page_count, sizeof(*residency));
    if (residency == NULL) {
        perror("calloc");
        close(fd);
        return 1;
    }

    void *mapping = mmap(NULL, map_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        free(residency);
        close(fd);
        return 1;
    }
    if (mincore(mapping, map_bytes, residency) != 0) {
        perror("mincore");
        munmap(mapping, map_bytes);
        free(residency);
        close(fd);
        return 1;
    }

    size_t resident_pages = 0;
    for (size_t page = 0; page < page_count; ++page) {
        if (((unsigned char)residency[page] & 1u) != 0) ++resident_pages;
    }
    const uintmax_t resident_bytes = (uintmax_t)resident_pages * page_size;
    const double resident_percent = 100.0 * (double)resident_pages / (double)page_count;

    if (json) {
        fputs("{\"schema\":1,\"path\":", stdout);
        json_string(path);
        printf(",\"file_bytes\":%" PRIuMAX
               ",\"page_bytes\":%zu,\"pages\":%zu"
               ",\"resident_pages\":%zu,\"resident_bytes\":%" PRIuMAX
               ",\"resident_percent\":%.6f}\n",
               file_bytes, page_size, page_count, resident_pages, resident_bytes,
               resident_percent);
    } else {
        printf("path: %s\n", path);
        printf("file bytes: %" PRIuMAX "\n", file_bytes);
        printf("page bytes: %zu\n", page_size);
        printf("pages: %zu\n", page_count);
        printf("resident pages: %zu\n", resident_pages);
        printf("resident bytes: %" PRIuMAX "\n", resident_bytes);
        printf("resident: %.2f%%\n", resident_percent);
    }

    munmap(mapping, map_bytes);
    free(residency);
    close(fd);
    return 0;
}
