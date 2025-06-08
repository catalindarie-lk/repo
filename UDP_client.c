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
#define FILE_TRANSFER_TIMEOUT_SEC 60    // Seconds after which a stalled file transfer is cleaned up
#define CLIENT_ID           11          // Example client ID, can be set dynamically
#define CLIENT_NAME         "lkdc UDP Text/File Transfer Client"

typedef enum{
    DISCONNECTED = 0,
    CONNECTED = 1
}SessionStatus;

typedef struct{ 
    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    uint32_t frame_count;       // this will be sent as seq_num

    char server_name[NAME_SIZE];       // Human readable server name
    Queue queue_ack;            // Queue for frames to be ack by the client
    HANDLE queue_event;
    CRITICAL_SECTION queue_mutex; // Mutex for thread-safe access to the queue
} SessionInfo;

typedef struct{
    SOCKET client_socket;
    struct sockaddr_in server_addr;
    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];
} ClientInfo;

ClientInfo client_info;
SessionInfo session_info;


const char *server_ip = "127.0.0.1"; // loopback address
volatile unsigned int running = 1;

// --- Function Prototypes ---
void init_winsock();
void cleanup_winsock();

int init_client_info();
void send_connect_request(const ClientInfo *client_info, SessionInfo* session_info);
void send_text_message(const char* text_data, const ClientInfo *client_info, SessionInfo* session_info);
unsigned int WINAPI receive_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);
//void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *server_addr);

int init_client_info(){
  
    memset(&client_info, 0, sizeof(ClientInfo));
    memset(&session_info, 0, sizeof(SessionInfo));

    InitializeCriticalSection(&session_info.queue_mutex);
    session_info.queue_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    uint32_t len = sizeof(CLIENT_NAME) - 1;
    strncpy(client_info.client_name, CLIENT_NAME, len);
    client_info.client_name[len] = '\0'; // Ensure null termination

    client_info.client_id = CLIENT_ID;
    // Create UDP socket
    client_info.client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_info.client_socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client_info.client_socket);
        cleanup_winsock();
        return -1;
    }
    
    // Define server address
    memset(&client_info.server_addr, 0, sizeof(client_info.server_addr));
    client_info.server_addr.sin_family = AF_INET;
    client_info.server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client_info.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return -1;
    };
 
    return 1;
}

