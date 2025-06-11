#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "UDP_lib.h"

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

// --- Constants ---
#define SERVER_PORT 12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS 100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC 30        // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS 20

typedef uint8_t ServerStatus;
enum ServerStatus {
    SERVER_STOP = 0,
    SERVER_BUSY = 2,
    SERVER_READY = 3,
    SERVER_ERROR = 4    
};

typedef uint8_t ClientConnection;
enum ClientConnection {
    CLIENT_DISCONNECTED = 0,
    CLIENT_CONNECTED = 1
};

typedef uint8_t ClientSlotStatus;
enum ClientSlotStatus {
    SLOT_FREE = 0,
    SLOT_BUSY = 1
};

typedef struct {  
    struct sockaddr_in client_addr;         // Client's address
    SOCKET server_socket;
    uint32_t client_id;                     // Unique ID received from the client
    char client_name[NAME_SIZE];            // Optional: human-readable identifier received from the client
    uint8_t flag;                          // Flags received from the client (e.g., protocol version, capabilities)
    uint32_t client_to_srv_frame_count;     // Frame count sent by the client to the server
    ClientConnection connection;
 

    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)
    uint32_t srv_to_client_frame_count;     // Frame count sent by the server to the client               

    QueueAck queue_ack;                      // Queue for frames received from the client that the server needs to ACK   

    uint32_t slot_num;
    ClientSlotStatus slot_status;            //0->FREE; 1->BUSY

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
    HANDLE client_timeout_thread; 
    HANDLE send_ack_thread[MAX_CLIENTS];
    
    QueueFrame queue_frame;

    StructClient client[MAX_CLIENTS];      // Array of connected clients 
    CRITICAL_SECTION client_mutex;         // For thread-safe access to connected_clients

    HANDLE server_command_thread;

}StructServer;

StructServer server;

// UDP communication functions
void send_connect_response(const uint32_t session_id, const uint32_t session_timeout, const uint8_t server_status, const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr);

// Thread functions
unsigned int WINAPI server_command_thread_func(LPVOID lpParam);
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);
unsigned int WINAPI client_timeout_thread_func(LPVOID lpParam);

