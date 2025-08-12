
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset

#include "include/mem_pool.h"

//--------------------------------------------------------------------------------------------------------------------------
void init_pool(MemPool* pool, const uint64_t block_size, const uint64_t block_count) {

    pool->block_size = block_size;
    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in pool_init.\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in pool_init.\n");
        _aligned_free(pool->next); // Clean up previously allocated memory
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)
    
    // Allocate the main memory buffer for the pool
    pool->memory = (char*)_aligned_malloc(pool->block_size * pool->block_count, 64);
    if (!pool->memory) {
        fprintf(stderr, "Memory allocation failed for pool memory in pool_init.\n");
        _aligned_free(pool->next); // Clean up previously allocated memory
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->memory, 0, pool->block_size * pool->block_count);
    
    // Initialize the free list: all blocks are initially free
    pool->free_head = 0; // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t i = 0; i < pool->block_count - 1; i++) {
        pool->next[i] = i + 1;      // Link to the next block
        pool->used[i] = FREE_BLOCK;       // Mark as unused
    }
    // The last block points to POOL_END, indicating the end of the free list
    pool->next[pool->block_count - 1] = END_BLOCK;        // Use POOL_END to indicate end of list
    pool->used[pool->block_count - 1] = FREE_BLOCK;           // Last block is also unused
    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
