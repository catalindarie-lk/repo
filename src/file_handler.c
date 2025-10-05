
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

void init_client_pool(ServerClientPool* pool, const uint64_t block_count) {

    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in init_client_pool().\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in init_client_pool().\n");
        _aligned_free(pool->next);
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)

    // Allocate the main memory buffer for the pool
    pool->client = (ServerClient*)_aligned_malloc(sizeof(ServerClient) * pool->block_count, 64);
    if (!pool->client) {
        fprintf(stderr, "Memory allocation failed for fstream in init_client_pool().\n");
        _aligned_free(pool->next);
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->client, 0, sizeof(ServerClient) * pool->block_count);

    // Initialize the free list: all blocks are initially free
    pool->free_head = 0;                                        // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    uint64_t last_block = pool->block_count - 1;
    for (uint64_t index = 0; index < last_block; index++) {
        pool->next[index] = index + 1;                          // Link to the next block
        pool->used[index] = FREE_BLOCK;                         // Mark as unused

        pool->client[index].connection_status = CLIENT_DISCONNECTED;
        pool->client[index].last_activity_time = time(NULL);

        InitializeSRWLock(&pool->client[index].lock);
        InitializeSRWLock(&pool->client[index].sack_buff.lock);
        init_queue_seq(&pool->client[index].queue_ack_seq, SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ);   
    }
    // The last block points to END_BLOCK, indicating the end of the free list
    pool->next[last_block] = END_BLOCK;              // Use END_BLOCK to indicate end of list
    pool->used[last_block] = FREE_BLOCK;             // Last block is also unused

    pool->client[last_block].connection_status = CLIENT_DISCONNECTED;
    pool->client[last_block].last_activity_time = time(NULL);

    InitializeSRWLock(&pool->client[last_block].lock);
    InitializeSRWLock(&pool->client[last_block].sack_buff.lock);
    init_queue_seq(&pool->client[last_block].queue_ack_seq, SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ);

    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