// Client management functions
StructClient* find_client(StructServer *server, const uint32_t session_id);
StructClient* add_client(StructServer *server, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
uint32_t remove_client(StructServer *server, const uint32_t session_id);

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

    server->server_status = SERVER_BUSY;
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
    server->session_timeout = CLIENT_SESSION_TIMEOUT_SEC;
    server->clients_count = 0;
    server->unique_session_id = 0xFF; // Start with 0 or a random value
    strncpy(server->server_name, SERVER_NAME, NAME_SIZE - 1);
    server->server_name[NAME_SIZE - 1] = '\0'; // Ensure null-termination
    server->server_status = SERVER_READY; // Set initial status to ready

    InitializeCriticalSection(&server->client_mutex);

    server->queue_frame.head = 0;
    server->queue_frame.tail = 0;
    InitializeCriticalSection(&server->queue_frame.mutex);

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
    server->client_timeout_thread = (HANDLE)_beginthreadex(NULL, 0, client_timeout_thread_func, server, 0, NULL);
    if (server->client_timeout_thread == NULL) {
        fprintf(stderr, "Failed to create timeout thread. Error: %d\n", GetLastError());
        return;
    }
    server->server_command_thread = (HANDLE)_beginthreadex(NULL, 0, server_command_thread_func, server, 0, NULL);
    if (server->server_command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
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
    if (server->client_timeout_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(server->client_timeout_thread, INFINITE);
        CloseHandle(server->client_timeout_thread);
    }
    for(int i = 0; i < MAX_CLIENTS; i++){
        if (server->send_ack_thread[i]) {
            // Signal the receive thread to stop and wait for it to finish
            WaitForSingleObject(server->send_ack_thread[i], INFINITE);
            CloseHandle(server->send_ack_thread[i]);
        }
    }
    DeleteCriticalSection(&server->client_mutex);
    DeleteCriticalSection(&server->queue_frame.mutex);
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
void send_connect_response(const uint32_t session_id, const uint32_t session_timeout, const uint8_t server_status, const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = 0;
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
StructClient* find_client(StructServer *server, const uint32_t session_id) {
        
    // Search for the client with the given session ID
    if (server->clients_count == 0) {
        return NULL; // No clients connected
    }
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        if(server->client[slot].slot_status == SLOT_FREE) continue;
        if(server->client[slot].session_id == session_id){
            return &server->client[slot];
        }
    }
    return NULL;
}
// Add a new client
StructClient* add_client(StructServer *server, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    // Assumes client_list_mutex is locked by caller

    uint32_t free_slot = 0;
    while(free_slot < MAX_CLIENTS){
        if(server->client[free_slot].slot_status == SLOT_FREE) {
            break;
        }
        free_slot++;
    }
    if(free_slot >= MAX_CLIENTS){
        fprintf(stderr, "Max clients reached. Cannot add new client.\n");
        return NULL;
    }

    StructClient *new_client = &server->client[free_slot];
    memset(new_client, 0, sizeof(StructClient));
 
    new_client->slot_num = free_slot;
    new_client->slot_status = SLOT_BUSY;
    memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
    new_client->server_socket = server->socket;
    new_client->connection = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->client_id = ntohl(recv_frame->payload.request.client_id); // Simple ID assignment
    new_client->session_id = ++server->unique_session_id; // Assign a unique session ID based on current count
    new_client->flag = recv_frame->payload.request.flag;
    new_client->srv_to_client_frame_count = 0;
 
    strncpy(new_client->client_name, recv_frame->payload.request.client_name, sizeof(NAME_SIZE - 1));
    new_client->client_name[NAME_SIZE - 1] = '\0';

    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, addr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(client_addr->sin_port);
    fprintf(stdout, "[ADDING NEW CLIENT] %s:%d Session ID:%d\n", addr, port, new_client->session_id);

    // Initialize queues and mutexes for the new client
    memset(&new_client->queue_ack, 0, sizeof(QueueAck));
    InitializeCriticalSection(&new_client->queue_ack.mutex);
    server->send_ack_thread[free_slot] = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, new_client, 0, NULL);
    if (server->send_ack_thread[free_slot] == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        return NULL;
    }
    server->clients_count++;
    return &server->client[free_slot];
}
// Remove a client
uint32_t remove_client(StructServer *server, const uint32_t slot) {
    // Search for the client with the given session ID
    if (slot < 0 && slot >= MAX_CLIENTS) {
        return -1; 
    }
    memset(&server->client[slot], 0, sizeof(StructClient));
    server->client[slot].connection = CLIENT_DISCONNECTED;
    server->clients_count--;
    return 1;
}

// --- Process server command ---
unsigned int WINAPI server_command_thread_func(LPVOID lpParam){

    StructServer *server = (StructServer *)lpParam;

//    printf("Exit server_command_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Receive Thread Function ---
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {
    
    StructServer *server = (StructServer *)lpParam;

    UdpFrame received_frame;
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (server->server_status == SERVER_READY) {
        int bytes_received = recvfrom(server->socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECVFROM_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
            FrameData frame_data;
            memset(&frame_data, 0, sizeof(FrameData));
            memcpy(&frame_data.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_data.src_addr, &src_addr, sizeof(struct sockaddr_in));
            frame_data.bytes_received = bytes_received;
            if(push_frame(&server->queue_frame, &frame_data) == -1){
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

        if(server->queue_frame.head == server->queue_frame.tail) {
            Sleep(10); // No frames to process, yield CPU
            continue; // Re-check the queue after waking up
        }
        // Pop a frame from the queue
        FrameData frame_data;
        
        if(pop_frame(&server->queue_frame, &frame_data) == -1){
            continue;
        };

        UdpFrame *frame = &frame_data.frame;
        struct sockaddr_in *src_addr = &frame_data.src_addr;
        uint32_t bytes_received = frame_data.bytes_received;

        // Extract header fields   
        uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
        uint8_t  received_frame_type = frame->header.frame_type;
        uint32_t received_seq_num = ntohl(frame->header.seq_num);
        uint32_t received_session_id = ntohl(frame->header.session_id);

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        uint16_t src_port = ntohs(src_addr->sin_port);

        // 1. Validate Delimiter
        if (received_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, received_delimiter);
            continue;
        }
        // 2. Validate Checksum
        if (!is_checksum_valid(frame, bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }
        // Log the received frame
        log_recv_frame(frame, src_addr);
        // Find or add client (thread-safe access)
        StructClient *client = NULL;
        EnterCriticalSection(&server->client_mutex);

        if(received_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = find_client(server, received_session_id);
            if(client != NULL){
                fprintf(stdout, "Client already connected\n");
                LeaveCriticalSection(&server->client_mutex);
                continue;
            }
            client = add_client(server, frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                LeaveCriticalSection(&server->client_mutex);
                continue; // Do not process further if client addition failed
            }                      
        } else {
            client = find_client(server, received_session_id);
            if(client == NULL){
                fprintf(stdout, "Received frame from unknown %s:%d. Ignoring...\n", src_ip, src_port);
                LeaveCriticalSection(&server->client_mutex);
                continue;
            }
        }
        LeaveCriticalSection(&server->client_mutex);
        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST: 
                send_connect_response(client->session_id, server->session_timeout, server->server_status, server->server_name, server->socket, &client->client_addr);
                break;
            
            case FRAME_TYPE_ACK: 
                client->last_activity_time = time(NULL);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
            
            case FRAME_TYPE_TEXT_MESSAGE: 
                client->last_activity_time = time(NULL);
                uint32_t len = ntohl(frame->payload.text_msg.len);
                // Ensure null termination for safe printing, cap at max payload size
                if (len >= sizeof(frame->payload.text_msg.text)) {
                    len = sizeof(frame->payload.text_msg.text) - 1;
                }
                frame->payload.text_msg.text[len] = '\0';
                // push received sequence number to ACK queue so it can be acknowledged by the send_ack_thread
                if(push_seq_num(&client->queue_ack, received_seq_num) == -1){
                    break;
                };
                // TODO: Route message to another client if specified
                break;
            
            case FRAME_TYPE_LONG_TEXT_MESSAGE_DATA: 
                break;
            
            case FRAME_TYPE_DISCONNECT:
                EnterCriticalSection(&server->client_mutex);
                remove_client(server, client->slot_num);
                LeaveCriticalSection(&server->client_mutex);
                break;

            case FRAME_TYPE_PING:
                client->last_activity_time = time(NULL);
                uint32_t ping_ack_num = ntohl(frame->payload.ping_pong.ack_num);
                uint8_t ping_flag = frame->payload.ping_pong.flag;
                send_ping_pong(FRAME_TYPE_PONG, ping_ack_num, client->session_id, server->socket, &client->client_addr);
                break;
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

    while(client->connection == CLIENT_CONNECTED) {
        if(client->queue_ack.head == client->queue_ack.tail) {
            Sleep(10); // No ACKs to send, yield CPU
            continue; // Re-check the queue after waking up
        }
        uint32_t ack_seq_num = client->queue_ack.seq_num[client->queue_ack.head];
        if(pop_seq_num(&client->queue_ack) == -1) {
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, client->session_id, client->server_socket, &client->client_addr);
    }
    fprintf(stdout, "Exit send_ack_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(LPVOID lpParam){

    StructServer *server = (StructServer *)lpParam;

    while(server->server_status == SERVER_READY) {
        EnterCriticalSection(&server->client_mutex);
        for(int slot = 0; slot < MAX_CLIENTS; slot++){           
            if(server->client[slot].slot_status == SLOT_FREE) 
                continue;
            if(time(NULL) - (time_t)server->client[slot].last_activity_time < (time_t)server->session_timeout)
                continue;
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", server->client[slot].session_id);
            send_disconnect(DISCONNECT_TIMEOUT, server->client[slot].session_id, server->socket, &server->client[slot].client_addr);         
            remove_client(server, slot);
            fprintf(stdout, "Remaining connected clients: %d\n", server->clients_count);            
        }
        LeaveCriticalSection(&server->client_mutex);
        Sleep(1000);
    } 
    fprintf(stdout, "Exit client_timeout_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}