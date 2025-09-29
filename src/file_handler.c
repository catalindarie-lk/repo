
#include <stdint.h>
#include <stdio.h>
#include <time.h>
//#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "include/file_handler.h"
#include "include/protocol_frames.h"
#include "include/resources.h"
#include "include/server.h"
#include "include/server_frames.h"
#include "include/netendians.h"
#include "include/queue.h"
#include "include/hash.h"
#include "include/bitmap.h"
#include "include/crc32.h"
#include "include/mem_pool.h"
#include "include/fileio.h"
#include "include/folders.h"


void init_fstream_pool(ServerFstreamPool* pool, const uint64_t block_count) {

    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in init_fstream_pool().\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in init_fstream_pool().\n");
        _aligned_free(pool->next);
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)
    
    // Allocate the main memory buffer for the pool
    pool->fstream = (ServerFileStream*)_aligned_malloc(sizeof(ServerFileStream) * pool->block_count, 64);
    if (!pool->fstream) {
        fprintf(stderr, "Memory allocation failed for fstream in init_fstream_pool().\n");
        _aligned_free(pool->next);
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->fstream, 0, sizeof(ServerFileStream) * pool->block_count);
    
    // Initialize the free list: all blocks are initially free
    pool->free_head = 0;     // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t index = 0; index < pool->block_count - 1; index++) {
        pool->next[index] = index + 1;                          // Link to the next block
        pool->used[index] = FREE_BLOCK;                         // Mark as unused
        InitializeSRWLock(&pool->fstream[index].lock);
    }
    // The last block points to END_BLOCK, indicating the end of the free list
    pool->next[pool->block_count - 1] = END_BLOCK;              // Use END_BLOCK to indicate end of list
    pool->used[pool->block_count - 1] = FREE_BLOCK;             // Last block is also unused
   
    InitializeSRWLock(&pool->fstream[pool->block_count - 1].lock);
    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
