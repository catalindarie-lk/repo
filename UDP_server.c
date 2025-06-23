#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "udp_lib.h"
#include "udp_queue.h"
#include "udp_bitmap.h"

// --- Constants ---
#define SERVER_PORT                     12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS             100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC      30        // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME                     "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                     20

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
    struct sockaddr_in addr;            // Server address structure
    ServerStatus status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    uint32_t session_id_counter;        // Global counter for unique session IDs
    char name[NAME_SIZE];               // Human-readable server name
}ServerData;

typedef struct{
    FILETIME ft;
    ULARGE_INTEGER prev_uli;
    unsigned long long prev_microseconds;

    ULARGE_INTEGER crt_uli;
    unsigned long long crt_microseconds;

    float crt_file_transfer_speed;
    float prev_file_transfer_speed;

    float avg_file_transfer_speed;
    float file_transfer_progress;
}Statistics;

typedef struct{
    char *buffer;
    uint32_t *bitmap;
    uint32_t id;
    uint32_t bytes_received;
    uint32_t fragment_count;
    uint32_t bitmap_entries_count;
}IncomingMessageEntry;

typedef struct {  
    struct sockaddr_in addr;                // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    
    uint32_t client_id;                     // Unique ID received from the client
    char name[NAME_SIZE];                   // Optional: human-readable identifier received from the client
    uint8_t flag;                           // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;
 
    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    volatile time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)             

    uint32_t slot_num;
    uint8_t slot_status;            //0->FREE; 1->BUSY
 
    char *file_buffer;
    uint32_t *file_bitmap;
    uint32_t file_id;
    uint32_t file_size;
    uint32_t file_fragment_count;   
    uint32_t file_bytes_received;
    char file_name[FILE_NAME_SIZE];

    IncomingMessageEntry inc_message[10];
    char message_file_name[FILE_NAME_SIZE];
    uint32_t message_file_count;

    char log_path[FILE_NAME_SIZE];
    Statistics statistics;

} ClientData;

typedef struct{
    ClientData client[MAX_CLIENTS];      // Array of connected clients
    CRITICAL_SECTION mutex;         // For thread-safe access to connected_clients
}ClientList;

ServerData server;
ClientList list;

QueueFrame queue_frame;
QueueFrame queue_frame_ctrl;
QueueSeqNum queue_seq_num;


HANDLE receive_frame_thread;
HANDLE ack_nak_thread;
HANDLE process_frame_thread;  
HANDLE client_timeout_thread;
HANDLE server_command_thread;

//SeqNumNode *hash_table[HASH_SIZE] = {NULL};

const char *server_ip = "10.10.10.1"; // IPv4 example

// Client management functions
ClientData* find_client(ClientList *list, const uint32_t session_id);
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
int remove_client(ClientList *list, const uint32_t session_id);
int create_output_file(const char *buffer, const uint32_t count, const char *path);

int process_file_metadata_frame(ClientData *client, UdpFrame *frame);
int process_file_fragment_frame(ClientData *client, UdpFrame *frame);
void update_statistics(ClientData *client);

