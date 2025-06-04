#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>   // Primary Winsock header
#include <ws2tcpip.h>   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>    // For Windows-specific functions like CreateThread, Sleep
#include <time.h>       // For time-based IDs if needed
#include <process.h>    // For _beginthreadex (preferred over CreateThread for CRT safety)

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

// --- Constants ---
#define SERVER_PORT         12345       // Port the server listens on
#define MAX_PAYLOAD_SIZE    1024        // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER     0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS     100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_TIMEOUT_SEC  3000        // Seconds after which an inactive client is considered disconnected
#define FILE_TRANSFER_TIMEOUT_SEC 60    // Seconds after which a stalled file transfer is cleaned up

// --- Frame Types (Enums for clarity) ---
typedef enum {
    FRAME_TYPE_TEXT_MESSAGE = 1,              // Single-part text message
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
    uint16_t start_delimiter;    // Magic number (e.g., 0xAABB)
    uint8_t  frame_type;         // Discriminator: what kind of payload is in the union
    uint32_t seq_num;            // Global sequence number for this frame from the sender
    uint32_t checksum;           // Checksum for this frame's header + active union member (CRC32 recommended)
} FrameHeader;

// Payload Structures for different frame types

typedef struct {
    uint32_t client_id;         // Unique identifier assigned by the client
    uint8_t  protocol_ver;      // Version of the protocol the client supports
    char     client_name[64];   // Optional: human-readable identifier
} ConnectRequestPayload;

typedef struct {
    uint32_t connection_id;     // Unique identifier assigned to the client
    uint8_t  status_code;       // Success (1) or failure (0), with potential error codes
    uint32_t session_timeout;   // Suggested timeout period for client inactivity
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
    uint32_t total_file_size;   // Total size of the file being transferred
    uint8_t  file_hash[16];      // For MD5 hash (adjust size for SHA256 etc.)
    char     filename[256];      // Max filename length
} FileMetadataPayload;

typedef struct {
    uint32_t file_id;
    uint32_t fragment_offset;
    uint32_t payload_len;
    uint8_t  fragment_data[MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 3)]; // Adjusted size
} FileDataPayload;

typedef struct {
    uint32_t acknowledged_seq_num;
    uint32_t target_id;          // Could be file_id, message_id, or client_id if applicable
    uint32_t target_offset;      // If ACKing a specific offset (e.g., for file fragments)
} AckNackPayload;

typedef struct {
    uint32_t file_id;
    uint8_t  final_hash[16];     // Hash of the completely received file
    uint8_t  success;            // 1 for success, 0 for failure
} FileTransferStatusPayload;

// Main UDP Frame Structure
typedef struct {
    FrameHeader header;
    union {
        ConnectRequestPayload   request;
        ConnectResponsePayload  response;
        TextPayload             text_msg;
        LongTextPayload         long_text_msg;
        FileMetadataPayload     file_metadata;
        FileDataPayload         file_data;
        AckNackPayload          ack_nack;
        FileTransferStatusPayload file_transfer_status;
        uint8_t                 raw_payload[MAX_PAYLOAD_SIZE]; // For generic access or padding
    } payload_data;
} UdpFrame;
#pragma pack(pop)

typedef struct{ 
   uint32_t connection_id;
   uint32_t session_timeout;
   uint32_t frame_count;
} SessionInfo;

typedef struct{
   SOCKET  client_socket;
   struct sockaddr_in server_addr;
   uint32_t client_id;
   uint8_t protocol_ver;
   char client_name[64];  
} ClientInfo;

ClientInfo client_info = {0};
volatile unsigned int running = 1;  

// --- Function Prototypes ---
void init_winsock();
void cleanup_winsock();
uint32_t calculate_crc32(const void *data, size_t len); // Changed to CRC32
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);
uint32_t send_frame(const UdpFrame *frame, const ClientInfo *connection_info);


// --- CRC32 Implementation (Simple example, use a proper library for production) ---
// You'd typically use a lookup table for speed. This is a basic polynomial calculation.
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

// Validates the received frame's checksum
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received) {
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
int init_client_info();
void send_connect_request(const ClientInfo *client_info, SessionInfo* session_info);
void send_text_message(const char* text_data, const uint32_t length, const ClientInfo *client_info, SessionInfo* session_info);
unsigned int WINAPI receive_thread_func(LPVOID lpParam);

int init_client_info(){
    
    const char *server_ip = "127.0.0.1"; // loopback address
    strncpy(client_info.client_name, "Client Name", sizeof("Client Name"));
    client_info.client_id = 12;
    // Create UDP socket
    client_info.client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_info.client_socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client_info.client_socket);
        cleanup_winsock();
        return 1;
    }
    
    // Define server address
    memset(&client_info.server_addr, 0, sizeof(client_info.server_addr));
    client_info.server_addr.sin_family = AF_INET;
    client_info.server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client_info.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return 1;
    };
    return 0;
}

