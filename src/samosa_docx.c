#include "samosa_docx.h"

#include "samosa_html.h"

#include "miniz.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DOCX_MAX_ENTRIES 2048u
#define DOCX_MAX_TOTAL_EXPANDED (128u * 1024u * 1024u)
#define DOCX_MAX_ENTRY_EXPANDED (32u * 1024u * 1024u)
#define DOCX_MAX_XML_ENTRY (16u * 1024u * 1024u)
#define DOCX_MAX_AUXILIARY_FILES 64u
#define DOCX_MAX_RATIO 200u

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    int limited;
} DocxBuffer;

typedef struct {
    mz_uint index;
    char name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    mz_uint64 size;
} DocxPart;

static int reserve(DocxBuffer *buffer, size_t extra) {
    if (extra > buffer->limit || buffer->len > buffer->limit - extra) {
        buffer->limited = 1;
        return 0;
    }
    size_t need = buffer->len + extra + 1;
    if (need <= buffer->cap) return 1;
    size_t cap = buffer->cap ? buffer->cap : 4096;
    while (cap < need) {
        if (cap > (buffer->limit + 1) / 2) {
            cap = buffer->limit + 1;
            break;
        }
        cap *= 2;
    }
    char *next = realloc(buffer->data, cap);
    if (!next) return 0;
    buffer->data = next;
    buffer->cap = cap;
    return 1;
}

static int putn(DocxBuffer *buffer, const char *text, size_t length) {
    if (!reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->len, text, length);
    buffer->len += length;
    buffer->data[buffer->len] = '\0';
    return 1;
}

static int put(DocxBuffer *buffer, const char *text) {
    return putn(buffer, text, strlen(text));
}

static int ascii_prefix_ci(const unsigned char *data, size_t length,
                           size_t at, const char *prefix) {
    size_t n = strlen(prefix);
    if (at > length || n > length - at) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = data[at + i];
        unsigned char expected = (unsigned char)prefix[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (expected >= 'A' && expected <= 'Z')
            expected = (unsigned char)(expected + ('a' - 'A'));
        if (c != expected) return 0;
    }
    return 1;
}

static int contains_ci(const unsigned char *data, size_t length,
                       const char *needle) {
    size_t n = strlen(needle);
    if (!n || n > length) return 0;
    for (size_t i = 0; i + n <= length; ++i)
        if (ascii_prefix_ci(data, length, i, needle)) return 1;
    return 0;
}