void handle_file_metadata(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr);
void handle_file_fragment(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr);
void handle_message_fragment(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI ack_nak_thread_func(void* ptr);
unsigned int WINAPI client_timeout_thread_func(void* ptr);
unsigned int WINAPI server_command_thread_func(void* ptr);

void get_network_config();
void init_winsock();
void cleanup_winsock();
void init_server();
void start_threads();
void shutdown_server();

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
void get_network_config(){
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
// Create output file
int create_output_file(const char *buffer, const uint32_t count, const char *path){
    FILE *file = fopen(path, "wb");           
    if(file == NULL){
        fprintf(stderr, "Error creating message file!!!\n");
        return RET_VAL_ERROR;
    }
    size_t written = fwrite(buffer, 1, count, file);
    if (written != count) {
        fprintf(stderr, "Incomplete message written to file. Expected: %d, Written: %zu\n", count, written);
        fclose(file);
        return RET_VAL_ERROR;
    }
    fclose(file);
    fprintf(stdout, "Creating output file: %s\n", path);
    return RET_VAL_SUCCESS;
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
    
    // int size = 16 * 1024 * 1024;
    // setsockopt(server.socket, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size));
    // setsockopt(server.socket, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size));
    
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

    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);

    queue_frame_ctrl.head = 0;
    queue_frame_ctrl.tail = 0;
    InitializeCriticalSection(&queue_frame_ctrl.mutex);

    queue_seq_num.head = 0;
    queue_seq_num.tail = 0;
    InitializeCriticalSection(&queue_seq_num.mutex);
    
    server.session_timeout = CLIENT_SESSION_TIMEOUT_SEC;
 
    server.session_id_counter = 0xFF; // Start with 0 or a random value
    snprintf(server.name, NAME_SIZE, "%.*s", NAME_SIZE - 1, SERVER_NAME);
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
    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_frame_ctrl.mutex);
    DeleteCriticalSection(&queue_seq_num.mutex);
    closesocket(server.socket);
    printf("Server shut down cleanly.\n");
}

