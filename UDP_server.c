#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "udp_lib.h"
#include "udp_queue.h"
#include "udp_bitmap.h"
#include "safefileio.h"

// --- Constants ---
#define SERVER_PORT                     12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS             100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC      120       // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME                     "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                     20
#define FILE_PATH "E:\\out_file.txt"

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
    uint64_t file_size;
    uint32_t file_fragment_count;   
    uint64_t file_bytes_received;
    char file_name[PATH_SIZE];

    char log_path[PATH_SIZE];
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
QueueSeqNum queue_seq_num_ctrl;

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

int process_file_metadata_frame(ClientData *client, UdpFrame *frame);
void update_statistics(ClientData *client);

int handle_file_metadata(ClientData *client, UdpFrame *frame);
int handle_file_fragment(ClientData *client, UdpFrame *frame);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI ack_nak_thread_func(void* ptr);
unsigned int WINAPI client_timeout_thread_func(void* ptr);
unsigned int WINAPI server_command_thread_func(void* ptr);

// --- Main server function ---

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
// Create output file



// Safe write function to handle errors
int file_fragment_write(const char *path, const void *buffer, size_t size, size_t offset) {
    
    FILE *fp = fopen(path, "ab");

    if (!fp) {
        fprintf(stderr, "Error: Failed to open file\n");
        return RET_VAL_ERROR;
    }

    // Move file pointer to the correct offset
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        fprintf(stderr, "Error: Failed to seek to offset %zu\n", offset);
        return RET_VAL_ERROR;
    }

    // Write data to file
    size_t written = fwrite(buffer, 1, size, fp);
    if (written != size) {
        fclose(fp);
        fprintf(stderr, "Error: Failed to write data (expected %zu, wrote %zu)\n", size, written);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    return RET_VAL_SUCCESS;  // Success
}

