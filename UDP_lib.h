#ifndef _UDP_LIB_H
#define _UDP_LIB_H
#include <time.h>       // For time functions
#include <stdint.h>    // For fixed-width integer types
#include <stdio.h>     // For printf and fprintf

#define MAX_QUEUE_SIZE      2048

typedef enum{
    LOG_INFO, 
    LOG_DEBUG, 
    LOG_ERROR 
}EventType;

typedef struct{
    uint32_t buffer[MAX_QUEUE_SIZE];       // Sequence numbers of frames that need to be ACKed
    uint32_t head, tail;                    // Head and tail indices for the circular queue
}Queue;

void log_event(EventType event, const char* message);
void push_queue(Queue *queue, uint32_t value, uint32_t buffer_size);
void pop_queue(Queue *queue, uint32_t buffer_size);
uint32_t calculate_crc32(const void *data, size_t len);

void log_event(EventType event, const char* message){
    // Get the current UTC time
    time_t current_time;
    time(&current_time);
    struct tm* utc_time = gmtime(&current_time);

    const char* event_str = NULL;
    // Determine the event type string based on the event_type
    if (event == LOG_INFO){
        event_str = "INFO";
    } else if(event == LOG_DEBUG){
        event_str = "DEBUG";
    } else if(event == LOG_ERROR){
        event_str = "ERROR";
    } else {
        event_str = "UNKNOWN";
    }
    // Print the event to stdout with UTC time
    // Note: Using fprintf to stdout for logging, which is common for informational logs
//     fprintf(stdout,"\n[%s] [UTC %04d-%02d-%02d %02d:%02d:%02d] - %s",
//         event_str,
//         utc_time->tm_year + 1900,
//         utc_time->tm_mon + 1,
//         utc_time->tm_mday,
//         utc_time->tm_hour,
//         utc_time->tm_min,
//         utc_time->tm_sec,
//         event_message);
// }

// Note: The format string is adjusted to match the expected output format
    fprintf(stdout,"\n[%s] - %s",
        event_str,
        message);
}

void push_queue(Queue *queue, uint32_t value, uint32_t buffer_size){
    // Check if the queue is full before adding a new ACK
    if((queue->tail + 1) % buffer_size == queue->head){
        printf("Queue Full\n");
        return;
    }
    // Add the sequence number to the ACK queue 
    queue->buffer[queue->tail] = value;
    printf("Added ACK seq nr: Queue[%d] = %d\n", queue->tail, queue->buffer[queue->tail]);
    // Move the tail index forward    
    ++queue->tail;
    queue->tail %= buffer_size;
    return;
}
// Removes a ACK frame from the client's ACK queue
void pop_queue(Queue *queue, uint32_t buffer_size) {       
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        printf("ACK queue is empty nothing to remove\n");
        return;
    }
    printf("Removing ACK seq nr: Queue[%d] = %d\n", queue->head, queue->buffer[queue->head]);
    // Reset the sequence number at the head of the queue
    queue->buffer[queue->head] = 0;
    // Move the head index forward
    ++queue->head;
    queue->head %= buffer_size;
    return;
}

uint32_t calculate_crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF; // Initial value
    const uint8_t *byte_data = (const uint8_t *)data;
    uint32_t polynomial = 0xEDB88320; // IEEE 802.3 polynomial (reversed)

    for (size_t i = 0; i < len; i++) {
        crc ^= byte_data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc; // Final XOR (sometimes not used depending on CRC variant)
}

#endif // _UDP_LIB_H