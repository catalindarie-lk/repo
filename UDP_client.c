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

typedef uint8_t ClientStatus;
enum ClientStatus {
    CLIENT_STOP = 0,
    CLIENT_READY = 1,
    CLIENT_ERROR = 2,
    CLIENT_BUSY = 3
};

typedef struct{

    SOCKET socket;
    struct sockaddr_in server_addr;
    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];

    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    uint32_t frame_count;       // this will be sent as seq_num

    char server_name[NAME_SIZE];       // Human readable server name
    Queue queue_ack;            // Queue for frames to be ack by the client

    FrameQueue frame_queue;

    HANDLE recieve_frame_thread;
    HANDLE send_ack_thread;
    HANDLE process_frame_thread;

    ClientStatus client_status;

} StructClient;

StructClient client;

const char *server_ip = "127.0.0.1"; // loopback address
volatile unsigned int running = 1;

// --- Function prototypes ---
void send_connect_request(const uint32_t seq_nr, const uint32_t client_id, const uint32_t flags, const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
void send_text_message(const uint32_t seq_nr, const uint32_t session_id, const char* text, const uint32_t len, const SOCKET src_socket, const struct sockaddr_in *dest_addr);

unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);

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
void init_client(StructClient *client){
  
    memset(client, 0, sizeof(StructClient));

    // Create UDP socket
    client->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client->socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client->socket);
        cleanup_winsock();
        return;
    }
    
    // Define server address
    memset(&client->server_addr, 0, sizeof(client->server_addr));
    client->server_addr.sin_family = AF_INET;
    client->server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client->server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return;
    };

    client->queue_ack.head = 0;
    client->queue_ack.tail = 0;
    InitializeCriticalSection(&client->queue_ack.mutex);

       // Initialize the frame queue buffer
    client->frame_queue.head = 0;
    client->frame_queue.tail = 0;
    InitializeCriticalSection(&client->frame_queue.mutex);

    uint32_t len = sizeof(CLIENT_NAME) - 1;
    strncpy(client->client_name, CLIENT_NAME, len);
    client->client_name[len] = '\0'; // Ensure null termination
    client->client_id = CLIENT_ID;
 
    client->client_status = CLIENT_READY;
    return;
}

void start_threads(StructClient *client){

    client->recieve_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, client, 0, NULL);
    if (client->recieve_frame_thread == NULL) {
        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
        client->client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    client->process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, client, 0, NULL);
    if (client->process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client->client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    client->send_ack_thread = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, client, 0, NULL);
        if (client->send_ack_thread == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        client->client_status = CLIENT_STOP; // Signal immediate shutdown
    }
}    
void shutdown_client(StructClient *client){

    client->client_status = CLIENT_STOP;

    if (client->recieve_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client->recieve_frame_thread, INFINITE);
        CloseHandle(client->recieve_frame_thread);
    }
    if (client->process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client->process_frame_thread, INFINITE);
        CloseHandle(client->process_frame_thread);
    }
    if (client->send_ack_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client->send_ack_thread, INFINITE);
        CloseHandle(client->send_ack_thread);
    }
    DeleteCriticalSection(&client->frame_queue.mutex);
    DeleteCriticalSection(&client->queue_ack.mutex);
    // Cleanup
    closesocket(client->socket);
    cleanup_winsock();
}
// --- Main function ---
int main() {
    WSADATA wsaData;
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    char out_message[255];

    init_client(&client);
    start_threads(&client);

    int i = 0;
    while(i < 3 && client.client_status == CLIENT_READY){
        
        if(client.session_status == DISCONNECTED){

            send_connect_request(++client.frame_count, client.client_id, client.flags, client.client_name, client.socket, &client.server_addr);
            printf("Attempting to connect to server...\n");
            Sleep(1000);
            continue; // Wait for the connection to be established
        }

        // printf("\nMessage to send:");
        // fgets(out_message, sizeof(out_message), stdin);
        out_message[strcspn(out_message, "\n")] = '\0';
        out_message[0] = '\0'; // Clear the buffer
        strcpy(out_message, "client message"); // Clear the buffer
        send_text_message(++client.frame_count, client.session_id, out_message, strlen(out_message), client.socket, &client.server_addr);
        i++;
        Sleep(10); // Simulate some delay between messages
        
    }
 
    while (client.session_status == CONNECTED) {

        printf("Press 'q' to quit...\n");
        char c = getchar();
        if (c == 'q' || c == 'Q') {
            client.session_status = DISCONNECTED;
            client.client_status = CLIENT_STOP; // Signal threads to stop
            break;
        }
        Sleep(10); // Prevent busy-waiting
    }
    shutdown_client(&client);
    return 0;
}

