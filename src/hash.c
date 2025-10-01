//  // This file contains the implementation of the hash table for frames and unique identifiers.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "include/protocol_frames.h"
#include "include/resources.h"
#include "include/netendians.h"
#include "include/mem_pool.h"
#include "include/hash.h"

//--------------------------------------------------------------------------------------------------------------------------
void init_table_id(TableIDs *table, size_t size, const size_t max_nodes){
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for init_table_id()\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'size' for init_table_id()\n");
        return;
    }
    if(max_nodes < size){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'max_nodes' for init_table_id()\n");
        return;
    }
    table->size = size;
    table->bucket = (NodeTableIDs **)_aligned_malloc(sizeof(NodeTableIDs*) * size, 64);
    if(!table->bucket){
        fprintf(stderr, "CRITICAL ERROR: Unable to allocate memory for buckets init_table_id()\n");
        return;
    }   
    memset(table->bucket, 0, sizeof(NodeTableIDs*) * size);

    init_pool(&table->pool, sizeof(NodeTableIDs), max_nodes);
    InitializeSRWLock(&table->mutex); 
    table->count = 0;
}
size_t ht_get_hash_id(uint32_t id, const size_t size) {
    return ((size_t)id % size);
}
int ht_insert_id(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status) {
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_insert_id()\n");
        return RET_VAL_ERROR;
    }
    size_t index = ht_get_hash_id(id, table->size);
    AcquireSRWLockExclusive(&table->mutex);
    NodeTableIDs *node = (NodeTableIDs*)pool_alloc(&table->pool);
    if(!node){
        ReleaseSRWLockExclusive(&table->mutex);
        fprintf(stderr, "CRITICAL ERROR: fail to allocate memory for ht_insert_id()\n");
        return RET_VAL_ERROR;
    }
    node->sid = sid;
    node->id = id;    
    node->status = status;
    node->next = (NodeTableIDs *)table->bucket[index];  // Insert at the head (linked list)
    table->bucket[index] = node;
    table->count++;
    ReleaseSRWLockExclusive(&table->mutex);
    return RET_VAL_SUCCESS;
}
int ht_remove_id(TableIDs *table, const uint32_t sid, const uint32_t id) {
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_remove_id()\n");
        return RET_VAL_ERROR;
    }   
    size_t index = ht_get_hash_id(id, table->size);
    AcquireSRWLockExclusive(&table->mutex);   
    NodeTableIDs *curr = table->bucket[index];
    NodeTableIDs *prev = NULL;
    while (curr) {     
        if (curr->id == id && curr->sid == sid) {
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                table->bucket[index] = curr->next;
            }
            table->count--;
            pool_free(&table->pool, (void*)curr);
            ReleaseSRWLockExclusive(&table->mutex);
            return RET_VAL_SUCCESS;
        }
        prev = curr;
        curr = curr->next;
    }
    ReleaseSRWLockExclusive(&table->mutex);
    fprintf(stderr, "DEBUG: Node not found in table sid: %u, fid: %u\n", sid, id);
    return RET_VAL_ERROR;
}
void ht_remove_all_sid(TableIDs *table, const uint32_t sid) {
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_remove_all_sid()\n");
        return;
    }
    AcquireSRWLockExclusive(&table->mutex);
    for (size_t i = 0; i < table->size; ++i) {
        NodeTableIDs *curr = table->bucket[i];
        NodeTableIDs *prev = NULL;
        while (curr) {
            if (curr->sid == sid) {
                NodeTableIDs *to_remove = curr;
                if (prev) {
                    prev->next = curr->next;
                } else {
                    table->bucket[i] = curr->next;
                }
                curr = curr->next;
                pool_free(&table->pool, (void*)to_remove);
                table->count--;
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
    }
    ReleaseSRWLockExclusive(&table->mutex);
}
BOOL ht_search_id(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status) {
    
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_search_id()\n");
        return FALSE;
    }
    size_t index = ht_get_hash_id(id, table->size);
    AcquireSRWLockShared(&table->mutex);
    NodeTableIDs *curr = table->bucket[index];
    while (curr) {
        if (curr->sid == sid && curr->id == id && curr->status == status){
            ReleaseSRWLockShared(&table->mutex);
            return TRUE;
        }
        curr = curr->next;
    }
    ReleaseSRWLockShared(&table->mutex);
    return FALSE;
}
uint8_t ht_status_id(TableIDs *table, const uint32_t sid, const uint32_t id) {
    
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_status_id()\n");
        return ID_UNKNOWN;
    }
    size_t index = ht_get_hash_id(id, table->size);
    AcquireSRWLockShared(&table->mutex);
    NodeTableIDs *curr = table->bucket[index];
    while (curr) {
        if (curr->sid == sid && curr->id == id){
            ReleaseSRWLockShared(&table->mutex);
            return curr->status;
        }
        curr = curr->next;
    }
    ReleaseSRWLockShared(&table->mutex);
    return ID_UNKNOWN;
}
int ht_update_id_status(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status) {
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_update_id_status()\n");
        return RET_VAL_ERROR;
    } 
    size_t index = ht_get_hash_id(id, table->size);
    AcquireSRWLockExclusive(&table->mutex); 
    NodeTableIDs *curr = table->bucket[index];
    while (curr) {
        if (curr->id == id && curr->sid == sid){
            curr->status = status;
            ReleaseSRWLockExclusive(&table->mutex);
            return RET_VAL_SUCCESS;
        }           
        curr = curr->next;
    }
    ReleaseSRWLockExclusive(&table->mutex);
    return RET_VAL_ERROR;
}
void ht_clean_id(TableIDs *table) {
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_clean_id()\n");
        return;
    }
    AcquireSRWLockExclusive(&table->mutex);
    for (size_t index = 0; index < table->size; index++) {
        NodeTableIDs *curr = table->bucket[index];
        while (curr) {
            NodeTableIDs *next = curr->next;
            pool_free(&table->pool, (void*)curr);
            table->count--;
            curr = next;
        }
        table->bucket[index] = NULL;
    }
    ReleaseSRWLockExclusive(&table->mutex);
}