ServerClient* alloc_client(ServerClientPool* pool) {
    // Enter critical section to protect shared pool data
    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to alloc_client() in an unallocated pool!\n");
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
    return &pool->client[index];
}
ServerClient* find_client(ServerClientPool* pool, const uint32_t sid) {

    ServerClient *client = NULL;

    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to find_client() in an unallocated pool!\n");
        return NULL;
    }

    AcquireSRWLockShared(&pool->lock);
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        client = &pool->client[index];
        if(client->sid == sid && client->connection_status == CLIENT_CONNECTED){
            ReleaseSRWLockShared(&pool->lock);
            return client;
        }
    }
    ReleaseSRWLockShared(&pool->lock);
    return NULL;
}
int init_client(ServerClient *client, const uint32_t sid, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr){

    if(!client){
        fprintf(stderr, "Invalid client pointer passed for init!\n");
        return RET_VAL_ERROR;
    }

    AcquireSRWLockExclusive(&client->lock);

    memcpy(&client->client_addr, client_addr, sizeof(struct sockaddr_in));
    client->connection_status = CLIENT_CONNECTED;
    client->last_activity_time = time(NULL);

    client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
    client->sid = sid;
    client->flags = recv_frame->payload.connection_request.flags;

    snprintf(client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, client->ip, INET_ADDRSTRLEN);
    client->port = _ntohs(client_addr->sin_port);

    memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
    client->sack_buff.ack_pending = 0;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    client->sack_buff.start_timestamp = FILETIME_TO_UINT64(ft);
    client->sack_buff.start_recorded = FALSE;

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", client->ip, client->port, client->sid);

    ReleaseSRWLockExclusive(&client->lock);

    return RET_VAL_SUCCESS;

}
void free_client(ServerClientPool* pool, ServerClient* client) {
    
    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to free_client() in an unallocated pool!\n");
        return;
    }
    if (!client) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in client pool!\n");
        return;
    }
    AcquireSRWLockExclusive(&pool->lock);
    // Calculate the index of the block to be freed
    uint64_t index = (uint64_t)(((char*)client - (char*)pool->client) / sizeof(ServerClient));
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {       
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid client from pool!\n");
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
void close_client(ServerClient *client){
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }

    AcquireSRWLockExclusive(&client->lock);

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &pool_fstreams->fstream[i];
        //AcquireSRWLockExclusive(&fstream->lock);
        if(fstream->sid == client->sid){
            close_fstream(fstream);
            free_fstream(pool_fstreams, fstream);
        }
        //ReleaseSRWLockExclusive(&fstream->lock);
    }

    ht_remove_all_sid(table_file_id, client->sid);
 
    AcquireSRWLockExclusive(&client->sack_buff.lock);
    client->sack_buff.ack_pending = 0;
    client->sack_buff.start_recorded = 0;
    memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
    ReleaseSRWLockExclusive(&client->sack_buff.lock);

    memset(&client->client_addr, 0, sizeof(struct sockaddr_in));
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    client->port = 0;
    client->cid = 0;
    memset(&client->name, 0, MAX_NAME_SIZE);
    client->flags = 0;
    client->connection_status = CLIENT_DISCONNECTED;
    client->last_activity_time = time(NULL);
    client->sid = 0;
    free_client(pool_clients, client);
    ReleaseSRWLockExclusive(&client->lock);
    return;
}



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
   
    pool->fstream[pool->block_count - 1].fid = pool->block_count - 1;
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
                pool_free(_pool_file_block, (void*)fstream->file_block[i]);
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

    // if(check_bitmap(fstream->received_file_bitmap, fstream->fragment_count)){
    //                     // ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_RECV_COMPLETE);
    
    //     if (!FlushFileBuffers(fstream->iocp_file_handle)) {
    //         DWORD error = GetLastError();
    //         fprintf(stderr, "CRITICAL ERROR: Flush file fail error: %lu\n", error);
    //     }
    //     RenameFileByHandle(fstream->iocp_file_handle, fstream->unicode_path);  
    
    // } else {
    //     // Incomplete file transfer, delete the temp file
    //     DeleteFileByHandle(fstream->iocp_file_handle);
    // }

    if(ht_search_id(table_file_id, fstream->sid, fstream->fid, ID_RECV_COMPLETE)){
        // Flush buffers
        if (!FlushFileBuffers(fstream->iocp_file_handle)) {
            DWORD error = GetLastError();
            fprintf(stderr, "CRITICAL ERROR: Flush file fail error: %lu\n", error);
        }
        RenameFileByHandle(fstream->iocp_file_handle, fstream->unicode_path);        
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_WRITE_COMPLETE);
    } else {
        ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
        DeleteFileByHandle(fstream->iocp_file_handle);
        // ht_remove_id(table_file_id, fstream->sid, fstream->fid);
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

uint8_t init_fstream(ServerFileStream *fstream, UdpFrame *frame, const struct sockaddr_in *client_addr) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    uint8_t op_code = UNDEFINED;

    if(!fstream || !frame || !client_addr){
        fprintf(stderr, "ERROR: init_fstream - Invalid NULL parameter(s)!\n");
        return ERR_STREAM_INIT;
    }
    
    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    fstream->sid = _ntohl(frame->header.session_id);
    fstream->fid = _ntohl(frame->payload.file_metadata.file_id);
    fstream->file_size = _ntohll(frame->payload.file_metadata.file_size);

    // size_t thread_id = hash_stream(fstream->sid, fstream->fid, SERVER_MAX_THREADS_PROCESS_FSTREAM);

    // --- Proper string copy and validation for rpath ---
    uint32_t received_rpath_len = _ntohl(frame->payload.file_metadata.rpath_len);

    // Validate the received length against the destination buffer's capacity (MAX_PATH - 1 for content + null)
    if (received_rpath_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_fstream - Received rpath length (%u) is too large for buffer (max %d).\n",
                received_rpath_len, MAX_PATH - 1);
        fstream->rpath_len = 0;
        fstream->rpath[0] = '\0';
        op_code = ERR_INVALID_FRAME_DATA;
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
            op_code = ERR_INVALID_FRAME_DATA;
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
        op_code = ERR_INVALID_FRAME_DATA;
        goto exit_err;
    } else {
        int result = snprintf(fstream->fname, sizeof(fstream->fname),
                              "%.*s", (int)received_fname_len, frame->payload.file_metadata.fname);

        if (result < 0 || (size_t)result != received_fname_len) {
            fprintf(stderr, "ERROR: init_fstream - Failed to copy fname: snprintf returned %d, expected %u.\n",
                    result, received_fname_len);
            fstream->fname_len = 0;
            fstream->fname[0] = '\0';
            op_code = ERR_INVALID_FRAME_DATA;
            goto exit_err;
        } else {
            fstream->fname_len = received_fname_len;
        }
    }
    // --- End of string copy and validation ---

    memcpy(&fstream->client_addr, client_addr, sizeof(struct sockaddr_in));

    fstream->written_bytes_count = 0;       // Total bytes written to disk for this file so far.
    fstream->end_of_file = FALSE;
    memset(&fstream->received_sha256, 0, 32);
    

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
        op_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->received_file_bitmap, 0, fstream->file_bitmap_size * sizeof(uint64_t));

    fstream->file_block = malloc(fstream->block_count * sizeof(ServerFileBlock*));
    if(fstream->file_block == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file block mem!!!\n");
        op_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->file_block, 0, fstream->block_count * sizeof(ServerFileBlock*));

    fstream->recv_block_bytes = malloc(fstream->block_count * sizeof(uint64_t));
    if(fstream->recv_block_bytes == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file recv block bytes!!!\n");
        op_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->recv_block_bytes, 0, fstream->block_count * sizeof(uint64_t));

    fstream->recv_block_status = malloc(fstream->block_count * sizeof(uint64_t));
    if(fstream->recv_block_status == NULL){
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file recv block status!!!\n");
        op_code = ERR_STREAM_MEMORY_ALLOCATION;
        goto exit_err;
    }
    memset(fstream->recv_block_status, 0, fstream->block_count * sizeof(uint64_t));

    if(!DriveExists(SERVER_PARTITION_DRIVE)){
        fprintf(stderr, "ERROR: init_fstream - Drive Partition \"%s\" doesn't exit\n", SERVER_PARTITION_DRIVE);
        op_code = ERR_INVALID_DRIVE_PARTITION;
        goto exit_err;
    }

    // creating root folder\\session_folder
    char rootFolder[MAX_PATH];
    snprintf(rootFolder, MAX_PATH, "%s%s%d", SERVER_ROOT_FOLDER, SERVER_SID_FOLDER_NAME_FOR_CLIENT, fstream->sid);

    if (CreateAbsoluteFolderRecursive(rootFolder) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\". Error code: %lu\n", rootFolder, GetLastError());
        op_code = ERR_CREATE_FILE_PATH;
        goto exit_err;
    }

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        op_code = ERR_CREATE_FILE_PATH;
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
        op_code = ERR_CREATE_FILE_PATH;
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
        op_code = ERR_CREATE_FILE_PATH;
        goto exit_err;
    }

    if(FileExists(fstream->ansi_path)){
        fprintf(stderr, "ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->ansi_path);
        op_code = ERR_EXISTING_DISK_FILE;
        goto exit_err;
    }
    
    if(FileExists(fstream->temp_ansi_path)){
        fprintf(stderr, "CRITICAL ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->temp_ansi_path);
        op_code = ERR_EXISTING_DISK_TEMP_FILE;
        goto exit_err;
    }

    // CREATE FILE AT TEMP UNICODE PATH
    fstream->iocp_file_handle = CreateFileW(
        fstream->temp_unicode_path,            // File path
        GENERIC_WRITE | DELETE,
        FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,                               // Default security
        CREATE_ALWAYS,                      // Create or overwrite
        FILE_FLAG_OVERLAPPED,              // Enable async I/O
        NULL                                // No template
    );
    
    if (fstream->iocp_file_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create file \"%ls\". Error code: %lu\n", fstream->temp_unicode_path, GetLastError());
        op_code = ERR_CREATE_FILE;
        goto exit_err;
    }

    if (!CreateIoCompletionPort(fstream->iocp_file_handle, server->iocp_process_fstream_handle, (uintptr_t)fstream, 0)) {
        fprintf(stderr, "Failed to associate file_test with IOCP: %lu\n", GetLastError());
        op_code = ERR_CREATE_FILE;
        goto exit_err;
    }
    
    if(ht_insert_id(table_file_id, fstream->sid, fstream->fid, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: init_fstream - Error updating status of file in hash table!!!\n");
        op_code = ERR_CREATE_FILE;
        goto exit_err;
    }
    return STS_CONFIRM_FILE_METADATA;

exit_err:
    ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
    return op_code;
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
void handle_file_metadata(ServerClient *client, UdpFrame *frame) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_metadata()\n");
        return;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_metadata()\n");
        return;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);

    uint8_t op_code = UNDEFINED;
    PoolEntrySendFrame *entry_send_frame = NULL;

    if(recv_file_size == 0ULL){
        fprintf(stderr, "Received metadata frame Seq: %llu for fID: %u sID %u with zero file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }

    // uint8_t file_status = ht_status_id(table_file_id, recv_session_id, recv_file_id);
    // switch (file_status) {
    //     case ID_STATUS_NONE:
    //         fprintf(stderr, "Received metadata frame Seq: %llu; for invalid state file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_UNKNOWN_FILE_ID;
    //         goto exit;

    //     case ID_RECV_COMPLETE:
    //     case ID_WRITE_COMPLETE:
    //         fprintf(stderr, "Received metadata frame Seq: %llu; for old completed fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_COMPLETED_FILE;
    //         goto exit;

    //     case ID_TRANSFER_ERROR:
    //         fprintf(stderr, "Received metadata frame Seq: %llu; for failed transfer file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_STREAM_ALREADY_FAILED;
    //         goto exit;

    //     case ID_WAITING_FRAGMENTS:
    //         op_code = STS_WAITING_FILE_FRAGMENTS;
    //         goto exit;
        
    //     case ID_UNKNOWN:
    //         // continue
    //         break;

    //     default:
    //         break;
    // }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(fstream != NULL){
        fprintf(stderr, "Received metadata frame Seq: %llu for already opened stream fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = STS_WAITING_FILE_FRAGMENTS;
        goto exit;
    }

    fstream = alloc_fstream(pool_fstreams);
    if(!fstream){
        op_code = ERR_ALL_STREAMS_BUSY;
        goto exit;
    }

    AcquireSRWLockExclusive(&fstream->lock);
    op_code = init_fstream(fstream, frame, &client->client_addr);
    ReleaseSRWLockExclusive(&fstream->lock);
    
    goto exit;

exit:
    
    switch (op_code) {
        // error before stream is initiated
        case ERR_MALFORMED_FRAME:
        // case ERR_COMPLETED_FILE:
        // case ERR_STREAM_ALREADY_FAILED:
        case ERR_ALL_STREAMS_BUSY:
        case ERR_STREAM_INIT:
            break;

        // error after stream is initialized -> safely close fstream
        case ERR_EXISTING_DISK_FILE:
        case ERR_EXISTING_DISK_TEMP_FILE:
        case ERR_INVALID_FRAME_DATA:
        case ERR_STREAM_MEMORY_ALLOCATION:
        case ERR_INVALID_DRIVE_PARTITION:
        case ERR_CREATE_FILE_PATH:
        case ERR_CREATE_FILE:
            IocpOperation *iocp_op = (IocpOperation*)s_pool_alloc(&sspu->pool_iocp_operation);
            memset(&iocp_op->overlapped, 0, sizeof(OVERLAPPED));
            iocp_op->io_type = IO_FSTREAM_CLOSE;
            PostQueuedCompletionStatus(server->iocp_process_fstream_handle, 0, (uintptr_t)fstream, &iocp_op->overlapped);
            break;

        case STS_CONFIRM_FILE_METADATA:
        case STS_WAITING_FILE_FRAGMENTS:
            break;     
        
        default:
            fprintf(stdout, "CRITICAL ERROR: Unknown response code for file metadata\n");
            break;
    }

    entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_prio_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for metadata ack error frame\n");
        return;
    }

   

    construct_file_metadata_response_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_prio_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file metadata ack error frame to queue\n");
        return;
    }
}
// Process received file fragment frame
void handle_file_fragment(ServerClient *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    char buffer[FILE_FRAGMENT_SIZE];
    FILETIME ft;

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_fragment()\n");
        return;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_fragment()\n");
        return;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    // size_t thread_id = hash_stream(recv_session_id, recv_file_id, SERVER_MAX_THREADS_PROCESS_FSTREAM);

    uint8_t op_code = UNDEFINED;

    if(recv_fragment_size == 0 || recv_fragment_size > FILE_FRAGMENT_SIZE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid fragment size: %u\n", recv_seq_num, recv_file_id, recv_session_id, recv_fragment_size);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }

    if(recv_session_id == 0 || recv_file_id == 0){
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid session or file ID\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }

    // uint8_t file_status = ht_status_id(table_file_id, recv_session_id, recv_file_id);
    // switch (file_status) {
    //     case ID_UNKNOWN:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for unknown file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_UNKNOWN_FILE_ID;
    //         goto exit;

    //     case ID_STATUS_NONE:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for invalid state file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_INVALID_FILE_STATUS;
    //         goto exit;

    //     case ID_RECV_COMPLETE:
    //     case ID_WRITE_COMPLETE:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_COMPLETED_FILE;
    //         goto exit;

    //     case ID_TRANSFER_ERROR:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for failed transfer file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_STREAM_ALREADY_FAILED;
    //         goto exit;
        
    //     case ID_WAITING_FRAGMENTS:
    //         // continue
    //         break;

    //     default:
            
    //         break;
    // }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_INVALID_STREAM;
        goto exit;
    }

    AcquireSRWLockExclusive(&fstream->lock);

    if(recv_fragment_offset + recv_fragment_size > fstream->file_size){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid fragment offset + size exceeding max file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }
    if(check_fragment_received(fstream->received_file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "DEBUG: file_handler() - Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit;
    }

    uint64_t block_nr = recv_fragment_offset / SERVER_FILE_BLOCK_SIZE;
    uint64_t block_fragment_offset = recv_fragment_offset - (block_nr * SERVER_FILE_BLOCK_SIZE);

    if(fstream->recv_block_status[block_nr] == BLOCK_STATUS_NONE && fstream->recv_block_bytes[block_nr] == 0){
        fstream->file_block[block_nr] = (ServerFileBlock*)pool_alloc(_pool_file_block);
        if(!fstream->file_block[block_nr]){
            ReleaseSRWLockExclusive(&fstream->lock);
            fprintf(stderr, "DEBUG: file_handler() - Failed to allocate memory for file block from pool\n");
            op_code = ERR_RESOURCE_LIMIT;
            goto exit;    
        }
        fstream->file_block[block_nr]->block_offset = block_nr * SERVER_FILE_BLOCK_SIZE;

        if(block_nr < fstream->block_count - 1){
            fstream->file_block[block_nr]->block_size = SERVER_FILE_BLOCK_SIZE;
        } else if (block_nr == fstream->block_count - 1){
            fstream->file_block[block_nr]->block_size = fstream->file_size - ((fstream->block_count - 1) * SERVER_FILE_BLOCK_SIZE);
        } else {
            ReleaseSRWLockExclusive(&fstream->lock);
            fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid block number: %llu\n", recv_seq_num, recv_file_id, recv_session_id, block_nr);
            op_code = ERR_MALFORMED_FRAME;
            goto exit;
        }

        fstream->recv_block_status[block_nr] = BLOCK_STATUS_RECEIVEING;
    }

    // Bounds check before memcpy
    if (block_fragment_offset + recv_fragment_size > fstream->file_block[block_nr]->block_size) {
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "ERROR: file_handler() - Fragment exceeds block size: offset=%llu, size=%lu, block_size=%llu\n",
                block_fragment_offset, recv_fragment_size, fstream->file_block[block_nr]->block_size);
        op_code = ERR_MALFORMED_FRAME;
        goto exit;
    }

    memcpy(fstream->file_block[block_nr]->block_data + block_fragment_offset, frame->payload.file_fragment.bytes, recv_fragment_size);
    fstream->recv_block_bytes[block_nr] += recv_fragment_size;

    mark_fragment_received(fstream->received_file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE);

    // if((recv_file_id % 100) == 0){
    //     fstream->recv_block_bytes[block_nr] = SERVER_FILE_BLOCK_SIZE + 1;
    // }

    if(fstream->recv_block_bytes[block_nr] < fstream->file_block[block_nr]->block_size){
        ReleaseSRWLockExclusive(&fstream->lock);
    } else if (fstream->recv_block_bytes[block_nr] > 0 && fstream->recv_block_bytes[block_nr] == fstream->file_block[block_nr]->block_size){
        fstream->recv_block_status[block_nr] = BLOCK_STATUS_RECEIVED;
               
        IocpOperation *iocp_op = (IocpOperation*)s_pool_alloc(&sspu->pool_iocp_operation);   
        memset(&iocp_op->overlapped, 0, sizeof(OVERLAPPED));
        iocp_op->overlapped.Offset = (DWORD)(fstream->file_block[block_nr]->block_offset);                 // Lower 32 bits
        iocp_op->overlapped.OffsetHigh = (DWORD)(fstream->file_block[block_nr]->block_offset >> 32);       // Upper 32 bits
        iocp_op->io_type = IO_FILE_BLOCK_FLUSH;
        ReleaseSRWLockExclusive(&fstream->lock);

        if (retry_async_write(fstream->iocp_file_handle, fstream->file_block[block_nr]->block_data, fstream->file_block[block_nr]->block_size, &iocp_op->overlapped) == RET_VAL_ERROR) {
            // fprintf(stderr, "CRITICAL ERROR: Terminate file stream (TODO)!\n");
            op_code = ERR_FILE_TRANSFER;
            goto exit;
        }
    } else {
        ReleaseSRWLockExclusive(&fstream->lock);
        op_code = ERR_FILE_TRANSFER;
        goto exit;
    }
    

    // The fragment frames are cumulated to a sack frame before sending
    if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack seq to queue\n");
        return;
    }
    if(push_ptr(queue_client_ptr, (uintptr_t)client) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push client to to client queue\n");
        return;
    };

    return;