// Find client by session ID
ClientData* find_client(ClientList *list, const uint32_t session_id) {    
    // Search for the client with the given session ID
    for (uint32_t slot = 0; slot < MAX_CLIENTS; slot++) {
        if(list->client[slot].slot_status == SLOT_FREE) 
            continue;
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

    new_client->file_buffer = NULL;
    new_client->file_bitmap = NULL;
    new_client->inc_message[0].buffer = NULL;
    new_client->inc_message[0].bitmap = NULL;
 
    new_client->slot_num = free_slot;
    new_client->slot_status = SLOT_BUSY;
    memcpy(&new_client->addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->client_id = ntohl(recv_frame->payload.request.client_id); 
    new_client->session_id = ++server.session_id_counter; // Assign a unique session ID based on current count
    new_client->flag = recv_frame->payload.request.flag;
 
    snprintf(new_client->name, NAME_SIZE, "%.*s", NAME_SIZE - 1, recv_frame->payload.request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    new_client->port = ntohs(client_addr->sin_port);

    fprintf(stdout, "[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->session_id);

    #ifdef ENABLE_FRAME_LOG
        create_log_frame_file(0, new_client->session_id, new_client->log_path);
    #endif
    return new_client;
}
// Remove a client
int remove_client(ClientList *list, const uint32_t slot) {
    // Search for the client with the given session ID
    if(list == NULL){
        fprintf(stderr, "Invalid client pointer!\n");
        return RET_VAL_ERROR;
    }
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "Invalid client slot nr:  %d", slot);
        return RET_VAL_ERROR; 
    }
    fprintf(stdout, "Removing client with session ID: %d from slot %d\n", list->client[slot].session_id, list->client[slot].slot_num);

    free(list->client[slot].file_buffer);
    list->client[slot].file_buffer = NULL;

    free(list->client[slot].file_bitmap);
    list->client[slot].file_bitmap = NULL;

    memset(&list->client[slot], 0, sizeof(ClientData));
    fprintf(stdout, "Removed client successfully!\n");
    return RET_VAL_SUCCESS;
}
// Process received file metadata frame
int process_file_metadata_frame(ClientData *client, UdpFrame *frame){

    client->file_id = ntohl(frame->payload.file_metadata.file_id);
    client->file_size = ntohl(frame->payload.file_metadata.file_size);
    client->file_bytes_received = 0;
    client->file_buffer = NULL;
    client->file_bitmap = NULL;

    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, File Size: %d, Fragment Size: %d\n", client->session_id, client->file_id, client->file_size, (uint32_t)FILE_FRAGMENT_SIZE);

    client->file_buffer = malloc(client->file_size);   
    if(client->file_buffer == NULL){
        fprintf(stderr, "Memory allocation fail for file buffer!!!\n");
        return RET_VAL_ERROR;
    }
    memset(client->file_buffer, 0, client->file_size);

    client->file_fragment_count = client->file_size / (uint32_t)FILE_FRAGMENT_SIZE;
    if(client->file_size % (uint32_t)FILE_FRAGMENT_SIZE > 0){
        client->file_fragment_count++;
    }
    fprintf(stdout, "Fragments count: %d\n", client->file_fragment_count);

    uint32_t file_bitmap_entries = 0;
    file_bitmap_entries = client->file_fragment_count / 32;
    if(client->file_fragment_count % 32 > 0){
        file_bitmap_entries++;
    }
    fprintf(stdout, "Bitmap 32bits entries needed: %d\n", file_bitmap_entries);

    client->file_bitmap = malloc(file_bitmap_entries * sizeof(uint32_t));
    if(client->file_bitmap == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        return RET_VAL_ERROR;        
    }
    memset(client->file_bitmap, 0, file_bitmap_entries * sizeof(uint32_t));

    return RET_VAL_SUCCESS;
}
// Process received file fragment frame
int process_file_fragment_frame(ClientData *client, UdpFrame *frame){

    uint32_t recv_file_id = ntohl(frame->payload.file_fragment.file_id);
    uint32_t recv_fragment_size = ntohl(frame->payload.file_fragment.size);
    uint32_t recv_fragment_offset = ntohl(frame->payload.file_fragment.offset);

    if(client->file_id != recv_file_id){
        //fprintf(stderr, "No metadata for file fragment\n");
        return RET_VAL_ERROR;
    }
    //copy the received fragment text to the buffer
    if(client->file_buffer == NULL){
        //fprintf(stderr, "Memory not allocated for file buffer!!!\n");
        return RET_VAL_ERROR;
    }
    if(check_fragment_received(client->file_bitmap, recv_fragment_offset, (uint32_t)FILE_FRAGMENT_SIZE)){
        //fprintf(stderr, "Received duplicate frame (offset: %d)!!!\n", fragment_offset);
        return RET_VAL_SUCCESS;
    }
    if(recv_fragment_offset >= client->file_size){
        fprintf(stderr, "Received fragment with offset out of limits. File size: %d, Received offset: %d\n", client->file_size, recv_fragment_offset);
        return RET_VAL_ERROR;
    }
    if (recv_fragment_offset + recv_fragment_size > client->file_size) {
        fprintf(stderr, "Fragment extends past file bounds\n");
        return RET_VAL_ERROR;
    }

    char *dest = client->file_buffer + recv_fragment_offset;
    char *src = frame->payload.file_fragment.bytes;
    memcpy(dest, src, recv_fragment_size);
    client->file_bytes_received += recv_fragment_size;

    update_statistics(client);
    mark_fragment_received(client->file_bitmap, recv_fragment_offset, (uint32_t)FILE_FRAGMENT_SIZE);

    if(client->file_bytes_received == client->file_size && check_bitmap(client->file_bitmap, client->file_fragment_count)){       
        snprintf(client->file_name, FILE_NAME_SIZE, "E:\\out_file_%d.txt", client->session_id);
        if(create_output_file(client->file_buffer, client->file_bytes_received, client->file_name) != RET_VAL_SUCCESS){
            return RET_VAL_ERROR;
        }
        fprintf(stdout, "Received file from %s:%d File ID: %d, Size: %d\n", client->ip, client->port, client->file_id, client->file_bytes_received);   
        // free(client->file_buffer);
        // client->file_buffer = NULL;
        // free(client->file_bitmap);
        // client->file_bitmap = NULL;
    }
    return RET_VAL_SUCCESS;
}
// Process received message fragment frame
int process_message_fragment_frame(ClientData *client, UdpFrame *frame){

    // Extract the long text fragment and recombine the long message
    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    if(client->inc_message[0].bitmap != NULL){
        if(check_fragment_received(client->inc_message[0].bitmap, recv_fragment_offset, (uint32_t)TEXT_FRAGMENT_SIZE)){
            //fprintf(stderr, "Received duplicate frame (offset: %d)!!!\n", recv_fragment_offset);
            return RET_VAL_SUCCESS;
        }
    }
    if(recv_fragment_offset >= recv_message_len){
        fprintf(stderr, "Received fragment with offset out of limits. File size: %d, Received offset: %d\n", recv_message_len, recv_fragment_offset);
        return RET_VAL_ERROR;
    }
    if (recv_fragment_offset + recv_fragment_len > recv_message_len) {
        fprintf(stderr, "Fragment extends past message bounds\n");
        return RET_VAL_ERROR;
    }

    BOOL message_id_found = FALSE;

    if(client->inc_message[0].id == recv_message_id){       
        message_id_found = TRUE;

        char *dest = client->inc_message[0].buffer + recv_fragment_offset;
        char *src = frame->payload.long_text_msg.fragment_text;                                              
        memcpy(dest, src, recv_fragment_len);
        client->inc_message[0].bytes_received += recv_fragment_len;
        
        update_statistics(client);
        mark_fragment_received(client->inc_message[0].bitmap, recv_fragment_offset, (uint32_t)TEXT_FRAGMENT_SIZE);

        //check if received full message (bytes received is equal to total payload)
        if(client->inc_message[0].bytes_received == recv_message_len && check_bitmap(client->inc_message[0].bitmap, client->inc_message[0].fragment_count)){                                                      
            client->inc_message[0].buffer[recv_message_len] = '\0';                                     
            snprintf(client->message_file_name, FILE_NAME_SIZE, "E:\\message%d_%d.txt", client->message_file_count++, client->session_id);
            if(create_output_file(client->inc_message[0].buffer, client->inc_message[0].bytes_received, client->message_file_name) != RET_VAL_SUCCESS){
                return RET_VAL_ERROR;
            }
            // fprintf(stdout, "\nReceived long text msg from %s:%d - Bytes: %d Session ID: %d, Message ID: %d\n", client->ip, client->port, client->inc_message[0].bytes_received, client->session_id, recv_message_id);
            // fprintf(stdout, "Message: %s\n", client->inc_message[0].buffer);
            // free(client->inc_message[0].buffer);
            // client->inc_message[0].buffer = NULL;
            // free(client->inc_message[0].bitmap);
            // client->inc_message[0].bitmap = NULL;
        }
    }

    if(message_id_found == TRUE) 
        return RET_VAL_SUCCESS;
    //check if the received sequence number is a duplicate (if it exists in the hash then it was allready received)
       
    client->inc_message[0].fragment_count = recv_message_len / (uint32_t)TEXT_FRAGMENT_SIZE;
    if((recv_message_len % (uint32_t)TEXT_FRAGMENT_SIZE) > 0){
        client->inc_message[0].fragment_count++;
    }
    client->inc_message[0].bitmap_entries_count = client->inc_message[0].fragment_count / 32;  
    if(client->inc_message[0].fragment_count % 32 > 0){
        client->inc_message[0].bitmap_entries_count++;
    }
    client->inc_message[0].bitmap = malloc(client->inc_message[0].bitmap_entries_count * sizeof(uint32_t));
    if(client->inc_message[0].bitmap == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        return RET_VAL_ERROR;        
    }
    memset(client->inc_message[0].bitmap, 0, client->inc_message[0].bitmap_entries_count * sizeof(uint32_t));
    
    fprintf(stdout, "Message size: %d\n", recv_message_len);
    fprintf(stdout, "Nr of Fragments: %d\n", client->inc_message[0].fragment_count);                
    fprintf(stdout, "Nr of Bitmap 32bit entries: %d\n", client->inc_message[0].bitmap_entries_count);

    //copy the received fragment text to the buffer            
    client->inc_message[0].id = recv_message_id;
    client->inc_message[0].buffer = malloc(recv_message_len);
    if(client->inc_message[0].buffer == NULL){
        fprintf(stdout, "Error allocating memory!!!\n");
        return RET_VAL_ERROR;
    }
    memset(client->inc_message[0].buffer, 0, recv_message_len);
    char *dest = client->inc_message[0].buffer + recv_fragment_offset;
    char *src = frame->payload.long_text_msg.fragment_text;
    memcpy(dest, src, recv_fragment_len);
    //update received bytes counter
    client->inc_message[0].bytes_received = recv_fragment_len;

    mark_fragment_received(client->inc_message[0].bitmap, recv_fragment_offset, (uint32_t)TEXT_FRAGMENT_SIZE);

    //check if received full message (bytes received is equal to total payload)
    if(client->inc_message[0].bytes_received == recv_message_len && check_bitmap(client->inc_message[0].bitmap, client->inc_message[0].fragment_count)){       
        client->inc_message[0].buffer[recv_message_len] = '\0';  
        snprintf(client->message_file_name, FILE_NAME_SIZE, "E:\\message%d_%d.txt", client->message_file_count++, client->session_id);
        if(create_output_file(client->inc_message[0].buffer, client->inc_message[0].bytes_received, client->message_file_name) != RET_VAL_SUCCESS){
            return RET_VAL_ERROR;
        }
        // fprintf(stdout, "\nReceived long text msg from %s:%d - Bytes: %d Session ID: %d, Message ID: %d\n", client->ip, client->port, client->inc_message[0].bytes_received, client->session_id, recv_message_id);
        // fprintf(stdout, "Message: %s\n", client->inc_message[0].buffer);
        free(client->inc_message[0].buffer);
        client->inc_message[0].buffer = NULL;
        free(client->inc_message[0].bitmap);
        client->inc_message[0].bitmap = NULL;
    }
    return RET_VAL_SUCCESS; 
}

// update file transfer progress and speed in MBs
void update_statistics(ClientData * client){

    //update file transfer speed in MB/s
    GetSystemTimePreciseAsFileTime(&client->statistics.ft);
    client->statistics.crt_uli.LowPart = client->statistics.ft.dwLowDateTime;
    client->statistics.crt_uli.HighPart = client->statistics.ft.dwHighDateTime;
    client->statistics.crt_microseconds = client->statistics.crt_uli.QuadPart / 10;

    //TRANSFER SPEED
    //current speed (1 cycle)
    client->statistics.crt_file_transfer_speed = (float)MAX_PAYLOAD_SIZE / (float)((client->statistics.crt_microseconds - client->statistics.prev_microseconds));
    //prev time = current time
    client->statistics.prev_microseconds = client->statistics.crt_microseconds;
    //first order filter for speed calc - filtered_speed = prev_filtered_speed + factor * (crt_speed - prev_filtered_speed)
    client->statistics.avg_file_transfer_speed = client->statistics.prev_file_transfer_speed + 0.0001 * (client->statistics.crt_file_transfer_speed - client->statistics.prev_file_transfer_speed);
    //prev = current
    client->statistics.prev_file_transfer_speed = client->statistics.avg_file_transfer_speed;

    //PROGRESS - update file transfer progress percentage
    client->statistics.file_transfer_progress = (float)client->file_bytes_received / (float)client->file_size * 100.0;

    fprintf(stdout, "\rFile transfer progress: %.2f %% - Speed: %.2f MB/s", client->statistics.file_transfer_progress, client->statistics.avg_file_transfer_speed);
    fflush(stdout);
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
    QueueFrameEntry frame_entry;
    
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);
    int recv_error_code;

    int bytes_received;

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (server.status == SERVER_READY) {

        memset(&received_frame, 0, sizeof(UdpFrame));
        memset(&src_addr, 0, sizeof(src_addr));

        bytes_received = recvfrom(server.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            recv_error_code = WSAGetLastError();
            if (recv_error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECVFROM_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", recv_error_code);
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
           
            memset(&frame_entry, 0, sizeof(QueueFrameEntry));
            memcpy(&frame_entry.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));
            frame_entry.bytes_received = (uint32_t)bytes_received;

            if(frame_entry.frame.header.frame_type == FRAME_TYPE_KEEP_ALIVE || 
                    frame_entry.frame.header.frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                    frame_entry.frame.header.frame_type == FRAME_TYPE_FILE_METADATA ||
                    frame_entry.frame.header.frame_type == FRAME_TYPE_DISCONNECT){
                if(push_frame(&queue_frame_ctrl, &frame_entry) != RET_VAL_SUCCESS){
                    continue;
                }

            } else {
                if(push_frame(&queue_frame, &frame_entry) != RET_VAL_SUCCESS){
                    continue;
                }
            }
        }
    }
    printf("Exit receive_frame_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(void* ptr) {

    uint16_t header_delimiter;
    uint8_t  header_frame_type;
    uint64_t header_seq_num;
    uint32_t header_session_id;

    QueueFrameEntry frame_entry;
    QueueSeqNumEntry seq_num_entry;

    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    uint32_t bytes_received;

    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;

    ClientData *client;

    while(server.status == SERVER_READY) {
        // Pop a frame from the queue
        // Pop a frame from the queue (prioritize control queue)
        if (pop_frame(&queue_frame_ctrl, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame_ctrl
        } else if (pop_frame(&queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame
        } else {
            Sleep(1); // No frames to process, yield CPU
            continue;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        bytes_received = frame_entry.bytes_received;

        // Extract header fields   
        header_delimiter = ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = ntohll(frame->header.seq_num);
        header_session_id = ntohl(frame->header.session_id);

        //fprintf(stdout, "received buffered frame seq nu: %d, nr of bytes: %d\n", header_seq_num, bytes_received);
        
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = ntohs(src_addr->sin_port);

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

        client = NULL;

        EnterCriticalSection(&list.mutex);
        if(header_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = find_client(&list, header_session_id);
            if(client != NULL){
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);
                fprintf(stdout, "Client already connected\n");
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr, client->log_path);
                continue;
            }
            client = add_client(&list, frame, src_addr);
            if (client == NULL) {
                LeaveCriticalSection(&list.mutex);
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                continue; // Do not process further if client addition failed
            }                      
        } else {
            client = find_client(&list, header_session_id);
            if(client == NULL){
                LeaveCriticalSection(&list.mutex);
                //fprintf(stdout, "Received frame from unknown %s:%d. Ignoring...\n", src_ip, src_port);
                continue;
            }
        }
        LeaveCriticalSection(&list.mutex);
        // 3. Process Payload based on Frame Type
        switch (header_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST:
                client->last_activity_time = time(NULL);
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr, client->log_path);
                break;
            
            case FRAME_TYPE_ACK:
                client->last_activity_time = time(NULL);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues

            case FRAME_TYPE_KEEP_ALIVE:
                client->last_activity_time = time(NULL);
                send_ack_nak(FRAME_TYPE_ACK, header_seq_num, header_session_id, server.socket, &client->addr, client->log_path);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
                       
            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                handle_message_fragment(client, frame, src_addr);
                break;

            case FRAME_TYPE_FILE_METADATA:
                handle_file_metadata(client, frame, src_addr);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                handle_file_fragment(client, frame, src_addr);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "Client %s:%d with session ID %d requested disconnect...\n", client->ip, client->port, client->session_id);
                EnterCriticalSection(&list.mutex);
                remove_client(&list, client->slot_num);
                LeaveCriticalSection(&list.mutex);
                break;
            default:
                break;
        }
        #ifdef ENABLE_FRAME_LOG
        if(client != NULL){
            log_frame(LOG_FRAME_RECV, frame, src_addr, client->log_path);
        }           
        #endif      
    }
    printf("Exit process_frame_thread...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(void* ptr){

    time_t current_time;

    while(server.status == SERVER_READY) {
        
        for(int slot = 0; slot < MAX_CLIENTS; slot++){  
            current_time = time(NULL);
            if(list.client[slot].slot_status == SLOT_FREE){
                continue;
            }                
            if(current_time - (time_t)list.client[slot].last_activity_time < (time_t)server.session_timeout){
                continue;
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", list.client[slot].session_id);
            send_disconnect(list.client[slot].session_id, server.socket, &list.client[slot].addr, list.client[slot].log_path);
            EnterCriticalSection(&list.mutex);
            remove_client(&list, slot);
            LeaveCriticalSection(&list.mutex);
        }
        Sleep(1000);
    } 
    fprintf(stdout, "Exit client_timeout_thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- SendAck Thread Function ---
unsigned int WINAPI ack_nak_thread_func(void* ptr){

    ClientData *client = NULL;
    QueueSeqNumEntry seq_num_entry;   
    while (server.status == SERVER_READY) {
        memset(&seq_num_entry, 0, sizeof(QueueSeqNumEntry));   
        if(pop_seq_num(&queue_seq_num, &seq_num_entry) != RET_VAL_SUCCESS){
            Sleep(100);
            continue;
        }
        // check if client session still open (could be optional)
        EnterCriticalSection(&list.mutex);
        ClientData *client = find_client(&list, seq_num_entry.session_id);
        LeaveCriticalSection(&list.mutex);
        if(client == NULL) {
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nak(seq_num_entry.type, seq_num_entry.seq_num, seq_num_entry.session_id, server.socket, &seq_num_entry.addr, client->log_path);
    }
    fprintf(stdout, "Exit ack/nak thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}

//handler for file metadata frame
void handle_file_metadata(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr) {
    EnterCriticalSection(&list.mutex);
    if (process_file_metadata_frame(client, frame) != RET_VAL_SUCCESS) {
        return;
    }
    client->last_activity_time = time(NULL);
    LeaveCriticalSection(&list.mutex);
    QueueSeqNumEntry ack_entry = {
        .seq_num = ntohll(frame->header.seq_num),
        .type = FRAME_TYPE_ACK,
        .session_id = ntohl(frame->header.session_id)
    };
    memcpy(&ack_entry.addr, addr, sizeof(struct sockaddr_in));
    push_seq_num(&queue_seq_num, &ack_entry);
    return;
}
//handler for file fragment frame
void handle_file_fragment(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr) {
    EnterCriticalSection(&list.mutex);
    if (process_file_fragment_frame(client, frame) != RET_VAL_SUCCESS) {
        return;
    }
    client->last_activity_time = time(NULL);
    LeaveCriticalSection(&list.mutex);
    QueueSeqNumEntry ack_entry = {
        .seq_num = ntohll(frame->header.seq_num),
        .type = FRAME_TYPE_ACK,
        .session_id = ntohl(frame->header.session_id)
    };
    memcpy(&ack_entry.addr, addr, sizeof(struct sockaddr_in));
    push_seq_num(&queue_seq_num, &ack_entry);
    return;
}
//handler for message fragment frame
void handle_message_fragment(ClientData *client, UdpFrame *frame, const struct sockaddr_in *addr) {
    EnterCriticalSection(&list.mutex);
    if (process_message_fragment_frame(client, frame) != RET_VAL_SUCCESS) {
        return;
    }
    client->last_activity_time = time(NULL);
    LeaveCriticalSection(&list.mutex);
    QueueSeqNumEntry ack_entry = {
        .seq_num = ntohll(frame->header.seq_num),
        .type = FRAME_TYPE_ACK,
        .session_id = ntohl(frame->header.session_id)
    };
    memcpy(&ack_entry.addr, addr, sizeof(struct sockaddr_in));
    push_seq_num(&queue_seq_num, &ack_entry);
    return;
}

