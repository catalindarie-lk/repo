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
#define NAME_SIZE                            255
#define PATH_SIZE                            255
//#define ENABLE_FRAME_LOG                1

#define TEXT_FRAGMENT_SIZE              (MAX_PAYLOAD_SIZE - sizeof(uint32_t) * 4)
#define FILE_FRAGMENT_SIZE              (MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 2) - sizeof(uint64_t))

#define RET_VAL_ERROR                   -1
#define RET_VAL_SUCCESS                 0

typedef uint8_t LogType;
enum LogType{
    LOG_FRAME_RECV = 1,
    LOG_FRAME_SENT = 2
};

typedef uint8_t AckErrorCode;
enum AckErrorCode {
    STS_ACK = 0,
    STS_KEEP_ALIVE = 1,
    STS_TRANSFER_COMPLETE = 2,     // Transfer was complete, receive buffer de-allocated

    ERR_INVALID_FILE_ID = 100,     // Server has completed this transfer
    ERR_INVALID_SESSION = 101,     // Session ID not recognized
    ERR_DUPLICATE_FRAME = 102,     // Frame was already received
    ERR_TIMEOUT = 104,             // Session timed out due to inactivity
    ERR_UNSUPPORTED_FRAME = 105,   // Frame type not supported
    ERR_MALFORMED_FRAME = 106,     // Frame structure or size invalid
    ERR_RESOURCE_LIMIT = 107,      // Server ran out of memory or slots
    ERR_UNAUTHORIZED = 108,        // Authentication/authorization failed
    ERR_INTERNAL_ERROR = 109,      // Catch-all for unexpected server fault

};

// --- Frame Types ---
typedef uint8_t FrameType;
enum FrameType{

    FRAME_TYPE_CONNECT_REQUEST = 1,             // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 2,            // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 3,                 // Client requests to disconnect

    FRAME_TYPE_ACK = 4,                         // Acknowledgment for a received frame
    FRAME_TYPE_KEEP_ALIVE = 6,

    FRAME_TYPE_FILE_METADATA = 20,       // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_FRAGMENT = 22,                   // File data fragment
};

//---------------------------------------------------------------------------------------------
#pragma pack(push, 1) 
// Common Header for all frames
typedef struct {
    uint16_t start_delimiter;       // Magic number (e.g., 0xAABB)
    uint8_t  frame_type;            // Discriminator: what kind of payload is in the union
    uint64_t seq_num;               // Global sequence number for this frame from the sender
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
    uint8_t op_code;
} AckPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint64_t file_size;       // Total size of the file being transferred
    uint8_t  file_hash[32];      // For MD5 hash (adjust size for SHA256 etc.)
    char     filename[NAME_SIZE];      // Max filename length
} FileMetadataPayload;

typedef struct {
    uint32_t file_id;           // Unique identifier for the file transfer session
    uint64_t offset;       // Offset of this fragment within the file
    uint32_t size;           // Length of actual data in 'fragment_data'
    char  bytes[FILE_FRAGMENT_SIZE]; // Adjusted size
} FileFragmentPayload;

// Main UDP Frame Structure
typedef struct {
    FrameHeader header;
    union {
        ConnectRequestPayload request;              // Client's connect request
        ConnectResponsePayload response;            // Server's response to client connect
        AckPayload ack;
        FileMetadataPayload file_metadata;      // File metadata request/response
        FileFragmentPayload file_fragment;                  // File data fragment
        uint8_t raw_payload[MAX_PAYLOAD_SIZE]; // For generic access or padding
    } payload;
} UdpFrame;
#pragma pack(pop)

