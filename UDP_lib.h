#ifndef _UDP_LIB_H
#define _UDP_LIB_H

//#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdint.h>    // For fixed-width integer types
#include <stdio.h>     // For printf and fprintf
#include <string.h>    // For string manipulation functions
#include <time.h>       // For time functions
#include <winsock2.h>   // Primary Winsock header
#include <ws2tcpip.h>   // For modern IP address functions (inet_pton, inet_ntop)
#include <process.h>    // For _beginthreadex (preferred over CreateThread for CRT safety)
#include <windows.h>    // For Windows-specific functions like CreateThread, Sleep

//#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

#define MAX_PAYLOAD_SIZE    40        // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER     0xAABB      // A magic number to identify valid frames
#define QUEUE_SIZE          1048576        // Queue buffer size
#define NAME_SIZE           64
#define SILENT_FRAMES       0

#define RET_VAL_ERROR       -1
#define RET_VAL_SUCCESS     0

typedef uint32_t EventType;
enum EventType{
    LOG_INFO, 
    LOG_DEBUG, 
    LOG_ERROR 
};

typedef uint8_t DisconnectCode;
enum DisconnectCode{
    DISCONNECT_REQUEST = 1,
    DISCONNECT_TIMEOUT = 11,
};

// --- Frame Types ---
typedef uint8_t FrameType;
enum FrameType{
    FRAME_TYPE_TEXT_MESSAGE = 1,                // Single-part text message
    FRAME_TYPE_LONG_TEXT_MESSAGE = 2,      // Fragment of a long text message
    FRAME_TYPE_FILE_METADATA_REQUEST = 3,       // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_METADATA_RESPONSE = 4,      // Server grants file transfer (assigns file_id)
    FRAME_TYPE_FILE_DATA = 5,                   // File data fragment
    FRAME_TYPE_ACK = 6,                         // Acknowledgment for a received frame
    FRAME_TYPE_NACK = 7,                        // Negative Acknowledgment (request retransmission)
    FRAME_TYPE_CONNECT_REQUEST = 8,             // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 9,            // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 10,                 // Client requests to disconnect

    FRAME_TYPE_PING = 20,                       // Client sends periodically to maintain connection status
    FRAME_TYPE_PONG = 21,
    FRAME_TYPE_FILE_TRANSFER_COMPLETE = 30,     // Server confirms file transfer completion and hash verification
    FRAME_TYPE_FILE_TRANSFER_FAILED = 31,       // Server informs client of failed file transfer (e.g., hash mismatch)
};

//---------------------------------------------------------------------------------------------
#pragma pack(push, 1) 
// Common Header for all frames
typedef struct {
    uint16_t start_delimiter;       // Magic number (e.g., 0xAABB)
    uint8_t  frame_type;            // Discriminator: what kind of payload is in the union
    uint32_t seq_num;               // Global sequence number for this frame from the sender
    uint32_t session_id;            // Unique identifier for the session (e.g., client ID or session ID)
    uint32_t checksum;              // Checksum for this frame's header + active union member (CRC32 recommended)
} FrameHeader;
// Payload Structures for different frame types
typedef struct {
    uint32_t client_id;             // Unique identifier of the sender
    uint8_t  flag;                 // Protocol the client supports
    char     client_name[NAME_SIZE];// Optional: human-readable identifier
} ConnectRequestPayload;

typedef struct {
    uint32_t session_timeout;   // Suggested timeout period for client inactivity
    uint8_t  server_status;     // BUSY (0) READY (1) or ERR (x), etc
    char     server_name[NAME_SIZE];   // Optional: human-readable identifier
} ConnectResponsePayload;

typedef struct {
    uint32_t len;           // Length of the text message
    char     text[MAX_PAYLOAD_SIZE - sizeof(uint32_t)]; // Actual text
} TextPayload;

typedef struct {
    uint32_t message_id;         // Unique ID for this specific long message
    uint32_t total_text_len;          // Total length of the original message
    uint32_t fragment_len;        // Length of actual text data in 'fragment_data'
    uint32_t fragment_offset;    // Offset of this fragment within the long message
    char     fragment_text[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 4)]; // Adjusted size
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
    uint8_t flag;           // For future use
} AckNakPayload;

typedef struct {
    uint8_t flag;
}DisconnectPayload;

