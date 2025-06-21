#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "udp_lib.h"
#include "udp_queue.h"
#include "udp_hash.h"

// --- Constants ---
#define SERVER_PORT                     12345       // Port the server listens on
#define FRAME_DELIMITER                 0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS                 100         // Timeout for recvfrom in milliseconds in the receive thread
#define FILE_TRANSFER_TIMEOUT_SEC       60          // Seconds after which a stalled file transfer is cleaned up
#define CLIENT_ID                       0xAA        // Example client ID, can be set dynamically
#define CLIENT_NAME                     "lkdc UDP Text/File Transfer Client"

#define TEXT_FRAGMENT_SIZE              (MAX_PAYLOAD_SIZE - sizeof(uint32_t) * 4)
#define TEXT_CHUNK_SIZE                 (FILE_FRAGMENT_SIZE * 128)

#define FILE_FRAGMENT_SIZE              (MAX_PAYLOAD_SIZE - sizeof(uint32_t) * 3)
#define FILE_CHUNK_SIZE                 (FILE_FRAGMENT_SIZE * 128)

#define RESEND_TIMEOUT                  5           //seconds
#define RESEND_TIME_TRANSFER            1000        //miliseconds
#define RESEND_TIME_IDLE                10          //miliseconds


#define MAX_FILE_TRANSFER_THREADS       10



typedef uint8_t SessionStatus;
enum SessionStatus{
    SESSION_DISCONNECTED = 0,
    SESSION_CONNECTING = 1,
    SESSION_CONNECTED = 2
};

typedef uint8_t ClientStatus;
enum ClientStatus {
    CLIENT_STOP = 0,
    CLIENT_BUSY = 1,
    CLIENT_READY = 2,
    CLIENT_ERROR = 3
};

typedef struct{
    SOCKET socket;
    struct sockaddr_in client_addr;
    struct sockaddr_in server_addr;

    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];
    ClientStatus client_status;         
    SessionStatus session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint32_t frame_count;       // this will be sent as seq_num
    uint32_t message_id_count;
    uint32_t file_id_count;
    CRITICAL_SECTION frame_count_mutex;

    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[NAME_SIZE];       // Human readable server name
    
    uint32_t last_ping_ack_num;
    uint32_t last_pong_ack_num;
    time_t last_active_time;
    
    char log_file_path[255];

    uint32_t file_id;
    uint32_t total_bytes_to_send;
 
} ClientData;

ClientData client;

QueueFrame queue_frame;
QueueFrame queue_frame_ctrl;
//QueueSeqNum queue_seq_num;

HANDLE recieve_frame_thread;
HANDLE process_frame_thread;
HANDLE keep_alive_thread;
HANDLE command_thread;
HANDLE resend_thread;
HANDLE file_transfer_thread;


CRITICAL_SECTION hash_table_mutex;

HANDLE connection_successfull;
HANDLE send_file_event;


AckHashNode *hash_table[HASH_SIZE] = {NULL};

const char *server_ip = "10.10.10.1"; // loopback address
const char *client_ip = "10.10.10.3";

