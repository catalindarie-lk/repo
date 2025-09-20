#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "include/fileio.h"

long long get_file_size(const char *filepath, FILE **fp){

    FILE *fptr = fopen(filepath, "rb"); // Open in binary mode
    if (!fptr) {
        fprintf(stderr, "Error: Could not open file!\n");
        return RET_VAL_ERROR;
    }
    // Seek to end to determine file size
    if (_fseeki64(fptr, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek");
        fclose(fptr);
        return RET_VAL_ERROR;
    }
    long long size = _ftelli64(fptr);
    if(size == 0){
        fprintf(stderr, "Error file is empty (size 0)! _ftelli64()\n");
        fclose(fptr);
        return RET_VAL_ERROR;
    }
    if(size == RET_VAL_ERROR){
        fprintf(stderr, "Error reading file size! _ftelli64()\n");
        fclose(fptr);
        return RET_VAL_ERROR;
    }
    rewind(fptr); // Optional: reset file pointer to beginning
    *fp = fptr; // Return the open file pointer

    // fclose(fptr);
    return(size);
}

// Create output file
int create_output_file(const char *buffer, const uint64_t size, const char *path){
    FILE *fp = fopen(path, "wb");           
    if(fp == NULL){
        fprintf(stderr, "Error creating output file!!!\n");
        return RET_VAL_ERROR;
    }
    size_t written = safe_fwrite(fp, buffer, size);
    if (written != size) {
        fprintf(stderr, "Incomplete bytes written to file. Expected: %llu, Written: %zu\n", size, written);
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    fprintf(stderr, "Creating output file: %s\n", path);
    return RET_VAL_SUCCESS;
}

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


BOOL RenameFileByHandle(HANDLE hFile, const wchar_t* newPath) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE || !newPath) {
        return FALSE;
    }

    DWORD nameLen = (DWORD)(wcslen(newPath) * sizeof(wchar_t));
    if (nameLen > MAX_PATH * sizeof(wchar_t)) {
        // Path too long for static buffer
        return FALSE;
    }

    // Static buffer sized for FILE_RENAME_INFO + MAX_PATH
    BYTE buffer[sizeof(FILE_RENAME_INFO) + MAX_PATH * sizeof(wchar_t)];
    FILE_RENAME_INFO* renameInfo = (FILE_RENAME_INFO*)buffer;

    ZeroMemory(buffer, sizeof(buffer));
    renameInfo->ReplaceIfExists = TRUE;
    renameInfo->RootDirectory = NULL;
    renameInfo->FileNameLength = nameLen;
    memcpy(renameInfo->FileName, newPath, nameLen);

    BOOL result = SetFileInformationByHandle(
        hFile,
        FileRenameInfo,
        renameInfo,
        sizeof(FILE_RENAME_INFO) + nameLen
    );

    if(!result){
        fwprintf(stderr, L"CRITICAL ERROR: failed to rename temp file to: %ls\n", newPath);
    }

    return result;
}

BOOL DeleteFileByHandle(HANDLE hFile) {
    FILE_DISPOSITION_INFO disposition = { TRUE };

    BOOL result = SetFileInformationByHandle(
        hFile,
        FileDispositionInfo,
        &disposition,
        sizeof(disposition)
    );

    if(!result){
        fprintf(stderr, "Failed to delete file!\n");
    }

    return result;
}