//crc32 lookup table
uint32_t crc32_table[256] = {
    0x00000000,    0x77073096,    0xEE0E612C,    0x990951BA,    0x076DC419,    0x706AF48F,    0xE963A535,    0x9E6495A3,
    0x0EDB8832,    0x79DCB8A4,    0xE0D5E91E,    0x97D2D988,    0x09B64C2B,    0x7EB17CBD,    0xE7B82D07,    0x90BF1D91,
    0x1DB71064,    0x6AB020F2,    0xF3B97148,    0x84BE41DE,    0x1ADAD47D,    0x6DDDE4EB,    0xF4D4B551,    0x83D385C7,
    0x136C9856,    0x646BA8C0,    0xFD62F97A,    0x8A65C9EC,    0x14015C4F,    0x63066CD9,    0xFA0F3D63,    0x8D080DF5,
    0x3B6E20C8,    0x4C69105E,    0xD56041E4,    0xA2677172,    0x3C03E4D1,    0x4B04D447,    0xD20D85FD,    0xA50AB56B,
    0x35B5A8FA,    0x42B2986C,    0xDBBBC9D6,    0xACBCF940,    0x32D86CE3,    0x45DF5C75,    0xDCD60DCF,    0xABD13D59,
    0x26D930AC,    0x51DE003A,    0xC8D75180,    0xBFD06116,    0x21B4F4B5,    0x56B3C423,    0xCFBA9599,    0xB8BDA50F,
    0x2802B89E,    0x5F058808,    0xC60CD9B2,    0xB10BE924,    0x2F6F7C87,    0x58684C11,    0xC1611DAB,    0xB6662D3D,
    0x76DC4190,    0x01DB7106,    0x98D220BC,    0xEFD5102A,    0x71B18589,    0x06B6B51F,    0x9FBFE4A5,    0xE8B8D433,
    0x7807C9A2,    0x0F00F934,    0x9609A88E,    0xE10E9818,    0x7F6A0DBB,    0x086D3D2D,    0x91646C97,    0xE6635C01,
    0x6B6B51F4,    0x1C6C6162,    0x856530D8,    0xF262004E,    0x6C0695ED,    0x1B01A57B,    0x8208F4C1,    0xF50FC457,
    0x65B0D9C6,    0x12B7E950,    0x8BBEB8EA,    0xFCB9887C,    0x62DD1DDF,    0x15DA2D49,    0x8CD37CF3,    0xFBD44C65,
    0x4DB26158,    0x3AB551CE,    0xA3BC0074,    0xD4BB30E2,    0x4ADFA541,    0x3DD895D7,    0xA4D1C46D,    0xD3D6F4FB,
    0x4369E96A,    0x346ED9FC,    0xAD678846,    0xDA60B8D0,    0x44042D73,    0x33031DE5,    0xAA0A4C5F,    0xDD0D7CC9,
    0x5005713C,    0x270241AA,    0xBE0B1010,    0xC90C2086,    0x5768B525,    0x206F85B3,    0xB966D409,    0xCE61E49F,
    0x5EDEF90E,    0x29D9C998,    0xB0D09822,    0xC7D7A8B4,    0x59B33D17,    0x2EB40D81,    0xB7BD5C3B,    0xC0BA6CAD,
    0xEDB88320,    0x9ABFB3B6,    0x03B6E20C,    0x74B1D29A,    0xEAD54739,    0x9DD277AF,    0x04DB2615,    0x73DC1683,
    0xE3630B12,    0x94643B84,    0x0D6D6A3E,    0x7A6A5AA8,    0xE40ECF0B,    0x9309FF9D,    0x0A00AE27,    0x7D079EB1,
    0xF00F9344,    0x8708A3D2,    0x1E01F268,    0x6906C2FE,    0xF762575D,    0x806567CB,    0x196C3671,    0x6E6B06E7,
    0xFED41B76,    0x89D32BE0,    0x10DA7A5A,    0x67DD4ACC,    0xF9B9DF6F,    0x8EBEEFF9,    0x17B7BE43,    0x60B08ED5,
    0xD6D6A3E8,    0xA1D1937E,    0x38D8C2C4,    0x4FDFF252,    0xD1BB67F1,    0xA6BC5767,    0x3FB506DD,    0x48B2364B,
    0xD80D2BDA,    0xAF0A1B4C,    0x36034AF6,    0x41047A60,    0xDF60EFC3,    0xA867DF55,    0x316E8EEF,    0x4669BE79,
    0xCB61B38C,    0xBC66831A,    0x256FD2A0,    0x5268E236,    0xCC0C7795,    0xBB0B4703,    0x220216B9,    0x5505262F,
    0xC5BA3BBE,    0xB2BD0B28,    0x2BB45A92,    0x5CB36A04,    0xC2D7FFA7,    0xB5D0CF31,    0x2CD99E8B,    0x5BDEAE1D,
    0x9B64C2B0,    0xEC63F226,    0x756AA39C,    0x026D930A,    0x9C0906A9,    0xEB0E363F,    0x72076785,    0x05005713,
    0x95BF4A82,    0xE2B87A14,    0x7BB12BAE,    0x0CB61B38,    0x92D28E9B,    0xE5D5BE0D,    0x7CDCEFB7,    0x0BDBDF21,
    0x86D3D2D4,    0xF1D4E242,    0x68DDB3F8,    0x1FDA836E,    0x81BE16CD,    0xF6B9265B,    0x6FB077E1,    0x18B74777,
    0x88085AE6,    0xFF0F6A70,    0x66063BCA,    0x11010B5C,    0x8F659EFF,    0xF862AE69,    0x616BFFD3,    0x166CCF45,
    0xA00AE278,    0xD70DD2EE,    0x4E048354,    0x3903B3C2,    0xA7672661,    0xD06016F7,    0x4969474D,    0x3E6E77DB,
    0xAED16A4A,    0xD9D65ADC,    0x40DF0B66,    0x37D83BF0,    0xA9BCAE53,    0xDEBB9EC5,    0x47B2CF7F,    0x30B5FFE9,
    0xBDBDF21C,    0xCABAC28A,    0x53B39330,    0x24B4A3A6,    0xBAD03605,    0xCDD70693,    0x54DE5729,    0x23D967BF,
    0xB3667A2E,    0xC4614AB8,    0x5D681B02,    0x2A6F2B94,    0xB40BBE37,    0xC30C8EA1,    0x5A05DF1B,    0x2D02EF8D
};
//calculate crc32 with table
uint32_t calculate_crc32_table(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    uint8_t *byte_data = (uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        uint8_t index = (uint8_t)((crc ^ byte_data[i]) & 0xFF);
        crc = (crc >> 8) ^ crc32_table[index];
    }
    return crc ^ 0xFFFFFFFF;
}
// Helper functions

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

    uint32_t calculated_checksum = calculate_crc32_table(&temp_frame_for_checksum, bytes_received);
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
            fprintf(file,"%s\n   FRAME_TYPE_ACK\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n",
                                                    buffer,                                           
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            break;
        case FRAME_TYPE_KEEP_ALIVE:
            fprintf(file,"%s\n   FRAME_TYPE_KEEP_ALIVE\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n",
                                                    buffer,                                           
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum));
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            fprintf(file, "%s\n   FRAME_TYPE_CONNECT_REQUEST\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n   Client ID: %d\n   Flags: %d\n   Client Name: %s\n", 
                                                    buffer,
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.request.client_id), 
                                                    ntohl(frame->payload.request.flag), frame->payload.request.client_name);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            fprintf(file, "%s\n   FRAME_TYPE_CONNECT_RESPONSE\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n   Session Timeout: %d\n   Sever Status: %d\n   Server Name: %s\n", 
                                                    buffer,
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.response.session_timeout), 
                                                    frame->payload.response.server_status, 
                                                    frame->payload.response.server_name);
            break;
        case FRAME_TYPE_FILE_METADATA:
            fprintf(file, "%s   FRAME_TYPE_FILE_METADATA\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   File Size: %d\n", 
                                                    buffer,
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.file_metadata.file_id), 
                                                    ntohl(frame->payload.file_metadata.file_size));                                                    
                                                    break;
        case FRAME_TYPE_FILE_FRAGMENT:
            fprintf(file, "%s   FRAME_TYPE_FILE_FRAGMENT\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   Current Fragment Size: %d\n   Fragment Offset: %d\n   Fragment Bytes: %s\n", 
                                                    buffer,
                                                    ntohll(frame->header.seq_num), 
                                                    ntohl(frame->header.session_id), 
                                                    ntohl(frame->header.checksum),
                                                    ntohl(frame->payload.file_fragment.file_id), 
                                                    ntohl(frame->payload.file_fragment.size),
                                                    ntohl(frame->payload.file_fragment.offset),
                                                    frame->payload.file_fragment.bytes);                                                    
                                                    break;

        case FRAME_TYPE_DISCONNECT:
            fprintf(file, "%s\n   FRAME_TYPE_DISCONNECT\n   Seq Num: %zu\n   Session ID: %d\n   Checksum: %d\n", 
                                                    buffer,
                                                    ntohll(frame->header.seq_num), 
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
    char file_name[PATH_SIZE] = {0};
    if(type == 0){
        snprintf(file_name, PATH_SIZE, "srv_%d.txt", session_id);
    } else {
        snprintf(file_name, PATH_SIZE, "cli_%d.txt", session_id);
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
    size_t frame_size = 0;
    switch (frame->header.frame_type) {
        case FRAME_TYPE_FILE_METADATA:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_FRAGMENT:
            frame_size = sizeof(FrameHeader) + sizeof(FileFragmentPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_ACK:
            frame_size = sizeof(FrameHeader) + sizeof(AckPayload); // Acknowledgment frame
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectRequestPayload); // Assuming ConnectRequest is similar
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectResponsePayload); // Assuming ConnectResponse is similar
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

    int bytes_sent = sendto(src_socket, (const char*)frame, frame_size, 0, (SOCKADDR*)dest_addr, sizeof(*dest_addr));
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto() failed with error: %d\n", WSAGetLastError());
        return SOCKET_ERROR;        
    }

    return bytes_sent;
}
// Send Ack/Nak type frame
int send_ack_nak(const uint64_t seq_num, const uint32_t session_id, const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
    UdpFrame ack_frame;
    //initialize frame
    memset(&ack_frame, 0, sizeof(ack_frame));
    // Set the header fields
    ack_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    ack_frame.header.frame_type = FRAME_TYPE_ACK;
    ack_frame.header.seq_num = htonll(seq_num);
    ack_frame.header.session_id = htonl(session_id); // Use the session ID provided
    ack_frame.payload.ack.op_code = op_code;
    // Calculate CRC32 for the ACK/NACK frame
    ack_frame.header.checksum = htonl(calculate_crc32(&ack_frame, sizeof(FrameHeader) + sizeof(AckPayload)));
    
    int bytes_sent = send_frame(&ack_frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ack() failed\n");
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
    frame.header.seq_num = UINT64_MAX;
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
int send_connect_response(const uint64_t seq_num, const uint32_t session_id, const uint32_t session_timeout, const uint8_t status, 
                                const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id); // Use client's session ID

    frame.payload.response.session_timeout = htonl(session_timeout);
    frame.payload.response.server_status = status;

    snprintf(frame.payload.response.server_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, server_name);

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
int send_connect_request(const uint64_t seq_num, const uint32_t session_id, const uint32_t client_id, const uint32_t flag, 
                                        const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id);
    frame.payload.request.client_id = htonl(client_id);
    frame.payload.request.flag = flag;

    snprintf(frame.payload.request.client_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, client_name);

    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
    #endif
    return bytes_sent; 
};
// --- Send keep alive --- (client function)
int send_keep_alive(const uint64_t seq_num, const uint32_t session_id, 
                        const SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path){
    UdpFrame frame;

    // Initialize the ACK/NACK frame
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_KEEP_ALIVE;
    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id); // Use the session ID provided  
    // Calculate CRC32 for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
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