// --- Function prototypes ---
int send_long_text_fragment(const uint32_t seq_num, const uint32_t session_id, const uint32_t message_id, const uint32_t text_len, const uint32_t text_offset,
                                        const char* fragment_text, const uint32_t fragment_len, const uint32_t chunk_offset, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_file_metadata(const uint32_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint32_t file_size, const uint32_t max_fragment_size, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_file_fragment(const uint32_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint32_t fragment_offset,
                                        const char* chunk_buffer, const uint32_t fragment_size, const uint32_t chunk_offset, const SOCKET src_socket, const struct sockaddr_in *dest_addr);

unsigned int WINAPI command_thread_func(void* ptr);
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI keep_alive_thread_func(void* ptr);
unsigned int WINAPI resend_thread_func(void *ptr);

unsigned int WINAPI file_transfer_thread_func(void *ptr);

long get_text_file_size(const char *filepath){
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        printf("Error: Could not open file.\n");
        return -1;
    }
    // Seek to end to determine file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fclose(file);
    return(fileSize);
}
long get_file_size(const char *filepath){
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        printf("Error: Could not open file.\n");
        return -1;
    }
    // Seek to end to determine file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fclose(file);
    return(fileSize);
}
int read_text_file(const char *filepath, char *buffer, long size) {
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        printf("Error: Could not open file.\n");
        return -1;
    }
   if (buffer == NULL) {
        printf("Buffer error.\n");
        return -1;
    }
    memset(buffer, 0, size);
    // Read file into buffer
    fread(buffer, size, 1, file);
    buffer[size] = '\0'; // Null-terminate the buffer

    fclose(file);
    return 0;
}
int read_file(const char *filepath, char *buffer, long size) {
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        fprintf(stderr,"Error: Could not open file!!!\n");
        return -1;
    }
   if (buffer == NULL) {
        fprintf(stderr, "Buffer error!!!\n");
        return -1;
    }
    memset(buffer, 0, size);
    // Read file into buffer
    fread(buffer, size, 1, file);
    
    fclose(file);
    return 0;
}

