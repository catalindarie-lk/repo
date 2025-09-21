
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "include/message_handler.h"
#include "include/protocol_frames.h"
#include "include/resources.h"
#include "include/server.h"
#include "include/netendians.h"
#include "include/queue.h"
#include "include/hash.h"
#include "include/bitmap.h"
#include "include/crc32.h"
#include "include/mem_pool.h"
#include "include/fileio.h"


void init_mstream_pool(ServerMstreamPool* pool, const uint64_t block_count) {

    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in init_mstream_pool().\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in init_mstream_pool().\n");
        _aligned_free(pool->next);
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)
    
    // Allocate the main memory buffer for the pool
    pool->mstream = (ServerMessageStream*)_aligned_malloc(sizeof(ServerMessageStream) * pool->block_count, 64);
    if (!pool->mstream) {
        fprintf(stderr, "Memory allocation failed for mstream in init_mstream_pool().\n");
        _aligned_free(pool->next);
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->mstream, 0, sizeof(ServerMessageStream) * pool->block_count);
    
    // Initialize the free list: all blocks are initially free
    pool->free_head = 0;                                        // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t index = 0; index < pool->block_count - 1; index++) {
        pool->next[index] = index + 1;                          // Link to the next block
        pool->used[index] = FREE_BLOCK;                         // Mark as unused
        InitializeSRWLock(&pool->mstream[index].lock);
    }
    // The last block points to END_BLOCK, indicating the end of the free list
    pool->next[pool->block_count - 1] = END_BLOCK;              // Use END_BLOCK to indicate end of list
    pool->used[pool->block_count - 1] = FREE_BLOCK;             // Last block is also unused
    InitializeSRWLock(&pool->mstream[pool->block_count - 1].lock);
    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
