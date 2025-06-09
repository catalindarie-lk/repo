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
#define SERVER_NAME         "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS 10

typedef uint8_t ServerStatus;
enum ServerStatus {
    SERVER_STOP = 0,
    SERVER_READY = 1,
    SERVER_ERROR = 2,
    SERVER_BUSY = 3
};

typedef struct {
    struct sockaddr_in client_addr;                // Client's address
    uint32_t client_id;                     // Unique ID received from the client
    char client_name[NAME_SIZE];                   // Optional: human-readable identifier received from the client
    uint8_t flags;                          // Flags received from the client (e.g., protocol version, capabilities)
    uint32_t client_to_srv_frame_count;     // Frame count sent by the client to the server

    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)
    uint32_t srv_to_client_frame_count;     // Frame count sent by the server to the client               

    Queue queue_ack;                        // Queue for frames received from the client that the server needs to ACK   

    // In a real system, you'd add:
    // - A queue for outgoing reliable packets (with retransmission info)
    // - A receive buffer for out-of-order packets (for sliding window)
} StructClient;

typedef struct{
    SOCKET socket;                      // Global server socket for receiving and sending UDP frames
    struct sockaddr_in server_addr;            // Server address structure
    ServerStatus server_status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    uint32_t clients_count;             // Number of currently connected clients
    uint32_t unique_session_id;         // Global counter for unique session IDs
    char server_name[NAME_SIZE];        // Human-readable server name

    HANDLE receive_frame_thread;
    HANDLE process_frame_thread;   
    HANDLE send_ack_thread[MAX_CLIENTS];
    
    FrameQueue frame_queue;

    StructClient active_clients[MAX_CLIENTS];      // Array of connected clients
    CRITICAL_SECTION active_clients_mutex;         // For thread-safe access to connected_clients

}StructServer;

StructServer server;

// UDP communication functions
void send_connect_response(const uint_least32_t seq_nr, const uint32_t session_id, const uint32_t session_timeout, const uint8_t server_status, const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);

// Client management functions
StructClient* find_client(const uint32_t session_id);
StructClient* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);

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
// Server initialization and management functions
void init_server(StructServer *server){
    // Initialize server information
    memset(server, 0, sizeof(StructServer));

    // 1. Create Socket
    server->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const char *server_ip = "127.0.0.1"; // IPv4 example
    if (server->socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        cleanup_winsock();
        return;
    }
    // 2. Set up the server address structure
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server->server_addr.sin_addr);
    // 3. Bind the socket
    int iResult = bind(server->socket, (SOCKADDR*)&server->server_addr, sizeof(server->server_addr));
    if (iResult == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server->socket);
        cleanup_winsock();
        return;
    }
    // 4. Initialize server status
    server->session_timeout = CLIENT_TIMEOUT_SEC;
    server->clients_count = 0;
    server->unique_session_id = 0xFF; // Start with 0 or a random value
    strncpy(server->server_name, SERVER_NAME, NAME_SIZE - 1);
    server->server_name[NAME_SIZE - 1] = '\0'; // Ensure null-termination
    server->server_status = SERVER_READY; // Set initial status to ready

    InitializeCriticalSection(&server->active_clients_mutex);

    server->frame_queue.head = 0;
    server->frame_queue.tail = 0;
    InitializeCriticalSection(&server->frame_queue.mutex);

    printf("Server listening on port %d...\n", SERVER_PORT);

    return; // Indicate success
}
// --- Start server threads ---
void start_threads(StructServer *server) {
    // Create threads for receiving and processing frames
    server->receive_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, server, 0, NULL);
    if (server->receive_frame_thread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        return;
    }
    server->process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, server, 0, NULL);
    if (server->process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process thread. Error: %d\n", GetLastError());
        return;
    }
    return;
}
// --- Server shutdown ---
void shutdown_server(StructServer *server) {

    server->server_status = SERVER_STOP;

    if (server->receive_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(server->receive_frame_thread, INFINITE);
        CloseHandle(server->receive_frame_thread);
    }
    if (server->process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(server->process_frame_thread, INFINITE);
        CloseHandle(server->process_frame_thread);
    } 

    DeleteCriticalSection(&server->active_clients_mutex);
    DeleteCriticalSection(&server->frame_queue.mutex);
    closesocket(server->socket);
    cleanup_winsock();
    printf("Server shut down cleanly.\n");
}
// --- Main server function ---
int main() {
    init_winsock();
    

    init_server(&server);
    start_threads(&server);
    // Main server loop for general management, timeouts, and state updates
    while (server.server_status == SERVER_READY) {

        printf("Press 'q' to quit...\n");
        char c = getchar();
        if (c == 'q' || c == 'Q') {
            server.server_status = SERVER_STOP; // Signal threads to stop
            break;
        }
        Sleep(10); // Prevent busy-waiting
    }
    shutdown_server(&server);
    // --- Server Shutdown Sequence ---
    return 0;
}

