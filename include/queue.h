#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include "include/protocol_frames.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

//----------------------------------------------------------------------------------------------------------------
__declspec(align(64))typedef struct {
    size_t size;
    uintptr_t *ptr;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    SRWLOCK lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
}QueuePtr;

int init_queue_ptr(QueuePtr *queue, const size_t size);
int push_ptr(QueuePtr *queue,  const uintptr_t ptr);
uintptr_t pop_ptr(QueuePtr *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uintptr_t *ptr;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    SRWLOCK lock;
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    HANDLE pop_semaphore;       
}s_QueuePtr;

int s_init_queue_ptr(s_QueuePtr *queue, const size_t size);
int s_push_ptr(s_QueuePtr *queue,  const uintptr_t ptr);
uintptr_t s_pop_ptr(s_QueuePtr *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uint32_t *slot;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    SRWLOCK lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
}QueueClientSlot;

//----------------------------------------------------------------------------------------------------------------

int init_queue_slot(QueueClientSlot *queue, size_t slot);
int push_slot(QueueClientSlot *queue,  const uint32_t slot);
uint32_t pop_slot(QueueClientSlot *queue);

__declspec(align(64))typedef struct {
    size_t size;
    uint64_t *seq;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    SRWLOCK lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
}QueueSeq;


int init_queue_seq(QueueSeq *queue, const size_t size);
int push_seq(QueueSeq *queue,  const uint64_t seq);
uint64_t pop_seq(QueueSeq *queue);

#endif 