ServerMessageStream* find_mstream(ServerMstreamPool* pool, const uint32_t sid, const uint32_t mid) {

    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to find_mstream() in an unallocated pool!\n");
        return NULL;
    }

    if(sid == 0 || mid == 0){
        fprintf(stderr, "ERROR: Invalid sid or fid values to find_mstream() in pool");
        return NULL;
    }

    ServerMessageStream *mstream = NULL;
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        mstream = &pool->mstream[index];
        // AcquireSRWLockShared(&mstream->lock);
        if(mstream->busy && mstream->sid == sid && mstream->mid == mid){
            // ReleaseSRWLockShared(&mstream->lock);
            return mstream;
        }
        // ReleaseSRWLockShared(&mstream->lock);
    }
    return NULL;
}
ServerMessageStream* alloc_mstream(ServerMstreamPool* pool) {
    // Enter critical section to protect shared pool data
    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to alloc_mstream() in an unallocated pool!\n");
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
    AcquireSRWLockExclusive(&pool->mstream[index].lock);
    pool->mstream[index].busy = TRUE;
    ReleaseSRWLockExclusive(&pool->mstream[index].lock);
    return &pool->mstream[index];
}
static uint8_t init_mstream(ServerMessageStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len) {

    AcquireSRWLockExclusive(&mstream->lock);

    mstream->sid = session_id;
    mstream->mid = message_id;
    mstream->mlen = message_len;

    // Calculate total fragments
    mstream->fragment_count = ((mstream->mlen - 1) / TEXT_FRAGMENT_SIZE) + 1;

    // Calculate number of 64-bit bitmap entries (chunks)
    mstream->bitmap_entries_count = ((mstream->fragment_count - 1ULL) / 64ULL) + 1;  
    //fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", mstream->bitmap_entries_count);

    mstream->bitmap = malloc(mstream->bitmap_entries_count * sizeof(uint64_t));
    if(mstream->bitmap == NULL){
        fprintf(stderr, "ERROR: Memory allocation fail for file bitmap!!!\n");
        goto exit_err;
    }
    memset(mstream->bitmap, 0, mstream->bitmap_entries_count * sizeof(uint64_t));
    
    //copy the received fragment text to the buffer            
    mstream->mid = message_id;
    mstream->buffer = malloc(message_len + 1);
    if(mstream->buffer == NULL){
        fprintf(stderr, "ERROR: Failed to allocate memory for message buffer - malloc(message_len + 1)\n");
        goto exit_err;
    }
    memset(mstream->buffer, 0, message_len);

    char messageFolder[MAX_PATH];
    snprintf(messageFolder, MAX_PATH, "%s", SERVER_MESSAGE_TEXT_FILES_FOLDER);

    if (CreateAbsoluteFolderRecursive(messageFolder) == FALSE) {
        fprintf(stderr, "ERROR: Failed to create recursive path for message folder: \"%s\". Error code: %lu\n", messageFolder, GetLastError());
        goto exit_err;
    }

    // Constructs a filename for storing the received message, incorporating session and message IDs for uniqueness.
    snprintf(mstream->fname, MAX_PATH, SERVER_MESSAGE_TEXT_FILES_FOLDER"xmessage_SID_%d_ID%d.txt", session_id, message_id);
    fprintf(stdout, "Nr of Bitmaps: %llu; Fragments: %llu\n", mstream->bitmap_entries_count, mstream->fragment_count);

    ReleaseSRWLockExclusive(&mstream->lock);
    return RET_VAL_SUCCESS;

exit_err:
    close_mstream(mstream);
    ReleaseSRWLockExclusive(&mstream->lock);
    return ERR_STREAM_INIT;
}
void free_mstream(ServerMstreamPool* pool, ServerMessageStream* mstream) {
    // Handle NULL pointer case

    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to free_mstream() in an unallocated pool!\n");
        return;
    }
    if (!mstream) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in mstream pool!\n");
        return;
    }
    // Calculate the index of the block to be freed
    uint64_t index = (uint64_t)(((char*)mstream - (char*)pool->mstream) / sizeof(ServerMessageStream));
    // uint64_t index = (uint64_t)(mstream - pool->mstream);
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {       
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid mstream from pool!\n");
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
void close_mstream(ServerMessageStream *mstream) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(mstream == NULL){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer message stream\n");
        return;
    }
    if(mstream->buffer){
        free(mstream->buffer);
        mstream->buffer = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    if(mstream->bitmap != NULL){
        free(mstream->bitmap); // Free the memory allocated for the bitmap.
        mstream->bitmap = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    mstream->busy = FALSE; // Reset the busy flag.
    mstream->stream_err = 0; // Reset error status.
    mstream->sid = 0; // Reset session ID.
    mstream->mid = 0; // Reset message ID.
    mstream->mlen = 0; // Reset message length.
    mstream->fragment_count = 0; // Reset fragment count.
    mstream->chars_received = 0; // Reset characters received counter.
    mstream->bitmap_entries_count = 0; // Reset bitmap entries count.
    memset(mstream->fname, 0, MAX_NAME_SIZE); // Clear the file name buffer by filling it with zeros.
    free_mstream(pool_mstreams, mstream);
    return;

}
void destroy_mstream_pool(ServerMstreamPool* pool) {
    // Check for NULL pool pointer
    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to destroy_mstream_pool() on an unallocated pool!\n");
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
    if (pool->mstream) {
        _aligned_free(pool->mstream);
        pool->mstream = NULL;
    }
    pool->free_blocks = 0;
}

static uint8_t validate_fragment(ServerMessageStream *mstream, UdpFrame *frame) {

    // PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    AcquireSRWLockExclusive(&mstream->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_message_id = _ntohl(frame->payload.text_fragment.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.text_fragment.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.text_fragment.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.text_fragment.fragment_offset);

    BOOL is_duplicate_fragment = mstream->bitmap && mstream->buffer &&
                                    check_fragment_received(mstream->bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);

    if (is_duplicate_fragment == TRUE) {
        fprintf(stderr, "ERROR: Received duplicate text message fragment - Session ID: %d, Message ID: %d, Offset: %d,\n", mstream->sid, recv_message_id, recv_fragment_offset);
        return ERR_DUPLICATE_FRAME;

    }
    if(recv_fragment_offset >= recv_message_len){
        fprintf(stderr, "ERROR: Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", mstream->sid, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return ERR_MALFORMED_FRAME;
    }
    if ((recv_fragment_offset + recv_fragment_len) > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        fprintf(stderr, "ERROR: Fragment len past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", mstream->sid, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return ERR_MALFORMED_FRAME;
    }

    ReleaseSRWLockExclusive(&mstream->lock);
    return RET_VAL_SUCCESS;
}
static void attach_fragment(ServerMessageStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len) {
    AcquireSRWLockExclusive(&mstream->lock);
    char *dest = mstream->buffer + fragment_offset;
    char *src = fragment_buffer;                                              
    memcpy(dest, src, fragment_len);
    mstream->chars_received += fragment_len;
    mark_fragment_received(mstream->bitmap, fragment_offset, TEXT_FRAGMENT_SIZE);
    ReleaseSRWLockExclusive(&mstream->lock);
    return;
}
static uint8_t check_completion_and_record(ServerMessageStream *mstream) {
    // Check if the message is fully received by verifying total bytes and the fragment bitmap.
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    AcquireSRWLockExclusive(&mstream->lock);

    BOOL message_is_complete = (mstream->chars_received == mstream->mlen) && check_bitmap(mstream->bitmap, mstream->fragment_count);

    if (message_is_complete == FALSE) {
        // The message is not yet complete. No action needed for now.
        ReleaseSRWLockExclusive(&mstream->lock);
        return RET_VAL_SUCCESS;
    }

    // --- Null terminate the message ---
    mstream->buffer[mstream->mlen] = '\0';
    // Attempt to write the in-memory buffer to a file on disk.
    int msg_creation_status = create_output_file(mstream->buffer, mstream->mlen, mstream->fname);
        
    if (msg_creation_status != RET_VAL_SUCCESS) {
        // If file creation failed, return an error.
        fprintf(stderr, "ERROR: Failed to create output message for file_id %d\n", mstream->mid);
        remove(mstream->fname);
        ReleaseSRWLockExclusive(&mstream->lock);
        return ERR_MEMORY_ALLOCATION;
    }

    ht_update_id_status(table_message_id, mstream->sid, mstream->mid, ID_RECV_COMPLETE);
    
    close_mstream(mstream);
 
    // File was successfully created and saved.
    ReleaseSRWLockExclusive(&mstream->lock);
    return RET_VAL_SUCCESS;
}

// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    // int slot;
    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }
    
    AcquireSRWLockExclusive(&client->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_message_id = _ntohl(frame->payload.text_fragment.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.text_fragment.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.text_fragment.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.text_fragment.fragment_offset);

    uint8_t op_code = 0;

    if(ht_search_id(table_message_id, recv_session_id, recv_message_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received message frame for completed message Seq: %llu; sID: %u; mID: %u;\n", recv_seq_num, recv_session_id, recv_message_id);
        op_code = ERR_EXISTING_MESSAGE;
        goto exit_err;
    }

    ServerMessageStream *mstream = find_mstream(pool_mstreams, recv_session_id, recv_message_id);
    if(mstream){

        op_code = validate_fragment(mstream, frame);
        if (op_code != RET_VAL_SUCCESS) {
            goto exit_err;
        }
        
        attach_fragment(mstream, frame->payload.text_fragment.chars, recv_fragment_offset, recv_fragment_len);
        if (check_completion_and_record(mstream) == RET_VAL_ERROR){
            fprintf(stderr, "Final check of the message failed\n");
            op_code = ERR_MESSAGE_FINAL_CHECK;
            goto exit_err;
        }


        // entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_frame);
        // if(!entry){
        //     fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment ack frame\n");
        //     ReleaseSRWLockExclusive(&client->lock);
        //     return RET_VAL_ERROR;
        // }
        // construct_ack_frame(entry, recv_seq_num, recv_session_id, STS_CONFIRM_MESSAGE_FRAGMENT, server->socket, &client->client_addr);
        // if(push_ptr(queue_message_ack_frame, (uintptr_t)entry) == RET_VAL_ERROR){
        //     fprintf(stderr, "ERROR: Failed to push to queue message ack.\n");
        //     pool_free(pool_queue_ack_frame, entry);
        // }
        // ReleaseSRWLockExclusive(&client->lock);
        
        
        // AcquireSRWLockShared(&client->lock);
        if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
            ReleaseSRWLockExclusive(&client->lock);
            fprintf(stderr, "ERROR: Failed to push message fragment ack seq to queue\n");
            return RET_VAL_ERROR;
        }
        if(push_slot(queue_client_slot, client->slot) == RET_VAL_ERROR){
            ReleaseSRWLockExclusive(&client->lock);
            fprintf(stderr, "ERROR: Failed to push client slot to to slot queue\n");
            return RET_VAL_ERROR;
        };
        ReleaseSRWLockExclusive(&client->lock);
        


        
        return RET_VAL_SUCCESS;
        
    } else {

        ServerMessageStream *mstream = alloc_mstream(pool_mstreams);
        if(!mstream){
            fprintf(stderr, "Maximum message streams reached for client ID: %d\n", client->cid);
            op_code = ERR_RESOURCE_LIMIT;
            goto exit_err; 
        }

        op_code = validate_fragment(mstream, frame);
        if (op_code != RET_VAL_SUCCESS) {
            goto exit_err;
        }

        op_code = init_mstream(mstream, recv_session_id, recv_message_id, recv_message_len);
        if (op_code != RET_VAL_SUCCESS){
            goto exit_err;
        }

        attach_fragment(mstream, frame->payload.text_fragment.chars, recv_fragment_offset, recv_fragment_len);       

        if(ht_insert_id(table_message_id, recv_session_id, recv_message_id, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
            fprintf(stderr, "Failed to allocate memory for message ID in hash table\n");
            op_code = ERR_MEMORY_ALLOCATION;
            goto exit_err;
        }

        op_code = check_completion_and_record(mstream);
        if (op_code != RET_VAL_SUCCESS){
            goto exit_err;
        }

        // entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_frame);
        // if(!entry){
        //     fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment ack frame\n");
        //     ReleaseSRWLockExclusive(&client->lock);
        //     return RET_VAL_ERROR;
        // }
        // construct_ack_frame(entry, recv_seq_num, recv_session_id, STS_CONFIRM_MESSAGE_FRAGMENT, server->socket, &client->client_addr);
        // if(push_ptr(queue_message_ack_frame, (uintptr_t)entry) == RET_VAL_ERROR){
        //     fprintf(stderr, "ERROR: Failed to push message ack to queue.\n");
        //     pool_free(pool_queue_ack_frame, entry);
        // }
        // ReleaseSRWLockExclusive(&client->lock);


        // AcquireSRWLockShared(&client->lock);
        if(push_seq(&client->queue_ack_seq, frame->header.seq_num) == RET_VAL_ERROR){
            ReleaseSRWLockExclusive(&client->lock);
            fprintf(stderr, "ERROR: Failed to push message fragment ack seq to queue\n");
            return RET_VAL_ERROR;
        }
        if(push_slot(queue_client_slot, client->slot) == RET_VAL_ERROR){
            ReleaseSRWLockExclusive(&client->lock);
            fprintf(stderr, "ERROR: Failed to push client slot to to slot queue\n");
            return RET_VAL_ERROR;
        };
        ReleaseSRWLockExclusive(&client->lock);




        return RET_VAL_SUCCESS;
    }
exit_err:
    PoolEntrySendFrame *entry_send_frame = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
    if(!entry_send_frame){
        ReleaseSRWLockExclusive(&client->lock);
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment error ack frame\n");
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry_send_frame, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send_frame) == RET_VAL_ERROR){
        pool_free(pool_send_udp_frame, entry_send_frame);
        fprintf(stderr, "ERROR: Failed to push message ack error to queue priority.\n");
    }

    ReleaseSRWLockExclusive(&client->lock);
    return RET_VAL_ERROR;
}