int main() {
    WSADATA wsaData;

    int client_initialized = 0;
    char out_message[255];
    HANDLE hRecvThread = NULL;
    HANDLE hAckThread = NULL;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    //initialize client information
    client_initialized = init_client_info();
    //start frame receive thread
    if(client_initialized){
        hRecvThread = (HANDLE)_beginthreadex(NULL, 0, receive_thread_func, &client_info, 0, NULL);
        if (hRecvThread == NULL) {
            fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
            running = 0; // Signal immediate shutdown
        }
        hAckThread = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, NULL, 0, NULL);
                if (hRecvThread == NULL) {
            fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
            running = 0; // Signal immediate shutdown
        }
    }    

 
    int i = 0;
    while(i < 10 && running){
        
        if(session_info.session_status == DISCONNECTED){
            send_connect_request(&client_info, &session_info);
            printf("Attempting to connect to server...\n");
            Sleep(1000);
            continue; // Wait for the connection to be established
        }


        // printf("\nMessage to send:");
        // fgets(out_message, sizeof(out_message), stdin);
        // printf("\n");
        // out_message[strcspn(out_message, "\n")] = '\0';
        strcpy(out_message, "client message"); // Clear the buffer
        send_text_message(out_message, &client_info, &session_info);
        i++;
        Sleep(100); // Simulate some delay between messages
        
    }
 
    while(1){
        printf("Press 'q' to quit...\n");
        char c = getchar();
        if (c == 'q' || c == 'Q') {
            running = 0; // Signal threads to stop
            break;
        }
    }

    if (hRecvThread) {
        // Signal the receive thread to stop and wait for it to finish
        running = 0;
        WaitForSingleObject(hRecvThread, INFINITE);
        CloseHandle(hRecvThread);
    }

    if (hAckThread) {
        // Signal the receive thread to stop and wait for it to finish
        running = 0;
        WaitForSingleObject(hRecvThread, INFINITE);
        CloseHandle(hAckThread);
    }
 
    DeleteCriticalSection(&session_info.queue_mutex);
    CloseHandle(session_info.queue_event);
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
    memset(&connect_request_frame, 0, sizeof(UdpFrame));
    // Set the header fields
    connect_request_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    connect_request_frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    connect_request_frame.header.seq_num = htonl(++session_info->frame_count);
    connect_request_frame.header.session_id = 0;
    connect_request_frame.payload_data.request.client_id = htonl(client_info->client_id);
    connect_request_frame.payload_data.request.flags = client_info->flags;

    // Copy the client name into the payload
    uint32_t len = sizeof(client_info->client_name) - 1;
    strncpy(connect_request_frame.payload_data.request.client_name, client_info->client_name, len);
    connect_request_frame.payload_data.request.client_name[len] = '\0';
    // Calculate the checksum for the frame
    connect_request_frame.header.checksum = htonl(calculate_crc32(&connect_request_frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    log_sent_frame(&connect_request_frame, &client_info->server_addr);
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
    if(session_info->session_status == DISCONNECTED){
        fprintf(stderr, "Session status yet confirmed by the served. Discarding message...");
        return;
    }
    if(session_info->session_id == 0){
        fprintf(stderr, "Session ID yet confirmed by the served. Discarding message...");
        return;
    }

    // Initialize the text message frame
    memset(&text_message_frame, 0, sizeof(UdpFrame));
    // Set the header fields
    text_message_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    text_message_frame.header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    text_message_frame.header.seq_num = htonl(++session_info->frame_count);
    text_message_frame.header.session_id = htonl(session_info->session_id);
    // Set the payload fields
    text_message_frame.payload_data.text_msg.text_len = htonl(text_len);
    // Copy the text data into the payload
    strncpy(text_message_frame.payload_data.text_msg.text_data, text_data, text_len);
    text_message_frame.payload_data.text_msg.text_data[text_len] = '\0';
    // Calculate the checksum for the frame
    text_message_frame.header.checksum = htonl(calculate_crc32(&text_message_frame, sizeof(FrameHeader) + sizeof(TextPayload)));
    log_sent_frame(&text_message_frame, &client_info->server_addr);
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
void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *sender_addr) {
    // Convert header fields from network byte order to host byte order
    uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
    uint8_t  received_frame_type = frame->header.frame_type; // No byte order for uint8_t
    uint32_t received_seq_num = ntohl(frame->header.seq_num);
    
    char sender_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(sender_addr->sin_addr), sender_ip, INET_ADDRSTRLEN);
    uint16_t sender_port = ntohs(sender_addr->sin_port);

    // 1. Validate Delimiter
    if (received_delimiter != FRAME_DELIMITER) {
        fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n",
                sender_ip, sender_port, received_delimiter);
        return;
    }
    if(strcmp(sender_ip, server_ip) != 0){
        fprintf(stderr, "Received frame from %s:%d: Discarding.\n", sender_ip, sender_port);

    }
    // 2. Validate Checksum
    if (!is_checksum_valid(frame, bytes_received)) {
        fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n",
                sender_ip, sender_port);
        // Optionally send ACK for checksum mismatch if this is part of a reliable stream
        // For individual datagrams, retransmission is often handled by higher layers or ignored.
        return;
    }
    log_recv_frame(frame, sender_addr);
    // 3. Process Payload based on Frame Type
    switch (received_frame_type) {
        case FRAME_TYPE_CONNECT_RESPONSE: {
            uint32_t session_id = ntohl(frame->header.session_id);
            uint8_t server_status = frame->payload_data.response.server_status;
            if(session_id == 0 || server_status == 0){
                fprintf(stderr, "Session not established!\n");
                session_info.session_status = DISCONNECTED;
                break;
            }
            session_info.session_id = session_id;
            session_info.server_status = frame->payload_data.response.server_status;
            session_info.session_timeout = frame->payload_data.response.session_timeout;
            uint32_t len = sizeof(frame->payload_data.response.server_name) - 1;
            strncpy(session_info.server_name, frame->payload_data.response.server_name, len);
            session_info.server_name[len] = '\0';
            session_info.session_status = CONNECTED;
            if(&session_info.queue_mutex == NULL){
                fprintf(stderr, "Session queue mutex not initialized!\n");
                return;
            }
            push_queue(&session_info.queue_ack, received_seq_num, &session_info.queue_mutex);
            SetEvent(session_info.queue_event);    // Signal the send_ack_thread to process the ACK queue
//            SetEvent(queue_event);
            break; 
        }

        case FRAME_TYPE_ACK: {
            uint32_t ack_seq_num = ntohl(frame->payload_data.ack_nack.ack_seq_num);
            uint8_t flags = frame->payload_data.ack_nack.flags;
        }

        case FRAME_TYPE_DISCONNECT: {
            break;
        }
        case FRAME_TYPE_KEEP_ALIVE: {
            break;
        }
        case FRAME_TYPE_CONNECT_REQUEST: {
            break;
        }
            
        default:
            break;
    }
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
            process_received_frame(&received_frame, bytes_received, &sender_addr);
        }
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}

unsigned int WINAPI send_ack_thread_func(LPVOID lpParam){

    while(running){
        WaitForSingleObject(session_info.queue_event, INFINITE);
        uint32_t ack_seq_num = session_info.queue_ack.buffer[session_info.queue_ack.head];
        if(pop_queue(&session_info.queue_ack, &session_info.queue_mutex) == -1) {
            fprintf(stderr,"ACK queue is empty nothing to remove\n");
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, session_info.session_id, client_info.client_socket, &client_info.server_addr);
    }



    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