typedef struct{
    uint32_t ack_num;
    uint8_t flag;
}PingPongPayload;

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
        AckNakPayload ack_nak;                // Acknowledgment or Negative Acknowledgment    
        DisconnectPayload disconnect;
        PingPongPayload ping_pong;
        FileTransferStatusPayload file_transfer_status;     // File transfer completion or failure status
        uint8_t raw_payload[MAX_PAYLOAD_SIZE]; // For generic access or padding
    } payload;
} UdpFrame;

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
//---------------------------------------------------------------------------------------------


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

// Helper functions
int calculate_crc32(const void *data, size_t len);
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);

// Logging functions for debug
void log_event(EventType event, const char* message);
void log_recv_frame(UdpFrame *frame, const struct sockaddr_in *src_addr, const char *file_path);
void log_sent_frame(UdpFrame *frame, const struct sockaddr_in *dest_addr);

// Queue buffers
int push_frame(QueueFrame *queue, FrameEntry *frame_entry);
int pop_frame(QueueFrame *queue, FrameEntry *frame_entry);
int push_seq_num(QueueSeqNum *queue, SeqNumEntry *seq_num_entry);
int pop_seq_num(QueueSeqNum *queue, SeqNumEntry *seq_num_entry);

// UDP communication functions
int send_frame(const UdpFrame *frame, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_ack_nak(const uint8_t type, const uint32_t seq_num, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_disconnect(const uint8_t flag, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_ping_pong(const uint8_t type, const uint32_t ack_num, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr);

// ----- Function implementations -----
// Log Events
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
    fprintf(stdout,"\n[%s] [UTC %04d-%02d-%02d %02d:%02d:%02d] - %s",
        event_str,
        utc_time->tm_year + 1900,
        utc_time->tm_mon + 1,
        utc_time->tm_mday,
        utc_time->tm_hour,
        utc_time->tm_min,
        utc_time->tm_sec,
        message);
}
// CRC32 calculation
int calculate_crc32(const void *data, size_t len){
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
// Checksum validation
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
    return (ntohl(frame->header.checksum) == calculated_checksum); // Use ntohl for 32-bit checksum
}
// Send frame function
int send_frame(const UdpFrame *frame, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = sizeof(FrameHeader);
    switch (frame->header.frame_type) {
        case FRAME_TYPE_TEXT_MESSAGE:
            frame_size += sizeof(TextPayload); // Or just header + text_len + sizeof(text_len)
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
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
            frame_size += sizeof(AckNakPayload); // Acknowledgment frame
            break;
        case FRAME_TYPE_NACK:
            frame_size += sizeof(AckNakPayload);
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
            frame_size += sizeof(DisconnectPayload);
            break;
        case FRAME_TYPE_PING:
        case FRAME_TYPE_PONG:
            frame_size += sizeof(PingPongPayload); // These typically have no specific payload
            break;
        default:
            frame_size = sizeof(UdpFrame); // Fallback to max size
            break;
    }

    uint32_t bytes_sent = sendto(src_socket, (const char*)frame, frame_size, 0, (SOCKADDR*)dest_addr, sizeof(*dest_addr));
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto() failed with error: %d\n", WSAGetLastError());
        return SOCKET_ERROR;        
    }
    // char addr[INET_ADDRSTRLEN];
    // inet_ntop(AF_INET, &dest_addr->sin_addr, addr, INET_ADDRSTRLEN);
    // uint16_t port = ntohs(dest_addr->sin_port);
    // fprintf(stdout, "\nSent %d bytes to %s:%d\n", bytes_sent, addr, port);
    return bytes_sent;
}
// Send Ack/Nak type frame
int send_ack_nak(const uint8_t type, const uint32_t seq_num, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    UdpFrame ack_frame;
    // Check frame type is valid
    if((type != FRAME_TYPE_ACK) && (type != FRAME_TYPE_NACK)){
        fprintf(stderr, "Wrong frame type\n");
        return SOCKET_ERROR;
    }
    // Initialize the ACK/NACK frame
    memset(&ack_frame, 0, sizeof(ack_frame));
    // Set the header fields
    ack_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    ack_frame.header.frame_type = type;
    ack_frame.header.seq_num = htonl(seq_num);
    ack_frame.header.session_id = htonl(session_id); // Use the session ID provided
    ack_frame.payload.ack_nak.flag = 0;
    // Calculate CRC32 for the ACK/NACK frame
    ack_frame.header.checksum = htonl(calculate_crc32(&ack_frame, sizeof(FrameHeader) + sizeof(AckNakPayload)));
    
    uint32_t bytes_sent = send_frame(&ack_frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ack_nack() failed\n");
        return SOCKET_ERROR;
    }
    log_sent_frame(&ack_frame, dest_addr);
    return bytes_sent;
}
// Send Disconnect type frame
int send_disconnect(const uint8_t flag, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    UdpFrame frame;
    
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_DISCONNECT;
    frame.header.seq_num = 0;
    frame.header.session_id = htonl(session_id); // Use the session ID provided
    frame.payload.disconnect.flag = flag;   
    // Calculate CRC32 for the ACK/NACK frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(DisconnectPayload)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_disconnect() failed\n");
        return SOCKET_ERROR;
    }
    log_sent_frame(&frame, dest_addr);
    return bytes_sent;
}
// Send Ping-Pong type frame
int send_ping_pong(const uint8_t type, const uint32_t ack_num, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    UdpFrame frame;
    // Check frame type is valid
    if((type != FRAME_TYPE_PING) && (type != FRAME_TYPE_PONG)){
        fprintf(stderr, "Wrong frame type\n");
        return SOCKET_ERROR;
    }
    // Initialize the ACK/NACK frame
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = type;
    frame.header.seq_num = 0;
    frame.header.session_id = htonl(session_id); // Use the session ID provided
    frame.payload.ping_pong.ack_num = htonl(ack_num);   
    frame.payload.ping_pong.flag = 0;
    // Calculate CRC32 for the ACK/NACK frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(PingPongPayload)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ping_pong() failed\n");
        return SOCKET_ERROR;
    }
    log_sent_frame(&frame, dest_addr);
    return bytes_sent;
}
// Log received frame
void log_recv_frame(UdpFrame *frame, const struct sockaddr_in *src_addr, const char *file_path){

    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src_addr->sin_addr, addr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(src_addr->sin_port);

    FILE *file = fopen(file_path, "a");

    // file_ptr = fopen(final_path, "w");
    // bytes_written = fwrite(client->long_msg_buff[slot].text, 1, payload_total_text_len, file_ptr);

    fprintf(file, "Received frame from %s:%d\n", addr, port);

    switch(frame->header.frame_type){
        case FRAME_TYPE_ACK:
            fprintf(file, "   FRAME_TYPE_ACK\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Flags: %d\n", frame->payload.ack_nak.flag);
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            fprintf(file, "   FRAME_TYPE_CONNECT_REQUEST\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Client ID: %d\n   Flags: %d\n   Client Name: %s\n", 
                                                    ntohl(frame->payload.request.client_id), 
                                                    ntohl(frame->payload.request.flag), frame->payload.request.client_name);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            fprintf(file, "   FRAME_TYPE_CONNECT_RESPONSE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Session Timeout: %d\n   Sever Status: %d\n   Server Name: %s\n", 
                                                    ntohl(frame->payload.response.session_timeout), 
                                                    frame->payload.response.server_status, 
                                                    frame->payload.response.server_name);
            break;
        case FRAME_TYPE_TEXT_MESSAGE:
            fprintf(file, "   FRAME_TYPE_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Text Length: %d\n   Text: %s\n", 
                                                    ntohl(frame->payload.text_msg.len), frame->payload.text_msg.text);
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            fprintf(file, "   FRAME_TYPE_LONG_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Message ID: %d\n   Total Length: %d\n   Fragment Length: %d\n   Fragment Offset: %d\n   Fragment Text: %s\n", 
                                                    ntohl(frame->payload.long_text_msg.message_id), 
                                                    ntohl(frame->payload.long_text_msg.total_text_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_offset), 
                                                    frame->payload.long_text_msg.fragment_text); 
            break;

        case FRAME_TYPE_DISCONNECT:
            fprintf(file, "   FRAME_TYPE_DISCONNECT\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Disconnect flag: %d\n", frame->payload.disconnect.flag);
            break;
        case FRAME_TYPE_PING:
            fprintf(file, "   FRAME_TYPE_PING\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Ack Ping Seq: %d\n   Flags: %d\n", 
                                                    ntohl(frame->payload.ping_pong.ack_num), frame->payload.ping_pong.flag);
            break;
        case FRAME_TYPE_PONG:
            fprintf(file, "   FRAME_TYPE_PONG\n   Session ID: %d\n   Checksum: %d\n", 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            fprintf(file, "   Ack Pong Seq: %d\n   Flags: %d\n", 
                                                    ntohl(frame->payload.ping_pong.ack_num), 
                                                    frame->payload.ping_pong.flag);
            break;
        default:
//                fprintf(stdout, "   FRAME_TYPE_INVALID\n");
            break;
    }
    fclose(file); // Close the file
   
    return;
}
// Log sent frame
void log_sent_frame(UdpFrame *frame, const struct sockaddr_in *dest_addr){

    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dest_addr->sin_addr, addr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(dest_addr->sin_port);
    fprintf(stdout, "Sent frame to %s:%d\n", addr, port);
    switch(frame->header.frame_type){
        case FRAME_TYPE_ACK:
            fprintf(stdout, "   FRAME_TYPE_ACK\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Flags: %d\n", frame->payload.ack_nak.flag);
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            fprintf(stdout, "   FRAME_TYPE_CONNECT_REQUEST\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Client ID: %d\n   Flags: %d\n   Client Name: %s\n", ntohl(frame->payload.request.client_id), ntohl(frame->payload.request.flag), frame->payload.request.client_name);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            fprintf(stdout, "   FRAME_TYPE_CONNECT_RESPONSE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Session Timeout: %d\n   Sever Status: %d\n   Server Name: %s\n", ntohl(frame->payload.response.session_timeout), frame->payload.response.server_status, frame->payload.response.server_name);
            break;
        case FRAME_TYPE_TEXT_MESSAGE:
            fprintf(stdout, "   FRAME_TYPE_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Text Length: %d\n   Text: %s\n", ntohl(frame->payload.text_msg.len), frame->payload.text_msg.text);
            break;
        case FRAME_TYPE_DISCONNECT:
            fprintf(stdout, "   FRAME_TYPE_DISCONNECT\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Disconnect Flag: %d\n", frame->payload.disconnect.flag);
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            fprintf(stdout, "   FRAME_TYPE_LONG_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.seq_num), ntohl(frame->header.session_id), ntohl(frame->header.checksum));
            fprintf(stdout, "   Message ID: %d\n   Total Length: %d\n   Fragment Length: %d\n   Fragment Offset: %d\n   Fragment Text: %s\n", 
                                                    ntohl(frame->payload.long_text_msg.message_id), 
                                                    ntohl(frame->payload.long_text_msg.total_text_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_offset), 
                                                    frame->payload.long_text_msg.fragment_text); 
            break;

        // case FRAME_TYPE_PING:
        //     fprintf(stdout, "   FRAME_TYPE_PING\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.session_id), ntohl(frame->header.checksum));
        //     fprintf(stdout, "   Ack Ping Seq: %d\n   Flags: %d\n", ntohl(frame->payload.ping_pong.ack_num), frame->payload.ping_pong.flags);
        //     break;
        // case FRAME_TYPE_PONG:
        //     fprintf(stdout, "   FRAME_TYPE_PONG\n   Session ID: %d\n   Checksum: %d\n", ntohl(frame->header.session_id), ntohl(frame->header.checksum));
        //     fprintf(stdout, "   Ack Pong Seq: %d\n   Flags: %d\n", ntohl(frame->payload.ping_pong.ack_num), frame->payload.ping_pong.flags);
        //     break;
        default:
//                fprintf(stdout, "   FRAME_TYPE_INVALID\n");
            break;
    }
    return;  
}
// Push sequence number to queue - used for received frames (seq_num) that need to be acked

// ---------------------- QUEUE FOR BUFFERING RECEIVED FRAMES ----------------------
// Push frame data to queue - received frames are buffered to a queue before processing; receive thread pushes the frame to the queue
int push_frame(QueueFrame *queue, FrameEntry *frame_entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_SIZE == queue->head){
        printf("Queue Full\n");
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    EnterCriticalSection(&queue->mutex);
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
    memset(frame_entry, 0, sizeof(FrameEntry)); // Initialize the structure to zero
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        return RET_VAL_ERROR;
    }
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
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_SIZE == queue->head){
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    EnterCriticalSection(&queue->mutex);
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
    memset(seq_num_entry, 0, sizeof(SeqNumEntry));
    if (queue == NULL || &queue->mutex == NULL) {
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(seq_num_entry, &queue->seq_num_entry[queue->head], sizeof(SeqNumEntry));
    memset(&queue->seq_num_entry[queue->head], 0, sizeof(SeqNumEntry));
    // Move the head index forward
    ++queue->head;
    queue->head %= QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

#endif // _UDP_LIB_H