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
#include <iphlpapi.h>

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library
#pragma comment(lib, "iphlpapi.lib")

#define MAX_PAYLOAD_SIZE                1400        // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER                 0xAABB      // A magic number to identify valid frames
#define NAME_SIZE                       64
#define FILE_NAME_SIZE                  64
//#define ENABLE_FRAME_LOG                1

#define RET_VAL_ERROR                   -1
#define RET_VAL_SUCCESS                 0

typedef uint8_t LogType;
enum LogType{
    LOG_FRAME_RECV = 1,
    LOG_FRAME_SENT = 2
};

// --- Frame Types ---
typedef uint8_t FrameType;
enum FrameType{

    FRAME_TYPE_CONNECT_REQUEST = 1,             // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 2,            // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 3,                 // Client requests to disconnect

    FRAME_TYPE_ACK = 4,                         // Acknowledgment for a received frame
    FRAME_TYPE_NACK = 5,                        // Negative Acknowledgment (request retransmission)
    FRAME_TYPE_KEEP_ALIVE = 6,

    FRAME_TYPE_LONG_TEXT_MESSAGE = 10,      // Fragment of a long text message

    FRAME_TYPE_FILE_METADATA = 20,       // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_METADATA_RESPONSE = 21,      // Server grants file transfer (assigns file_id)
    FRAME_TYPE_FILE_FRAGMENT = 22,                   // File data fragment
    FRAME_TYPE_FILE_TRANSFER_COMPLETE = 23,     // Server confirms file transfer completion and hash verification
    FRAME_TYPE_FILE_TRANSFER_FAILED = 24       // Server informs client of failed file transfer (e.g., hash mismatch)
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
    uint32_t message_id;         // Unique ID for this specific long message
    uint32_t total_text_len;          // Total length of the original message
    uint32_t fragment_len;        // Length of actual text data in 'fragment_data'
    uint32_t fragment_offset;    // Offset of this fragment within the long message
    char     fragment_text[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 4)]; // Adjusted size
} LongTextPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint32_t file_size;       // Total size of the file being transferred
    uint32_t max_fragment_size;
    uint8_t  file_hash[32];      // For MD5 hash (adjust size for SHA256 etc.)
    char     filename[256];      // Max filename length
} FileMetadataPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint32_t offset;       // Offset of this fragment within the file
    uint32_t size;           // Length of actual data in 'fragment_data'
    char  bytes[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 3)]; // Adjusted size
} FileFragmentPayload;

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
        LongTextPayload long_text_msg;          // Fragment of a long text message
        FileMetadataPayload file_metadata;      // File metadata request/response
        FileFragmentPayload file_fragment;                  // File data fragment
        FileTransferStatusPayload file_transfer_status;     // File transfer completion or failure status
        uint8_t raw_payload[MAX_PAYLOAD_SIZE]; // For generic access or padding
    } payload;
} UdpFrame;
#pragma pack(pop)


// Helper functions
int calculate_crc32(const void *data, size_t len);
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);

// Logging functions for debug - required here because send_ functions call log frame
void create_log_frame_file(uint8_t type, const uint32_t session_id, char buffer[]);
void log_frame(uint8_t log_type, UdpFrame *frame, const struct sockaddr_in *addr, const char *file_path);


// ----- Function implementations -----
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

