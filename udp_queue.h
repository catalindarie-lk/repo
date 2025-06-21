#ifndef _UDP_QUEUE_H
#define _UDP_QUEUE_H

#include "udp_lib.h"

#define QUEUE_SIZE                      524288      // Queue buffer size

#pragma pack(push, 1)
typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t bytes_received; // Number of bytes received for this frame
}FrameEntry;

typedef struct{
    uint32_t seq_num;       // The sequence number of the frame that require ack/nak
    FrameType type;         // ACK/NAK
    uint32_t session_id;    // Session ID of the frame (used to identify the connected client)
    struct sockaddr_in addr; // Address of the sender
}SeqNumEntry;
#pragma pack(pop)

typedef struct {
    FrameEntry frame_entry[QUEUE_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
}QueueFrame;

typedef struct {
    SeqNumEntry seq_num_entry[QUEUE_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
}QueueSeqNum;


// ---------------------- QUEUE FOR BUFFERING RECEIVED FRAMES ----------------------
// Push frame data to queue - received frames are buffered to a queue before processing; receive thread pushes the frame to the queue
int push_frame(QueueFrame *queue, FrameEntry *frame_entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->mutex);
    if((queue->tail + 1) % QUEUE_SIZE == queue->head){
        LeaveCriticalSection(&queue->mutex);
        //fprintf(stdout, "Frame queue full\n");
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    
    // Add the sequence number to the ACK queue 
    memcpy(&queue->frame_entry[queue->tail], frame_entry, sizeof(FrameEntry)); // Copy the frame to the queue
    // Move the tail index forward    
    ++queue->tail;
    queue->tail %= QUEUE_SIZE;
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}
// Pop frame data from queue - frames are poped from the queue by the frame processing thread
int pop_frame(QueueFrame *queue, FrameEntry *frame_entry){       
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        return RET_VAL_ERROR;
    }
    memset(frame_entry, 0, sizeof(FrameEntry)); // Initialize the structure to zero
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(frame_entry, &queue->frame_entry[queue->head], sizeof(FrameEntry)); // Copy the frame from the queue
    memset(&queue->frame_entry[queue->head], 0, sizeof(FrameEntry)); // Clear the frame at the head
    // Move the head index forward
    ++queue->head;
    queue->head %= QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

// ---------------------- QUEUE FOR ACK/NAK FRAMES ----------------------
// Push sequence num data to queue - frames that need ack/nak are buffered in a circular queue
int push_seq_num(QueueSeqNum *queue, SeqNumEntry *seq_num_entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_SIZE == queue->head){
        LeaveCriticalSection(&queue->mutex);
        fprintf(stdout, "Seq Num queue full\n");
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    // Add the sequence number to the ACK queue 
    memcpy(&queue->seq_num_entry[queue->tail], seq_num_entry, sizeof(SeqNumEntry));
    // Move the tail index forward    
    ++queue->tail;
    queue->tail %= QUEUE_SIZE;
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}
// Pop sequence num data from queue -> send ack/nak (separate thread)
int pop_seq_num(QueueSeqNum *queue, SeqNumEntry *seq_num_entry){       
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        return RET_VAL_ERROR;
    }
     memset(seq_num_entry, 0, sizeof(SeqNumEntry));
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(seq_num_entry, &queue->seq_num_entry[queue->head], sizeof(SeqNumEntry));
    memset(&queue->seq_num_entry[queue->head], 0, sizeof(SeqNumEntry));
    // Move the head index forward
    ++queue->head;
    queue->head %= QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

#endif