int main() {
    WSADATA wsaData;
    SessionInfo session_info = {0};
    int client_initialized = -1;
    char out_message[255];
     HANDLE hRecvThread = NULL;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    //initialize client information
    client_initialized = init_client_info();
    //start frame receive thread
    if(!client_initialized){
        hRecvThread = (HANDLE)_beginthreadex(NULL, 0, receive_thread_func, &client_info, 0, NULL);
        if (hRecvThread == NULL) {
            fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
            running = 0; // Signal immediate shutdown
        }
    }    
    //send connection request frame to server
    send_connect_request(&client_info, &session_info);

    //send text messages to server
    while(running){
        printf("\nMessage to send:");
        fgets(out_message, sizeof(out_message), stdin);
        printf("\n");
        out_message[strcspn(out_message, "\n")] = '\0';
        send_text_message(out_message, strlen(out_message), &client_info, &session_info);
    }
 
    if (hRecvThread) {
        // Signal the receive thread to stop and wait for it to finish
        running = 0;
        WaitForSingleObject(hRecvThread, INFINITE);
        CloseHandle(hRecvThread);
    }
 
    // Cleanup
    closesocket(client_info.client_socket);
    cleanup_winsock();
    return 0;
}

void send_connect_request(const ClientInfo *client_info, SessionInfo* session_info){

    UdpFrame connect_request_frame;
    memset(&connect_request_frame, 0, sizeof(connect_request_frame));

    connect_request_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    connect_request_frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    connect_request_frame.header.seq_num = htonl(++(session_info->frame_count));
    memcpy(connect_request_frame.payload_data.request.client_name, client_info->client_name, sizeof(client_info->client_name));
    connect_request_frame.payload_data.request.client_id = htonl(client_info->client_id);
    connect_request_frame.payload_data.request.protocol_ver = client_info->protocol_ver;

    connect_request_frame.header.checksum = htonl(calculate_crc32(&connect_request_frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));    
    send_frame(&connect_request_frame, client_info);
};

void send_text_message(const char* text_data, const uint32_t length, const ClientInfo *client_info, SessionInfo* session_info){

    UdpFrame text_message_frame;
    if(text_data == NULL){
        printf("Invalid text!\n");
        return;
    }
    if(length >= MAX_PAYLOAD_SIZE - sizeof(text_message_frame.payload_data.text_msg.text_len)){
        printf("Text length too long!\n");
        return;
    }
    if(strlen(text_data) >= MAX_PAYLOAD_SIZE - sizeof(text_message_frame.payload_data.text_msg.text_len)){
        printf("Text strlen() too long!\n");
        return;
    }
    if(strlen(text_data) != length){
        printf("Text length mismatch!\n");
        return;
    }
    memset(&text_message_frame, 0, sizeof(text_message_frame));
    text_message_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    text_message_frame.header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    text_message_frame.header.seq_num = htonl(++session_info->frame_count);
    text_message_frame.payload_data.text_msg.text_len = htonl(length);
    strncpy(text_message_frame.payload_data.text_msg.text_data, text_data, length);
    text_message_frame.payload_data.text_msg.text_data[length] = '\0';
    text_message_frame.header.checksum = htonl(calculate_crc32(&text_message_frame, sizeof(FrameHeader) + sizeof(text_message_frame.payload_data.text_msg.text_len) + length));
    send_frame(&text_message_frame, client_info);

};

// --- Helper Functions ---

void init_winsock() {
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }
}

void cleanup_winsock() {
    WSACleanup();
}

// Sends a UDP frame to a destination address
uint32_t send_frame(const UdpFrame *frame, const ClientInfo *client_info) {
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = sizeof(FrameHeader);
    
    switch (frame->header.frame_type) {
        case FRAME_TYPE_TEXT_MESSAGE:
            frame_size += (sizeof(frame->payload_data.text_msg.text_len) + 
                                ntohl(frame->payload_data.text_msg.text_len)); // Or just header + text_len + sizeof(text_len)
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
        case FRAME_TYPE_NACK:
            frame_size += sizeof(AckNackPayload);
            break;
        case FRAME_TYPE_FILE_TRANSFER_COMPLETE:
        case FRAME_TYPE_FILE_TRANSFER_FAILED:
            frame_size += sizeof(FileTransferStatusPayload);
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            frame_size += sizeof(ConnectRequestPayload);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
        case FRAME_TYPE_DISCONNECT:
        case FRAME_TYPE_KEEP_ALIVE:
            frame_size = sizeof(FrameHeader); // These typically have no specific payload
            break;
        default:
            frame_size = sizeof(UdpFrame); // Fallback to max size
            break;
    }
    printf("Sending frame nr: %d, type: %d, size: %d bytes\n", ntohl(frame->header.seq_num), frame->header.frame_type, frame_size);
    int bytes_sent = sendto(client_info->client_socket, (const char*)frame, frame_size, 0,
                            (const SOCKADDR* )&client_info->server_addr, sizeof(client_info->server_addr));
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto failed with error: %d\n", WSAGetLastError());
        return(WSAGetLastError());
    }
    return 0;
}

// --- Receive Thread Function ---
unsigned int WINAPI receive_thread_func(LPVOID lpParam) {

    ClientInfo *client_info = (ClientInfo *)lpParam;

    UdpFrame received_frame;
    struct sockaddr_in sender_addr;
    int sender_addr_len = sizeof(sender_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECV_TIMEOUT_MS;
    if (setsockopt(client_info->client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }

    while (running) {
        int bytes_received = recvfrom(client_info->client_socket, (char*)&received_frame, sizeof(UdpFrame), 0,
                                      (SOCKADDR*)&sender_addr, &sender_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECV_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
                // For critical errors, you might set 'running = 0;' here to shut down the server.
            }
        } else if (bytes_received > 0) {
            printf("\nbytes_received: %d\n", bytes_received);
            //process_received_frame(&received_frame, bytes_received, &sender_addr);
        }
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}