void* pool_alloc(MemPool* pool) {
    // Enter critical section to protect shared pool data
    AcquireSRWLockExclusive(&pool->lock);
    // Check if the pool is exhausted
    if (pool->free_head == END_BLOCK) { // Check against POOL_END
        ReleaseSRWLockExclusive(&pool->lock);
        return NULL; // Pool exhausted
    }
    // Get the index of the first free block
    uint64_t index = pool->free_head;
    // Update the free head to the next free block
    pool->free_head = pool->next[index];
    // Mark the allocated block as used
    pool->used[index] = USED_BLOCK;
    // Leave critical section
    pool->free_blocks--;
    // Return the memory address of the allocated block
    memset(pool->memory + index * pool->block_size, 0, pool->block_size);
    ReleaseSRWLockExclusive(&pool->lock);
    return (void *)(pool->memory + index * pool->block_size);
}
void pool_free(MemPool* pool, void* ptr) {
    // Handle NULL pointer case
    if (ptr == NULL) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in pool!\n");
        return;
    }
    // Enter critical section
    AcquireSRWLockExclusive(&pool->lock);
    // Calculate the index of the block to be freed
    uint64_t index = ((char*)ptr - pool->memory) / pool->block_size;
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {
        // Log critical errors with maximum detail
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid or already freed chunk!\n");
        // fprintf(stderr, "   Pointer to free: %p\n", ptr);
        // fprintf(stderr, "   Calculated index: %llu\n", index);
        // fprintf(stderr, "   Pool base address: %p\n", pool->memory);
        // fprintf(stderr, "   Block size: %llu bytes\n", pool->block_size);
        // fprintf(stderr, "   Total block count: %llu\n", pool->block_count);
        // fprintf(stderr, "   Is within bounds (0 to %llu)? %s\n", pool->block_count - 1,
        //                 (index < pool->block_count) ? "Yes" : "No");
        // fprintf(stderr, "   Was block marked as used? %s\n", 
        //                 (index < pool->block_count && pool->used[index]) ? "Yes" : "No (Double-Free/Corruption)\n");
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
void pool_destroy(MemPool* pool) {
    // Check for NULL pool pointer
    if (pool == NULL) {
        fprintf(stderr, "ERROR: Attempt to destroy a unallocated pool!\n");
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
    if (pool->memory) {
        _aligned_free(pool->memory);
        pool->memory = NULL;
    }
    pool->free_blocks = 0;
}
//--------------------------------------------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------------------------------------------
void s_init_pool(s_MemPool* pool, const uint64_t block_size, const uint64_t block_count) {

    pool->block_size = block_size;
    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in pool_init.\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in pool_init.\n");
        _aligned_free(pool->next); // Clean up previously allocated memory
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)
    
    // Allocate the main memory buffer for the pool
    pool->memory = (char*)_aligned_malloc(pool->block_size * pool->block_count, 64);
    if (!pool->memory) {
        fprintf(stderr, "Memory allocation failed for pool memory in pool_init.\n");
        _aligned_free(pool->next); // Clean up previously allocated memory
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->memory, 0, pool->block_size * pool->block_count);
    
    pool->semaphore = CreateSemaphore(NULL, block_count, LONG_MAX, NULL);
    if (!pool->semaphore) {
        fprintf(stderr, "Semaphore creation fail s_MemPool.\n");
        _aligned_free(pool->next); // Clean up previously allocated memory
        _aligned_free(pool->used);
        _aligned_free(pool->memory);
        return; // early return in case of failure
    }

    // Initialize the free list: all blocks are initially free
    pool->free_head = 0; // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t i = 0; i < pool->block_count - 1; i++) {
        pool->next[i] = i + 1;      // Link to the next block
        pool->used[i] = FREE_BLOCK;       // Mark as unused
    }
    // The last block points to POOL_END, indicating the end of the free list
    pool->next[pool->block_count - 1] = END_BLOCK;        // Use POOL_END to indicate end of list
    pool->used[pool->block_count - 1] = FREE_BLOCK;           // Last block is also unused
    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
void* s_pool_alloc(s_MemPool* pool) {
    // Enter critical section to protect shared pool data
    WaitForSingleObject(pool->semaphore, INFINITE);
    AcquireSRWLockExclusive(&pool->lock);
    // Check if the pool is exhausted
    if (pool->free_head == END_BLOCK) { // Check against POOL_END
        ReleaseSRWLockExclusive(&pool->lock);
        return NULL; // Pool exhausted
    }
    // Get the index of the first free block
    uint64_t index = pool->free_head;
    // Update the free head to the next free block
    pool->free_head = pool->next[index];
    // Mark the allocated block as used
    pool->used[index] = USED_BLOCK;
    // Leave critical section
    pool->free_blocks--;
    // Return the memory address of the allocated block
    memset(pool->memory + index * pool->block_size, 0, pool->block_size);
    ReleaseSRWLockExclusive(&pool->lock);
    return (void *)(pool->memory + index * pool->block_size);
}
void s_pool_free(s_MemPool* pool, void* ptr) {
    // Handle NULL pointer case
    if (ptr == NULL) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in pool!\n");
        return;
    }
    // Enter critical section
    AcquireSRWLockExclusive(&pool->lock);
    // Calculate the index of the block to be freed
    uint64_t index = ((char*)ptr - pool->memory) / pool->block_size;
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {
        // Log critical errors with maximum detail
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid or already freed chunk!\n");
        // fprintf(stderr, "   Pointer to free: %p\n", ptr);
        // fprintf(stderr, "   Calculated index: %llu\n", index);
        // fprintf(stderr, "   Pool base address: %p\n", pool->memory);
        // fprintf(stderr, "   Block size: %llu bytes\n", pool->block_size);
        // fprintf(stderr, "   Total block count: %llu\n", pool->block_count);
        // fprintf(stderr, "   Is within bounds (0 to %llu)? %s\n", pool->block_count - 1,
        //                 (index < pool->block_count) ? "Yes" : "No");
        // fprintf(stderr, "   Was block marked as used? %s\n", 
        //                 (index < pool->block_count && pool->used[index]) ? "Yes" : "No (Double-Free/Corruption)\n");
        ReleaseSRWLockExclusive(&pool->lock);
        return;
    }
    // Add the freed block back to the head of the free list
    pool->next[index] = pool->free_head;
    pool->free_head = index;
    // Mark the block as unused
    pool->used[index] = FREE_BLOCK;
    pool->free_blocks++;
    // memset(pool->memory + index * pool->block_size, 0, pool->block_size);
    ReleaseSemaphore(pool->semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&pool->lock);
    return;
}
void s_pool_destroy(s_MemPool* pool) {
    // Check for NULL pool pointer
    if (pool == NULL) {
        fprintf(stderr, "ERROR: Attempt to destroy a unallocated pool!\n");
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
    if (pool->memory) {
        _aligned_free(pool->memory);
        pool->memory = NULL;
    }
    pool->free_blocks = 0;
}
//--------------------------------------------------------------------------------------------------------------------------
