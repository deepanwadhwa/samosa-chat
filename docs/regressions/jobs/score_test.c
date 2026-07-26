/* Replicates candidate_score() and contains_case() VERBATIM from
 * src/samosa_gateway.c (lines 1804-1810, 1845-1867, path_copy 99-102)
 * to demonstrate scoring behavior on synthetic filenames.
 * No real files are touched. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int path_copy(char *out, size_t cap, const char *value) {
    int n = snprintf(out, cap, "%s", value ? value : "");
    return n >= 0 && (size_t)n < cap;
}

static int contains_case(const char *text, const char *word) {
    size_t length = strlen(word);
    if (!length) return 0;
    for (; *text; ++text)
        if (!strncasecmp(text, word, length)) return 1;
    return 0;
}

static int candidate_score(const char *goal, const char *name) {
    char copy[1024];
    if (!path_copy(copy, sizeof(copy), goal)) return 0;
    static const char *stop[] = {"find","locate","search","look","file","files","folder",
        "record","records","please","could","would","should","this","that","with","from","your","my"};
    int score = 0;
    for (char *save = NULL, *word = strtok_r(copy, " \\t.,?!:;/\\\"'()[]{}", &save);
         word; word = strtok_r(NULL, " \\t.,?!:;/\\\"'()[]{}", &save)) {
        int ignored = strlen(word) < 3;
        for (size_t i = 0; !ignored && i < sizeof(stop) / sizeof(stop[0]); ++i)
            ignored = !strcasecmp(word, stop[i]);
        if (!ignored && contains_case(name, word)) score += 4;
    }
    if ((contains_case(goal, "cat") || contains_case(goal, "pet")) &&
        (contains_case(name, "vet") || contains_case(name, "medical") ||
         contains_case(name, "vaccin") || contains_case(name, "rabies") ||
         contains_case(name, "clinic") || contains_case(name, "health"))) score += 3;
    if (contains_case(goal, "medical") &&
        (contains_case(name, "medical") || contains_case(name, "health") ||
         contains_case(name, "clinic") || contains_case(name, "lab") ||
         contains_case(name, "prescription") || contains_case(name, "vet"))) score += 2;
    return score;
}

int main(void) {
    const char *goal =
        "Can you find all files pertaining to my cat's medical records? my cat's name is Titli";

    /* 1. Show what tokens the scorer actually derives from the goal. */
    char copy[1024];
    path_copy(copy, sizeof(copy), goal);
    printf("Tokens extracted from the goal (delimiters incl. literal 't' and '\\\\'):\n ");
    for (char *save = NULL, *w = strtok_r(copy, " \\t.,?!:;/\\\"'()[]{}", &save);
         w; w = strtok_r(NULL, " \\t.,?!:;/\\\"'()[]{}", &save))
        printf(" [%s]", w);
    printf("\n\n");

    /* 2. Score synthetic filenames: a perfectly-named vet record vs. junk. */
    const char *names[] = {
        "Titli vaccination record 2023.pdf",   /* the ideal target file  */
        "titli_vet_visit.pdf",                 /* another ideal target   */
        "CamScanner 03-15-2024 14.22.pdf",     /* anonymous phone scan   */
        "CamScanner 11-02-2023 09.10.pdf",     /* anonymous phone scan   */
        "medicare_and_you_2024.pdf",           /* human-medicine junk    */
        "medical_coding_reference.pdf",        /* human-medicine junk    */
        "wallpaper_gallery.zip",               /* pure junk              */
        "training_schedule.txt",               /* pure junk              */
    };
    printf("candidate_score() results:\n");
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        printf("  %2d  %s\n", candidate_score(goal, names[i]), names[i]);
    return 0;
}
