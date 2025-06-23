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

    HANDLE frame_event;                     // Event to signal when a new frame is received for processing
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

// --- File Transfer Management ---
#define MAX_FILE_TRANSFERS 5
typedef struct {
    uint32_t file_id;                   // Unique identifier for the file transfer session
    struct sockaddr_in client_address; // Source client address
    char filename[256];
    size_t total_size;
    size_t received_bytes;
    uint8_t expected_file_hash[16];
    FILE *temp_file_handle;        // Pointer to the temp file being written
    char temp_filepath[MAX_PATH];  // Path to the temporary file
    uint32_t last_received_fragment_seq; // Last sequence number of a file fragment received for this transfer
    // A more advanced system would use a bitmap/array to track received fragments
    // for selective repeat and ACK generation.
    time_t last_fragment_time; // To detect stalled transfers and clean up
} FileTransferContext;

FileTransferContext active_file_transfers[MAX_FILE_TRANSFERS];
int num_active_file_transfers = 0;
CRITICAL_SECTION file_transfer_mutex; // For thread-safe access to active_file_transfers
volatile uint32_t next_file_id = 1; // Global counter for unique file IDs

// --- Function Prototypes ---
void init_winsock();
void cleanup_winsock();
// Frame handling functions

// UDP communication functions
int send_connect_response(StructClient *client);
void send_file_transfer_status(const struct sockaddr_in *dest_addr, uint32_t file_id, const uint8_t *final_hash, BOOL success);
void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *sender_addr);

// Thread functions
unsigned int WINAPI receive_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);

// Client management functions
StructClient* find_client(const uint32_t session_id);
StructClient* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *addr);
void remove_client(const struct sockaddr_in *addr);
void manage_clients_and_transfers();

// File transfer management functions
FileTransferContext* find_file_transfer(uint32_t file_id);
FileTransferContext* start_new_file_transfer(const struct sockaddr_in *sender_addr, const char *filename, uint32_t total_size, const uint8_t *file_hash);
void finalize_file_transfer(FileTransferContext *ctx);
void remove_file_transfer(uint32_t file_id);

