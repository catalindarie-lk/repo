#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "UDP_lib.h"

// --- Constants ---
#define SERVER_PORT 12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS 100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC 90        // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS 20
#define MAX_CONCURRENT_LONG_MESSAGES_PER_CLIENT 255

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

typedef struct{
    SOCKET socket;
    struct sockaddr_in addr;     // Server address structure
    ServerStatus status;         // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    uint32_t session_id_counter;         // Global counter for unique session IDs
    char name[NAME_SIZE];        // Human-readable server name
}ServerData;

typedef struct{
    uint32_t message_id;
    char *text;
    uint32_t bytes_received;
}LongMessageBufferEntry;

typedef struct {  
    struct sockaddr_in addr;         // Client's address
    
    uint32_t client_id;                     // Unique ID received from the client
    char name[NAME_SIZE];            // Optional: human-readable identifier received from the client
    uint8_t flag;                          // Flags received from the client (e.g., protocol version, capabilities)
    uint32_t client_to_srv_frame_count;     // Frame count sent by the client to the server
    ClientConnection connection;
 
    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)
    uint32_t srv_to_client_frame_count;     // Frame count sent by the server to the client               

    uint32_t slot_num;
    ClientSlotStatus slot_status;            //0->FREE; 1->BUSY

    LongMessageBufferEntry long_msg_buff[MAX_CONCURRENT_LONG_MESSAGES_PER_CLIENT];

    char log_file_path[255];

    // In a real system, you'd add:
    // - A queue for outgoing reliable packets (with retransmission info)
    // - A receive buffer for out-of-order packets (for sliding window)
} ClientData;

typedef struct{
    uint32_t count;             // Number of currently connected clients
    ClientData client[MAX_CLIENTS];      // Array of connected clients
    CRITICAL_SECTION mutex;         // For thread-safe access to connected_clients
}ClientList;

ServerData server;
QueueFrame queue_frame;
QueueSeqNum queue_seq_num;
ClientList list;

HANDLE receive_frame_thread;
HANDLE ack_nak_thread;
HANDLE process_frame_thread;  
HANDLE client_timeout_thread;
HANDLE server_command_thread;

const char *server_ip = "10.10.10.1"; // IPv4 example