uint32_t get_new_seq_num(){
    uint32_t new_seq_num = 0;
    EnterCriticalSection(&client.frame_count_mutex);
    new_seq_num = ++client.frame_count;
    LeaveCriticalSection(&client.frame_count_mutex);
    return new_seq_num;
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
void init_client(){
  
    memset(&client, 0, sizeof(ClientData));

    client.client_status = CLIENT_BUSY;

    client.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client.socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client.socket);
        cleanup_winsock();
        return;
    }
    // int size = 16 * 1024 * 1024;
    // setsockopt(client.socket, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size));
    // setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size));

    client.client_addr.sin_family = AF_INET;
    client.client_addr.sin_port = htons(0); // Let OS choose port
    client.client_addr.sin_addr.s_addr = inet_addr(client_ip);

    if (bind(client.socket, (struct sockaddr *)&client.client_addr, sizeof(client.client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        return;
    }
   
    // Define server address
    memset(&client.server_addr, 0, sizeof(client.server_addr));
    client.server_addr.sin_family = AF_INET;
    client.server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return;
    };

    //Initialize frame buffers (queue)
    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);

    queue_frame_ctrl.head = 0;
    queue_frame_ctrl.tail = 0;
    InitializeCriticalSection(&queue_frame_ctrl.mutex);
    
    client.frame_count = 0;
    client.message_id_count = 0xBB;
    client.file_id_count = 0xCC;
    strncpy(client.client_name, CLIENT_NAME, sizeof(CLIENT_NAME));
    client.client_name[sizeof(CLIENT_NAME) - 1] = '\0'; // Ensure null termination
    client.client_id = CLIENT_ID;
    client.client_status = CLIENT_READY;
    client.session_status = SESSION_DISCONNECTED;
    client.log_file_path[0] = '\0';
    InitializeCriticalSection(&client.frame_count_mutex);
    InitializeCriticalSection(&hash_table_mutex);
    

    client.file_id = 0;
    client.total_bytes_to_send = 0;

    send_file_event = CreateEvent(
        NULL,    // default security attributes
        TRUE,    // manual-reset event
        FALSE,   // initial state is non-signaled
        NULL     // unnamed event
    );

    if (send_file_event == NULL) {
        fprintf(stdout, "CreateEvent failed (%lu)\n", GetLastError());
        return;
    }

    connection_successfull = CreateEvent(
        NULL,    // default security attributes
        TRUE,    // manual-reset event
        FALSE,   // initial state is non-signaled
        NULL     // unnamed event
    );

    if (connection_successfull == NULL) {
        fprintf(stdout, "CreateEvent failed (%lu)\n", GetLastError());
        return;
    }

    return;
}
void start_threads(){

    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    resend_thread = (HANDLE)_beginthreadex(NULL, 0, resend_thread_func, NULL, 0, NULL);
    if (resend_thread == NULL) {
        fprintf(stderr, "Failed to create resend frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    command_thread = (HANDLE)_beginthreadex(NULL, 0, command_thread_func, NULL, 0, NULL);
    if (command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    file_transfer_thread = (HANDLE)_beginthreadex(NULL, 0, file_transfer_thread_func, NULL, 0, NULL);
        if (file_transfer_thread == NULL) {
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STOP; // Signal immediate shutdown
        }
}
void shutdown_client(){

    client.client_status = CLIENT_STOP;

    if (recieve_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(recieve_frame_thread, INFINITE);
        CloseHandle(recieve_frame_thread);
    }
    if (process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(process_frame_thread, INFINITE);
        CloseHandle(process_frame_thread);
    }
    if (resend_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(resend_thread, INFINITE);
        CloseHandle(resend_thread);
    }
    if (command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(command_thread, INFINITE);
        CloseHandle(command_thread);
    }
    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_frame_ctrl.mutex);
    DeleteCriticalSection(&client.frame_count_mutex);
    CloseHandle(send_file_event);
    //    DeleteCriticalSection(&queue_seq_num.mutex);
    // Cleanup
    closesocket(client.socket);
    cleanup_winsock();
}
// --- Main function ---
int main() {

    init_winsock();
    init_client(&client);   
    start_threads(&client);

    while(client.client_status == CLIENT_BUSY || client.client_status == CLIENT_READY){
        if(client.session_status == SESSION_DISCONNECTED){
            EnterCriticalSection(&client.frame_count_mutex);
            client.frame_count = 0;       // this will be sent as seq_num
            LeaveCriticalSection(&client.frame_count_mutex);
            client.message_id_count = 0xBB;
            client.file_id_count = 0xCC;
            client.log_file_path[0] = '\0'; 
            client.session_id = 0;
            client.server_status = 0;
            client.session_timeout = 0;           
        }     
        Sleep(100); // Simulate some delay between messages        
    }
    shutdown_client(&client);
    return 0;
}
// --- Function implementations ---
// --- Send long text message fragment ---
int send_long_text_fragment(const uint32_t seq_num, const uint32_t session_id, const uint32_t message_id, const uint32_t text_len, const uint32_t text_offset,
                                        const char* chunk_text, const uint32_t fragment_len, const uint32_t chunk_fragment_offset, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(chunk_text == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_LONG_TEXT_MESSAGE;
    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.long_text_msg.message_id = htonl(message_id);
    frame.payload.long_text_msg.total_text_len = htonl(text_len);
    frame.payload.long_text_msg.fragment_len = htonl(fragment_len);
    frame.payload.long_text_msg.fragment_offset = htonl(text_offset);
    
    // Copy the text data into the payload
    const char *fragment_text = chunk_text + chunk_fragment_offset;
    strncpy(frame.payload.long_text_msg.fragment_text, fragment_text, fragment_len);
    frame.payload.long_text_msg.fragment_text[fragment_len] = '\0';
    
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  
    EnterCriticalSection(&hash_table_mutex);
    insert_frame(hash_table, &frame);
    LeaveCriticalSection(&hash_table_mutex);
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }

//    fprintf(stdout, "Inserted SeqNum -%d- to Table\n", seq_num);
//    printTable(hash_table);
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
    #endif
    return bytes_sent;
}
// Send file metadata frame
int send_file_metadata(const uint32_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint32_t file_size, const uint32_t max_fragment_size, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;

    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = htonl(file_id);
    frame.payload.file_metadata.file_size = htonl(file_size);
    frame.payload.file_metadata.max_fragment_size = htonl(max_fragment_size);
       
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));  
    EnterCriticalSection(&hash_table_mutex);
    insert_frame(hash_table, &frame);
    LeaveCriticalSection(&hash_table_mutex);
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }

