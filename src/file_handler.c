
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
#include "include/checksum.h"
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
    pool->free_head = 0;                                        // The first block is the head of the free list
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

    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to find_fstream() in an unallocated pool!\n");
        return NULL;
    }

    ServerFileStream *fstream = NULL;
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        fstream = &pool->fstream[index];
        AcquireSRWLockShared(&fstream->lock);
        if(fstream->fstream_busy && fstream->sid == sid && fstream->fid == fid){
            ReleaseSRWLockShared(&fstream->lock);
            return fstream;
        }
        ReleaseSRWLockShared(&fstream->lock);
    }
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
    AcquireSRWLockExclusive(&pool->fstream[index].lock);
    pool->fstream[index].fstream_busy = TRUE;
    ReleaseSRWLockExclusive(&pool->fstream[index].lock);
    return &pool->fstream[index];
}
static int init_fstream(ServerFileStream *fstream, UdpFrame *frame, const struct sockaddr_in *client_addr) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    AcquireSRWLockExclusive(&fstream->lock);

    // fstream->fstream_busy = TRUE;
    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    fstream->sid = _ntohl(frame->header.session_id);
    fstream->fid = _ntohl(frame->payload.file_metadata.file_id);
    fstream->fsize = _ntohll(frame->payload.file_metadata.file_size);

    // --- Proper string copy and validation for rpath ---
    uint32_t received_rpath_len = _ntohl(frame->payload.file_metadata.rpath_len);

    // Validate the received length against the destination buffer's capacity (MAX_PATH - 1 for content + null)
    if (received_rpath_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_fstream - Received rpath length (%u) is too large for buffer (max %d).\n",
                received_rpath_len, MAX_PATH - 1);
        fstream->fstream_err = STREAM_ERR_MALFORMED_DATA; // Define a new error for this
        fstream->rpath_len = 0;
        fstream->rpath[0] = '\0'; // Ensure it's null-terminated even on error
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
            fstream->fstream_err = STREAM_ERR_INTERNAL_COPY; // Define a new error
            fstream->rpath_len = 0;
            fstream->rpath[0] = '\0';
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
        fstream->fstream_err = STREAM_ERR_MALFORMED_DATA;
        fstream->fname_len = 0;
        fstream->fname[0] = '\0';
        goto exit_err;
    } else {
        int result = snprintf(fstream->fname, sizeof(fstream->fname),
                              "%.*s", (int)received_fname_len, frame->payload.file_metadata.fname);

        if (result < 0 || (size_t)result != received_fname_len) {
            fprintf(stderr, "ERROR: init_fstream - Failed to copy fname: snprintf returned %d, expected %u.\n",
                    result, received_fname_len);
            fstream->fstream_err = STREAM_ERR_INTERNAL_COPY;
            fstream->fname_len = 0;
            fstream->fname[0] = '\0';
            goto exit_err;
        } else {
            fstream->fname_len = received_fname_len;
        }
    }
    // --- End of string copy and validation ---

    fstream->fstream_err = STREAM_ERR_NONE;
    memcpy(&fstream->client_addr, client_addr, sizeof(struct sockaddr_in));

    // Calculate total fragments
    fstream->fragment_count = (fstream->fsize + (uint64_t)FILE_FRAGMENT_SIZE - 1ULL) / (uint64_t)FILE_FRAGMENT_SIZE;
    //fprintf(stdout, "Fragments count: %llu\n", fstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    fstream->bitmap_entries_count = (fstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;
    //fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", fstream->bitmap_entries_count);
 
    // Allocate memory for bitmap
    fstream->bitmap = calloc(fstream->bitmap_entries_count, sizeof(uint64_t));
    if(fstream->bitmap == NULL){
        fstream->fstream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file bitmap mem!!!\n");
        goto exit_err;
    }

    if(!DriveExists(SERVER_PARTITION_DRIVE)){
        fprintf(stderr, "ERROR: init_fstream - Drive Partition \"%s\" doesn't exit\n", SERVER_PARTITION_DRIVE);
        fstream->fstream_err = STREAM_ERR_DRIVE_PARTITION;
        goto exit_err;
    }

    // creating root folder\\session_folder
    char rootFolder[MAX_PATH];
    snprintf(rootFolder, MAX_PATH, "%s%s%d", SERVER_ROOT_FOLDER, SERVER_SID_FOLDER_NAME_FOR_CLIENT, fstream->sid);

    if (CreateAbsoluteFolderRecursive(rootFolder) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\". Error code: %lu\n", rootFolder, GetLastError());
        goto exit_err;
    }

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        goto exit_err;
    }

    snprintf(fstream->iocp_full_path, MAX_PATH, "%s%s%s", rootFolder, fstream->rpath, fstream->fname);

    if(FileExists(fstream->iocp_full_path)){
        fprintf(stderr, "ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->iocp_full_path);
        fstream->fstream_err = STREAM_ERR_FILENAME_EXIST;
        goto exit_err;
    }

    fstream->iocp_file_handle = CreateFile(
        fstream->iocp_full_path,            // File path
        GENERIC_READ | GENERIC_WRITE,       // Access: read + write
        0,                                  // No sharing
        NULL,                               // Default security
        CREATE_ALWAYS,                      // Create or overwrite
        FILE_FLAG_OVERLAPPED,               // Enable async I/O
        NULL                                // No template
    );
    
    if (!CreateIoCompletionPort(fstream->iocp_file_handle, server->iocp_file_handle, (uintptr_t)fstream, 0)) {
        printf("Failed to associate file with IOCP: %lu\n", GetLastError());
        CloseHandle(fstream->iocp_file_handle);
        goto exit_err;
    }
    if (fstream->iocp_file_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to open file \"%s\". Error code: %lu\n", fstream->iocp_full_path, GetLastError());
        fstream->fstream_err = STREAM_ERR_FWRITE;
        goto exit_err;
    }

    ReleaseSRWLockExclusive(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_err:

    if(fstream->fstream_err == STREAM_ERR_FILENAME_EXIST){
        close_fstream(fstream);
        ReleaseSRWLockExclusive(&fstream->lock);
        return ERR_EXISTING_FILE;
    } else {
        close_fstream(fstream);
        ReleaseSRWLockExclusive(&fstream->lock);
        return ERR_STREAM_INIT;
    }
}
void free_fstream(ServerFstreamPool* pool, ServerFileStream* fstream) {
    // Handle NULL pointer case

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
    // uint64_t index = (uint64_t)(fstream - pool->fstream);
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

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    if(!fstream){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer file stream\n");
        return;
    }
    CloseHandle(fstream->iocp_file_handle);
    fstream->iocp_file_handle = NULL;
    fstream->sid = 0;                                   // Session ID associated with this file stream.
    fstream->fid = 0;                                   // File ID, unique identifier for the file associated with this file stream.
    fstream->fsize = 0;                                 // Total size of the file being transferred.
    fstream->file_end_frame_seq_num = 0;
    fstream->fragment_count = 0;                    // Total number of fragments in the entire file.
    fstream->recv_bytes_count = 0;                  // Total bytes received for this file so far.
    fstream->written_bytes_count = 0;               // Total bytes written to disk for this file so far.
    fstream->bitmap_entries_count = 0;              // Number of uint64_t entries in the bitmap array.
    fstream->hashed_chunks_count = 0;
 
    fstream->fstream_err = STREAM_ERR_NONE;         // Stores an error code if something goes wrong with the stream.
    fstream->file_complete = FALSE;                 // True if the entire file has been received and written.
    fstream->file_bytes_received = FALSE;
    fstream->file_bytes_written = FALSE;
    fstream->file_hash_received = FALSE;
    fstream->file_hash_calculated = FALSE;
    fstream->file_hash_validated = FALSE;

    memset(&fstream->received_sha256, 0, 32);
    memset(&fstream->calculated_sha256, 0, 32);
    fstream->rpath_len = 0;
    memset(&fstream->rpath, 0, MAX_PATH);
    fstream->fname_len = 0;
    memset(&fstream->fname, 0, MAX_PATH);
    memset(&fstream->iocp_full_path, 0, MAX_PATH);
    memset(&fstream->client_addr, 0, sizeof(struct sockaddr_in));
    fstream->file_end_frame_seq_num = 0;
    fstream->fstream_busy = FALSE;                  // Indicates if this stream channel is currently in use for a transfer.    
    free_fstream(pool_fstreams, fstream);
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

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(client == NULL){
        fprintf(stdout, "ERROR: Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    AcquireSRWLockShared(&client->lock);
    if(client->slot_status == SLOT_FREE){
        ReleaseSRWLockShared(&client->lock);
        fprintf(stderr, "ERROR: Received file metadata frame for client with slot status SLOT_FREE!\n");
        return RET_VAL_ERROR;
    }
    ReleaseSRWLockShared(&client->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);

    uint8_t op_code = 0;
    PoolEntrySendFrame *entry_send_frame = NULL;

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_WAITING_FRAGMENTS) == TRUE){
        fprintf(stderr, "Received duplicated metadata frame Seq: %llu for fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }
    if(recv_file_size == 0ULL){
        fprintf(stderr, "Received metadata frame Seq: %llu for fID: %u sID %u with zero file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    AcquireSRWLockShared(&pool_fstreams->lock);
    if(pool_fstreams->free_blocks == 0){
        ReleaseSRWLockShared(&pool_fstreams->lock);
        // fprintf(stderr, "All fstreams are busy\n");
        op_code = ERR_RESOURCE_LIMIT;
        goto exit_err; 
    }
    ReleaseSRWLockShared(&pool_fstreams->lock);

    ServerFileStream *fstream = alloc_fstream(pool_fstreams);
    if(!fstream){
        op_code = ERR_RESOURCE_LIMIT;
        goto exit_err;
    }

    op_code = init_fstream(fstream, frame, &client->client_addr);
    if(op_code != RET_VAL_SUCCESS){
        fprintf(stderr, "Error initializing file stream for fID: %u sID: %u\n", recv_file_id, recv_session_id);
        goto exit_err;
    }

    if(ht_insert_id(table_file_id, recv_session_id, recv_file_id, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
        fprintf(stderr, "Failed to allocate memory for file ID in hash table\n");
        op_code = ERR_MEMORY_ALLOCATION;
        goto exit_err;
    }

    entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        op_code = ERR_MEMORY_ALLOCATION;
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for metadata ack frame\n");
        goto exit_err;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, STS_CONFIRM_FILE_METADATA, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        op_code = ERR_MEMORY_ALLOCATION;
        pool_free(pool_send_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file metadata ack frame to queue\n");
        goto exit_err;
    }
    return RET_VAL_SUCCESS;

exit_err:
    entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for metadata ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push file metadata ack error frame to queue\n");
    }
    return RET_VAL_ERROR;
}
// Process received file fragment frame
int handle_file_fragment(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    char buffer[FILE_FRAGMENT_SIZE];

    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    if(client->slot_status == SLOT_FREE){
        fprintf(stderr, "ERROR: Received file metadata frame for client with slot status SLOT_FREE!\n");
        return RET_VAL_ERROR;
    }

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    uint8_t op_code = 0;

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

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

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MISSING_METADATA;
        goto exit_err;
    }

    AcquireSRWLockShared(&fstream->lock);
    if(recv_fragment_offset + recv_fragment_size > fstream->fsize){
        ReleaseSRWLockShared(&fstream->lock);
        fprintf(stderr, "Received fragment frame Seq: %llu; for fID: %u; sID %u with invalid fragment offset + size exceeding max file size\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if(check_fragment_received(fstream->bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        ReleaseSRWLockShared(&fstream->lock);
        fprintf(stderr, "Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }
    ReleaseSRWLockShared(&fstream->lock);

    memcpy(buffer, frame->payload.file_fragment.bytes, recv_fragment_size);

    AcquireSRWLockExclusive(&server->lock_fragment_key);
    server->fragment_key++;
    NodeTableFileChunk *node = ht_insert_fchunk(table_file_chunk, server->fragment_key, OP_WR, buffer, recv_fragment_size);
    ReleaseSRWLockExclusive(&server->lock_fragment_key);

    if(!node){
        fprintf(stderr, "Failed to allocate memory for file chunk in hash table\n");
        op_code = ERR_RESOURCE_LIMIT;
        goto exit_err;
    }

    memset(&node->overlapped, 0, sizeof(OVERLAPPED));
    node->overlapped.Offset = (DWORD)(recv_fragment_offset);                 // Lower 32 bits
    node->overlapped.OffsetHigh = (DWORD)(recv_fragment_offset >> 32);       // Upper 32 bits

    // AcquireSRWLockShared(&fstream->lock);
    BOOL result = WriteFile(
        fstream->iocp_file_handle,
        node->buffer,
        node->buffer_size,
        NULL,
        &node->overlapped
    );
    // ReleaseSRWLockShared(&fstream->lock);
    if (!result) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            fprintf(stderr, "ERROR: Initiating async write: %lu\n", err);
            ht_remove_fchunk(table_file_chunk, server->fragment_key);
            op_code = ERR_INTERNAL_ERROR;
            goto exit_err;
        }
    }

    if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack seq to queue\n");
        return RET_VAL_ERROR;
    }
    if(push_slot(queue_client_slot, client->slot) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push client slot to to slot queue\n");
        return RET_VAL_ERROR;
    };
    return RET_VAL_SUCCESS;

exit_err:
    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file fragment ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push file fragment ack error frame to queue\n");
        pool_free(pool_send_udp_frame, entry_send_frame);
        return RET_VAL_ERROR;
    }
    return RET_VAL_ERROR;
}
// Process received file end frame
int handle_file_end(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    AcquireSRWLockShared(&client->lock);
    if(client->slot_status == SLOT_FREE){
        ReleaseSRWLockShared(&client->lock);
        fprintf(stderr, "ERROR: Received file metadata frame for client with slot status SLOT_FREE!\n");
        return RET_VAL_ERROR;
    }
    ReleaseSRWLockShared(&client->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);

    uint8_t op_code = 0;

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received file end frame for completed file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received end frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MISSING_METADATA;
        goto exit_err;
    }

    AcquireSRWLockExclusive(&fstream->lock);
    for(int i = 0; i < 32; i++){   
        fstream->received_sha256[i] = frame->payload.file_end.file_hash[i];
    }
    fstream->file_end_frame_seq_num = recv_seq_num;
    fstream->file_hash_received = TRUE;
    ReleaseSRWLockExclusive(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_err:
    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for file end ack error frame\n");
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_udp_frame, entry_send_frame);
    }
    return RET_VAL_ERROR;
}