// UDP communication functions
int send_connect_response(const uint32_t session_id, const uint32_t session_timeout, const uint8_t status, 
                                const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI ack_nak_thread_func(void* ptr);
unsigned int WINAPI client_timeout_thread_func(void* ptr);
unsigned int WINAPI server_command_thread_func(void* ptr);

// Client management functions
ClientData* find_client(ClientList *list, const uint32_t session_id);
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
int remove_client(ClientList *list, const uint32_t session_id);


void get_network_config() {
    DWORD bufferSize = 0;
    IP_ADAPTER_ADDRESSES *adapterAddresses = NULL, *adapter = NULL;

    // Get required buffer size
    GetAdaptersAddresses(AF_INET, 0, NULL, adapterAddresses, &bufferSize);
    adapterAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufferSize);

    if (GetAdaptersAddresses(AF_INET, 0, NULL, adapterAddresses, &bufferSize) == NO_ERROR) {
        for (adapter = adapterAddresses; adapter; adapter = adapter->Next) {
            fprintf(stdout, "Adapter: %ls\n", adapter->FriendlyName);
            IP_ADAPTER_UNICAST_ADDRESS *address = adapter->FirstUnicastAddress;
            while (address) {
                SOCKADDR_IN *sockaddr = (SOCKADDR_IN *)address->Address.lpSockaddr;
                printf("IP Address: %s\n", inet_ntoa(sockaddr->sin_addr));
                address = address->Next;
            }
        }
    } else {
        printf("Failed to retrieve adapter information.\n");
    }

    free(adapterAddresses);
    return;
}
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
void init_server(){
    // Initialize server information
    memset(&list, 0, sizeof(ClientList));

    server.status = SERVER_BUSY;
    // 1. Create Socket
    server.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server.socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        cleanup_winsock();
        return;
    }
    // 2. Set up the server address structure
    server.addr.sin_family = AF_INET;
    server.addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server.addr.sin_addr);
    // 3. Bind the socket
    int iResult = bind(server.socket, (SOCKADDR*)&server.addr, sizeof(server.addr));
    if (iResult == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server.socket);
        cleanup_winsock();
        return;
    }
    list.count = 0;
    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);

    queue_seq_num.head = 0;
    queue_seq_num.tail = 0;
    InitializeCriticalSection(&queue_seq_num.mutex);
    
    server.session_timeout = CLIENT_SESSION_TIMEOUT_SEC;
 
    server.session_id_counter = 0xFF; // Start with 0 or a random value
    strncpy(server.name, SERVER_NAME, NAME_SIZE - 1);
    server.name[NAME_SIZE - 1] = '\0'; // Ensure null-termination
    server.status = SERVER_READY; // Set initial status to ready
    InitializeCriticalSection(&list.mutex);

    printf("Server listening on port %d...\n", SERVER_PORT);
    return; // Indicate success
}
// --- Start server threads ---
void start_threads() {
    // Create threads for receiving and processing frames
    receive_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, NULL, 0, NULL);
    if (receive_frame_thread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        return;
    }
    ack_nak_thread = (HANDLE)_beginthreadex(NULL, 0, ack_nak_thread_func, NULL, 0, NULL);
    if (ack_nak_thread == NULL) {
        fprintf(stderr, "Failed to create ack/nak thread. Error: %d\n", GetLastError());
        return;
    }
    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process thread. Error: %d\n", GetLastError());
        return;
    }
    client_timeout_thread = (HANDLE)_beginthreadex(NULL, 0, client_timeout_thread_func, NULL, 0, NULL);
    if (client_timeout_thread == NULL) {
        fprintf(stderr, "Failed to create timeout thread. Error: %d\n", GetLastError());
        return;
    }
    server_command_thread = (HANDLE)_beginthreadex(NULL, 0, server_command_thread_func, NULL, 0, NULL);
    if (server_command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        return;
    }
    return;
}
// --- Server shutdown ---
void shutdown_server() {

    server.status = SERVER_STOP;

    if (ack_nak_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(ack_nak_thread, INFINITE);
        CloseHandle(ack_nak_thread);
    }
    if (process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(process_frame_thread, INFINITE);
        CloseHandle(process_frame_thread);
    }
    if (client_timeout_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client_timeout_thread, INFINITE);
        CloseHandle(client_timeout_thread);
    }
    DeleteCriticalSection(&list.mutex);
    DeleteCriticalSection(&queue_seq_num.mutex);
    closesocket(server.socket);
    printf("Server shut down cleanly.\n");
}
// --- Main server function ---
int main() {
//    get_network_config();
    init_winsock();
    init_server();
    start_threads();
    // Main server loop for general management, timeouts, and state updates
    while (server.status == SERVER_READY) {

        printf("Press 'q' to quit...\n");
        char c = getchar();
        if (c == 'q' || c == 'Q') {
            server.status = SERVER_STOP; // Signal threads to stop
            break;
        }
        Sleep(10); // Prevent busy-waiting
    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    cleanup_winsock();
    return 0;
}

// --- Send connect response ---
int send_connect_response(const uint32_t session_id, const uint32_t session_timeout, const uint8_t status, 
                                const char *server_name, SOCKET src_socket, const struct sockaddr_in *dest_addr, const char *log_file_path) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = 0;
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
    #ifdef ENABLE_LOGGING
        if(strlen(log_file_path) > 0){
            log_frame(LOG_FRAME_SENT, &frame, dest_addr, log_file_path);
        }
    #endif
    return bytes_sent;
}
// Find client by session ID
ClientData* find_client(ClientList *list, const uint32_t session_id) {
        
    // Search for the client with the given session ID
    if (list->count == 0) {
        return NULL; // No clients connected
    }
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        if(list->client[slot].slot_status == SLOT_FREE) continue;
        if(list->client[slot].session_id == session_id){
            return &list->client[slot];
        }
    }

    return NULL;
}
// Add a new client
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    // Assumes list_mutex is locked by caller

    uint32_t free_slot = 0;
    while(free_slot < MAX_CLIENTS){
        if(list->client[free_slot].slot_status == SLOT_FREE) {
            break;
        }
        free_slot++;
    }
    if(free_slot >= MAX_CLIENTS){
        fprintf(stderr, "Max clients reached. Cannot add new client.\n");
        return NULL;
    }

    ClientData *new_client = &list->client[free_slot];
    memset(new_client, 0, sizeof(ClientData));
 
    new_client->slot_num = free_slot;
    new_client->slot_status = SLOT_BUSY;
    memcpy(&new_client->addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->client_id = ntohl(recv_frame->payload.request.client_id); 
    new_client->session_id = ++server.session_id_counter; // Assign a unique session ID based on current count
    new_client->flag = recv_frame->payload.request.flag;
    new_client->srv_to_client_frame_count = 0;
 
    strncpy(new_client->name, recv_frame->payload.request.client_name, NAME_SIZE);
    new_client->name[NAME_SIZE - 1] = '\0';

    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, addr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(client_addr->sin_port);
    fprintf(stdout, "[ADDING NEW CLIENT] %s:%d Session ID:%d\n", addr, port, new_client->session_id);

    new_client->log_file_path[0] = '\0';

    list->count++;
    #ifdef ENABLE_LOGGING
        create_log_frame_file(0, new_client->session_id, new_client->log_file_path);
    #endif
    return &list->client[free_slot];
}
// Remove a client
int remove_client(ClientList *list, const uint32_t slot) {
    // Search for the client with the given session ID
    if (slot < 0 || slot >= MAX_CLIENTS) {
        return -1; 
    }
    memset(&list->client[slot], 0, sizeof(ClientData));
    list->client[slot].connection = CLIENT_DISCONNECTED;
    list->count--;
    return 0;
}
// --- Process server command ---
unsigned int WINAPI server_command_thread_func(void* ptr){

    //    printf("Exit server_command_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Receive Thread Function ---
unsigned int WINAPI receive_frame_thread_func(void* ptr) {
    
    UdpFrame received_frame;
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (server.status == SERVER_READY) {
        int bytes_received = recvfrom(server.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECVFROM_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
            FrameEntry frame_entry;
            memset(&frame_entry, 0, sizeof(FrameEntry));
            memcpy(&frame_entry.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));
            frame_entry.bytes_received = bytes_received;
            if(push_frame(&queue_frame, &frame_entry) == -1){
                fprintf(stderr, "Pushing frame error!!!\n");
                continue;
            };
        }
    }
    printf("Exit receive_frame_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(void* ptr) {

    while(server.status == SERVER_READY) {
        // Pop a frame from the queue
        FrameEntry frame_entry;
        SeqNumEntry seq_num_entry;

        if(pop_frame(&queue_frame, &frame_entry) == -1){
            Sleep(10); // No frames to process, yield CPU
            continue;
        };
        UdpFrame *frame = &frame_entry.frame;
        struct sockaddr_in *src_addr = &frame_entry.src_addr;
        uint32_t bytes_received = frame_entry.bytes_received;

        // Extract header fields   
        uint16_t header_delimiter = ntohs(frame->header.start_delimiter);
        uint8_t  header_frame_type = frame->header.frame_type;
        uint32_t header_seq_num = ntohl(frame->header.seq_num);
        uint32_t header_session_id = ntohl(frame->header.session_id);

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        uint16_t src_port = ntohs(src_addr->sin_port);

        //1. Validate Delimiter
        if (header_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, header_delimiter);
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
        
        ClientData *client = NULL;

        EnterCriticalSection(&list.mutex);
        if(header_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = find_client(&list, header_session_id);
            if(client != NULL){
                fprintf(stdout, "Client already connected\n");
                send_connect_response(client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr, client->log_file_path);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);
                continue;
            }
            client = add_client(&list, frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                LeaveCriticalSection(&list.mutex);
                continue; // Do not process further if client addition failed
            }                      
        } else {
            client = find_client(&list, header_session_id);
            if(client == NULL){
                fprintf(stdout, "Received frame from unknown %s:%d. Ignoring...\n", src_ip, src_port);
                LeaveCriticalSection(&list.mutex);
                continue;
            }
        }
        LeaveCriticalSection(&list.mutex);
        // 3. Process Payload based on Frame Type
        switch (header_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST:
                EnterCriticalSection(&list.mutex);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);
                send_connect_response(client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr, client->log_file_path);
                break;
            
            case FRAME_TYPE_ACK:
                EnterCriticalSection(&list.mutex);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
                       
            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                // Update client activity time
                EnterCriticalSection(&list.mutex);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);

                // Push sequence number to queue to send ACK
                seq_num_entry.seq_num = header_seq_num;
                seq_num_entry.type = FRAME_TYPE_ACK;
                seq_num_entry.session_id = header_session_id;
                memcpy(&seq_num_entry.addr, src_addr, sizeof(struct sockaddr_in));
                if(push_seq_num(&queue_seq_num, &seq_num_entry) == -1){
                    fprintf(stderr, "Pushing seq_num error!!!\n");
                };

                // Extract the long text fragment and recombine the long message
                uint32_t payload_message_id = ntohl(frame->payload.long_text_msg.message_id);
                uint32_t payload_total_text_len = ntohl(frame->payload.long_text_msg.total_text_len);
                uint32_t payload_fragment_text_len = ntohl(frame->payload.long_text_msg.fragment_len);
                uint32_t payload_fragment_text_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

                BOOL message_id_found = 0;
                uint32_t slot = 0;
                uint32_t free_slot = 0;

                for(slot = 0; slot < MAX_CONCURRENT_LONG_MESSAGES_PER_CLIENT; slot++){
                    if(client->long_msg_buff[slot].message_id == 0) {
                        free_slot = slot;
                    }
                    if(client->long_msg_buff[slot].message_id == payload_message_id){
                        message_id_found = TRUE;
                        char *dest = client->long_msg_buff[slot].text + payload_fragment_text_offset;
                        char *src = frame->payload.long_text_msg.fragment_text;                                              
                        memcpy(dest, src, payload_fragment_text_len);
                        client->long_msg_buff[slot].bytes_received += payload_fragment_text_len;
                        
                        fprintf(stdout, "Bytes Received: %d Out of: %d\n", client->long_msg_buff[slot].bytes_received, ntohl(frame->payload.long_text_msg.total_text_len));

                        if(client->long_msg_buff[slot].bytes_received >= payload_total_text_len){                                                      
                            client->long_msg_buff[slot].text[payload_total_text_len] = '\0';
                            
                            FILE *new_file = fopen("E:\\out.txt", "ab");
                            fprintf(new_file, "%s", client->long_msg_buff[slot].text);                   
                            fclose(new_file);                                                      
                            fprintf(stdout, "Received long text msg from %s:%d - Bytes: %d - ID: %d\n", src_ip, src_port, payload_total_text_len, payload_message_id);                   
                            //fprintf(stdout, "Message: %s\n", client->long_msg_buff[slot].text);                   
 
                            free(client->long_msg_buff[slot].text);
                            client->long_msg_buff[slot].message_id = 0;
                            client->long_msg_buff[slot].bytes_received = 0;
                        }
                        break;                     
                    }
                }
                if(message_id_found) 
                    break;
                client->long_msg_buff[free_slot].message_id = payload_message_id;
                client->long_msg_buff[free_slot].text = malloc(payload_total_text_len);

                char *dest = client->long_msg_buff[free_slot].text + payload_fragment_text_offset;
                char *src = frame->payload.long_text_msg.fragment_text;
                memcpy(dest, src, payload_fragment_text_len);               
                client->long_msg_buff[free_slot].bytes_received = payload_fragment_text_len;

                if(client->long_msg_buff[free_slot].bytes_received >= payload_total_text_len){
                    client->long_msg_buff[free_slot].text[payload_total_text_len] = '\0';

                    FILE *new_file = fopen("E:\\out.txt", "wb");
                    fprintf(new_file, "%s", client->long_msg_buff[free_slot].text);                   
                    fclose(new_file);
                    fprintf(stdout, "Received long text msg from %s:%d - Bytes: %d - ID:%d\n", src_ip, src_port, payload_total_text_len, payload_message_id);                   
                    //fprintf(stdout, "Message: %s\n", client->long_msg_buff[free_slot].text);                   
  
                    free(client->long_msg_buff[free_slot].text);
                    client->long_msg_buff[free_slot].message_id = 0;
                    client->long_msg_buff[free_slot].bytes_received = 0;
                }
                break;
            
            case FRAME_TYPE_DISCONNECT:
                EnterCriticalSection(&list.mutex);
                remove_client(&list, client->slot_num);
                LeaveCriticalSection(&list.mutex);
                client = NULL;
                break;

            case FRAME_TYPE_PING:
                EnterCriticalSection(&list.mutex);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);

                uint32_t ping_ack_num = ntohl(frame->payload.ping_pong.ack_num);
                uint8_t ping_flag = frame->payload.ping_pong.flag;
                send_ping_pong(FRAME_TYPE_PONG, ping_ack_num, client->session_id, server.socket, &client->addr, client->log_file_path);
                break;
            default:
                break;
        }
        #ifdef ENABLE_LOGGING
            if(client != NULL && strlen(client->log_file_path) > 0){
                log_frame(LOG_FRAME_RECV, frame, src_addr, client->log_file_path);
            }
        #endif      
    }
    printf("Exit process_frame_thread...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(void* ptr){

    while(server.status == SERVER_READY) {
        
        for(int slot = 0; slot < MAX_CLIENTS; slot++){  
            EnterCriticalSection(&list.mutex);         
            if(list.client[slot].slot_status == SLOT_FREE){
                LeaveCriticalSection(&list.mutex);
                continue;
            }                
            if(time(NULL) - (time_t)list.client[slot].last_activity_time < (time_t)server.session_timeout){
                LeaveCriticalSection(&list.mutex);
                continue;
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", list.client[slot].session_id);
            send_disconnect(DISCONNECT_TIMEOUT, list.client[slot].session_id, server.socket, &list.client[slot].addr, list.client[slot].log_file_path);         
            remove_client(&list, slot);
            LeaveCriticalSection(&list.mutex);
            fprintf(stdout, "Remaining connected clients: %d\n", list.count);            
        }
        Sleep(500);
    } 
    fprintf(stdout, "Exit client_timeout_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- SendAck Thread Function ---
unsigned int WINAPI ack_nak_thread_func(void* ptr){

    ClientData *client = NULL;
    SeqNumEntry seq_num_entry;   
    while (server.status == SERVER_READY) {
        memset(&seq_num_entry, 0, sizeof(SeqNumEntry));   
        if(pop_seq_num(&queue_seq_num, &seq_num_entry) == -1){
            Sleep(10);
            continue;
        }
        // check if client session still open (could be optional)
        EnterCriticalSection(&list.mutex);
        ClientData *client = find_client(&list, seq_num_entry.session_id);
        LeaveCriticalSection(&list.mutex);
        if(client == NULL) {
            Sleep(10);
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nak(seq_num_entry.type, seq_num_entry.seq_num, seq_num_entry.session_id, server.socket, &seq_num_entry.addr, client->log_file_path);
    }
    fprintf(stdout, "Exit ack/nak thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}

