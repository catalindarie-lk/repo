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
#define RECV_TIMEOUT_MS     100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_TIMEOUT_SEC  3000        // Seconds after which an inactive client is considered disconnected
#define FILE_TRANSFER_TIMEOUT_SEC 60    // Seconds after which a stalled file transfer is cleaned up
#define SERVER_NAME         "lkdc UDP Text/File Transfer Server"

typedef uint8_t ServerStatus;
enum ServerStatus {
    SERVER_BUSY = 0,
    SERVER_READY = 1,
    SERVER_ERR = 2
};

// --- Server Global State ---
SOCKET server_socket;       // Global server socket for receiving and sending UDP frames
volatile int running = 1; // Flag to control server loops

// --- Client Management ---
#define MAX_CLIENTS 10


typedef struct {
    struct sockaddr_in addr;                // Client's address
    uint32_t client_id;                     // Unique ID received from the client
    char client_name[64];                   // Optional: human-readable identifier received from the client
    uint8_t flags;                          // Flags received from the client (e.g., protocol version, capabilities)
    uint32_t client_to_srv_frame_count;     // Frame count sent by the client to the server

    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)
    uint32_t srv_to_client_frame_count;     // Frame count sent by the server to the client               

    Queue queue_ack;                        // Queue for frames received from the client that the server needs to ACK   
    CRITICAL_SECTION queue_mutex;           // Mutex for thread-safe access to the queue
    HANDLE queue_event;                     // Event to signal when there are frames to ACK in the queue    

    // In a real system, you'd add:
    // - A queue for outgoing reliable packets (with retransmission info)
    // - A receive buffer for out-of-order packets (for sliding window)
} StructClient;

StructClient connected_clients[MAX_CLIENTS];  // Array of connected clients
int num_connected_clients = 0;              // Number of currently connected clients
int session_id_counter = 0xFF;              // Global counter for unique session IDs
CRITICAL_SECTION client_list_mutex;         // For thread-safe access to connected_clients

HANDLE send_ack_thread[MAX_CLIENTS];
HANDLE process_frame_thread[MAX_CLIENTS];

typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t bytes_received; // Number of bytes received for this frame
}RecvFrameInfo;

typedef struct {
    RecvFrameInfo buffer[QUEUE_BUFFER_SIZE];
    uint32_t head;          
    uint32_t tail;
    HANDLE frame_event; // Event to signal when a new frame is available in the queue
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
} FrameQueue;
FrameQueue frame_queue;

// --- Function Prototypes ---
void init_winsock();
void cleanup_winsock();
// Frame handling functions

// UDP communication functions
int send_connect_response(StructClient *client);
void send_file_transfer_status(const struct sockaddr_in *dest_addr, uint32_t file_id, const uint8_t *final_hash, BOOL success);
void push_frame_queue(FrameQueue *queue, RecvFrameInfo *recv_frame_info);
RecvFrameInfo pop_frame_queue(FrameQueue *queue);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);

// Client management functions
StructClient* find_client(const uint32_t session_id);
StructClient* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *addr);

// --- Main Server Function ---
int main() {
    init_winsock();
    InitializeCriticalSection(&client_list_mutex);

    // Initialize the frame queue buffer
    frame_queue.head = 0;
    frame_queue.tail = 0;
    InitializeCriticalSection(&frame_queue.mutex);
    frame_queue.frame_event = CreateEvent(NULL, FALSE, FALSE, NULL); // Manual reset event for frame queue

    // 1. Create Socket
    server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const char *server_ip = "127.0.0.1"; // IPv4 example
    if (server_socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        cleanup_winsock();
        return 1;
    }

    // 2. Set up server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);


    // 3. Bind the socket
    int iResult = bind(server_socket, (SOCKADDR*)&server_addr, sizeof(server_addr));
    if (iResult == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        cleanup_winsock();
        return 1;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    HANDLE hRecvThread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, &frame_queue, 0, NULL);
    if (hRecvThread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        running = 0; // Signal immediate shutdown
    }
    HANDLE hProcessThread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, &frame_queue, 0, NULL);
    if (hRecvThread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        running = 0; // Signal immediate shutdown
    }


    // Main server loop for general management, timeouts, and state updates
    while (running) {
        Sleep(100); // Prevent busy-waiting, yield CPU
    }

    // --- Server Shutdown Sequence ---
    printf("Server shutting down...\n");
    if (hRecvThread) {
        // Signal the receive thread to stop and wait for it to finish
        running = 0;
        WaitForSingleObject(hRecvThread, INFINITE);
        CloseHandle(hRecvThread);
    }

    
    closesocket(server_socket);
    cleanup_winsock();

    DeleteCriticalSection(&client_list_mutex);

    DeleteCriticalSection(&frame_queue.mutex);
    CloseHandle(frame_queue.frame_event);

    printf("Server shut down cleanly.\n");
    return 0;
}
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

