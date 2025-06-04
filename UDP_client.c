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

#include "UDP_lib.h"

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

// --- Constants ---
#define SERVER_PORT         12345       // Port the server listens on
#define MAX_PAYLOAD_SIZE    1024        // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER     0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS     100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_TIMEOUT_SEC  3000        // Seconds after which an inactive client is considered disconnected
#define FILE_TRANSFER_TIMEOUT_SEC 60    // Seconds after which a stalled file transfer is cleaned up
#define CLIENT_ID 11 // Example client ID, can be set dynamically
#define CLIENT_NAME "lkdc UDP Text/File Transfer Client"

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

ClientInfo client_info;
SessionInfo session_info;
volatile unsigned int running = 1;  

// --- Function Prototypes ---
void init_winsock();
void cleanup_winsock();
uint32_t calculate_crc32(const void *data, size_t len); // Changed to CRC32
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);

int init_client_info();
void send_connect_request(const ClientInfo *client_info, SessionInfo* session_info);
void send_text_message(const char* text_data, const ClientInfo *client_info, SessionInfo* session_info);
unsigned int WINAPI receive_thread_func(LPVOID lpParam);

int init_client_info(){
    
    const char *server_ip = "127.0.0.1"; // loopback address
    strncpy(client_info.client_name, CLIENT_NAME, sizeof(CLIENT_NAME) - 1);
    client_info.client_name[sizeof(CLIENT_NAME) - 1] = '\0'; // Ensure null termination
    client_info.client_id = CLIENT_ID;
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

    int client_initialized = -1;
    char out_message[255];
    HANDLE hRecvThread = NULL;

    memset(&client_info, 0, sizeof(ClientInfo));
    memset(&session_info, 0, sizeof(SessionInfo));

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
        send_text_message(out_message, &client_info, &session_info);
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
    if(client_info == NULL || session_info == NULL){
        fprintf(stderr, "Invalid client or session information.\n");
        return;
    }
    if (strlen(client_info->client_name) >= sizeof(connect_request_frame.payload_data.request.client_name)) {
        fprintf(stderr, "Client name exceeds maximum allowed length!\n");
        return;
    }
    // Initialize the connect request frame    
    memset(&connect_request_frame, 0, sizeof(connect_request_frame));
    // Set the header fields
    connect_request_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    connect_request_frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    connect_request_frame.header.seq_num = htonl(++(session_info->frame_count));
    connect_request_frame.payload_data.request.client_id = htonl(client_info->client_id);
    connect_request_frame.payload_data.request.protocol_ver = client_info->protocol_ver;
    // Copy the client name into the payload
    strncpy(connect_request_frame.payload_data.request.client_name, client_info->client_name, sizeof(client_info->client_name) - 1);
    // Ensure null termination
    connect_request_frame.payload_data.request.client_name[sizeof(client_info->client_name) - 1] = '\0';
    // Calculate the checksum for the frame
    connect_request_frame.header.checksum = htonl(calculate_crc32(&connect_request_frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    printf("Client Checksum: %u\n", ntohl(connect_request_frame.header.checksum));    
    
    send_frame(&connect_request_frame, client_info->client_socket, &client_info->server_addr);
};

void send_text_message(const char* text_data, const ClientInfo *client_info, SessionInfo* session_info){
    UdpFrame text_message_frame;
    if(text_data == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return;
    }
    uint32_t text_len = strlen(text_data); // Use a local variable for length
    if(text_len >= sizeof(text_message_frame.payload_data.text_msg.text_data)){      
        fprintf(stderr, "Text data exceeds maximum allowed length!\n");
        return;
    }
    // Initialize the text message frame
    memset(&text_message_frame, 0, sizeof(text_message_frame));
    // Set the header fields
    text_message_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    text_message_frame.header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    text_message_frame.header.seq_num = htonl(++session_info->frame_count);
    text_message_frame.payload_data.text_msg.text_len = htonl(text_len);
    // Copy the text data into the payload
    strncpy(text_message_frame.payload_data.text_msg.text_data, text_data, text_len);
    // Ensure null termination
    text_message_frame.payload_data.text_msg.text_data[text_len] = '\0';
    // Calculate the checksum for the frame
    text_message_frame.header.checksum = htonl(calculate_crc32(&text_message_frame, sizeof(FrameHeader) + sizeof(TextPayload)));
    
    send_frame(&text_message_frame, client_info->client_socket, &client_info->server_addr);
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