// --- Main Server Function ---
int main() {
    init_winsock();
    InitializeCriticalSection(&client_list_mutex);
    InitializeCriticalSection(&file_transfer_mutex);

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

    HANDLE hRecvThread = (HANDLE)_beginthreadex(NULL, 0, receive_thread_func, NULL, 0, NULL);
    if (hRecvThread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        running = 0; // Signal immediate shutdown
    }

    // Main server loop for general management, timeouts, and state updates
    while (running) {
        manage_clients_and_transfers(); // Periodic maintenance
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
    DeleteCriticalSection(&file_transfer_mutex);
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
// Sends a UDP frame to a destination address
// Sends an RESPONSE frame back to the sender
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
void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *sender_addr) {
    if (bytes_received < sizeof(FrameHeader)) {
        fprintf(stderr, "Received frame too small from %s:%d. Discarding.\n",
                inet_ntoa(sender_addr->sin_addr), ntohs(sender_addr->sin_port));
        return; // Frame too small to be valid
    }
    // Extract header fields   
    uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
    uint8_t  received_frame_type = frame->header.frame_type;
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
    // 2. Validate Checksum
    if (!is_checksum_valid(frame, bytes_received)) {
        fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n",
                sender_ip, sender_port);
        // Optionally send ACK for checksum mismatch if this is part of a reliable stream
        // For individual datagrams, retransmission is often handled by higher layers or ignored.
        return;
    }
    // Log the received frame
    log_recv_frame(frame, sender_addr);
    // Find or add client (thread-safe access)
    StructClient *client = NULL;
    if(received_frame_type == FRAME_TYPE_CONNECT_REQUEST){
        client = add_client(frame, sender_addr);
        if (client == NULL) {
            fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", sender_ip, sender_port);
            // Optionally send NACK indicating server full
            return; // Do not process further if client addition failed
        }
    } else {
        uint32_t received_session_id = ntohl(frame->header.session_id);
        client = find_client(received_session_id);
        if (client == NULL) {
            fprintf(stderr, "Received frame from unknown client %s:%d (Type: %u, Seq: %u). Ignoring.\n",
                    sender_ip, sender_port, received_frame_type, received_seq_num);
            // Do not send ACK/NACK to unknown clients
            return;
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
            uint32_t msg_id = ntohl(frame->payload_data.long_text_msg.message_id);
            uint32_t total_msg_len = ntohl(frame->payload_data.long_text_msg.total_message_len);
            uint32_t offset = ntohl(frame->payload_data.long_text_msg.fragment_offset);
            uint32_t payload_len = ntohl(frame->payload_data.long_text_msg.payload_len); // Changed to uint32_t
            printf("\n[LONG TEXT DATA from %s:%d] Seq: %u, MsgID: %u, Offset: %u, Len: %u\n",
                    sender_ip, sender_port, received_seq_num, msg_id, offset, payload_len);
            // TODO: Implement logic to buffer and reassemble long text messages.
            // This would involve a data structure similar to FileTransferContext
            // for tracking each ongoing long message assembly, along with a reassembly buffer.
            break;
        }
        case FRAME_TYPE_DISCONNECT: {
            printf("[DISCONNECT from %s:%d] Client disconnected.\n", sender_ip, sender_port);
            remove_client(sender_addr); // Remove client from active list
            break;
        }
        case FRAME_TYPE_KEEP_ALIVE: {
            printf("[KEEP_ALIVE from %s:%d] Seq: %u\n", sender_ip, sender_port, received_seq_num);
            // Client's last_activity_time already updated by find_client/add_client logic.
            break;
        }
 
        default:
            fprintf(stderr, "Received unhandled frame type %u from %s:%d. Seq: %u\n",
                    received_frame_type, sender_ip, sender_port, received_seq_num);
            break;
    }
}
// --- Receive Thread Function ---
unsigned int WINAPI receive_thread_func(LPVOID lpParam) {
    UdpFrame received_frame;
    struct sockaddr_in sender_addr;
    int sender_addr_len = sizeof(sender_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECV_TIMEOUT_MS;
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (running) {
        int bytes_received = recvfrom(server_socket, (char*)&received_frame, sizeof(UdpFrame), 0,
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
// Remove a client
void remove_client(const struct sockaddr_in *addr) {
    EnterCriticalSection(&client_list_mutex);
    // for (int i = 0; i < num_connected_clients; i++) {
    //     if (connected_clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
    //         connected_clients[i].addr.sin_port == addr->sin_port) {
    //         // Shift elements to fill the gap
    //         for (int j = i; j < num_connected_clients - 1; j++) {
    //             connected_clients[j] = connected_clients[j + 1];
    //         }
    //         num_connected_clients--;
    //         char ip_str[INET_ADDRSTRLEN];
    //         inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    //         printf("Client %s:%d removed.\n", ip_str, ntohs(addr->sin_port));
    //         CloseHandle(&connected_clients[i].queue_mutex);
    //         CloseHandle(&connected_clients[i].queue_event);
    //         break;
    //     }
    // }
    LeaveCriticalSection(&client_list_mutex);
}
// // --- Periodic Server Management ---
 void manage_clients_and_transfers() {
    time_t current_time = time(NULL);

    // Manage clients
    EnterCriticalSection(&client_list_mutex);
    for (int i = 0; i < num_connected_clients; /* no increment here */) {
        if (current_time - connected_clients[i].last_activity_time > CLIENT_TIMEOUT_SEC) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(connected_clients[i].addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            printf("Client %s:%d timed out (inactive for %d seconds). Disconnecting.\n",
                   ip_str, ntohs(connected_clients[i].addr.sin_port), CLIENT_TIMEOUT_SEC);
            remove_client(&connected_clients[i].addr); // This will decrement num_connected_clients and adjust 'i'
        } else {
            i++; // Only increment if client is not removed
        }
    }
    LeaveCriticalSection(&client_list_mutex);

}