int send_connect_response(StructClient *client) {
    UdpFrame response_frame;
    // Initialize the response frame
    memset(&response_frame, 0, sizeof(response_frame));
    // Set the header fields
    response_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    response_frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;
    response_frame.header.seq_num = htonl(++client->srv_to_client_frame_count);
    response_frame.header.session_id = htonl(client->session_id); // Use client's session ID
    
    response_frame.payload_data.response.session_timeout = htonl(CLIENT_TIMEOUT_SEC);
    response_frame.payload_data.response.server_status = SERVER_READY; // Assuming server is ready

    uint32_t len = sizeof(SERVER_NAME) - 1;
    strncpy(response_frame.payload_data.response.server_name, SERVER_NAME, len);
    response_frame.payload_data.response.server_name[len] = '\0'; // Ensure null-termination

    // Calculate CRC32 for the ACK frame
    response_frame.header.checksum = htonl(calculate_crc32(&response_frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));
    // Send the response frame
    int bytes_sent = send_frame(&response_frame, server_socket, &client->addr);
    if(bytes_sent == SEND_FRAME_ERROR) {
    fprintf(stderr, "Failed to send connect response to client %s:%d\n",
            inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));
        --client->srv_to_client_frame_count;          // Decrement frame count on failure
        return SEND_FRAME_ERROR;    
    }
    log_sent_frame(&response_frame, &client->addr);
    //TODO: Add the response frame seq_num to a queue of sent frames that need acknowledgment
    return bytes_sent;
}

