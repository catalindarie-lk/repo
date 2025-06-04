#ifndef _UDP_LIB_H
#define _UDP_LIB_H
#include <time.h>       // For time functions
#include <stdint.h>    // For fixed-width integer types
#include <stdio.h>     // For printf and fprintf
#include <string.h>    // For string manipulation functions

#define MAX_PAYLOAD_SIZE    1024        // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER     0xAABB      // A magic number to identify valid frames
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

// --- Frame Types (Enums for clarity) ---
typedef enum {
    FRAME_TYPE_TEXT_MESSAGE = 1,        // Single-part text message
    FRAME_TYPE_LONG_TEXT_MESSAGE_DATA = 2,    // Fragment of a long text message
    FRAME_TYPE_FILE_METADATA_REQUEST = 3,     // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_METADATA_RESPONSE = 4,    // Server grants file transfer (assigns file_id)
    FRAME_TYPE_FILE_DATA = 5,                 // File data fragment
    FRAME_TYPE_ACK = 6,                       // Acknowledgment for a received frame
    FRAME_TYPE_NACK = 7,                      // Negative Acknowledgment (request retransmission)
    FRAME_TYPE_CONNECT_REQUEST = 8,           // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 9,          // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 10,               // Client requests to disconnect
    FRAME_TYPE_KEEP_ALIVE = 11,               // Client sends periodically to maintain connection status
    FRAME_TYPE_FILE_TRANSFER_COMPLETE = 12,   // Server confirms file transfer completion and hash verification
    FRAME_TYPE_FILE_TRANSFER_FAILED = 13      // Server informs client of failed file transfer (e.g., hash mismatch)
} FrameType;

// --- UDP Frame Structures (with Union) ---
#pragma pack(push, 1) // Using #pragma pack to ensure no padding for network transmission
// Common Header for all frames
typedef struct {
    uint16_t start_delimiter;       // Magic number (e.g., 0xAABB)
    uint8_t  frame_type;            // Discriminator: what kind of payload is in the union
    uint32_t seq_num;               // Global sequence number for this frame from the sender
    uint32_t checksum;              // Checksum for this frame's header + active union member (CRC32 recommended)
} FrameHeader;

// Payload Structures for different frame types

typedef struct {
    uint32_t client_id;         // Unique identifier assigned by the client
    uint8_t  protocol_ver;      // Version of the protocol the client supports
    char     client_name[64];   // Optional: human-readable identifier
} ConnectRequestPayload;

typedef struct {
    uint32_t session_id;        // Unique identifier assigned to the client
    uint32_t session_timeout;   // Suggested timeout period for client inactivity
    uint8_t  server_status;     // BUSY (0) READY (1) or ERR (x), etc
    char     server_name[64];   // Optional: human-readable identifier
} ConnectResponsePayload;

typedef struct {
    uint32_t text_len;           // Length of the text message
    char     text_data[MAX_PAYLOAD_SIZE - sizeof(uint32_t)]; // Actual text
} TextPayload;

typedef struct {
    uint32_t message_id;         // Unique ID for this specific long message
    uint32_t total_message_len;  // Total length of the original message
    uint32_t fragment_offset;    // Offset of this fragment within the long message
    uint32_t payload_len;        // Length of actual text data in 'fragment_data'
    char     fragment_data[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 4)]; // Adjusted size
} LongTextPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint32_t total_file_size;       // Total size of the file being transferred
    uint8_t  file_hash[16];      // For MD5 hash (adjust size for SHA256 etc.)
    char     filename[256];      // Max filename length
} FileMetadataPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint32_t fragment_offset;       // Offset of this fragment within the file
    uint32_t payload_len;           // Length of actual data in 'fragment_data'
    uint8_t  fragment_data[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 3)]; // Adjusted size
} FileDataPayload;

typedef struct {
    uint32_t ack_seq_num;       // Acknowledged sequence number of the frame being acknowledged
    uint32_t session_id;          // Unique identifier assigned to the client
    uint8_t status_code;         // Success (1) or failure (0), with potential error codes
} AckNackPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint8_t  final_hash[16];     // Hash of the completely received file
    uint8_t  success;            // 1 for success, 0 for failure
} FileTransferStatusPayload;

