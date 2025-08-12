#ifndef MEM_POOL_H
#define MEM_POOL_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

#define END_BLOCK UINT64_MAX
#define FREE_BLOCK 0
#define USED_BLOCK 1

//--------------------------------------------------------------------------------------------------------------------------
__declspec(align(64)) typedef struct {
    char* memory;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_size;        // Size of each block in bytes
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;
    // HANDLE semaphore;
} MemPool;

void init_pool(MemPool* pool, const uint64_t block_size, const uint64_t block_count);
void* pool_alloc(MemPool* pool);
void pool_free(MemPool* pool, void* ptr);
void pool_destroy(MemPool* pool);

//--------------------------------------------------------------------------------------------------------------------------

__declspec(align(64)) typedef struct {
    char* memory;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_size;        // Size of each block in bytes
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;
    HANDLE semaphore;
} s_MemPool;

//--------------------------------------------------------------------------------------------------------------------------
void s_init_pool(s_MemPool* pool, const uint64_t block_size, const uint64_t block_count);
void* s_pool_alloc(s_MemPool* pool);
void s_pool_free(s_MemPool* pool, void* ptr);
void s_pool_destroy(s_MemPool* pool);

#endif // MEM_POOL_H