// Processes a received UDP frame
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam) {

    FrameQueue *frame_queue = (FrameQueue *)lpParam;

    while(running){

        WaitForSingleObject(frame_queue->frame_event, INFINITE); // Wait for a frame to be available
        RecvFrameInfo recv_frame_info = pop_frame_queue(frame_queue);

        UdpFrame *frame = &recv_frame_info.frame;
        struct sockaddr_in *src_addr = &recv_frame_info.src_addr;
        uint32_t bytes_received = recv_frame_info.bytes_received;

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
        StructClient *client = NULL;
        if(received_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = add_client(frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                continue; // Do not process further if client addition failed
            }
        } else {
            uint32_t received_session_id = ntohl(frame->header.session_id);
            client = find_client(received_session_id);
            if (client == NULL) {
                fprintf(stderr, "Received frame from unknown client %s:%d (Type: %u, Seq: %u). Ignoring.\n",
                        src_ip, src_port, received_frame_type, received_seq_num);
                // Do not send ACK/NACK to unknown clients
                continue;
            }

        }

        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST: {
                // Send a connect response to the new client               
                send_connect_response(client);
                // push received sequence number to ACK queue so it can be acknowledged by the send_ack_thread    
                push_queue(&client->queue_ack, received_seq_num, &client->queue_mutex);           
                SetEvent(client->queue_event);    // Signal the send_ack_thread to process the ACK queue
                break;
            }
            case FRAME_TYPE_ACK: {
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
            }
            case FRAME_TYPE_TEXT_MESSAGE: {
                uint32_t text_len = ntohl(frame->payload_data.text_msg.text_len);
                // Ensure null termination for safe printing, cap at max payload size
                if (text_len >= sizeof(frame->payload_data.text_msg.text_data)) {
                    text_len = sizeof(frame->payload_data.text_msg.text_data) - 1;
                }
                frame->payload_data.text_msg.text_data[text_len] = '\0';
                // push received sequence number to ACK queue so it can be acknowledged by the send_ack_thread
                push_queue(&client->queue_ack, received_seq_num, &client->queue_mutex);
                SetEvent(client->queue_event);    // Signal the send_ack_thread to process the ACK queue
                // TODO: Route message to another client if specified
                break;
            }
            case FRAME_TYPE_LONG_TEXT_MESSAGE_DATA: {
                break;
            }
            case FRAME_TYPE_DISCONNECT: {
                break;
            }
            case FRAME_TYPE_KEEP_ALIVE: {
                break;
            } 
            default:
                break;
        }

    }
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Receive Thread Function ---
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {
    
    FrameQueue *frame_queue = (FrameQueue *)lpParam;
 
    UdpFrame received_frame;
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECV_TIMEOUT_MS;
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (running) {
        int bytes_received = recvfrom(server_socket, (char*)&received_frame, sizeof(UdpFrame), 0,
                                      (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECV_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
                // For critical errors, you might set 'running = 0;' here to shut down the server.
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
            RecvFrameInfo received_frame_info;
            memset(&received_frame_info, 0, sizeof(RecvFrameInfo));
            memcpy(&received_frame_info.frame, &received_frame, sizeof(UdpFrame));
            
            memcpy(&received_frame_info.src_addr, &src_addr, sizeof(struct sockaddr_in));
            
            received_frame_info.bytes_received = bytes_received;
            // printf("Received frame of type %u from %s:%d bytes received:%d\n",
            //        received_frame.header.frame_type,
            //        inet_ntoa(received_frame_info.src_addr.sin_addr), ntohs(received_frame_info.src_addr.sin_port), received_frame_info.bytes_received);
            // log_recv_frame(&received_frame_info.frame, &src_addr);
            push_frame_queue(frame_queue, &received_frame_info);
            SetEvent(frame_queue->frame_event); // Signal that a new frame is available

        }
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- SendAck Thread Function ---
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam){

    StructClient *client = (StructClient *)lpParam;

    while(running) {
        WaitForSingleObject(client->queue_event, INFINITE);
        uint32_t ack_seq_num = client->queue_ack.buffer[client->queue_ack.head];
        if(pop_queue(&client->queue_ack, &client->queue_mutex) == -1) {
            fprintf(stderr,"ACK queue is empty nothing to remove\n");
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, client->session_id, server_socket, &client->addr);
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Client Management Implementation Details ---
// Find client by address
StructClient* find_client(const uint32_t session_id) {
    EnterCriticalSection(&client_list_mutex);
    // Search for the client with the given session ID
    if (num_connected_clients == 0) {
        LeaveCriticalSection(&client_list_mutex);
        return NULL; // No clients connected
    }
    for (int i = 0; i < num_connected_clients; i++) {
        if(connected_clients[i].session_id == session_id){
            LeaveCriticalSection(&client_list_mutex);
            return &connected_clients[i];
        }
    }
    LeaveCriticalSection(&client_list_mutex);
    return NULL;
}
// Add a new client
StructClient* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *addr) {
    // Assumes client_list_mutex is locked by caller
    EnterCriticalSection(&client_list_mutex);
    if (num_connected_clients >= MAX_CLIENTS) {
        fprintf(stderr, "Max clients reached. Cannot add new client.\n");
        return NULL;
    }
    
    StructClient *new_client = &connected_clients[num_connected_clients];
    memset(new_client, 0, sizeof(StructClient));
    memcpy(&new_client->addr, addr, sizeof(struct sockaddr_in));
    new_client->last_activity_time = time(NULL);
    new_client->client_id = ntohl(recv_frame->payload_data.request.client_id); // Simple ID assignment
    new_client->session_id = ++session_id_counter; // Assign a unique session ID based on current count
    new_client->flags = recv_frame->payload_data.request.flags;
    new_client->srv_to_client_frame_count = 0;
    uint32_t len = sizeof(recv_frame->payload_data.request.client_name) - 1;
    strncpy(new_client->client_name, recv_frame->payload_data.request.client_name, len);
    new_client->client_name[len] = '\0';

    printf("\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), new_client->session_id);

    // Initialize queues and mutexes for the new client
    memset(&new_client->queue_ack, 0, sizeof(Queue));
    new_client->queue_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&new_client->queue_mutex);
    send_ack_thread[num_connected_clients] = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, new_client, 0, NULL);

    num_connected_clients++;
    LeaveCriticalSection(&client_list_mutex);
    return new_client;
}

void push_frame_queue(FrameQueue *queue, RecvFrameInfo *recv_frame_info){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return;
    }
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_BUFFER_SIZE == queue->head){
        printf("Queue Full\n");
        return;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    EnterCriticalSection(&queue->mutex);
    // Add the sequence number to the ACK queue 
    memcpy(&queue->buffer[queue->tail], recv_frame_info, sizeof(RecvFrameInfo)); // Copy the frame to the queue
    // Move the tail index forward    
    ++queue->tail;
    queue->tail %= QUEUE_BUFFER_SIZE;
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return;
}
// Removes element from queue head
RecvFrameInfo pop_frame_queue(FrameQueue *queue){       
    // Check if the queue is initialized

    RecvFrameInfo recv_frame_info;
    memset(&recv_frame_info, 0, sizeof(RecvFrameInfo)); // Initialize the structure to zero

    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Queue or mutex is not initialized.\n");
        return recv_frame_info; // Return an empty RecvFrameInfo
    }
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        printf("Frame queue is empty nothing to remove\n");
        return recv_frame_info;
    }    
    // Acquire the mutex to ensure thread-safe access to the queue
    EnterCriticalSection(&queue->mutex);
    // Remove the sequence number from the ACK queue
      
    memcpy(&recv_frame_info, &queue->buffer[queue->head], sizeof(RecvFrameInfo)); // Copy the frame from the queue
    memset(&queue->buffer[queue->head], 0, sizeof(RecvFrameInfo)); // Clear the frame at the head
    // Move the head index forward
    ++queue->head;
    queue->head %= QUEUE_BUFFER_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return recv_frame_info;
}