// Main UDP Frame Structure
typedef struct {
    FrameHeader header;
    union {
        ConnectRequestPayload request;              // Client's connect request
        ConnectResponsePayload response;            // Server's response to client connect
        TextPayload text_msg;                   // Single-part text message
        LongTextPayload long_text_msg;          // Fragment of a long text message
        FileMetadataPayload file_metadata;      // File metadata request/response
        FileDataPayload file_data;                  // File data fragment
        AckNackPayload ack_nack;                // Acknowledgment or Negative Acknowledgment    
        FileTransferStatusPayload file_transfer_status;     // File transfer completion or failure status
        uint8_t raw_payload[MAX_PAYLOAD_SIZE]; // For generic access or padding
    } payload_data;
} UdpFrame;
#pragma pack(pop)


void log_event(EventType event, const char* message);
void push_queue(Queue *queue, uint32_t value, uint32_t buffer_size);
void pop_queue(Queue *queue, uint32_t buffer_size);
uint32_t calculate_crc32(const void *data, size_t len);
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);
void send_frame(const UdpFrame *frame, const SOCKET src_socket, const struct sockaddr_in *dest_addr);

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
    fprintf(stdout, "Added ACK seq nr: Queue[%d] = %d\n", queue->tail, queue->buffer[queue->tail]);
    // Move the tail index forward    
    ++queue->tail;
    queue->tail %= buffer_size;
    return;
}
// Removes a ACK frame from the client's ACK queue
void pop_queue(Queue *queue, uint32_t buffer_size){       
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        printf("ACK queue is empty nothing to remove\n");
        return;
    }
    fprintf(stdout, "Removing ACK seq nr: Queue[%d] = %d\n", queue->head, queue->buffer[queue->head]);
    // Reset the sequence number at the head of the queue
    queue->buffer[queue->head] = 0;
    // Move the head index forward
    ++queue->head;
    queue->head %= buffer_size;
    return;
}

uint32_t calculate_crc32(const void *data, size_t len){
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

BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received){
    // Create a temporary frame to calculate checksum without its own checksum field
    UdpFrame temp_frame_for_checksum;
    // Copy only the header and the part of the payload that was actually sent.
    // This is crucial if payloads are variable length or smaller than MAX_PAYLOAD_SIZE.
    // For simplicity, we assume fixed size for now (sizeof(UdpFrame)).
    // In a real scenario, you'd use header.payload_len or similar.
    memcpy(&temp_frame_for_checksum, frame, bytes_received);
    temp_frame_for_checksum.header.checksum = 0; // Zero out checksum field for calculation

    uint32_t calculated_checksum = calculate_crc32(&temp_frame_for_checksum, bytes_received);
    printf("Calculated Checksum: %u\n", calculated_checksum);
    printf("Received Checksum: %u\n", ntohl(frame->header.checksum));
    return (ntohl(frame->header.checksum) == calculated_checksum); // Use ntohl for 32-bit checksum
}

void send_frame(const UdpFrame *frame, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = sizeof(FrameHeader);
    switch (frame->header.frame_type) {
        case FRAME_TYPE_TEXT_MESSAGE:
            frame_size += sizeof(TextPayload); // Or just header + text_len + sizeof(text_len)
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE_DATA:
            frame_size += sizeof(LongTextPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_METADATA_REQUEST:
        case FRAME_TYPE_FILE_METADATA_RESPONSE:
            frame_size += sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_DATA:
            frame_size += sizeof(FileDataPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_ACK:
            frame_size += sizeof(AckNackPayload); // Acknowledgment frame
            break;
        case FRAME_TYPE_NACK:
            frame_size += sizeof(AckNackPayload);
            break;
        case FRAME_TYPE_FILE_TRANSFER_COMPLETE:
        case FRAME_TYPE_FILE_TRANSFER_FAILED:
            frame_size += sizeof(FileTransferStatusPayload);
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            frame_size += sizeof(ConnectRequestPayload); // Assuming ConnectRequest is similar
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            frame_size += sizeof(ConnectResponsePayload); // Assuming ConnectResponse is similar
            break;
        case FRAME_TYPE_DISCONNECT:
        case FRAME_TYPE_KEEP_ALIVE:
            frame_size = sizeof(FrameHeader); // These typically have no specific payload
            break;
        default:
            frame_size = sizeof(UdpFrame); // Fallback to max size
            break;
    }

    int bytes_sent = sendto(src_socket, (const char*)frame, frame_size, 0,
                            (SOCKADDR*)dest_addr, sizeof(*dest_addr));
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto failed with error: %d\n", WSAGetLastError());
    }
}
// Note: The function send_frame should be called with the correct socket and destination address

#endif // _UDP_LIB_H