int create_output_file(const char *buffer, const uint64_t size, const char *path){
    FILE *fp = fopen(path, "wb");           
    if(fp == NULL){
        fprintf(stderr, "Error creating output file!!!\n");
        return RET_VAL_ERROR;
    }
    size_t written = safe_fwrite(fp, buffer, size);
    if (written != size) {
        fprintf(stderr, "Incomplete bytes written to file. Expected: %zu, Written: %zu\n", size, written);
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    fprintf(stderr, "Creating output file: %s\n", path);
    return RET_VAL_SUCCESS;
}
// Server initialization and management functions
int init_server(){
    
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }
    memset(&list, 0, sizeof(ClientList));
    server.status = SERVER_BUSY;

    server.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server.socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return RET_VAL_ERROR;
    }
    server.addr.sin_family = AF_INET;
    server.addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server.addr.sin_addr);
 
    if (bind(server.socket, (SOCKADDR*)&server.addr, sizeof(server.addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server.socket);
        WSACleanup();
        return RET_VAL_ERROR;
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
    queue_seq_num_ctrl.head = 0;
    queue_seq_num_ctrl.tail = 0;
    InitializeCriticalSection(&queue_seq_num_ctrl.mutex);
    
    server.session_timeout = CLIENT_SESSION_TIMEOUT_SEC;
    server.session_id_counter = 0xFF;
    snprintf(server.name, NAME_SIZE, "%.*s", NAME_SIZE - 1, SERVER_NAME);
    server.status = SERVER_READY;
    InitializeCriticalSection(&list.mutex);

    printf("Server listening on port %d...\n", SERVER_PORT);
    return RET_VAL_SUCCESS;
}
// --- Start server threads ---
int start_threads() {
    // Create threads for receiving and processing frames
    receive_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, NULL, 0, NULL);
    if (receive_frame_thread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    ack_nak_thread = (HANDLE)_beginthreadex(NULL, 0, ack_nak_thread_func, NULL, 0, NULL);
    if (ack_nak_thread == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client_timeout_thread = (HANDLE)_beginthreadex(NULL, 0, client_timeout_thread_func, NULL, 0, NULL);
    if (client_timeout_thread == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    server_command_thread = (HANDLE)_beginthreadex(NULL, 0, server_command_thread_func, NULL, 0, NULL);
    if (server_command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}
// --- Server shutdown ---
void shutdown_server() {

    server.status = SERVER_STOP;

    if (ack_nak_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(ack_nak_thread, INFINITE);
        CloseHandle(ack_nak_thread);
    }
    fprintf(stdout,"ack thread closed...\n");
    if (process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(process_frame_thread, INFINITE);
        CloseHandle(process_frame_thread);
    }
    if (receive_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(receive_frame_thread, INFINITE);
        CloseHandle(receive_frame_thread);
    }
    fprintf(stdout,"receive frame thread closed...\n");
    fprintf(stdout,"process thread closed...\n");
    if (client_timeout_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client_timeout_thread, INFINITE);
        CloseHandle(client_timeout_thread);
    }
    fprintf(stdout,"client timeout thread closed...\n");
    if (server_command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(server_command_thread, INFINITE);
        CloseHandle(server_command_thread);
    }
    fprintf(stdout,"server command thread closed...\n");
    DeleteCriticalSection(&list.mutex);
    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_frame_ctrl.mutex);
    DeleteCriticalSection(&queue_seq_num.mutex);
    DeleteCriticalSection(&queue_seq_num_ctrl.mutex);

    closesocket(server.socket);
    WSACleanup();
    printf("Server shut down!\n");
}
// --- MAIN FUNCTION ---
int main() {
//    get_network_config();
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
    return 0;
}

// Find client by session ID
ClientData* find_client(ClientList *list, const uint32_t session_id) {    
    // Search for the client with the given session ID
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
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
int handle_file_metadata(ClientData *client, UdpFrame *frame){

    QueueSeqNumEntry entry;

    uint32_t recv_file_id = ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = ntohll(frame->payload.file_metadata.file_size);

    EnterCriticalSection(&list.mutex);
    client->file_id = recv_file_id;
    client->file_size = recv_file_size;
    client->file_bytes_received = 0;
    client->file_buffer = NULL;
    client->file_bitmap = NULL;

    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, File Size: %zu, Fragment Size: %d\n", client->session_id, recv_file_id, recv_file_size, (uint32_t)FILE_FRAGMENT_SIZE);

    client->file_buffer = malloc(recv_file_size);   
    if(client->file_buffer == NULL){
        LeaveCriticalSection(&list.mutex);
        fprintf(stderr, "Memory allocation fail for file buffer!!!\n");
        return RET_VAL_ERROR;
    }
    memset(client->file_buffer, 0, recv_file_size);

    client->file_fragment_count = recv_file_size / FILE_FRAGMENT_SIZE;
    if(recv_file_size % FILE_FRAGMENT_SIZE > 0){
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
        LeaveCriticalSection(&list.mutex);
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        return RET_VAL_ERROR;        
    }
    memset(client->file_bitmap, 0, file_bitmap_entries * sizeof(uint32_t));
 
    entry.seq_num = ntohll(frame->header.seq_num);
    entry.op_code = STS_ACK;
    entry.session_id = ntohl(frame->header.session_id);
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));  
    LeaveCriticalSection(&list.mutex);
    push_seq_num(&queue_seq_num_ctrl, &entry);

    return RET_VAL_SUCCESS;
}
// Process received file fragment frame
int handle_file_fragment(ClientData *client, UdpFrame *frame){

    // Validate input pointers
    // Handle no file in progress
    // Check file ID and fragment overlap
    // Copy fragment to buffer
    // Push ACK
    // Finalize file if complete

    QueueSeqNumEntry entry;

    uint32_t recv_file_id = ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = ntohl(frame->payload.file_fragment.size);
 
    EnterCriticalSection(&list.mutex);

    if (client->file_id != recv_file_id){ 
        entry.seq_num = ntohll(frame->header.seq_num);
        entry.op_code = ERR_INVALID_FILE_ID;            
        entry.session_id = ntohl(frame->header.session_id);
        memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
        // Release active clients list mutex
        LeaveCriticalSection(&list.mutex);
        push_seq_num(&queue_seq_num, &entry);
        fprintf(stderr, "No file transfer in progress (no file buffer allocated)!!!\n");
        return RET_VAL_ERROR;

    } else if(client->file_buffer == NULL){
        entry.seq_num = ntohll(frame->header.seq_num);
        entry.op_code = STS_TRANSFER_COMPLETE;            //TODO - figure out how to differentiate between files that have no metadata and files that finished copying
        entry.session_id = ntohl(frame->header.session_id);
        memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
        // Release active clients list mutex
        LeaveCriticalSection(&list.mutex);       
        push_seq_num(&queue_seq_num, &entry);
        //fprintf(stderr, "No metadata for file fragment - assuming transfer was complete\n");
        return RET_VAL_ERROR;
    }
    //copy the received fragment text to the buffer

    if(check_fragment_received(client->file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        //TODO - send nack frame with duplicate frame code?
        //ack frame is sent in the handle via the queue
        entry.seq_num = ntohll(frame->header.seq_num);
        entry.op_code = ERR_DUPLICATE_FRAME;
        entry.session_id = ntohl(frame->header.session_id);
        memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
        // Release active clients list mutex
        LeaveCriticalSection(&list.mutex);       
        push_seq_num(&queue_seq_num, &entry);
        //fprintf(stderr, "Received duplicate frame (offset: %zu)!!!\n", recv_fragment_offset);
        return RET_VAL_ERROR;
    }

    if(recv_fragment_offset >= client->file_size){
        // Release active clients list mutex
        LeaveCriticalSection(&list.mutex);
        fprintf(stderr, "Received fragment with offset out of limits. File size: %zu, Received offset: %zu\n", client->file_size, recv_fragment_offset);
        return RET_VAL_ERROR;
    } else if (recv_fragment_offset + recv_fragment_size > client->file_size) {
        // Release active clients list mutex
        LeaveCriticalSection(&list.mutex);
        fprintf(stderr, "Fragment extends past file bounds\n");
        return RET_VAL_ERROR;
    }

    char *dest = client->file_buffer + recv_fragment_offset;
    char *src = frame->payload.file_fragment.bytes;
    memcpy(dest, src, recv_fragment_size);
    client->file_bytes_received += recv_fragment_size;
 
    //file_fragment_write(FILE_PATH, frame->payload.file_fragment.bytes, recv_fragment_size, recv_fragment_offset);
   
    mark_fragment_received(client->file_bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE);

    // Push seq num on the ack queue for acknowledge
    entry.seq_num = ntohll(frame->header.seq_num);
    entry.op_code = STS_ACK;
    entry.session_id = ntohl(frame->header.session_id);
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
    // Release active clients list mutex
    LeaveCriticalSection(&list.mutex);       
    push_seq_num(&queue_seq_num, &entry);

    update_statistics(client);

    if(client->file_bytes_received == client->file_size && check_bitmap(client->file_bitmap, client->file_fragment_count)){       
        snprintf(client->file_name, PATH_SIZE, "E:\\out_file_%d.txt", client->session_id);
        if(create_output_file(client->file_buffer, client->file_bytes_received, client->file_name) != RET_VAL_SUCCESS){
            LeaveCriticalSection(&list.mutex); 
            return RET_VAL_ERROR;
        }

        fprintf(stdout, "Received file from %s:%d File ID: %d, Size: %zu\n", client->ip, client->port, client->file_id, client->file_bytes_received);   
        free(client->file_buffer);
        client->file_buffer = NULL;
        free(client->file_bitmap);
        client->file_bitmap = NULL;
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
            frame_entry.frame_size = bytes_received;
            if(frame_entry.frame_size > sizeof(UdpFrame)){
                fprintf(stdout, "Frame received with bytes > max frame size!\n");
                continue;
            }

            uint8_t frame_type = frame_entry.frame.header.frame_type;
            BOOL is_high_priority_frame = frame_type == FRAME_TYPE_KEEP_ALIVE || frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                            frame_type == FRAME_TYPE_FILE_METADATA || frame_type == FRAME_TYPE_DISCONNECT;

            QueueFrame *target_queue = NULL;
            if (is_high_priority_frame == TRUE) {
                target_queue = &queue_frame_ctrl;
            } else {
                target_queue = &queue_frame;
            }
            if (push_frame(target_queue, &frame_entry) != RET_VAL_SUCCESS) {
                continue;
            }
        }
    }
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
    QueueSeqNumEntry ack_entry;

    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    uint32_t frame_bytes_received;

    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;

    ClientData *client;

    while(server.status == SERVER_READY) {
        // Pop a frame from the queue (prioritize control queue)
        if (pop_frame(&queue_frame_ctrl, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame_ctrl
        } else if (pop_frame(&queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame
        } else {
            Sleep(100); // No frames to process, yield CPU
            continue;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        // Extract header fields   
        header_delimiter = ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = ntohll(frame->header.seq_num);
        header_session_id = ntohl(frame->header.session_id);
       
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = ntohs(src_addr->sin_port);

        if (header_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, header_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }

        // Find or add new client
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
                ack_entry.seq_num = header_seq_num;
                ack_entry.op_code = STS_KEEP_ALIVE;
                ack_entry.session_id = header_session_id;
                memcpy(&ack_entry.addr, &client->addr, sizeof(struct sockaddr_in));   
                push_seq_num(&queue_seq_num_ctrl, &ack_entry);
                //send_ack_nak(header_seq_num, header_session_id, STS_KEEP_ALIVE, server.socket, &client->addr, client->log_path);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
                       
            case FRAME_TYPE_FILE_METADATA:
                client->last_activity_time = time(NULL);
                handle_file_metadata(client, frame);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                client->last_activity_time = time(NULL);
                handle_file_fragment(client, frame);
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
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(void* ptr){

    time_t time_now;

    while(server.status == SERVER_READY) {
        
        for(int slot = 0; slot < MAX_CLIENTS; slot++){  
            time_now = time(NULL);
            if(list.client[slot].slot_status == SLOT_FREE){
                continue;
            }                
            if(time_now - (time_t)list.client[slot].last_activity_time < (time_t)server.session_timeout){
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
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- SendAck Thread Function ---
unsigned int WINAPI ack_nak_thread_func(void* ptr){

    ClientData *client = NULL;
    QueueSeqNumEntry entry;

    while (server.status == SERVER_READY) {
        memset(&entry, 0, sizeof(QueueSeqNumEntry));

        if(pop_seq_num(&queue_seq_num_ctrl, &entry) == RET_VAL_SUCCESS){

            fprintf(stdout, "Sending ctrl ack frame for session ID: %d, seq num: %zu, Ack op code: %d\n", entry.session_id, entry.seq_num, entry.op_code);
        } else if(pop_seq_num(&queue_seq_num, &entry) == RET_VAL_SUCCESS){
 
        } else {
            Sleep(100);
            continue;
        }     
        //check if client session still open (could be optional)
        // EnterCriticalSection(&list.mutex);
        // ClientData *client = find_client(&list, seq_num_entry.session_id);
        // LeaveCriticalSection(&list.mutex);
        // if(client == NULL) {
        //     fprintf(stdout, "Client is null\n");
        //     continue; // Nothing to send, skip to next iteration
        // }
        send_ack_nak(entry.seq_num, entry.session_id, entry.op_code, server.socket, &entry.addr, client->log_path);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}


// --- Process server command ---
unsigned int WINAPI server_command_thread_func(void* ptr){

    _endthreadex(0);
    return 0;
}