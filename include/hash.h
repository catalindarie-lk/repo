#ifndef FRAMES_HASH_H
#define FRAMES_HASH_H

#include <stdint.h>
#include <windows.h>

#include "include/protocol_frames.h"
#include "include/mem_pool.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

//--------------------------------------------------------------------------------------------------------------------------
#define HASH_SIZE_ID                  (1024)

typedef uint8_t TableStatus;
enum TableStatus{
    ID_STATUS_NONE = 0,
    ID_WAITING_FRAGMENTS = 1,
    ID_RECV_COMPLETE= 2
};

typedef struct NodeTableIDs{
    uint32_t id;
    uint32_t sid;
    uint8_t status;                         //1 - Pending; 2 - Finished
    struct NodeTableIDs *next;
}NodeTableIDs;

typedef struct{
    NodeTableIDs **entry;
    CRITICAL_SECTION mutex;
    size_t size;
    size_t count;
    MemPool pool_nodes;                     // memory pool of nodes; when new node is inserted into table, memory is allocated                                        // from this pre-allocated mem pool;
}TableIDs;

void init_table_id(TableIDs *table, size_t size, const size_t max_nodes);
uint64_t ht_get_hash_id(uint32_t id, const size_t size);
int ht_insert_id(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_remove_id(TableIDs *table, const uint32_t sid, const uint32_t id);
void ht_remove_all_sid(TableIDs *table, const uint32_t sid);
BOOL ht_search_id(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status);
int ht_update_id_status(TableIDs *table, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_clean_id(TableIDs *table);
void ht_print_id(TableIDs *table);


//--------------------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct TableNodeSendFrame{
    uintptr_t entry;                        // pointer to mem pool with entry (frame + extras)
    time_t sent_time;                       // timestamp of the last time when frame was sent
    uint16_t sent_count;                    // nr of times the frame was sent
    struct TableNodeSendFrame *next;        // next node in linked list
}TableNodeSendFrame;

__declspec(align(64))typedef struct{
    size_t size;                            // size of the hash table base array. each index in array is a pointer to a linked list 
                                            // of nodes which have overlapping hash for the sequence number
    TableNodeSendFrame **node;              // array of pointers to TableNodeSendFrame
    CRITICAL_SECTION mutex;                 // mutex for shared data access
    size_t count;                           // nr of inserted frmes in the table
    MemPool pool_nodes;                     // memory pool of nodes; when new node is inserted into table, memory is allocated
                                            // from this pre-allocated mem pool;
}TableSendFrame;

void init_table_send_frame(TableSendFrame *table, const size_t size, const size_t max_nodes);
uint64_t get_hash_table_send_frame(const uint64_t seq_num, const size_t size);
int insert_table_send_frame(TableSendFrame *table, const uintptr_t entry);
uintptr_t remove_table_send_frame(TableSendFrame *table, const uint64_t seq_num);
uintptr_t search_table_send_frame(TableSendFrame *table, const uint64_t seq_num);

 
#endif // FRAMES_HASH_H