ServerFileStream* find_fstream(ServerFstreamPool* pool, const uint32_t sid, const uint32_t fid) {

    ServerFileStream *fstream = NULL;

    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to find_fstream() in an unallocated pool!\n");
        return NULL;
    }

    AcquireSRWLockShared(&pool->lock);
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        fstream = &pool->fstream[index];
        if(fstream->sid == sid && fstream->fid == fid){
            ReleaseSRWLockShared(&pool->lock);
            return fstream;
        }
    }
    ReleaseSRWLockShared(&pool->lock);
    return NULL;
}
ServerFileStream* alloc_fstream(ServerFstreamPool* pool) {
    // Enter critical section to protect shared pool data
    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to alloc_fstream() in an unallocated pool!\n");
        return NULL;
    }
    AcquireSRWLockExclusive(&pool->lock);
    // Check if the pool is exhausted
    if (pool->free_head == END_BLOCK) { // Check against END_BLOCK
        ReleaseSRWLockExclusive(&pool->lock);
        return NULL; // Pool exhausted
    }
    // Get the index of the first free block
    uint64_t index = pool->free_head;
    // Update the free head to the next free block
    pool->free_head = pool->next[index];
    // Mark the allocated block as used
    pool->used[index] = USED_BLOCK;
    pool->free_blocks--;
    ReleaseSRWLockExclusive(&pool->lock);
    return &pool->fstream[index];
}
static uint8_t init_fstream(ServerFileStream *fstream, UdpFrame *frame, const struct sockaddr_in *client_addr) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    uint8_t err_code = STS_STREAM_UNDEFINED;

    if(!fstream || !frame || !client_addr){
        fprintf(stderr, "ERROR: init_fstream - Invalid NULL parameter(s)!\n");
        return ERR_STREAM_INVALID_PARAMETERS;
    }

    AcquireSRWLockExclusive(&fstream->lock);
    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    fstream->sid = _ntohl(frame->header.session_id);
    fstream->fid = _ntohl(frame->payload.file_metadata.file_id);
    fstream->file_size = _ntohll(frame->payload.file_metadata.file_size);

    // --- Proper string copy and validation for rpath ---
    uint32_t received_rpath_len = _ntohl(frame->payload.file_metadata.rpath_len);

    // Validate the received length against the destination buffer's capacity (MAX_PATH - 1 for content + null)
    if (received_rpath_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_fstream - Received rpath length (%u) is too large for buffer (max %d).\n",
                received_rpath_len, MAX_PATH - 1);
        fstream->rpath_len = 0;
        fstream->rpath[0] = '\0'; // Ensure it's null-terminated even on error
        err_code = ERR_STREAM_INVALID_PAYLOAD_DATA;
        goto exit_err; // Exit function on critical error
    } else {
        // Use snprintf with precision to copy exactly 'received_rpath_len' characters.
        // snprintf will null-terminate the buffer as long as `received_rpath_len < MAX_PATH`.
        int result = snprintf(fstream->rpath, sizeof(fstream->rpath),
                              "%.*s", (int)received_rpath_len, frame->payload.file_metadata.rpath);

        // Verify snprintf's return value. It should equal the number of characters copied.
        if (result < 0 || (size_t)result != received_rpath_len) {
            fprintf(stderr, "ERROR: init_fstream - Failed to copy rpath: snprintf returned %d, expected %u.\n",
                    result, received_rpath_len);
            fstream->rpath_len = 0;
            fstream->rpath[0] = '\0';
            err_code = ERR_STREAM_INVALID_PAYLOAD_DATA;
            goto exit_err;
        } else {
            // Copy successful, store the actual content length
            fstream->rpath_len = received_rpath_len;
        }
    }

    // --- Proper string copy and validation for fname ---
    uint32_t received_fname_len = _ntohl(frame->payload.file_metadata.fname_len);

    if (received_fname_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_fstream - Received fname length (%u) is too large for buffer (max %d).\n",
                received_fname_len, MAX_PATH - 1);
        fstream->fname_len = 0;
        fstream->fname[0] = '\0';
        err_code = ERR_STREAM_INVALID_PAYLOAD_DATA;
        goto exit_err;
    } else {
        int result = snprintf(fstream->fname, sizeof(fstream->fname),
                              "%.*s", (int)received_fname_len, frame->payload.file_metadata.fname);

        if (result < 0 || (size_t)result != received_fname_len) {
            fprintf(stderr, "ERROR: init_fstream - Failed to copy fname: snprintf returned %d, expected %u.\n",
                    result, received_fname_len);
            fstream->fname_len = 0;
            fstream->fname[0] = '\0';
            err_code = ERR_STREAM_INVALID_PAYLOAD_DATA;
            goto exit_err;
        } else {
            fstream->fname_len = received_fname_len;
        }
    }
    // --- End of string copy and validation ---

    memcpy(&fstream->client_addr, client_addr, sizeof(struct sockaddr_in));

    // Calculate total fragments
    fstream->fragment_count = ((fstream->file_size - 1ULL) / FILE_FRAGMENT_SIZE) + 1ULL;
    // Calculate total blocks
    fstream->block_count = ((fstream->file_size - 1ULL) / SERVER_FILE_BLOCK_SIZE) + 1ULL;

    // fprintf(stdout, "Nr of Bytes: %llu, Nr of Fragments: %llu, Blocks: %llu\n", fstream->file_size, fstream->fragment_count, fstream->block_count);

    // Calculate total 64-bit bitmap entries
    fstream->file_bitmap_size = ((fstream->fragment_count - 1ULL) / 64ULL) + 1;

    // Allocate memory for bitmap
    fstream->received_file_bitmap = malloc(fstream->file_bitmap_size * sizeof(uint64_t));
    if(fstream->received_file_bitmap == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file bitmap mem!!!\n");
        err_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->received_file_bitmap, 0, fstream->file_bitmap_size * sizeof(uint64_t));

    fstream->file_block = malloc(fstream->block_count * sizeof(char*));
    if(fstream->file_block == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file block mem!!!\n");
        err_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->file_block, 0, fstream->block_count * sizeof(char*));

    fstream->recv_block_bytes = malloc(fstream->block_count * sizeof(uint64_t));
    if(fstream->recv_block_bytes == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file recv block bytes!!!\n");
        err_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->recv_block_bytes, 0, fstream->block_count * sizeof(uint64_t));

    fstream->recv_block_status = malloc(fstream->block_count * sizeof(uint64_t));
    if(fstream->recv_block_status == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file recv block status!!!\n");
        err_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->recv_block_status, 0, fstream->block_count * sizeof(uint64_t));

    if(!DriveExists(SERVER_PARTITION_DRIVE)){
        fprintf(stderr, "ERROR: init_fstream - Drive Partition \"%s\" doesn't exit\n", SERVER_PARTITION_DRIVE);
        err_code = ERR_STREAM_PATH_CREATE;
        goto exit_err;
    }

    // creating root folder\\session_folder
    char rootFolder[MAX_PATH];
    snprintf(rootFolder, MAX_PATH, "%s%s%d", SERVER_ROOT_FOLDER, SERVER_SID_FOLDER_NAME_FOR_CLIENT, fstream->sid);

    if (CreateAbsoluteFolderRecursive(rootFolder) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\". Error code: %lu\n", rootFolder, GetLastError());
        err_code = ERR_STREAM_PATH_CREATE;
        goto exit_err;
    }

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        err_code = ERR_STREAM_PATH_CREATE;
        goto exit_err;
    }

    // ANSI PATH
    snprintf(fstream->ansi_path, MAX_PATH, "%s%s%s", rootFolder, fstream->rpath, fstream->fname);
    
    // UNICODE PATH
    int result = MultiByteToWideChar(
        CP_ACP,                         // ANSI code page
        0,                              // No special flags
        fstream->ansi_path,             // Source string
        -1,                             // Null-terminated
        fstream->unicode_path,          // Destination buffer
        MAX_PATH                        // Buffer size
    );

    if (result == 0) {
        fprintf(stderr, "Path conversion from ansi to unicode failed: %lu\n", GetLastError());
        err_code = ERR_STREAM_PATH_CREATE;
        goto exit_err;
    }
    
    // TEMP ANSI PATH
    snprintf(fstream->temp_ansi_path, MAX_PATH, "%s%s%s%s", rootFolder, fstream->rpath, fstream->fname, ".temp");

    // TEMP UNICODE PATH
    result = MultiByteToWideChar(
        CP_ACP,                         // ANSI code page
        0,                              // No special flags
        fstream->temp_ansi_path,        // Source string
        -1,                             // Null-terminated
        fstream->temp_unicode_path,     // Destination buffer
        MAX_PATH                        // Buffer size
    );

    if (result == 0) {
        fprintf(stderr, "Temp path conversion from ansi to unicode failed: %lu\n", GetLastError());
        err_code = ERR_STREAM_PATH_CREATE;
        goto exit_err;
    }

    if(FileExists(fstream->ansi_path)){
        fprintf(stderr, "ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->ansi_path);
        err_code = ERR_STREAM_FILE_EXIST;
        goto exit_err;
    }
    
    // if(FileExists(fstream->temp_ansi_path)){
    //     fprintf(stderr, "CRITICAL ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->temp_ansi_path);
    //     err_code = ERR_STREAM_TEMP_FILE_EXIST;
    //     goto exit_err;
    // }

    // CREATE FILE AT TEMP UNICODE PATH
    fstream->iocp_file_handle = CreateFileW(
        fstream->temp_unicode_path,            // File path
        GENERIC_WRITE | DELETE,
        FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,                               // Default security
        CREATE_ALWAYS,                      // Create or overwrite
        FILE_FLAG_OVERLAPPED,               // Enable async I/O
        NULL                                // No template
    );
    
    if(ht_insert_id(table_file_id, fstream->sid, fstream->fid, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: init_fstream - Error updating status of file in hash table!!!\n");
        err_code = ERR_STREAM_TABLE_INSERT;
        goto exit_err;
    }

    if (fstream->iocp_file_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create file \"%ls\". Error code: %lu\n", fstream->temp_unicode_path, GetLastError());
        err_code = ERR_STREAM_FILE_HANDLE_INIT;
        goto exit_err;
    }
    if (!CreateIoCompletionPort(fstream->iocp_file_handle, server->iocp_file_handle, (uintptr_t)fstream, 0)) {
        fprintf(stderr, "Failed to associate file_test with IOCP: %lu\n", GetLastError());
        err_code = ERR_STREAM_FILE_HANDLE_ASSOCIATE;
        goto exit_err;
    }

    ReleaseSRWLockExclusive(&fstream->lock);
    return STS_STREAM_SUCCESS;

exit_err:
    ReleaseSRWLockExclusive(&fstream->lock);
    ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
    NodeTableFileBlock *node = ht_insert_fblock(table_file_block, InterlockedIncrement64(&server->file_block_count), 3, fstream->sid, fstream->fid, NULL, 0);
    PostQueuedCompletionStatus(server->iocp_file_handle, 0, (uintptr_t)fstream, &node->overlapped);
    return err_code;
}
void free_fstream(ServerFstreamPool* pool, ServerFileStream* fstream) {
    
    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to free_fstream() in an unallocated pool!\n");
        return;
    }
    if (!fstream) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in fstream pool!\n");
        return;
    }
    AcquireSRWLockExclusive(&pool->lock);
    // Calculate the index of the block to be freed
    uint64_t index = (uint64_t)(((char*)fstream - (char*)pool->fstream) / sizeof(ServerFileStream));
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {       
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid fstream from pool!\n");
        ReleaseSRWLockExclusive(&pool->lock);
        return;
    }
    // Add the freed block back to the head of the free list
    pool->next[index] = pool->free_head;
    pool->free_head = index;
    // Mark the block as unused
    pool->used[index] = FREE_BLOCK;
    pool->free_blocks++;
    ReleaseSRWLockExclusive(&pool->lock);
    return;
}
void close_fstream(ServerFileStream *fstream) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    if(!fstream){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer file stream\n");
        return;
    }

    if(fstream->received_file_bitmap){
        free(fstream->received_file_bitmap);
        fstream->received_file_bitmap = NULL;
    }

    if(fstream->block_count > 0){
        if(fstream->file_block){
            for(uint64_t i = 0; i < fstream->block_count; i++){
                if(!fstream->file_block[i]){
                    continue;
                }
                pool_free(pool_file_block, fstream->file_block[i]);
                fstream->file_block[i] = NULL;
            }
        }
        if(fstream->recv_block_bytes){
            for(uint64_t i = 0; i < fstream->block_count; i++){
                fstream->recv_block_bytes[i] = 0;
                fstream->recv_block_status[i] = BLOCK_STATUS_NONE;
            }
        }
    }
    if(fstream->file_block){
        free(fstream->file_block);
        fstream->file_block = NULL;
    }
    if(fstream->recv_block_bytes){
        free(fstream->recv_block_bytes);
        fstream->recv_block_bytes = NULL;
    }
    if(fstream->recv_block_status){
        free(fstream->recv_block_status);
        fstream->recv_block_status = NULL;
    }
    if(ht_search_id(table_file_id, fstream->sid, fstream->fid, ID_RECV_COMPLETE)){
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_SUCCESS);
        RenameFileByHandle(fstream->iocp_file_handle, fstream->unicode_path);
    } else if (ht_search_id(table_file_id, fstream->sid, fstream->fid, ID_STATUS_NONE)){
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
        DeleteFileByHandle(fstream->iocp_file_handle);
        // ht_remove_id(table_file_id, fstream->sid, fstream->fid);
    } else if (ht_search_id(table_file_id, fstream->sid, fstream->fid, ID_WAITING_FRAGMENTS)){
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
        DeleteFileByHandle(fstream->iocp_file_handle);
    }
    CloseHandle(fstream->iocp_file_handle);
    fstream->iocp_file_handle = NULL;
    memset(&fstream->ansi_path, 0, MAX_PATH);
    memset(&fstream->temp_ansi_path, 0, MAX_PATH);
    memset(&fstream->unicode_path, 0, MAX_PATH);
    memset(&fstream->temp_unicode_path, 0, MAX_PATH);
    fstream->sid = 0;                                   // Session ID associated with this file stream.
    fstream->fid = 0;                                   // File ID, unique identifier for the file associated with this file stream.
    fstream->file_size = 0;                                 // Total size of the file being transferred.
    fstream->fragment_count = 0;                    // Total number of fragments in the entire file.
    fstream->block_count = 0;
    fstream->file_bitmap_size = 0;              // Number of uint64_t entries in the bitmap array.

    fstream->written_bytes_count = 0;       // Total bytes written to disk for this file so far.

    memset(&fstream->received_sha256, 0, 32);
    fstream->end_of_file = FALSE;
    memset(&fstream->calculated_sha256, 0, 32);
    fstream->rpath_len = 0;
    memset(&fstream->rpath, 0, MAX_PATH);
    fstream->fname_len = 0;
    memset(&fstream->fname, 0, MAX_PATH);
    memset(&fstream->client_addr, 0, sizeof(struct sockaddr_in));
    // free_fstream(pool_fstreams, fstream);
    return;
}