exit:

    switch(op_code){
        // case ERR_UNKNOWN_FILE_ID:
        // case ERR_STREAM_ALREADY_FAILED:
        // case ERR_COMPLETED_FILE:
        case ERR_DUPLICATE_FRAME:
            break;

        case ERR_FILE_TRANSFER:
            AcquireSRWLockExclusive(&fstream->lock);
            ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_TRANSFER_ERROR);
            ReleaseSRWLockExclusive(&fstream->lock);
            
            IocpOperation *iocp_op = (IocpOperation*)s_pool_alloc(&sspu->pool_iocp_operation);
            memset(&iocp_op->overlapped, 0, sizeof(OVERLAPPED));
            iocp_op->io_type = IO_FSTREAM_CLOSE;
            PostQueuedCompletionStatus(server->iocp_process_fstream_handle, 0, (uintptr_t)fstream, &iocp_op->overlapped);
            break;

        case ERR_MALFORMED_FRAME:
        case ERR_RESOURCE_LIMIT:
            break;

        default:
            fprintf(stdout, "CRITICAL ERROR: Unknown response code for file fragment\n");
            break;
    }

    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_prio_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file fragment ack error frame\n");
        return;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack error frame to queue\n");
        pool_free(pool_send_prio_udp_frame, entry_send_frame);
        return;
    }
    return;
}
// Process received file end frame
void handle_file_end(ServerClient *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Received invalid client pointer handle_file_end()\n");
        return;
    }
    if(!frame){
        fprintf(stdout, "Received invalid frame pointer handle_file_end()\n");
        return;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);

    // size_t thread_id = hash_stream(recv_session_id, recv_file_id, SERVER_MAX_THREADS_PROCESS_FSTREAM);

    uint8_t op_code = UNDEFINED;

    // uint8_t file_status = ht_status_id(table_file_id, recv_session_id, recv_file_id);

    // switch (file_status) {
    //     case ID_UNKNOWN:
    //     case ID_STATUS_NONE:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for unknown or invalid state file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_UNKNOWN_FILE_ID;
    //         goto exit;

    //     case ID_RECV_COMPLETE:
    //     case ID_WRITE_COMPLETE:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_COMPLETED_FILE;
    //         goto exit;

    //     case ID_TRANSFER_ERROR:
    //         fprintf(stderr, "Received fragment frame Seq: %llu; for failed transfer file fID: %u; sID %u;\n",
    //                 recv_seq_num, recv_file_id, recv_session_id);
    //         op_code = ERR_STREAM_ALREADY_FAILED;
    //         goto exit;
        
    //     case ID_WAITING_FRAGMENTS:
    //         // continue
    //         break;

    //     default:
    //         break;
    // }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received end frame Seq: %llu for unknown stream fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_INVALID_STREAM;
        goto exit;
    }

    AcquireSRWLockExclusive(&fstream->lock);
    for(int i = 0; i < 32; i++){   
        fstream->received_sha256[i] = frame->payload.file_end.file_hash[i];
    }
    ReleaseSRWLockExclusive(&fstream->lock);

    op_code = STS_CONFIRM_FILE_END;
    goto exit;

