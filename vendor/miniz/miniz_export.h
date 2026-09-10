#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
/* Samosa vendors only ZIP reading and Inflate. Keep compression, archive
 * writing, and the zlib compatibility surface out of every translation unit
 * without relying on build-system command-line definitions. */
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#endif