// Deallocate all memory associated with the fstream pool
void destroy_fstream_pool(ServerFstreamPool* pool) {
    // Check for NULL pool pointer
    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to destroy_fstream_pool() on an unallocated pool!\n");
        return;
    }

    // Free allocated memory for 'next' array
    if (pool->next) {
        _aligned_free(pool->next);
        pool->next = NULL;
    }
    // Free allocated memory for 'used' array
    if (pool->used) {
        _aligned_free(pool->used);
        pool->used = NULL;
    }
    // Free the main memory buffer
    if (pool->fstream) {
        _aligned_free(pool->fstream);
        pool->fstream = NULL;
    }
    pool->free_blocks = 0;
}
// Process received file metadata frame
int handle_file_metadata(Client *client, UdpFrame *frame) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_metadata()\n");
        return RET_VAL_ERROR;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_metadata()\n");
        return RET_VAL_ERROR;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);

    uint8_t op_code = STS_ACK_UNDEFINED;
    PoolEntrySendFrame *entry_send_frame = NULL;

    if(recv_file_size == 0ULL){
        fprintf(stderr, "Received metadata frame Seq: %llu for fID: %u sID %u with zero file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_WAITING_FRAGMENTS) == TRUE){
        fprintf(stderr, "Received duplicated metadata frame Seq: %llu for fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_CONFIRM_DUPLICATE_FILE_METADATA;
        goto exit;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_TRANSFER_ERROR) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for transfer error fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_SERVER_TERMINATED_STREAM;
        goto exit;
    }

    ServerFileStream *fstream = alloc_fstream(pool_fstreams);
    if(!fstream){
        op_code = ERR_ALL_STREAMS_BUSY;
        goto exit;
    }
    // AcquireSRWLockExclusive(&fstream->lock);
    uint8_t init_result = init_fstream(fstream, frame, &client->client_addr);
    switch (init_result) {
        case STS_STREAM_SUCCESS:
            op_code = STS_CONFIRM_FILE_METADATA;
            break;

        case ERR_STREAM_FILE_EXIST:
        case ERR_STREAM_TEMP_FILE_EXIST:
            op_code = ERR_EXISTING_FILE;
            break;

        case ERR_STREAM_INVALID_PARAMETERS:
        case ERR_STREAM_INVALID_PAYLOAD_DATA:
            op_code = ERR_MALFORMED_FRAME;
            break;

        case ERR_STREAM_MEMORY_ALLOCATION:
        case ERR_STREAM_TABLE_INSERT:
            op_code = ERR_RESOURCE_LIMIT;
            break;

        case ERR_STREAM_PATH_CREATE:
        case ERR_STREAM_FILE_HANDLE_INIT:
        case ERR_STREAM_FILE_HANDLE_ASSOCIATE:
            op_code = ERR_INTERNAL_ERROR;
            break;
        
        default:
            op_code = ERR_UNKNOWN_ERROR;
            break;
    }
    // ReleaseSRWLockExclusive(&fstream->lock);
    goto exit;

exit:
    entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for metadata ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_file_metadata_response_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    // fprintf(stdout, "sending metadata response frame sid: %lu, fid: %lu\n", recv_session_id, recv_file_id);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file metadata ack error frame to queue\n");
        return RET_VAL_ERROR;
    }

    if(op_code != STS_CONFIRM_FILE_METADATA && op_code != ERR_CONFIRM_DUPLICATE_FILE_METADATA){
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;

}
// Process received file fragment frame
int handle_file_fragment(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    char buffer[FILE_FRAGMENT_SIZE];
    FILETIME ft;

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_fragment()\n");
        return RET_VAL_ERROR;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_fragment()\n");
        return RET_VAL_ERROR;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    uint8_t op_code = 0;

    if(recv_fragment_size == 0 || recv_fragment_size > FILE_FRAGMENT_SIZE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid fragment size: %u\n", recv_seq_num, recv_file_id, recv_session_id, recv_fragment_size);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if(recv_session_id == 0 || recv_file_id == 0){
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid session or file ID\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_STATUS_NONE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for unknown transfer file fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_UNKNOWN_FILE_ID;
        goto exit_err;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_TRANSFER_ERROR) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for failed transfer file fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_SERVER_TERMINATED_STREAM;
        goto exit_err;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_UNKNOWN_FILE_ID;
        goto exit_err;
    }
    AcquireSRWLockExclusive(&fstream->lock);

    if(recv_fragment_offset + recv_fragment_size > fstream->file_size){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid fragment offset + size exceeding max file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;     
        goto exit_err;
    }
    if(check_fragment_received(fstream->received_file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "DEBUG: file_handler() - Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    uint64_t block_nr = recv_fragment_offset / SERVER_FILE_BLOCK_SIZE;
    uint64_t block_offset = block_nr * SERVER_FILE_BLOCK_SIZE;
    uint64_t block_fragment_offset = recv_fragment_offset - block_offset;

    uint64_t block_size;
    if(block_nr < fstream->block_count - 1){
        block_size = SERVER_FILE_BLOCK_SIZE;
    } else if (block_nr == fstream->block_count - 1){
        block_size = fstream->file_size - ((fstream->block_count - 1) * SERVER_FILE_BLOCK_SIZE);
    } else {
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid block number: %llu\n", recv_seq_num, recv_file_id, recv_session_id, block_nr);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if(fstream->recv_block_status[block_nr] == BLOCK_STATUS_NONE && fstream->recv_block_bytes[block_nr] == 0){
        fstream->file_block[block_nr] = pool_alloc(pool_file_block);
        if(!fstream->file_block[block_nr]){
            ReleaseSRWLockExclusive(&fstream->lock);
            fprintf(stderr, "DEBUG: file_handler() - Failed to allocate memory for file block from pool\n");
            op_code = ERR_RESOURCE_LIMIT;
            goto exit_err;    
        }
        fstream->recv_block_status[block_nr] = BLOCK_STATUS_RECEIVEING;
    }

    memcpy(fstream->file_block[block_nr] + block_fragment_offset, frame->payload.file_fragment.bytes, recv_fragment_size);
    fstream->recv_block_bytes[block_nr] += recv_fragment_size;

    mark_fragment_received(fstream->received_file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE);

    // if((recv_file_id % 25) == 0){
    //     fstream->recv_block_bytes[block_nr] = block_size + 1;
    // }

    if(fstream->recv_block_bytes[block_nr] < block_size){
        ReleaseSRWLockExclusive(&fstream->lock);
    } else if (fstream->recv_block_bytes[block_nr] > 0 && fstream->recv_block_bytes[block_nr] == block_size){
        ReleaseSRWLockExclusive(&fstream->lock);
        NodeTableFileBlock *node = ht_insert_fblock(table_file_block, InterlockedIncrement64(&server->file_block_count), 1, fstream->sid, fstream->fid, fstream->file_block[block_nr], block_size);

        // IMPORTANT NOTE: the size of the table should be atleast the size of pool_file_block. this will ensure node will be allocated
        // TODO: at the moment can't safely clean the stream if this fails, need to implement a safe way to handle this
        if(!node){
            // only one thread now process a stream so terminating the stream seems ok.
            // need to implement on client side a stream terminate also and retry sending the file.
            ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
            // close_fstream(fstream);
            // free_fstream(pool_fstreams, fstream);
            fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for file block in hash table. Terminate file stream (TODO)!\n");
            op_code = ERR_SERVER_TERMINATED_STREAM;
            goto exit_err;
        }      

        memset(&node->overlapped, 0, sizeof(OVERLAPPED));
        node->overlapped.Offset = (DWORD)(block_offset);                 // Lower 32 bits
        node->overlapped.OffsetHigh = (DWORD)(block_offset >> 32);       // Upper 32 bits
        if (retry_async_write(fstream->iocp_file_handle, node->block_data, node->block_size, &node->overlapped) == RET_VAL_ERROR) {
            fprintf(stderr, "CRITICAL ERROR: Terminate file stream (TODO)!\n");
            // TODO: need a strategy to safely terminate the stream (since multiple threads are accessing it)
            ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
            ht_remove_fblock(table_file_block, server->file_block_count);
            op_code = ERR_SERVER_TERMINATED_STREAM;
            goto exit_err;
        }
    } else {
        // TODO: need a strategy to safely terminate the stream (since multiple threads are accessing it)
        ReleaseSRWLockExclusive(&fstream->lock);
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
        NodeTableFileBlock *node = ht_insert_fblock(table_file_block, InterlockedIncrement64(&server->file_block_count), 3, fstream->sid, fstream->fid, NULL, 0);
        PostQueuedCompletionStatus(server->iocp_file_handle, 0, (uintptr_t)fstream, &node->overlapped);
        
        // fprintf(stderr, "CRITICAL ERROR: Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid block received bytes: %llu; Terminate file stream (TODO)!\n", recv_seq_num, recv_file_id, recv_session_id, fstream->recv_block_bytes[block_nr]);
        op_code = ERR_SERVER_TERMINATED_STREAM;
        goto exit_err;
    }
    
    // The fragment frames are cumulated to a sack frame before sending
    if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack seq to queue\n");
        return RET_VAL_ERROR;
    }
    if(push_ptr(queue_client_ptr, (uintptr_t)client) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push client to to client queue\n");
        return RET_VAL_ERROR;
    };

    return RET_VAL_SUCCESS;

exit_err:
    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file fragment ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack error frame to queue\n");
        pool_free(pool_send_udp_frame, entry_send_frame);
        return RET_VAL_ERROR;
    }
    return RET_VAL_ERROR;
}
// Process received file end frame
int handle_file_end(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_end()\n");
        return RET_VAL_ERROR;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_end()\n");
        return RET_VAL_ERROR;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);

    uint8_t op_code = STS_ACK_UNDEFINED;

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_STATUS_NONE) == TRUE){
        fprintf(stderr, "Received file end frame for unknown file status file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        op_code = ERR_UNKNOWN_FILE_ID;
        goto exit;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received file end frame for completed file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        op_code = ERR_EXISTING_FILE;
        goto exit;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_TRANSFER_ERROR) == TRUE){
        fprintf(stderr, "Received file end frame for transfer error file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        op_code = ERR_SERVER_TERMINATED_STREAM;
        goto exit;
    }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received end frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_UNKNOWN_FILE_ID;
        goto exit;
    }

    // session_id = _ntohl(frame_buff->frame.header.session_id);
    // file_id = _ntohl(frame_buff->frame.payload.file_metadata.file_id);
    // size_t thread_id = hash_stream(recv_session_id, recv_file_id, SERVER_MAX_THREADS_PROCESS_FRAME);
    AcquireSRWLockExclusive(&fstream->lock);

    for(int i = 0; i < 32; i++){   
        fstream->received_sha256[i] = frame->payload.file_end.file_hash[i];
    }
    ReleaseSRWLockExclusive(&fstream->lock);
    NodeTableFileBlock *node = ht_insert_fblock(table_file_block, InterlockedIncrement64(&server->file_block_count), 2, fstream->sid, fstream->fid, NULL, 0);
    // memset(&node->overlapped, 0, sizeof(OVERLAPPED));
    PostQueuedCompletionStatus(server->iocp_file_handle, 0, (uintptr_t)fstream, &node->overlapped);  

    op_code = STS_CONFIRM_FILE_END;

    goto exit;

exit:
    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file end ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_file_end_response_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file end ack error frame to queue\n");
        return RET_VAL_ERROR;
    }
    
    if(op_code != STS_CONFIRM_FILE_END){
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}