exit:

    switch(op_code){
        
        case ID_WAITING_FRAGMENTS:
        // case ERR_UNKNOWN_FILE_ID:
        // case ERR_STREAM_ALREADY_FAILED:
        // case ERR_COMPLETED_FILE:
        case ERR_INVALID_STREAM:
        case ID_TRANSFER_ERROR:
            break;
        
        
        case STS_CONFIRM_FILE_END:
            IocpOperation *iocp_op = (IocpOperation*)s_pool_alloc(&sspu->pool_iocp_operation);
            memset(&iocp_op->overlapped, 0, sizeof(OVERLAPPED));
            iocp_op->io_type = IO_FILE_END_FRAME;
            PostQueuedCompletionStatus(server->iocp_process_fstream_handle, 0, (uintptr_t)fstream, &iocp_op->overlapped);
            break;
            
        default:
            fprintf(stdout, "CRITICAL ERROR: Unknown response code for file end\n");
            break;
    }

    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_prio_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file end ack error frame\n");
        return;
    }
    construct_file_end_response_frame(entry_send_frame, recv_seq_num, recv_session_id, recv_file_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_prio_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file end ack error frame to queue\n");
        return;
    }
}

ServerClient *handle_connection_request(PoolEntryRecvFrame *frame_buff){
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    char src_ip[INET_ADDRSTRLEN] = {0};
    uint16_t src_port = 0;

    UdpFrame *frame = &frame_buff->frame;
    struct sockaddr_in *src_addr = &frame_buff->src_addr;

    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint32_t session_id = _ntohl(frame->header.session_id);
    uint8_t server_status = frame->payload.connection_response.server_status;
    uint32_t session_timeout = _ntohl(frame->payload.connection_response.session_timeout);

    inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
    src_port = _ntohs(src_addr->sin_port);

    PoolEntrySendFrame *pool_send_entry = NULL;

    ServerClient *client = NULL;

    if(session_id == DEFAULT_CONNECT_REQUEST_SID && seq_num == DEFAULT_CONNECT_REQUEST_SEQ){
        client = alloc_client(pool_clients);
        if (!client) {
            fprintf(stderr, "Failed to alloc_client() from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
            return NULL;
        }
        init_client(client, InterlockedIncrement(&server->session_id_counter), frame, src_addr);
        AcquireSRWLockExclusive(&client->lock);
        client->last_activity_time = time(NULL);
        ReleaseSRWLockExclusive(&client->lock);

        pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_ctrl_udp_frame);
        if(!pool_send_entry){
            fprintf(stderr, "ERROR: Failed to allocate memory in the pool for connect response frame\n");
            close_client(client);
            return NULL;
        }
        construct_connect_response_frame(pool_send_entry,
                                DEFAULT_CONNECT_REQUEST_SEQ, 
                                client->sid, 
                                server->session_timeout, 
                                server->server_status, 
                                server->name, 
                                server->socket, &client->client_addr);

        if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
            fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
            pool_free(pool_send_ctrl_udp_frame, pool_send_entry);
            close_client(client);
            return NULL;
        }

        fprintf(stdout, "DEBUG: Client %s:%d Requested connection. Responding to connect request with Session ID: %u\n", 
                                                client->ip, client->port, client->sid);

        return client;

    } else if (session_id != DEFAULT_CONNECT_REQUEST_SID && seq_num == DEFAULT_CONNECT_REQUEST_SEQ) {
        // This is a re-connect request from an existing client.
        fprintf(stdout, "DEBUG: Client %s:%d requested re-connection with Session ID: %u\n", 
                                                src_ip, src_port, session_id);
        client = find_client(pool_clients, session_id);
        if(client == NULL){
            fprintf(stderr, "ERROR: Unknown client (invalid DEFAULT_CONNECT_REQUEST_SID)\n");
            return NULL;
        }
        AcquireSRWLockExclusive(&client->lock);
        client->last_activity_time = time(NULL);
        ReleaseSRWLockExclusive(&client->lock);

        pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_ctrl_udp_frame);
        if(!pool_send_entry){
            fprintf(stderr, "ERROR: failed to allocate memory for connect response frame\n");
            close_client(client);
            return NULL;
        }
        construct_connect_response_frame(pool_send_entry,
                                DEFAULT_CONNECT_REQUEST_SEQ, 
                                client->sid, 
                                server->session_timeout, 
                                server->server_status, 
                                server->name, 
                                server->socket, &client->client_addr);

        if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
            fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
            pool_free(pool_send_ctrl_udp_frame, pool_send_entry);
            close_client(client);
            return NULL;
        }

        fprintf(stdout, "DEBUG: Client %s:%d Requested re-connection. Responding to re-connect request with Session ID: %u\n", 
                                                client->ip, client->port, client->sid);
        return client;
    } else {
        fprintf(stdout, "DEBUG: Client %s:%d Invalid connection request\n", 
                                        client->ip, client->port, client->sid);
    }
    return NULL;
}