// Logging functions for debug
// Log frame to file
void log_frame(uint8_t log_type, UdpFrame *frame, const struct sockaddr_in *addr, const char *file_path){

    if(frame == NULL){
        fprintf(stderr, "No frame to log!\n");
        return;
    }
    if(file_path == NULL){
        fprintf(stderr, "Invalid log file pointer address!\n");
        return;
    }
    if(strlen(file_path) == 0){
        fprintf(stderr, "Invalid log file name!\n");
        return;
    }

    char str_addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, str_addr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(addr->sin_port);

    FILE *file = fopen(file_path, "ab");

    time_t current_time;
    time(&current_time);
    struct tm* utc_time = gmtime(&current_time);

    char buffer[255];

    if (log_type == LOG_FRAME_RECV){
        snprintf(buffer, sizeof(buffer), "Received frame from %s:%d - [UTC %04d-%02d-%02d %02d:%02d:%02d]\0", 
                        str_addr, port, utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday,
                        utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec);
    } else if(log_type == LOG_FRAME_SENT) {

        snprintf(buffer, sizeof(buffer), "Sent frame to %s:%d - [UTC %04d-%02d-%02d %02d:%02d:%02d]\0", 
                        str_addr, port, utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday,
                        utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec);       
    } else {
        fprintf(stdout, "Invalid Log Type!\n");
    }
    //-----------------------------------
    switch(frame->header.frame_type){
        case FRAME_TYPE_ACK:
            fprintf(file,"%s\n   FRAME_TYPE_ACK\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n",
                                                    buffer,                                           
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            break;
        case FRAME_TYPE_KEEP_ALIVE:
            fprintf(file,"%s\n   FRAME_TYPE_KEEP_ALIVE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n",
                                                    buffer,                                           
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            fprintf(file, "%s\n   FRAME_TYPE_CONNECT_REQUEST\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   Client ID: %d\n   Flags: %d\n   Client Name: %s\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.request.client_id), 
                                                    ntohl(frame->payload.request.flag), frame->payload.request.client_name);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            fprintf(file, "%s\n   FRAME_TYPE_CONNECT_RESPONSE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   Session Timeout: %d\n   Sever Status: %d\n   Server Name: %s\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.response.session_timeout), 
                                                    frame->payload.response.server_status, 
                                                    frame->payload.response.server_name);
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            fprintf(file, "%s   FRAME_TYPE_LONG_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   Message ID: %d\n   Total Length: %d\n   Fragment Length: %d\n   Fragment Offset: %d\n   Fragment Text: %s\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.long_text_msg.message_id), 
                                                    ntohl(frame->payload.long_text_msg.total_text_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_len),
                                                    ntohl(frame->payload.long_text_msg.fragment_offset), 
                                                    frame->payload.long_text_msg.fragment_text);
            break;
        case FRAME_TYPE_FILE_METADATA:
            fprintf(file, "%s   FRAME_TYPE_FILE_METADATA\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   File Size: %d\n   Max Fragment Size: %d\n\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.file_metadata.file_id), 
                                                    ntohl(frame->payload.file_metadata.file_size),
                                                    ntohl(frame->payload.file_metadata.max_fragment_size));
                                                    break;
        case FRAME_TYPE_FILE_FRAGMENT:
            fprintf(file, "%s   FRAME_TYPE_FILE_FRAGMENT\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   Current Fragment Size: %d\n   Fragment Offset: %d\n   Fragment Bytes: %s\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.file_fragment.file_id), 
                                                    ntohl(frame->payload.file_fragment.size),
                                                    ntohl(frame->payload.file_fragment.offset),
                                                    frame->payload.file_fragment.bytes);                                                    
                                                    break;

        case FRAME_TYPE_DISCONNECT:
            fprintf(file, "%s\n   FRAME_TYPE_DISCONNECT\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n", 
                                                    buffer,
                                                    ntohl(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            break;
        default:
            break;
    }
    fclose(file); // Close the file  
    return;
}
// Create file to log frames
void create_log_frame_file(uint8_t type, const uint32_t session_id, char buffer[]){

    if(buffer == NULL){
        fprintf(stdout, "Invalid buffer!\n");
    }
    buffer[0] = '\0';
    
    char* log_folder = "E:\\logs\\";
    char file_name[FILE_NAME_SIZE] = {0};
    if(type == 0){
        snprintf(file_name, FILE_NAME_SIZE, "srv_%d.txt", session_id);
    } else {
        snprintf(file_name, FILE_NAME_SIZE, "cli_%d.txt", session_id);
    } 
    
    if (CreateDirectory(log_folder, NULL)) {
        printf("Created folder '%s' for logs: \n", log_folder);
    } else {
        DWORD folder_create_error = GetLastError();
        if (folder_create_error == ERROR_ALREADY_EXISTS) {
            //printf("Folder '%s' already existed from a previous run. Good for testing.\n", client_folder_path);
        } else {
            fprintf(stderr, "Error creating log folder: %lu\n", folder_create_error);
            return; // Exit if we can't even set up the test
        }
    }

    strncpy(buffer, log_folder, strlen(log_folder));
    strncpy(buffer + strlen(log_folder), file_name, strlen(file_name));
    buffer[strlen(log_folder) + strlen(file_name)] = '\0';

    fprintf(stdout, "Session log file: %s\n", buffer);

    FILE *file = fopen(buffer, "wb");
    fclose(file);

    return;

}


// Send frame function
int send_frame(const UdpFrame *frame, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = sizeof(FrameHeader);
    switch (frame->header.frame_type) {
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            frame_size += sizeof(LongTextPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_METADATA:
            frame_size += sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_METADATA_RESPONSE:
            frame_size += sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_FRAGMENT:
            frame_size += sizeof(FileFragmentPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_ACK:
        case FRAME_TYPE_NACK:
            frame_size = sizeof(FrameHeader); // Acknowledgment frame
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
            frame_size = sizeof(FrameHeader);
            break;
        case FRAME_TYPE_KEEP_ALIVE:
            frame_size = sizeof(FrameHeader);
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
int send_ack_nak(const uint8_t type, const uint32_t seq_num, const uint32_t session_id, const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
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
    // Calculate CRC32 for the ACK/NACK frame
    ack_frame.header.checksum = htonl(calculate_crc32(&ack_frame, sizeof(FrameHeader)));
    
    uint32_t bytes_sent = send_frame(&ack_frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ack_nack() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &ack_frame, dest_addr, log_file_path);
    #endif
    return bytes_sent;
}
// Send Disconnect type frame
int send_disconnect(const uint32_t session_id, const SOCKET src_socket, 
                        const struct sockaddr_in *dest_addr, const char *log_file_path){
    UdpFrame frame;
    
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_DISCONNECT;
    frame.header.seq_num = 0;
    frame.header.session_id = htonl(session_id); // Use the session ID provided
    // Calculate CRC32 for the ACK/NACK frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_disconnect() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
    #endif
    return bytes_sent;
}
// --- Send connect response --- (server function)
int send_connect_response(const uint32_t seq_num, const uint32_t session_id, const uint32_t session_timeout, const uint8_t status, 
                                const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id); // Use client's session ID

    frame.payload.response.session_timeout = htonl(session_timeout);
    frame.payload.response.server_status = status;

    strncpy(frame.payload.response.server_name, server_name, NAME_SIZE - 1);
    frame.payload.response.server_name[NAME_SIZE - 1] = '\0';

    // Calculate CRC32 for the ACK frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "send_connect_respose() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
    #endif
    return bytes_sent;
}
// --- Send connect request --- (client function)
int send_connect_request(const uint32_t seq_num, const uint32_t session_id, const uint32_t client_id, const uint32_t flag, 
                                        const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id);
    frame.payload.request.client_id = htonl(client_id);
    frame.payload.request.flag = flag;

    strncpy(frame.payload.request.client_name, client_name, NAME_SIZE - 1);
    frame.payload.request.client_name[NAME_SIZE - 1] = '\0';

    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
    #endif
    return bytes_sent; 
};

int send_keep_alive(const uint32_t seq_num, const uint32_t session_id, 
                        const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
    UdpFrame frame;

    // Initialize the ACK/NACK frame
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_KEEP_ALIVE;
    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id); // Use the session ID provided  
    // Calculate CRC32 for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ping_pong() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
    #endif
    return bytes_sent;
}
#endif // _UDP_LIB_H