//--------------------------------------------------------------------------------------------------------------------------
void init_table_send_frame(TableSendFrame *table, const size_t size, const size_t max_nodes){
    
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for init_table_send_frame()\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'size' for init_table_send_frame()\n");
        return;
    }
    if(max_nodes < size){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'max_nodes' for init_table_send_frame()\n");
        return;
    }
    table->size = size;
    table->bucket = (TableNodeSendFrame **)_aligned_malloc(sizeof(TableNodeSendFrame*) * size, 64);
    if(!table->bucket){
        fprintf(stderr, "CRITICAL ERROR: Unable to allocate memory for table buckets\n");
        return;
    }
    memset(table->bucket, 0, sizeof(TableNodeSendFrame*) * size);

    init_pool(&table->pool, sizeof(TableNodeSendFrame), max_nodes);
    InitializeSRWLock(&table->mutex);
    table->count = 0;
    return;
}
size_t get_hash_table_send_frame(const uint64_t seq_num, const size_t size){
    return ((size_t)seq_num % size);
}
int insert_table_send_frame(TableSendFrame *table, const uintptr_t frame){

    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for insert_table_send_frame()\n");
        return RET_VAL_ERROR;
    }
    if(!frame){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'frame' pointer for insert_table_send_frame()\n");
        return RET_VAL_ERROR;
    }
    
    PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)frame;
    uint64_t seq_num = _ntohll(pool_entry->frame.header.seq_num);
    
    size_t index = get_hash_table_send_frame(seq_num, table->size);
    AcquireSRWLockExclusive(&table->mutex);   
    TableNodeSendFrame *node = (TableNodeSendFrame*)pool_alloc(&table->pool);
    if(node == NULL){
        ReleaseSRWLockExclusive(&table->mutex);
        fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for insert_table_send_frame()\n");
        return RET_VAL_ERROR;
    }
    node->frame = frame;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    node->timestamp = FILETIME_TO_UINT64(ft);
    node->sent_count = 1;

    node->next = (TableNodeSendFrame*)table->bucket[index];  // Insert at the head (linked list)
    table->bucket[index] = node;
    table->count++;
    ReleaseSRWLockExclusive(&table->mutex);
    return RET_VAL_SUCCESS;
}
uintptr_t remove_table_send_frame(TableSendFrame *table, const uint64_t seq_num){

    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for remove_table_send_frame()\n");
        return (uintptr_t)0;
    }
    
    size_t index = get_hash_table_send_frame(seq_num, table->size);
    AcquireSRWLockExclusive(&table->mutex);
    TableNodeSendFrame *curr = table->bucket[index];
    TableNodeSendFrame *prev = NULL;
    while (curr) {
        
        PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)curr->frame;
        uint64_t frame_seq_num = _ntohll(pool_entry->frame.header.seq_num);

        if (frame_seq_num == seq_num) {
            // fprintf(stdout, "DEBUG: Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                table->bucket[index] = curr->next;
            }
            pool_free(&table->pool, (void*)curr);
            table->count--;
            //fprintf(stdout, "Hash count: %d\n", *count);
            ReleaseSRWLockExclusive(&table->mutex);
            return (uintptr_t)pool_entry;
        }
        prev = curr;
        curr = curr->next;
    }
    ReleaseSRWLockExclusive(&table->mutex);
    return (uintptr_t)0;
 
}
uintptr_t search_table_send_frame(TableSendFrame *table, const uint64_t seq_num){

    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for search_table_send_frame()\n");
        return (uintptr_t)0;
    }
    
    size_t index = get_hash_table_send_frame(seq_num, table->size);
    AcquireSRWLockShared(&table->mutex);
    TableNodeSendFrame *curr = table->bucket[index];
    TableNodeSendFrame *prev = NULL;
    while (curr) {
        PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)curr->frame;
        uint64_t frame_seq_num = _ntohll(pool_entry->frame.header.seq_num);
        if (frame_seq_num == seq_num) {
            ReleaseSRWLockShared(&table->mutex);
            // fprintf(stdout, "DEBUG: Found frame node in hash table with seq_num: %llu at index: %llu\n", seq_num, index);
            return (uintptr_t)pool_entry;
        }
        prev = curr;
        curr = curr->next;
    }
    ReleaseSRWLockShared(&table->mutex);
    // fprintf(stdout, "DEBUG: Node frame not found in hash table with seq_num: %llu\n", seq_num);
    return (uintptr_t)0;
 
}
void check_timeout_table_send_frame(TableSendFrame *table, MemPool *pool){
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for check_timeout_table_send_frame()\n");
        return;
    }
    if(table->count == 0){
        return;
    }

    AcquireSRWLockExclusive(&table->mutex);
    for (size_t index = 0; index < table->size; index++) {
        TableNodeSendFrame *table_node = table->bucket[index];
        while (table_node) {
            PoolEntrySendFrame *frame = (PoolEntrySendFrame*)table_node->frame;
            if(!frame){
                fprintf(stderr, "CRITICAL ERROR: table node with frame pointer NULL!!! - should never happen\n");
                table_node = table_node->next;
                continue;
            }

            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            uint64_t current_time = FILETIME_TO_UINT64(ft);
            uint64_t elapsed_time = current_time - table_node->timestamp;

            if(frame->frame.header.frame_type == FRAME_TYPE_FILE_METADATA && elapsed_time > RESEND_FILE_METADATA_TIMEOUT){
                send_pool_frame(frame, pool);
                table_node->timestamp = current_time;
                table_node->sent_count++;
            } else if (elapsed_time > RESEND_FILE_FRAGMENT_TIMEOUT){
                send_pool_frame(frame, pool);
                // fprintf(stdout, "DEBUG: re-sending frame seq: %llu, elapsed_time: %llu\n", _ntohll(frame->frame.header.seq_num), elapsed_time);
                table_node->timestamp = current_time;
                table_node->sent_count++;
            }
            table_node = table_node->next;
        }
    }
    ReleaseSRWLockExclusive(&table->mutex);
    return;
}
void clean_table_send_frame(TableSendFrame *table) {
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for clean_table_send_frame()\n");
        return;
    }
    AcquireSRWLockExclusive(&table->mutex);
    for (size_t index = 0; index < table->size; index++) {
        TableNodeSendFrame *curr = table->bucket[index];
        while (curr) {
            TableNodeSendFrame *next = curr->next;
            pool_free(&table->pool, (void*)curr);
            table->count--;
            curr = next;
        }
        table->bucket[index] = NULL;
    }
    ReleaseSRWLockExclusive(&table->mutex);
}

