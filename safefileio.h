#ifndef SAFEFILEIO_H
#define SAFEFILEIO_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes large buffer to file in chunks. Returns total bytes written.
size_t safe_fwrite(FILE *fp, const void *buffer, size_t total_size);

// Reads large data from file into buffer in chunks. Returns total bytes read.
size_t safe_fread(FILE *fp, void *buffer, size_t total_size);

#ifdef __cplusplus
}
#endif


size_t safe_fwrite(FILE *fp, const void *buffer, size_t total_size) {
    const size_t max_chunk = 1UL << 30; // 1 GB
    const uint8_t *ptr = (const uint8_t *)buffer;
    size_t total_written = 0;

    while (total_written < total_size) {
        size_t chunk = (total_size - total_written > max_chunk)
                       ? max_chunk
                       : total_size - total_written;

        size_t written = fwrite(ptr + total_written, 1, chunk, fp);
        if (written != chunk) {
            perror("fwrite failed");
            break;
        }
        total_written += written;
    }

    return total_written;
}

size_t safe_fread(FILE *fp, void *buffer, size_t total_size) {
    const size_t max_chunk = 1UL << 30; // 1 GB
    uint8_t *ptr = (uint8_t *)buffer;
    size_t total_read = 0;

    while (total_read < total_size) {
        size_t chunk = (total_size - total_read > max_chunk)
                       ? max_chunk
                       : total_size - total_read;

        size_t read = fread(ptr + total_read, 1, chunk, fp);
        if (read == 0) {
            if (feof(fp)) break;
            perror("fread failed");
            break;
        }
        total_read += read;
    }

    return total_read;
}


#endif // SAFEFILEIO_H