// --- Function implementations ---
// --- Send connect request ---
void send_connect_request(const uint32_t seq_nr, const uint32_t client_id, const uint32_t flags, const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = htonl(seq_nr);
    frame.header.session_id = 0;
    frame.payload_data.request.client_id = htonl(client_id);
    frame.payload_data.request.flags = flags;

    strncpy(frame.payload_data.request.client_name, client_name, NAME_SIZE - 1);
    frame.payload_data.request.client_name[NAME_SIZE - 1] = '\0';

    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return;
    }
    log_sent_frame(&frame, dest_addr);
    return;
};
// --- Send text message ---
void send_text_message(const uint32_t seq_nr, const uint32_t session_id, const char* text, const uint32_t len, const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(text == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    frame.header.seq_num = htonl(seq_nr);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload_data.text_msg.text_len = htonl(len);
    // Copy the text data into the payload
    strncpy(frame.payload_data.text_msg.text_data, text, len);
    frame.payload_data.text_msg.text_data[len] = '\0';
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(TextPayload)));  
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return;
    }
    log_sent_frame(&frame, dest_addr);
    return;
};

// --- Receive frame ---
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {
    
    StructClient *client = (StructClient *)lpParam;
 
    UdpFrame received_frame;
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);
   
    DWORD timeout = RECV_TIMEOUT_MS;
    if (setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }

    while (client->client_status == CLIENT_READY) {
        int bytes_received = recvfrom(client->socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);

        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECV_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
                // For critical errors, you might set 'running = 0;' here to shut down the server.
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
            FrameData frame_data;
            memset(&frame_data, 0, sizeof(FrameData));
            memcpy(&frame_data.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_data.src_addr, &src_addr, sizeof(struct sockaddr_in));          
            frame_data.bytes_received = bytes_received;
            push_frame_queue(&client->frame_queue, &frame_data);
        }
    }
    fprintf(stdout, "Receive frame thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam) {

    StructClient *client = (StructClient *)lpParam;

    while(client->client_status == CLIENT_READY){

        if(client->frame_queue.head == client->frame_queue.tail) {
            Sleep(10); // No frames to process, yield CPU
            continue; // Re-check the queue after waking up
        }
          
        FrameData frame_data;
        pop_frame_queue(&client->frame_queue, &frame_data);     

        UdpFrame *frame = &frame_data.frame;
        struct sockaddr_in *src_addr = &frame_data.src_addr;
        uint32_t bytes_received = frame_data.bytes_received;

        // Extract header fields   
        uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
        uint8_t  received_frame_type = frame->header.frame_type;
        uint32_t received_seq_num = ntohl(frame->header.seq_num);

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        uint16_t src_port = ntohs(src_addr->sin_port);

        // 1. Validate Delimiter
        if (received_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n",
                    src_ip, src_port, received_delimiter);
            continue;
        }
        // 2. Validate Checksum
        if (!is_checksum_valid(frame, bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n",
                    src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }
        // Log the received frame
        log_recv_frame(frame, src_addr);
        // Find or add client (thread-safe access)

        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE: {
                uint32_t session_id = ntohl(frame->header.session_id);
                uint8_t server_status = frame->payload_data.response.server_status;
                if(session_id == 0 || server_status == 0){
                    fprintf(stderr, "Session not established!\n");
                    client->session_status = DISCONNECTED;
                    break;
                }
                client->session_id = session_id;
                client->server_status = frame->payload_data.response.server_status;
                client->session_timeout = frame->payload_data.response.session_timeout;
                uint32_t len = sizeof(frame->payload_data.response.server_name) - 1;
                strncpy(client->server_name, frame->payload_data.response.server_name, len);
                client->server_name[len] = '\0';
                client->session_status = CONNECTED;
                push_queue(&client->queue_ack, received_seq_num, &client->queue_ack.mutex); // Add the sequence number to the ACK queue
                break; 
            }

            case FRAME_TYPE_ACK: {
                uint32_t ack_seq_num = ntohl(frame->payload_data.ack_nack.ack_seq_num);
                uint8_t flags = frame->payload_data.ack_nack.flags;
                break;
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
    fprintf(stdout, "Process frame thread exiting...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Send Ack ---
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam){

    StructClient *client = (StructClient *)lpParam;

    while(client->client_status == CLIENT_READY){
        if(client->queue_ack.head == client->queue_ack.tail){
            Sleep(10); // No ACKs to send, yield CPU
            continue; // Re-check the queue after waking up
        }
        uint32_t ack_seq_num = client->queue_ack.buffer[client->queue_ack.head];
        if(pop_queue(&client->queue_ack, &client->queue_ack.mutex) == -1) {
            fprintf(stderr,"ACK queue is empty nothing to remove\n");
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, client->session_id, client->socket, &client->server_addr);
    }
    fprintf(stdout, "Send ack thread exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