// --- Send connect response ---
void send_connect_response(const uint_least32_t seq_nr, const uint32_t session_id, const uint32_t session_timeout, const uint8_t server_status, const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = htonl(seq_nr);
    frame.header.session_id = htonl(session_id); // Use client's session ID

    frame.payload.response.session_timeout = htonl(session_timeout);
    frame.payload.response.server_status = server_status;

    strncpy(frame.payload.response.server_name, server_name, NAME_SIZE - 1);
    frame.payload.response.server_name[NAME_SIZE - 1] = '\0';

    // Calculate CRC32 for the ACK frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "send_connect_respose() failed\n");
        return;
    }
    log_sent_frame(&frame, dest_addr);
    return;
}

// Find client by session ID
StructClient* find_client(const uint32_t session_id) {
    EnterCriticalSection(&server.active_clients_mutex);
    // Search for the client with the given session ID
    if (server.clients_count == 0) {
        LeaveCriticalSection(&server.active_clients_mutex);
        return NULL; // No clients connected
    }
    for (int i = 0; i < server.clients_count; i++) {
        if(server.active_clients[i].session_id == session_id){
            LeaveCriticalSection(&server.active_clients_mutex);
            return &server.active_clients[i];
        }
    }
    LeaveCriticalSection(&server.active_clients_mutex);
    return NULL;
}
// Add a new client
StructClient* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    // Assumes client_list_mutex is locked by caller
    EnterCriticalSection(&server.active_clients_mutex);
    if (server.clients_count >= MAX_CLIENTS) {
        fprintf(stderr, "Max clients reached. Cannot add new client.\n");
        return NULL;
    }
    
    StructClient *new_client = &server.active_clients[server.clients_count];
    memset(new_client, 0, sizeof(StructClient));
    memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
    new_client->last_activity_time = time(NULL);
    new_client->client_id = ntohl(recv_frame->payload.request.client_id); // Simple ID assignment
    new_client->session_id = ++server.unique_session_id; // Assign a unique session ID based on current count
    new_client->flags = recv_frame->payload.request.flags;
    new_client->srv_to_client_frame_count = 0;
 
    strncpy(new_client->client_name, recv_frame->payload.request.client_name, sizeof(NAME_SIZE - 1));
    new_client->client_name[NAME_SIZE - 1] = '\0';

    printf("\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port), new_client->session_id);

    // Initialize queues and mutexes for the new client
    memset(&new_client->queue_ack, 0, sizeof(Queue));
    InitializeCriticalSection(&new_client->queue_ack.mutex);
    server.send_ack_thread[server.clients_count] = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, new_client, 0, NULL);

    server.clients_count++;
    LeaveCriticalSection(&server.active_clients_mutex);
    return new_client;
}

// --- Receive Thread Function ---
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {
    
    StructServer *server = (StructServer *)lpParam;

    UdpFrame received_frame;
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECV_TIMEOUT_MS;
    if (setsockopt(server->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (server->server_status == SERVER_READY) {
        int bytes_received = recvfrom(server->socket, (char*)&received_frame, sizeof(UdpFrame), 0,
                                      (SOCKADDR*)&src_addr, &src_addr_len);
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
            if(push_frame(&server->frame_queue, &frame_data) == -1){
                continue;
            };
        }
    }
    printf("Exit receive_frame_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam) {

    StructServer *server = (StructServer *)lpParam;

    while(server->server_status == SERVER_READY) {

        if(server->frame_queue.head == server->frame_queue.tail) {
            Sleep(10); // No frames to process, yield CPU
            continue; // Re-check the queue after waking up
        }
        // Pop a frame from the queue
        FrameData frame_data;
        
        if(pop_frame(&server->frame_queue, &frame_data) == -1){
            continue;
        };

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
        StructClient *client = NULL;
        if(received_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = add_client(frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                continue; // Do not process further if client addition failed
            }
            // Send a connect response to the new client               
            send_connect_response(++client->srv_to_client_frame_count, client->session_id, server->session_timeout, server->server_status, server->server_name, server->socket, &client->client_addr);
            // push received sequence number to ACK queue so it can be acknowledged by the send_ack_thread    
            if(push_seq_num(&client->queue_ack, received_seq_num) == -1){
                continue;
            };    
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
                // allready treated
                break;
            }
            case FRAME_TYPE_ACK: {
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
            }
            case FRAME_TYPE_TEXT_MESSAGE: {
                uint32_t text_len = ntohl(frame->payload.text_msg.text_len);
                // Ensure null termination for safe printing, cap at max payload size
                if (text_len >= sizeof(frame->payload.text_msg.text_data)) {
                    text_len = sizeof(frame->payload.text_msg.text_data) - 1;
                }
                frame->payload.text_msg.text_data[text_len] = '\0';
                // push received sequence number to ACK queue so it can be acknowledged by the send_ack_thread
                if(push_seq_num(&client->queue_ack, received_seq_num) == -1){
                    break;
                };
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
    printf("Exit process_frame_thread...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- SendAck Thread Function ---
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam){

    StructClient *client = (StructClient *)lpParam;

    while(server.server_status == SERVER_READY) {
        if(client->queue_ack.head == client->queue_ack.tail) {
            Sleep(10); // No ACKs to send, yield CPU
            continue; // Re-check the queue after waking up
        }
        uint32_t ack_seq_num = client->queue_ack.buffer[client->queue_ack.head];
        if(pop_seq_num(&client->queue_ack) == -1) {
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, client->session_id, server.socket, &client->client_addr);
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