//    fprintf(stdout, "Inserted SeqNum -%d- to Table\n", seq_num);
//    printTable(hash_table);
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
    #endif
    return bytes_sent;
}
// Send file fragment frame
int send_file_fragment(const uint32_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint32_t fragment_offset,
                                        const char* chunk_buffer, const uint32_t fragment_size, const uint32_t chunk_offset, const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(chunk_buffer == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_FRAGMENT;
    frame.header.seq_num = htonl(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.file_fragment.file_id = htonl(file_id);
    frame.payload.file_fragment.size = htonl(fragment_size);
    frame.payload.file_fragment.offset = htonl(fragment_offset);
    
    // Copy the bytes data into the payload
    const char *fragment_bytes = chunk_buffer + chunk_offset;
    memcpy(frame.payload.file_fragment.bytes, fragment_bytes, fragment_size);
    
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileFragmentPayload)));  
    EnterCriticalSection(&hash_table_mutex);
    insert_frame(hash_table, &frame);
    LeaveCriticalSection(&hash_table_mutex);
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }

//    fprintf(stdout, "Inserted SeqNum -%d- to Table\n", seq_num);
//    printTable(hash_table);
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
    #endif
    return bytes_sent;
}

// --- Receive frame ---
unsigned int WINAPI receive_frame_thread_func(void* ptr) {
    
    while (client.session_status == SESSION_CONNECTING || client.session_status == SESSION_CONNECTED) {

        UdpFrame received_frame;
        memset(&received_frame, 0, sizeof(UdpFrame));
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        int src_addr_len = sizeof(src_addr);
    
        DWORD timeout = RECV_TIMEOUT_MS;
        if (setsockopt(client.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
            // Do not exit, but log the error
        }

        int bytes_received = recvfrom(client.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);

        if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECV_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
                // For critical errors, you might set 'running = 0;' here to shut down the server.
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue
            FrameEntry frame_entry;
            memset(&frame_entry, 0, sizeof(FrameEntry));
            memcpy(&frame_entry.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));          
            frame_entry.bytes_received = bytes_received;

            if(frame_entry.frame.header.frame_type == FRAME_TYPE_CONNECT_RESPONSE || frame_entry.frame.header.frame_type == FRAME_TYPE_DISCONNECT){
                if(push_frame(&queue_frame_ctrl, &frame_entry) == -1){
                    continue;
                }

            } else {
                if(push_frame(&queue_frame, &frame_entry) == -1){
                    continue;
                }
            }
        }
    }
    fprintf(stdout, "Receive frame thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(void* ptr) {

    FrameEntry frame_entry;

    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    uint32_t bytes_received;

    uint16_t received_delimiter;
    uint8_t  received_frame_type;
    uint32_t received_seq_num;
    uint32_t received_session_id;

    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;

    uint32_t received_session_timeout;
    uint8_t received_server_status;

    while(client.client_status == CLIENT_READY){

        if(pop_frame(&queue_frame_ctrl, &frame_entry) == -1){
            if(pop_frame(&queue_frame, &frame_entry) == -1){
                Sleep(100); // No frames to process, yield CPU
                continue;       
            }
        }       

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        bytes_received = frame_entry.bytes_received;

        // Extract header fields   
        received_delimiter = ntohs(frame->header.start_delimiter);
        received_frame_type = frame->header.frame_type;
        received_seq_num = ntohl(frame->header.seq_num);
        received_session_id = ntohl(frame->header.session_id);

        //fprintf(stdout, "received buffered frame seq nu: %d, nr of bytes: %d\n", received_seq_num, bytes_received);

        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        src_port = ntohs(src_addr->sin_port);

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
        // Find or add client (thread-safe access)
        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE:
                received_server_status = frame->payload.response.server_status;
                received_session_timeout = ntohl(frame->payload.response.session_timeout);               
                if(received_session_id == 0 || received_server_status == 0){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                 if(received_session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }                
                client.server_status = received_server_status;
                client.session_timeout = received_session_timeout;
                client.session_id = received_session_id;
                strncpy(client.server_name, frame->payload.response.server_name, sizeof(frame->payload.response.server_name) - 1);
                client.server_name[sizeof(frame->payload.response.server_name) - 1] = '\0';
                client.last_ping_ack_num = 0;
                client.last_pong_ack_num = 0;
                client.last_active_time = time(NULL);
                client.session_status = SESSION_CONNECTED;

                SetEvent(connection_successfull);
                
                keep_alive_thread = (HANDLE)_beginthreadex(NULL, 0, keep_alive_thread_func, NULL, 0, NULL);
                if (keep_alive_thread == NULL) {
                    fprintf(stderr, "Failed to create keep alive thread. Error: %d\n", GetLastError());
                    client.session_status = SESSION_DISCONNECTED;
                    client.client_status = CLIENT_STOP; // Signal immediate shutdown
                }
                #ifdef ENABLE_FRAME_LOG
                    create_log_frame_file(1, client.session_id, client.log_file_path);
                #endif
                break; 

            case FRAME_TYPE_ACK:
                if(received_session_id == client.session_id){
                    client.last_active_time = time(NULL);
                    EnterCriticalSection(&hash_table_mutex);
                    remove_frame(hash_table, received_seq_num);
                    LeaveCriticalSection(&hash_table_mutex);
                }
                break;

            case FRAME_TYPE_DISCONNECT:
                if(received_session_id == client.session_id){
                    client.session_status = SESSION_DISCONNECTED;
                    fprintf(stdout, "Session closed by server...\n");
                }
                break;

            case FRAME_TYPE_CONNECT_REQUEST:
                break;              
            default:
                break;
        }
        #ifdef ENABLE_FRAME_LOG
            log_frame(LOG_FRAME_RECV, frame, src_addr, client.log_file_path);
        #endif
    }
    fprintf(stdout, "Process frame thread exiting...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Send keep alive ---
unsigned int WINAPI keep_alive_thread_func(void* ptr){

    while(client.session_status == SESSION_CONNECTED){
        if(time(NULL) < (time_t)(client.last_active_time + client.session_timeout / 3)){
            Sleep(1000);
            continue;
        }
        send_keep_alive(get_new_seq_num(), client.session_id, client.socket, &client.server_addr, client.log_file_path);
        if(time(NULL) > (time_t)(client.last_active_time + client.session_timeout * 2)){
            client.session_status = SESSION_DISCONNECTED;
        }
//        Sleep(500);
    }
    fprintf(stdout, "Send keep alive thread exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Re-send frames that ack time expired ---
unsigned int WINAPI resend_thread_func(void *ptr){
   
    while(client.client_status == CLIENT_READY){
        if(client.session_status != SESSION_CONNECTED){
            EnterCriticalSection(&hash_table_mutex);
            clean_frame_hash_table(hash_table);
            LeaveCriticalSection(&hash_table_mutex);
            Sleep(1000);
            continue;
        }
        EnterCriticalSection(&hash_table_mutex);      
        for (int i = 0; i < HASH_SIZE; i++) {                           
            if(hash_table[i]){                       
                AckHashNode *ptr = hash_table[i];
                while (ptr) {     
                    if(time(NULL) - ptr->time > (time_t)RESEND_TIMEOUT){
                        ptr->time = time(NULL);
                        send_frame(&ptr->frame, client.socket, &client.server_addr);
                        //fprintf(stdout, "resent frame: %d\n", ntohl(ptr->frame.header.seq_num));
                    }
                    ptr = ptr->next;
                }
            }                                                
        }
        LeaveCriticalSection(&hash_table_mutex);
        //fprintf(stdout, "Bytes to send: %d\n", client.total_bytes_to_send);
        if(client.total_bytes_to_send > 0){
            Sleep(RESEND_TIME_TRANSFER);
        } else {
            Sleep(RESEND_TIME_IDLE);
        }
    }
    fprintf(stdout, "Exit resend thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Process command ---
unsigned int WINAPI command_thread_func(void* ptr) {

    char cmd;
    uint32_t command_num = 0;
    char *file_path = "E:\\test_file.txt";
    uint8_t file_chunk_buffer[FILE_CHUNK_SIZE];
    uint8_t text_chunk_buffer[TEXT_CHUNK_SIZE];
    FILE *file = NULL;
    uint32_t frame_fragment_offset = 0;
    uint32_t frame_fragment_len = 0;                              
    uint32_t total_bytes_to_send = 0;
    long file_size = 0;
    
    while(client.client_status == CLIENT_READY){

            fprintf(stdout,"Waiting for command...\n");

            cmd = getchar();
            switch(cmd) {
                case 'c':
                case 'C':
                    // if(client.session_status == SESSION_CONNECTED){
                    //     fprintf(stdout, "Already connected to server...\n");
                    //     break;
                    // }
                    send_connect_request(get_new_seq_num(), client.session_id, client.client_id, client.flags, client.client_name, client.socket, &client.server_addr, client.log_file_path);
                    client.session_status = SESSION_DISCONNECTED;
                    if (recieve_frame_thread) {
                        WaitForSingleObject(recieve_frame_thread, INFINITE);
                        CloseHandle(recieve_frame_thread);
                    }
                    client.session_status = SESSION_CONNECTING;
                    printf("Attempting to connect to server...\n");
                    Sleep(100);
                    recieve_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, NULL, 0, NULL);
                    if (recieve_frame_thread == NULL) {
                        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
                        client.session_status = SESSION_DISCONNECTED;
                        client.client_status = CLIENT_STOP; // Signal immediate shutdown
                    }
                    WaitForSingleObject(connection_successfull, 2500);
                    ResetEvent(connection_successfull);
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Connection to server failed...\n");
                        client.session_status = SESSION_DISCONNECTED;
                    } else {
                        fprintf(stdout, "Connection to server success...\n");
                    }                    
                    break;

                case 'd':
                case 'D':
                    send_disconnect(client.session_id, client.socket, &client.server_addr, client.log_file_path);
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Disconnecting from server...\n");
                    break;

                case 'q':
                case 'Q':
                    client.client_status = CLIENT_STOP;
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Shutting down...\n");
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 't':
                case 'T':

//                    fprintf(stdout, "Text to send: ");




                    uint8_t text_chunk_buffer[TEXT_CHUNK_SIZE];
                    uint32_t message_id = ++client.message_id_count;
                    frame_fragment_offset = 0;
                    frame_fragment_len = 0;
                    long text_size = get_text_file_size(file_path);
                    fprintf(stdout, "Total file bytes: %d\n", text_size);                    
                    file = fopen(file_path, "rb");
                    if(file == NULL){
                        fprintf(stdout, "Error opening file!\n");
                        break;
                    }                    
                    uint32_t total_bytes_to_send = text_size;
                    while(total_bytes_to_send > 0){ 
                        uint32_t bytes_read = fread(text_chunk_buffer, 1, TEXT_CHUNK_SIZE, file);
                        if (bytes_read == 0 && ferror(file)) {
                            fprintf(stdout, "Error reading file\n");
                            break;
                        }                    
                        uint32_t chunk_bytes_to_send = bytes_read;                        
                        // fprintf(stdout, "Chunk Bytes to send: %d\n", chunk_bytes_to_send);
                        // fprintf(stdout, "Fragment Offset: %d\n", frame_fragment_offset);                        
                        uint32_t chunk_fragment_offset = 0;
                        while (chunk_bytes_to_send > 0){
                            if(chunk_bytes_to_send > TEXT_FRAGMENT_SIZE){
                                frame_fragment_len = TEXT_FRAGMENT_SIZE;
                            } else {
                            frame_fragment_len = chunk_bytes_to_send;
                            } 
                            send_long_text_fragment(get_new_seq_num(), client.session_id, message_id, text_size, frame_fragment_offset, text_chunk_buffer, frame_fragment_len, chunk_fragment_offset, client.socket, &client.server_addr);
                            chunk_fragment_offset += frame_fragment_len;
                            frame_fragment_offset += frame_fragment_len;                       
                            chunk_bytes_to_send -= frame_fragment_len;
                            total_bytes_to_send -= frame_fragment_len;
                        }                       
                    }
                    fclose(file);
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'f':
                case 'F':
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Not connected to server\n");
                        break;
                    }
                    client.file_id = ++client.file_id_count;
                    SetEvent(send_file_event);
                    // if(!client.file_transfer_busy){
                    //     client.file_transfer_thread = (HANDLE)_beginthreadex(NULL, 0, file_transfer_thread_func, &file_id, 0, NULL);
                    //     client.file_transfer_busy = TRUE;

                        break;
                    fprintf(stdout, "Client busy -> file transfer in progress...\n");

                    break;

                case '\n':
                    break;
                default:
                    fprintf(stdout, "Invalid command!\n");
                    break;
                
            }
        Sleep(100); 
    }        
    fprintf(stdout, "Send command exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    
    return 0;
}
// --- File transfer thread function ---
unsigned int WINAPI file_transfer_thread_func(void *ptr){

    char *file_path = "E:\\test_file.txt";
    FILE *file = NULL;

    uint8_t chunk_buffer[FILE_CHUNK_SIZE];
    long file_size;
    //uint32_t total_bytes_to_send;
    uint32_t chunk_bytes_to_send;
    uint32_t chunk_fragment_offset;

    uint32_t frame_fragment_size;
    uint32_t frame_fragment_offset;

    while(client.client_status = CLIENT_READY){
        
        WaitForSingleObject(send_file_event, INFINITE);
        ResetEvent(send_file_event);

        if(client.session_status != SESSION_CONNECTED){
            fprintf(stdout, "Session with server closed!!!\n");
            continue;           
        }
        file = fopen(file_path, "rb");
        if(file == NULL){
            fprintf(stdout, "Error opening file!!!\n");
            continue;
        }
        file_size = get_file_size(file_path);
        if(file_size > UINT32_MAX){
            fprintf(stdout, "File too big!!!\n");
            continue;
        }

        send_file_metadata(get_new_seq_num(), client.session_id, client.file_id, file_size, FILE_FRAGMENT_SIZE, client.socket, &client.server_addr);
        
        frame_fragment_offset = 0;
        client.total_bytes_to_send = (uint32_t)file_size;

        while(client.total_bytes_to_send > 0){

            if(client.session_status != SESSION_CONNECTED){
                fprintf(stdout, "Session with server closed!!!\n");
                client.total_bytes_to_send = 0;
                continue;
            }

            chunk_bytes_to_send = fread(chunk_buffer, 1, FILE_CHUNK_SIZE, file);
            if (chunk_bytes_to_send == 0 && ferror(file)) {
                fprintf(stdout, "Error reading file\n");
                client.total_bytes_to_send = 0;
                continue;
            }
            
            // fprintf(stdout, "Chunk Bytes to send: %d\n", chunk_bytes_to_send);
            // fprintf(stdout, "Fragment Offset: %d\n", frame_fragment_offset);
            chunk_fragment_offset = 0;

            while (chunk_bytes_to_send > 0){
                if(chunk_bytes_to_send > FILE_FRAGMENT_SIZE){
                    frame_fragment_size = FILE_FRAGMENT_SIZE;
                } else {
                    frame_fragment_size = chunk_bytes_to_send;
                }    
                send_file_fragment(get_new_seq_num(), client.session_id, client.file_id, frame_fragment_offset, chunk_buffer, frame_fragment_size, chunk_fragment_offset, client.socket, &client.server_addr);
                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                client.total_bytes_to_send -= frame_fragment_size;
                fprintf(stdout, "\rProgress: %.2f %%", (float)(file_size - client.total_bytes_to_send) / (float)file_size * 100.0);
            }     
        }                  
        fprintf(stdout, "Sent bytes: %d\n", file_size);
        fclose(file);    

    }

    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;               
}