static int safe_archive_path(const char *name) {
    size_t length = strlen(name);
    if (!length || length + 1 >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE ||
        name[0] == '/' || name[0] == '\\' || strchr(name, '\\') ||
        strchr(name, ':')) return 0;
    const char *segment = name;
    for (const char *p = name;; ++p) {
        if (*p == '/' || !*p) {
            size_t n = (size_t)(p - segment);
            if (!n && *p) return 0;
            if ((n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (!*p) break;
            segment = p + 1;
        }
    }
    return 1;
}

static int auxiliary_part(const char *name) {
    if (!strcmp(name, "word/footnotes.xml") ||
        !strcmp(name, "word/endnotes.xml") ||
        !strcmp(name, "word/comments.xml")) return 1;
    const char *base = NULL;
    if (!strncmp(name, "word/header", 11)) base = name + 11;
    else if (!strncmp(name, "word/footer", 11)) base = name + 11;
    if (!base || !*base) return 0;
    while (*base >= '0' && *base <= '9') ++base;
    return !strcmp(base, ".xml");
}

static int part_compare(const void *left, const void *right) {
    const DocxPart *a = left, *b = right;
    return strcmp(a->name, b->name);
}

static unsigned char *extract_part(mz_zip_archive *zip, mz_uint index,
                                   mz_uint64 size, const char **error) {
    if (size > DOCX_MAX_XML_ENTRY || size > SIZE_MAX - 1) {
        *error = "docx_xml_size_limit";
        return NULL;
    }
    unsigned char *data = malloc((size_t)size + 1);
    if (!data) {
        *error = "out_of_memory";
        return NULL;
    }
    if (!mz_zip_reader_extract_to_mem(zip, index, data, (size_t)size, 0)) {
        free(data);
        *error = "docx_decompression_failed";
        return NULL;
    }
    data[size] = 0;
    return data;
}

static size_t xml_tag_end(const unsigned char *xml, size_t length, size_t at) {
    unsigned char quote = 0;
    for (size_t i = at; i < length; ++i) {
        unsigned char c = xml[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return length;
}

static int xml_local_name(const unsigned char *xml, size_t end, size_t at,
                          char out[32], int *closing, int *self_closing) {
    *closing = 0;
    *self_closing = 0;
    while (at < end && isspace(xml[at])) ++at;
    if (at < end && xml[at] == '/') {
        *closing = 1;
        ++at;
    }
    while (at < end && isspace(xml[at])) ++at;
    if (at >= end || !(isalpha(xml[at]) || xml[at] == '_')) return 0;
    size_t start = at;
    while (at < end && (isalnum(xml[at]) || xml[at] == '_' ||
                        xml[at] == '-' || xml[at] == '.' || xml[at] == ':')) ++at;
    size_t local = start;
    for (size_t i = start; i < at; ++i)
        if (xml[i] == ':') local = i + 1;
    size_t n = at - local;
    if (!n || n >= 32) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = xml[local + i];
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    out[n] = '\0';
    size_t tail = end;
    while (tail > at && isspace(xml[tail - 1])) --tail;
    *self_closing = tail > at && xml[tail - 1] == '/';
    return 1;
}

static size_t encode_utf8(uint32_t code, char out[4]) {
    if (code <= 0x7f) { out[0] = (char)code; return 1; }
    if (code <= 0x7ff) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        return 2;
    }
    if (code <= 0xffff && !(code >= 0xd800 && code <= 0xdfff)) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    if (code <= 0x10ffff) {
        out[0] = (char)(0xf0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[3] = (char)(0x80 | (code & 0x3f));
        return 4;
    }
    return 0;
}

static int xml_entity(const unsigned char *name, size_t length,
                      char out[4], size_t *out_length) {
    struct Named { const char *name; char value; };
    static const struct Named named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
        {"apos", '\''}, {NULL, 0}
    };
    if (length && name[0] == '#') {
        size_t at = 1;
        unsigned base = 10;
        if (at < length && (name[at] == 'x' || name[at] == 'X')) {
            base = 16;
            ++at;
        }
        if (at == length) return 0;
        uint32_t code = 0;
        for (; at < length; ++at) {
            unsigned digit;
            if (name[at] >= '0' && name[at] <= '9') digit = name[at] - '0';
            else if (base == 16 && name[at] >= 'a' && name[at] <= 'f') digit = name[at] - 'a' + 10;
            else if (base == 16 && name[at] >= 'A' && name[at] <= 'F') digit = name[at] - 'A' + 10;
            else return 0;
            if (code > (0x10ffffu - digit) / base) return 0;
            code = code * base + digit;
        }
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            (code >= 0xd800 && code <= 0xdfff)) return 0;
        *out_length = encode_utf8(code, out);
        return *out_length != 0;
    }
    for (size_t i = 0; named[i].name; ++i) {
        if (strlen(named[i].name) == length &&
            !memcmp(name, named[i].name, length)) {
            out[0] = named[i].value;
            *out_length = 1;
            return 1;
        }
    }
    return 0;
}

static void normalize_output(DocxBuffer *output) {
    if (!output->data) return;
    char *read = output->data;
    char *write = output->data;
    int newlines = 0;
    while (*read == ' ' || *read == '\t' || *read == '\r' || *read == '\n') ++read;
    while (*read) {
        if (*read == '\r') { ++read; continue; }
        if (*read == '\n') {
            while (write > output->data && (write[-1] == ' ' || write[-1] == '\t')) --write;
            if (newlines < 2) *write++ = '\n';
            ++newlines;
            ++read;
            while (*read == ' ' || *read == '\t' || *read == '\r') ++read;
        } else {
            *write++ = *read++;
            newlines = 0;
        }
    }
    while (write > output->data &&
           (write[-1] == ' ' || write[-1] == '\t' || write[-1] == '\n')) --write;
    *write = '\0';
    output->len = (size_t)(write - output->data);
}

static int append_word_xml(DocxBuffer *output, const unsigned char *xml,
                           size_t length, const char **error) {
    const char *utf8_error = NULL;
    if (!samosa_html_text_valid(xml, length, &utf8_error)) {
        *error = !strcmp(utf8_error, "html_binary_control")
            ? "docx_xml_binary_control" : "docx_xml_invalid_utf8";
        return 0;
    }
    if (contains_ci(xml, length, "<!doctype") ||
        contains_ci(xml, length, "<!entity")) {
        *error = "docx_xml_doctype_unsupported";
        return 0;
    }
    int text_depth = 0;
    int deleted_depth = 0;
    size_t at = length >= 3 && !memcmp(xml, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (at < length) {
        if (xml[at] == '<') {
            if (at + 4 <= length && !memcmp(xml + at, "<!--", 4)) {
                const unsigned char *end_comment = NULL;
                for (size_t i = at + 4; i + 3 <= length; ++i)
                    if (!memcmp(xml + i, "-->", 3)) { end_comment = xml + i; break; }
                if (!end_comment) { *error = "docx_xml_malformed"; return 0; }
                at = (size_t)(end_comment - xml) + 3;
                continue;
            }
            size_t end = xml_tag_end(xml, length, at + 1);
            if (end == length) { *error = "docx_xml_malformed"; return 0; }
            char tag[32];
            int closing = 0, self_closing = 0;
            if (xml_local_name(xml, end, at + 1, tag, &closing, &self_closing)) {
                if (!strcmp(tag, "del")) {
                    if (closing) { if (deleted_depth) --deleted_depth; }
                    else if (!self_closing) ++deleted_depth;
                } else if (!deleted_depth && !strcmp(tag, "t")) {
                    if (closing) { if (text_depth) --text_depth; }
                    else if (!self_closing) ++text_depth;
                } else if (!deleted_depth && !closing &&
                           (!strcmp(tag, "tab"))) {
                    if (!putn(output, "\t", 1)) goto output_failed;
                } else if (!deleted_depth && !closing &&
                           (!strcmp(tag, "br") || !strcmp(tag, "cr"))) {
                    if (!putn(output, "\n", 1)) goto output_failed;
                } else if (!deleted_depth && closing && !strcmp(tag, "tc")) {
                    if (!putn(output, "\t", 1)) goto output_failed;
                } else if (!deleted_depth && closing &&
                           (!strcmp(tag, "p") || !strcmp(tag, "tr"))) {
                    if (!putn(output, "\n", 1)) goto output_failed;
                }
            }
            at = end + 1;
            continue;
        }
        if (!deleted_depth && text_depth) {
            if (xml[at] == '&') {
                size_t semi = at + 1;
                while (semi < length && semi - at <= 16 && xml[semi] != ';' &&
                       xml[semi] != '<' && !isspace(xml[semi])) ++semi;
                char decoded[4];
                size_t decoded_length = 0;
                if (semi >= length || xml[semi] != ';' ||
                    !xml_entity(xml + at + 1, semi - at - 1,
                                decoded, &decoded_length)) {
                    *error = "docx_xml_entity_unsupported";
                    return 0;
                }
                if (!putn(output, decoded, decoded_length)) goto output_failed;
                at = semi + 1;
                continue;
            }
            if (!putn(output, (const char *)xml + at, 1)) goto output_failed;
        }
        ++at;
    }
    if (text_depth || deleted_depth) {
        *error = "docx_xml_malformed";
        return 0;
    }
    return 1;

output_failed:
    *error = output->limited ? "docx_text_output_limit" : "out_of_memory";
    return 0;
}

static unsigned long utf8_chars(const char *text) {
    unsigned long count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xc0) != 0x80) ++count;
    return count;
}

void samosa_docx_result_free(SamosaDocxResult *result) {
    if (!result) return;
    free(result->text);
    memset(result, 0, sizeof(*result));
}

int samosa_docx_extract(const unsigned char *data, size_t length,
                        size_t max_text_bytes, SamosaDocxResult *result,
                        const char **error) {
    mz_zip_archive zip;
    DocxBuffer output = {.limit = max_text_bytes};
    DocxPart auxiliaries[DOCX_MAX_AUXILIARY_FILES];
    size_t auxiliary_count = 0;
    mz_uint document_index = 0, content_types_index = 0, rels_index = 0;
    mz_uint64 document_size = 0, content_types_size = 0, rels_size = 0;
    int have_document = 0, have_content_types = 0, have_rels = 0;
    mz_uint64 total_expanded = 0, total_compressed = 0;
    if (error) *error = NULL;
    if (!result || !data || !length || !max_text_bytes) {
        if (error) *error = "docx_invalid_parameter";
        return 0;
    }
    memset(result, 0, sizeof(*result));
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data, length,
                                MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        if (error) *error = "docx_malformed";
        return 0;
    }
    if (!zip.m_total_files || zip.m_total_files > DOCX_MAX_ENTRIES) {
        *error = "docx_entry_count_limit";
        goto failed;
    }
    for (mz_uint i = 0; i < zip.m_total_files; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            *error = "docx_malformed";
            goto failed;
        }
        if (!safe_archive_path(stat.m_filename)) {
            *error = "docx_unsafe_path";
            goto failed;
        }
        if (stat.m_is_directory) continue;
        if (stat.m_is_encrypted) { *error = "docx_encrypted"; goto failed; }
        if (!stat.m_is_supported || (stat.m_method != 0 && stat.m_method != 8)) {
            *error = "docx_unsupported_compression";
            goto failed;
        }
        if (stat.m_uncomp_size > DOCX_MAX_ENTRY_EXPANDED ||
            total_expanded > DOCX_MAX_TOTAL_EXPANDED - stat.m_uncomp_size) {
            *error = "docx_expansion_limit";
            goto failed;
        }
        total_expanded += stat.m_uncomp_size;
        if (total_compressed > UINT64_MAX - stat.m_comp_size) {
            *error = "docx_expansion_limit";
            goto failed;
        }
        total_compressed += stat.m_comp_size;
        if (stat.m_uncomp_size > (1u << 20) &&
            (!stat.m_comp_size || stat.m_uncomp_size / stat.m_comp_size > DOCX_MAX_RATIO)) {
            *error = "docx_compression_ratio_limit";
            goto failed;
        }
        if (!strcmp(stat.m_filename, "word/document.xml")) {
            if (have_document) { *error = "docx_duplicate_required_entry"; goto failed; }
            have_document = 1; document_index = i; document_size = stat.m_uncomp_size;
        } else if (!strcmp(stat.m_filename, "[Content_Types].xml")) {
            if (have_content_types) { *error = "docx_duplicate_required_entry"; goto failed; }
            have_content_types = 1; content_types_index = i; content_types_size = stat.m_uncomp_size;
        } else if (!strcmp(stat.m_filename, "_rels/.rels")) {
            if (have_rels) { *error = "docx_duplicate_required_entry"; goto failed; }
            have_rels = 1; rels_index = i; rels_size = stat.m_uncomp_size;
        } else if (auxiliary_part(stat.m_filename)) {
            if (auxiliary_count >= DOCX_MAX_AUXILIARY_FILES) {
                *error = "docx_auxiliary_count_limit";
                goto failed;
            }
            auxiliaries[auxiliary_count].index = i;
            auxiliaries[auxiliary_count].size = stat.m_uncomp_size;
            strcpy(auxiliaries[auxiliary_count].name, stat.m_filename);
            ++auxiliary_count;
        }
    }
    if (total_expanded > (1u << 20) &&
        (!total_compressed || total_expanded / total_compressed > DOCX_MAX_RATIO)) {
        *error = "docx_compression_ratio_limit";
        goto failed;
    }
    if (!have_document || !have_content_types || !have_rels) {
        *error = "docx_required_entry_missing";
        goto failed;
    }

    unsigned char *content_types = extract_part(&zip, content_types_index,
                                                content_types_size, error);
    if (!content_types) goto failed;
    int valid_content_type = contains_ci(
        content_types, (size_t)content_types_size,
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
    free(content_types);
    if (!valid_content_type) { *error = "docx_content_type_invalid"; goto failed; }

    unsigned char *rels = extract_part(&zip, rels_index, rels_size, error);
    if (!rels) goto failed;
    int valid_rels = contains_ci(rels, (size_t)rels_size,
                                 "officeDocument") &&
                     contains_ci(rels, (size_t)rels_size,
                                 "word/document.xml");
    free(rels);
    if (!valid_rels) { *error = "docx_relationship_invalid"; goto failed; }

    unsigned char *document = extract_part(&zip, document_index,
                                           document_size, error);
    if (!document) goto failed;
    int document_ok = append_word_xml(&output, document,
                                      (size_t)document_size, error);
    free(document);
    if (!document_ok) goto failed;

    qsort(auxiliaries, auxiliary_count, sizeof(auxiliaries[0]), part_compare);
    for (size_t i = 0; i < auxiliary_count; ++i) {
        unsigned char *part = extract_part(&zip, auxiliaries[i].index,
                                           auxiliaries[i].size, error);
        if (!part) goto failed;
        if (output.len && !put(&output, "\n\n")) {
            free(part); *error = output.limited ? "docx_text_output_limit" : "out_of_memory";
            goto failed;
        }
        if (!put(&output, "[") || !put(&output, auxiliaries[i].name) ||
            !put(&output, "]\n") ||
            !append_word_xml(&output, part, (size_t)auxiliaries[i].size, error)) {
            free(part);
            if (!*error) *error = output.limited ? "docx_text_output_limit" : "out_of_memory";
            goto failed;
        }
        free(part);
    }
    if (!output.data && !putn(&output, "", 0)) {
        *error = "out_of_memory";
        goto failed;
    }
    normalize_output(&output);
    result->text = output.data;
    result->text_bytes = output.len;
    result->text_chars = utf8_chars(output.data);
    result->entry_count = zip.m_total_files;
    mz_zip_reader_end(&zip);
    return 1;

failed:
    free(output.data);
    mz_zip_reader_end(&zip);
    return 0;
}
