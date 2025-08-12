#ifndef FILEIO_H
#define FILEIO_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif


long long get_file_size(const char *filepath);
int create_output_file(const char *buffer, const uint64_t size, const char *path);
// Writes large buffer to file in chunks. Returns total bytes written.
size_t safe_fwrite(FILE *fp, const void *buffer, size_t total_size);
// Reads large data from file into buffer in chunks. Returns total bytes read.
size_t safe_fread(FILE *fp, void *buffer, size_t total_size);



#endif // SAFEFILEIO_H