//--------------------------------------------------------------------------------------------------------------------------
void init_table_fblock(TableFileBlock *table, size_t size, const size_t max_nodes) {
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for init_table_fblock()\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'size' for init_table_fblock()\n");
        return;
    }
    if(max_nodes < size){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'max_nodes' for init_table_fblock()\n");
        return;
    }
    table->size = size;
    table->bucket = (NodeTableFileBlock **)_aligned_malloc(sizeof(NodeTableFileBlock*) * size, 64);
    if(!table->bucket){
        fprintf(stderr, "CRITICAL ERROR: Unable to allocate memory for buckets init_table_fblock()\n");
        return;
    }   
    memset(table->bucket, 0, sizeof(NodeTableFileBlock*) * size);
    init_pool(&table->pool, sizeof(NodeTableFileBlock), max_nodes);
    InitializeSRWLock(&table->mutex); 
    table->count = 0;
}
size_t ht_get_hash_fblock(const uint64_t key, const size_t size) {
    return ((size_t)key % size);
}
NodeTableFileBlock *ht_insert_fblock(TableFileBlock *table, const uint64_t key, const uint8_t op_type ,const uint32_t sid, const uint32_t fid, char* block_data, size_t block_size) {

    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_insert_fblock()\n");
        return NULL;
    }
    // if(!block_data){
    //     fprintf(stderr, "CRITICAL ERROR: Invalid memory pool block pointer for ht_insert_fblock()\n");
    //     return NULL;
    // }

    size_t index = ht_get_hash_fblock(key, table->size);

    AcquireSRWLockExclusive(&table->mutex);
    NodeTableFileBlock *table_node = (NodeTableFileBlock*)pool_alloc(&table->pool);

    if(!table_node){
        ReleaseSRWLockExclusive(&table->mutex);
        fprintf(stderr, "CRITICAL ERROR: failed to allocate memory for file table node\n");
        return NULL;
    }

    table_node->key = key;
    table_node->sid = sid;
    table_node->fid = fid;
    table_node->block_data = block_data;
    table_node->block_size = block_size;
    table_node->op_type = op_type;
    table_node->next = (NodeTableFileBlock *)table->bucket[index];  // Insert at the head (linked list)
    table->bucket[index] = table_node;
    table->count++;
    ReleaseSRWLockExclusive(&table->mutex);
    return table_node;
}
int ht_remove_fblock(TableFileBlock *table, const uint64_t key) {
    
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_remove_fblock()\n");
        return RET_VAL_ERROR;
    }
    
    size_t index = ht_get_hash_fblock(key, table->size);
    AcquireSRWLockExclusive(&table->mutex);
    NodeTableFileBlock *curr = table->bucket[index];
    NodeTableFileBlock *prev = NULL;
    while (curr) {     
        if (curr->key == key) {
            if (prev) {
                prev->next = curr->next;
            } else {
                table->bucket[index] = curr->next;
            }
            pool_free(&table->pool, (void*)curr);
            curr = NULL;
            table->count--;
            ReleaseSRWLockExclusive(&table->mutex);
            return RET_VAL_SUCCESS;
        }
        prev = curr;
        curr = curr->next;
    }
    ReleaseSRWLockExclusive(&table->mutex);
    fprintf(stderr, "WARNING: Node to remove not found for ht_remove_fblock()\n");
    return RET_VAL_ERROR;
}
BOOL ht_search_fblock(TableFileBlock *table, const uint64_t key) {
    
    if(!table){
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_search_fblock()\n");
        return FALSE;
    }
    size_t index = ht_get_hash_fblock(key, table->size);
    AcquireSRWLockShared(&table->mutex);
    NodeTableFileBlock *curr = table->bucket[index];
    while (curr) {
        if (curr->key == key){
            ReleaseSRWLockShared(&table->mutex);
            return TRUE;
        }           
        curr = curr->next;
    }
    ReleaseSRWLockShared(&table->mutex);
    return FALSE;
}
void ht_clean_fblock(TableFileBlock *table) {
    if (!table) {
        fprintf(stderr, "CRITICAL ERROR: Invalid 'table' pointer for ht_clean_fblock()\n");
        return;
    }
    AcquireSRWLockExclusive(&table->mutex);
    for (size_t index = 0; index < table->size; index++) {
        NodeTableFileBlock *curr = table->bucket[index];
        while (curr) {
            NodeTableFileBlock *next = curr->next;
            pool_free(&table->pool, (void*)curr);
            table->count--;
            curr = next;
        }
        table->bucket[index] = NULL;
    }
    ReleaseSRWLockExclusive(&table->mutex);
}


//--------------------------------------------------------------------------------------------------------------------------
size_t hash_stream(const uint32_t session_id, const uint32_t file_id, const uint64_t num_threads) {
    // Combine the two 32-bit IDs into one 64-bit key
    if(session_id == 0 || file_id == 0) {
        return (size_t)num_threads;
    }
    uint64_t key = ((uint64_t)session_id << 32) | file_id;

    // Apply a strong 64-bit hash (MurmurHash3 finalizer style)
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;

    return (size_t)(key % num_threads);

}