void handle_keep_alive(ServerClient *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    PoolEntrySendFrame *pool_send_entry = NULL;

    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint32_t session_id = _ntohl(frame->header.session_id);

    AcquireSRWLockExclusive(&client->lock);
    client->last_activity_time = time(NULL);
    ReleaseSRWLockExclusive(&client->lock);

    pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_ctrl_udp_frame);
    if(!pool_send_entry){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for keep_alive ack frame\n");
        return;
    }

    uint32_t file_id = _ntohl(frame->payload.ack.file_id);

    construct_keep_alive_response_frame(pool_send_entry, seq_num, session_id, STS_KEEP_ALIVE, server->socket, &client->client_addr);
    if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
        pool_free(pool_send_ctrl_udp_frame, pool_send_entry);
        fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
    }

    return;
}
void handle_disconnect(ServerClient *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    PoolEntrySendFrame *pool_send_entry = NULL;

    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint32_t session_id = _ntohl(frame->header.session_id);

    fprintf(stdout, "DEBUG: Client %s:%d with session ID: %d requested disconnect...\n", client->ip, client->port, client->sid);
    pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_ctrl_udp_frame);
    if(!pool_send_entry){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for disconnect ack frame\n");
        return;
    }
    construct_ack_frame(pool_send_entry, seq_num, session_id, 0, STS_CONFIRM_DISCONNECT, server->socket, &client->client_addr);
    if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
        pool_free(pool_send_ctrl_udp_frame, pool_send_entry);
        fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
    }
    // TODO: if client is closed immediately then the server can crash since another worker thread will try to access
    // clear client memory in the pool. it is safe to close the client after no more frames are being send (timeout).
    // need to think of a strategy to safely close the client with frame request!
    // One ideea is to send a disconnect response and stop processing frames from this client and 
    // let the timeout disconnect the client naturally (probably need to introduce separate timeout with lower time value)
    AcquireSRWLockExclusive(&client->lock);
    client->connection_status = CLIENT_DISCONNECTED;
    ReleaseSRWLockExclusive(&client->lock);
    // close_client(client);
    return;
}
