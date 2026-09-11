#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
/* Samosa vendors only ZIP reading and Inflate. Keep compression, archive
 * writing, and the zlib compatibility surface out of every translation unit
 * without relying on build-system command-line definitions. */
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
/* DOCX input is already bounded and held in memory by the isolated reader.
 * Disable unused pathname/stdio APIs, including POSIX large-file functions
 * that strict C11 builds do not expose on Linux. */
#define MINIZ_NO_STDIO
#endif
