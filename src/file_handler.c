
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

    if(sid == 0 || fid == 0){
        fprintf(stderr, "ERROR: Invalid sid or fid values to find_fstream() in pool");
        return NULL;
    }

    ServerFileStream *fstream = NULL;
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        fstream = &pool->fstream[index];
        // AcquireSRWLockShared(&fstream->lock);
        if(fstream->fstream_busy && fstream->sid == sid && fstream->fid == fid){
            // ReleaseSRWLockShared(&fstream->lock);
            return fstream;
        }
        // ReleaseSRWLockShared(&fstream->lock);
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
 
    //the file size needs to be a multiple of FILE_FRAGMENT_SIZE and nr of fragments needs to be a multiple of 64 due to bitmap entries being 64 bits
    //otherwise the trailing bitmap entry will not be full of fragments (the mask will not be ~0ULL) and this needs to be treated separately 

    // Determine if there's a trailing chunk
    // A trailing chunk exists if:
    // 1. The total file size is not a perfect multiple of FILE_FRAGMENT_SIZE
    // OR
    // 2. The total number of fragments is not a perfect multiple of FRAGMENTS_PER_CHUNK (64)
    fstream->trailing_chunk = ((fstream->fsize % FILE_FRAGMENT_SIZE) != 0) || (((fstream->fsize / FILE_FRAGMENT_SIZE) % 64ULL) != 0);

    // Initialize trailing chunk status
    fstream->trailing_chunk_complete = FALSE;
    fstream->trailing_chunk_size = 0;

    // Allocate memory for bitmap
    fstream->bitmap = calloc(fstream->bitmap_entries_count, sizeof(uint64_t));
    if(fstream->bitmap == NULL){
        fstream->fstream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file bitmap mem!!!\n");
        goto exit_err;
    }

    // Allocate memory for flags
    fstream->flag = calloc(fstream->bitmap_entries_count, sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->fstream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for file entry flag!!!\n");
        goto exit_err;
    }

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = calloc(fstream->bitmap_entries_count, sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->fstream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "ERROR: init_fstream - Memory allocation fail for chunk mem blocks!!!\n");
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

    // fprintf(stdout,"RECEIVED RPATH: %s", fstream->rpath);

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == FALSE) {
        fprintf(stderr, "ERROR: init_fstream - Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        goto exit_err;
    }

    snprintf(fstream->full_path, MAX_PATH, "%s%s%s", rootFolder, fstream->rpath, fstream->fname);  

    if(FileExists(fstream->full_path)){
        fprintf(stderr, "ERROR: init_fstream - File \"%s\" already exits! Skipping...\n", fstream->full_path);
        fstream->fstream_err = STREAM_ERR_FILENAME_EXIST;
        goto exit_err;
    }
    fstream->fp = fopen(fstream->full_path, "wb+"); // "wb+" allows writing and reading, creates or truncates

    // char fpath[MAX_PATH] = {0};
    // fstream->fp = FopenRename(fstream->fpath, fpath, MAX_PATH, "wb+");
    // if(fstream->fp == NULL){
    //     fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fname, errno);
    //     fstream->fstream_err = STREAM_ERR_FP;
    //     goto exit_err;
    // }
    // fprintf(stdout, "Received file: %s\n", fpath);

    if(push_ptr(queue_process_fstream, (uintptr_t)fstream) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: init_fstream - Failed to push file stream to processing queue\n");
        fstream->fstream_err = STREAM_ERR_QUEUE_PUSH;
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
    // Calculate the index of the block to be freed
    uint64_t index = (uint64_t)(((char*)fstream - (char*)pool->fstream) / sizeof(ServerFileStream));
    // uint64_t index = (uint64_t)(fstream - pool->fstream);
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {       
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid fstream from pool!\n");
        return;
    }
    AcquireSRWLockExclusive(&pool->lock);
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
    if(fstream->fp && fstream->fstream_busy && !fstream->file_complete){
        fclose(fstream->fp);
        remove(fstream->full_path);
        fstream->fp = NULL;
    }
    if(fstream->fp){
        if(fflush(fstream->fp) != 0){
            fprintf(stderr, "Error flushing the file to disk. File is still in use.\n");
        } else {
            int fclosed = fclose(fstream->fp);
            Sleep(1);
            if(fclosed != 0){
                fprintf(stderr, "Error closing the file stream: %s (errno: %d)\n", fstream->full_path, errno);
            }
            fstream->fp = NULL; // Set the file pointer to NULL after closing.
        }
    }
    if(fstream->bitmap){
        free(fstream->bitmap);
        fstream->bitmap = NULL;
    }
    if(fstream->flag){
        free(fstream->flag);
        fstream->flag = NULL;
    }    
    for(long long index = 0; index < fstream->bitmap_entries_count; index++){
        if(fstream->pool_block_file_chunk[index]){
            pool_free(pool_file_chunk, fstream->pool_block_file_chunk[index]);
        }
        fstream->pool_block_file_chunk[index] = NULL;
    }

    fstream->sid = 0;                                   // Session ID associated with this file stream.
    fstream->fid = 0;                                   // File ID, unique identifier for the file associated with this file stream.
    fstream->fsize = 0;                                 // Total size of the file being transferred.
    fstream->trailing_chunk = FALSE;                // True if the last bitmap entry represents a partial chunk (less than 64 fragments).
    fstream->trailing_chunk_complete = FALSE;       // True if all bytes for the last, potentially partial, chunk have been received.
    fstream->trailing_chunk_size = 0;               // The actual size of the last chunk (if partial).
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
    memset(&fstream->full_path, 0, MAX_PATH);
    memset(&fstream->client_addr, 0, sizeof(struct sockaddr_in));
    fstream->file_end_frame_seq_num = 0;
    fstream->fstream_busy = FALSE;                  // Indicates if this stream channel is currently in use for a transfer.    
    free_fstream(pool_fstreams, fstream);
    return;
}
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
static uint8_t attach_fragment_to_chunk(ServerFileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!fstream || !fragment_buffer || fragment_size == 0){
        fprintf(stderr, "ERROR: Invalid parameters in file_attach_fragment_to_chunk()\n");
        return RET_VAL_ERROR; // Error code for invalid parameters
    }
    
    AcquireSRWLockExclusive(&fstream->lock);

    if(fragment_size == 0){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "ERROR: Fragment size is zero in file_attach_fragment_to_chunk()\n");
        return ERR_MALFORMED_FRAME;

    }

    if(fragment_offset >= fstream->fsize){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "Fragment offset past limits - offset: %llu, fragment size: %u, file size: %llu\n",
                                        fragment_offset, fragment_size, fstream->fsize);
        return ERR_MALFORMED_FRAME;
    }

    if (fragment_offset + fragment_size > fstream->fsize){
        ReleaseSRWLockExclusive(&fstream->lock);
        fprintf(stderr, "Fragment extends past file bounds - offset: %llu, fragment size: %u, file size: %llu\n",
                                        fragment_offset, fragment_size, fstream->fsize);
        return ERR_MALFORMED_FRAME;
    }

    // Calculate the index of the 64-bit bitmap entry (which corresponds to a "chunk" of 64 fragments)
    // to which this incoming fragment belongs.
    // fragment_offset / FILE_FRAGMENT_SIZE gives the fragment number.
    // Dividing by 64ULL (FRAGMENTS_PER_CHUNK) then gives the chunk index.
    uint64_t index = (fragment_offset / FILE_FRAGMENT_SIZE) / FRAGMENTS_PER_CHUNK;

    if(fstream->bitmap[index] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        fstream->pool_block_file_chunk[index] = pool_alloc(pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(!fstream->pool_block_file_chunk[index]){
            ReleaseSRWLockExclusive(&fstream->lock);
            fprintf(stderr, "ERROR: Failed to allocate memory chunk for file stream at index %llu\n", index);
            return ERR_RESOURCE_LIMIT;
        }
    }

    // Calculate the destination address within the target memory chunk where the fragment data will be copied.
    // fstream->pool_block_file_chunk[fstream_index] gives the base address of the chunk.
    // (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)) calculates the offset *within that chunk*.
    // This accounts for multiple fragments being part of one chunk.
    char *dest = fstream->pool_block_file_chunk[index] + (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK));
    char *src = fragment_buffer; // The source buffer containing the incoming fragment data.

    // Copy the fragment data from the source buffer to its calculated destination within the chunk's memory block.
    memcpy(dest, src, fragment_size);

    // Mark the flag for this chunk as CHUNK_BODY.
    // This assumes that by default, chunks are considered 'body' chunks unless specifically identified as 'trailing'.

    fstream->flag[index] = CHUNK_BODY;
    // fprintf(stdout, "Attaching fragment with offset: %llu to chunk: %llu at offset: %llu\n", fragment_offset, fstream_index, (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)));
    // Increment the total number of bytes received for this file stream.
    fstream->recv_bytes_count += fragment_size;

    // Check if this file stream is expected to have a trailing (potentially partial) chunk.
    if(fstream->trailing_chunk == TRUE){
        // Special handling for the last chunk, which might be the trailing chunk.
        // Check if the current fragment belongs to the last bitmap entry (chunk).
        if(index == fstream->bitmap_entries_count - 1ULL){
            // If it's the last chunk, accumulate the size of the received fragments for this chunk.
            fstream->trailing_chunk_size += fragment_size;
            // Mark the flag for this last chunk specifically as CHUNK_TRAILING.
            // This flag differentiates it from regular CHUNK_BODY entries for writing logic.
            fstream->flag[index] = CHUNK_TRAILING;
        }
        // Check if all expected bytes for the entire file have been received.
        if(fstream->recv_bytes_count == fstream->fsize){
            fstream->trailing_chunk_complete = TRUE;
            // fprintf(stdout, "Received complete file: %s, Size: %llu, Last chunk size: %llu\n", fstream->fname, fstream->recv_bytes_count, fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    ReleaseSRWLockExclusive(&fstream->lock);
    return RET_VAL_SUCCESS;

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
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    uint8_t op_code = 0;

    if(ht_search_id(table_file_id, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    // ServerFileStream *fstream = search_file_stream(recv_session_id, recv_file_id);
    ServerFileStream *fstream = find_fstream(pool_fstreams, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MISSING_METADATA;
        goto exit_err;
    }

    if(check_fragment_received(fstream->bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        fprintf(stderr, "Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    op_code = attach_fragment_to_chunk(fstream, frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size);
    if(op_code != RET_VAL_SUCCESS){
        fprintf(stderr, "ERROR: Failed to attach file fragment to chunk for sID: %u; fID: %u;\n", recv_session_id, recv_file_id);
        goto exit_err;
    }

    AcquireSRWLockShared(&client->lock);
    if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
        ReleaseSRWLockShared(&client->lock);
        fprintf(stderr, "ERROR: Failed to push file fragment ack seq to queue\n");
        return RET_VAL_ERROR;
    }
    if(push_slot(queue_client_slot, client->slot) == RET_VAL_ERROR){
        ReleaseSRWLockShared(&client->lock);
        fprintf(stderr, "ERROR: Failed to push client slot to to slot queue\n");
        return RET_VAL_ERROR;
    };
    ReleaseSRWLockShared(&client->lock);
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

