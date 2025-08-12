
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

// Circular queue for pointers entries (it has push and pop semaphores)
int s_init_queue_ptr(s_QueuePtr *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid s_queue ptr pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for s_queue ptr init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes)
    queue->ptr = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->ptr) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - s_queue ptr init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->ptr, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, size - 1, LONG_MAX, NULL);
    if (!queue->push_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - s_queue ptr init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->ptr);
        return RET_VAL_ERROR;
    }
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!queue->pop_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating pop_semaphore - s_queue ptr init\n");
        _aligned_free(queue->ptr);
        return RET_VAL_ERROR;
    }
    InitializeSRWLock(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int s_push_ptr(s_QueuePtr *queue,  const uintptr_t ptr){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - s_queue ptr not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Push - s_queue ptr invalid queue size\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    AcquireSRWLockExclusive(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        ReleaseSRWLockExclusive(&queue->lock);
        return RET_VAL_ERROR;
    }
    queue->ptr[queue->tail] = ptr;

    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&queue->lock);
    return RET_VAL_SUCCESS;
}
uintptr_t s_pop_ptr(s_QueuePtr *queue){       
    // Check if the queue is initialized
    uintptr_t pool_entry = 0;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - s_queue ptr not initialized.\n");
        return pool_entry;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Pop - s_queue ptr invalid queue size\n");
        return pool_entry;
    }

    WaitForSingleObject(queue->pop_semaphore, INFINITE);
    AcquireSRWLockExclusive(&queue->lock);
    // Check if the queue is empty before removing
    if (queue->head == queue->tail) {
        ReleaseSRWLockExclusive(&queue->lock);
        return pool_entry;
    }
    pool_entry = queue->ptr[queue->head];
    queue->ptr[queue->head] = 0;

    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&queue->lock);
    return pool_entry;
}

// Circular queue for pointer entries (it has only push semaphore)
int init_queue_ptr(QueuePtr *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid ptr queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for ptr queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->ptr = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->ptr) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - ptr queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->ptr, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - ptr queue init\n");
        _aligned_free(queue->ptr);
    return RET_VAL_ERROR;
    }
    InitializeSRWLock(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_ptr(QueuePtr *queue,  const uintptr_t ptr){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0) {
        fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    AcquireSRWLockExclusive(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head) {
        ReleaseSRWLockExclusive(&queue->lock);
        return RET_VAL_ERROR;
    }
    queue->ptr[queue->tail] = ptr;

    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uintptr_t pop_ptr(QueuePtr *queue){       
    // Check if the queue is initialized
    uintptr_t ptr = 0;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Ptr queue not initialized.\n");
        return ptr;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Pop - Ptr queue size not initialized\n");
        return ptr;
    }
    AcquireSRWLockExclusive(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        ReleaseSRWLockExclusive(&queue->lock);
        return ptr;
    }
    ptr = queue->ptr[queue->head];
    queue->ptr[queue->head] = 0;
    
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    ReleaseSRWLockExclusive(&queue->lock);
    return ptr;
}

// Circular queue for client slot numbers
int init_queue_slot(QueueClientSlot *queue, size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid slot queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for slot queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->slot = (uint32_t *)_aligned_malloc(sizeof(uint32_t) * size, 64);
    if (!queue->slot) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - slot queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->slot, 0, sizeof(uint32_t) * size);
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - frame queue init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->slot);
        return RET_VAL_ERROR;
    }
    InitializeSRWLock(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_slot(QueueClientSlot *queue,  const uint32_t slot){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    AcquireSRWLockExclusive(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        ReleaseSRWLockExclusive(&queue->lock);
        return RET_VAL_ERROR;
    }
    queue->slot[queue->tail] = slot;

    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uint32_t pop_slot(QueueClientSlot *queue){       
    // Check if the queue is initialized
    uint32_t slot = UINT32_MAX;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Slot queue not initialized.\n");
        return slot;
    }
    if(queue->size <= 0) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Slot queue size not initialized\n");
        return slot;
    }
    AcquireSRWLockExclusive(&queue->lock);
    // Check if the queue is empty before removing
    if (queue->head == queue->tail) {
        ReleaseSRWLockExclusive(&queue->lock);
        return slot;
    }
    slot = queue->slot[queue->head];
    queue->slot[queue->head] = 0;

    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    ReleaseSRWLockExclusive(&queue->lock);
    return slot;
}

// Circular queue for frame sequence numbers
int init_queue_seq(QueueSeq *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid seq queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for seq queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;
    // Allocate memory aligned to cache line (64 bytes typical)
    queue->seq = (uint64_t *)_aligned_malloc(sizeof(uint64_t) * size, 64);
    if (!queue->seq) {
        fprintf(stderr, "Error allocating aligned memory - seq queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->seq, 0, sizeof(uint64_t) * size);
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "Error creating push_semaphore - seq queue init\n");
        _aligned_free(queue->seq);
        return RET_VAL_ERROR;
    }
    InitializeSRWLock(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_seq(QueueSeq *queue,  const uint64_t seq){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - Seq queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Push - Seq queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    AcquireSRWLockExclusive(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        ReleaseSRWLockExclusive(&queue->lock);
        return RET_VAL_ERROR;
    }
    queue->seq[queue->tail] = seq;

    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    ReleaseSRWLockExclusive(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uint64_t pop_seq(QueueSeq *queue){       
    // Check if the queue is initialized
    uint64_t seq = 0;
    if (!queue) {
        fprintf(stderr, "Pop - Seq queue not initialized.\n");
        return seq;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - Seq queue size not initialized\n");
        return seq;
    }
    AcquireSRWLockExclusive(&queue->lock);
    // Check if the queue is empty before removing
    if (queue->head == queue->tail) {
        ReleaseSRWLockExclusive(&queue->lock);
        return seq;
    }
    seq = queue->seq[queue->head];
    queue->seq[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    ReleaseSRWLockExclusive(&queue->lock);
    return seq;
}

