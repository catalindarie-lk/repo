#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "UDP_lib.h"

//#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

// --- Constants ---
#define SERVER_PORT         12345       // Port the server listens on
#define FRAME_DELIMITER     0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS     100         // Timeout for recvfrom in milliseconds in the receive thread
#define FILE_TRANSFER_TIMEOUT_SEC 60    // Seconds after which a stalled file transfer is cleaned up
#define CLIENT_ID           0xAA        // Example client ID, can be set dynamically
#define CLIENT_NAME         "lkdc UDP Text/File Transfer Client"

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
    CRITICAL_SECTION frame_count_mutex;

    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[NAME_SIZE];       // Human readable server name
    
    uint32_t last_ping_ack_num;
    uint32_t last_pong_ack_num;
    time_t last_server_active_time;
    time_t last_client_active_time;

    char log_file_path[255];
 
} ClientData;

ClientData client;

QueueFrame queue_frame;
QueueSeqNum queue_sqn;

HANDLE recieve_frame_thread;
HANDLE process_frame_thread;
HANDLE ack_nak_thread;
HANDLE ping_pong_thread;
HANDLE command_thread;

const char *server_ip = "10.10.10.1"; // loopback address
const char *client_ip = "10.10.10.2";

// --- Function prototypes ---
int send_connect_request(const uint32_t session_id, const uint32_t client_id, const uint32_t flag, 
                                        const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_text_message(const uint32_t seq_nr, const uint32_t session_id, const char* text, const uint32_t len, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int send_long_text_fragment(const uint32_t seq_nr, const uint32_t session_id, const uint32_t message_id, const char* fragment_text, 
                                        const uint32_t total_len, const uint32_t fragment_len, const uint32_t fragment_offset, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr);                                        

unsigned int WINAPI command_thread_func(void* ptr);
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI ping_pong_thread_func(void* ptr);
unsigned int WINAPI ack_nak_thread_func(void* ptr);


long get_text_file_size(const char *filepath){
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (!file) {
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
    client.client_addr.sin_family = AF_INET;
    client.client_addr.sin_port = htons(0); // Let OS choose port
    client.client_addr.sin_addr.s_addr = inet_addr(client_ip);

    if (bind(client.socket, (struct sockaddr *)&client.client_addr, sizeof(client.client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        return;
    }

    // Create UDP socket
    
    
    // Define server address
    memset(&client.server_addr, 0, sizeof(client.server_addr));
    client.server_addr.sin_family = AF_INET;
    client.server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return;
    };

    queue_sqn.head = 0;
    queue_sqn.tail = 0;
    InitializeCriticalSection(&queue_sqn.mutex);
    
    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);
    
    client.frame_count = 0;    
    uint32_t len = sizeof(CLIENT_NAME) - 1;
    strncpy(client.client_name, CLIENT_NAME, len);
    client.client_name[len] = '\0'; // Ensure null termination
    client.client_id = CLIENT_ID;
    client.client_status = CLIENT_READY;
    client.session_status = SESSION_DISCONNECTED;
    client.log_file_path[0] = '\0';
    InitializeCriticalSection(&client.frame_count_mutex);
    return;
}
void start_threads(){

    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    ack_nak_thread = (HANDLE)_beginthreadex(NULL, 0, ack_nak_thread_func, NULL, 0, NULL);
        if (ack_nak_thread == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    command_thread = (HANDLE)_beginthreadex(NULL, 0, command_thread_func, NULL, 0, NULL);
    if (command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
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
    if (ack_nak_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(ack_nak_thread, INFINITE);
        CloseHandle(ack_nak_thread);
    }
    if (ping_pong_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(ping_pong_thread, INFINITE);
        CloseHandle(ping_pong_thread);
    }
    if (command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(command_thread, INFINITE);
        CloseHandle(command_thread);
    }
    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_sqn.mutex);
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
// --- Send connect request ---
int send_connect_request(const uint32_t session_id, const uint32_t client_id, const uint32_t flag, 
                                        const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = 0;
    frame.header.session_id = htonl(session_id);
    frame.payload.request.client_id = htonl(client_id);
    frame.payload.request.flag = flag;

    strncpy(frame.payload.request.client_name, client_name, NAME_SIZE - 1);
    frame.payload.request.client_name[NAME_SIZE - 1] = '\0';

    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_LOGGING
        if(&client != NULL && strlen(client.log_file_path) > 0){
            log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
        }
    #endif
    return bytes_sent; 
};
// --- Send text message ---
int send_text_message(const uint32_t seq_nr, const uint32_t session_id, const char* text, const uint32_t len, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(text == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    frame.header.seq_num = htonl(seq_nr);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.text_msg.len = htonl(len);
    // Copy the text data into the payload
    strncpy(frame.payload.text_msg.text, text, len);
    frame.payload.text_msg.text[len] = '\0';
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(TextPayload)));  
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_LOGGING
        if(&client != NULL && strlen(client.log_file_path) > 0){
            log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
        }
    #endif
    return bytes_sent;
};
// --- Send long text message fragment ---
int send_long_text_fragment(const uint32_t seq_nr, const uint32_t session_id, const uint32_t message_id, const char* text, 
                                        const uint32_t total_len, const uint32_t fragment_len, const uint32_t fragment_offset, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(text == NULL){
        fprintf(stderr, "Invalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_LONG_TEXT_MESSAGE;
    frame.header.seq_num = htonl(seq_nr);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.long_text_msg.message_id = htonl(message_id);
    frame.payload.long_text_msg.total_text_len = htonl(total_len);
    frame.payload.long_text_msg.fragment_len = htonl(fragment_len);
    frame.payload.long_text_msg.fragment_offset = htonl(fragment_offset);

    // Copy the text data into the payload
    const char *fragment_text = text + fragment_offset;
    strncpy(frame.payload.long_text_msg.fragment_text, fragment_text, fragment_len);
    frame.payload.long_text_msg.fragment_text[fragment_len] = '\0';
    
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_LOGGING
        if(&client != NULL && strlen(client.log_file_path) > 0){
            log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_file_path);
        }
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
            if(push_frame(&queue_frame, &frame_entry) == -1){
                fprintf(stderr, "Pushing frame error!!!\n");
                continue;
            };

        }
    }
    fprintf(stdout, "Receive frame thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(void* ptr) {

     while(client.client_status == CLIENT_READY){

        FrameEntry frame_entry;
        if(pop_frame(&queue_frame, &frame_entry) == -1) {
            Sleep(10); // No frames to process, yield CPU
            continue; // Re-check the queue after waking up
        }        

        UdpFrame *frame = &frame_entry.frame;
        struct sockaddr_in *src_addr = &frame_entry.src_addr;
        uint32_t bytes_received = frame_entry.bytes_received;

        // Extract header fields   
        uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
        uint8_t  received_frame_type = frame->header.frame_type;
        uint32_t received_seq_num = ntohl(frame->header.seq_num);

        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
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
        // Find or add client (thread-safe access)
        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE: {
                uint32_t session_id = ntohl(frame->header.session_id);
                uint32_t session_timeout = ntohl(frame->payload.response.session_timeout);
                uint8_t server_status = frame->payload.response.server_status;
                if(session_id == 0 || server_status == 0){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                 if(session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                client.session_id = session_id;
                client.server_status = server_status;
                client.session_timeout = session_timeout;
                uint32_t len = sizeof(frame->payload.response.server_name) - 1;
                strncpy(client.server_name, frame->payload.response.server_name, len);
                client.server_name[len] = '\0';
                client.last_ping_ack_num = 0;
                client.last_pong_ack_num = 0;
                client.last_server_active_time = time(NULL);
                client.session_status = SESSION_CONNECTED;
                
                ping_pong_thread = (HANDLE)_beginthreadex(NULL, 0, ping_pong_thread_func, NULL, 0, NULL);
                if (ping_pong_thread == NULL) {
                    fprintf(stderr, "Failed to create ping pong thread. Error: %d\n", GetLastError());
                    client.session_status = SESSION_DISCONNECTED;
                    client.client_status = CLIENT_STOP; // Signal immediate shutdown
                }
                #ifdef ENABLE_LOGGING
                    create_log_frame_file(1, client.session_id, client.log_file_path);
                #endif
                break; 
            }

            case FRAME_TYPE_ACK: {
                uint8_t flag = frame->payload.ack_nak.flag;
                client.last_server_active_time = time(NULL);
                break;
            }

            case FRAME_TYPE_DISCONNECT: {
                uint8_t flag = frame->payload.disconnect.flag;
                if(flag == DISCONNECT_TIMEOUT){
                    fprintf(stdout, "Server closed the session due to incativity...\n");
                } 
                client.session_status = SESSION_DISCONNECTED;
                fprintf(stdout, "Session closed...\n");
                break;
            }
            case FRAME_TYPE_PING:
                break;
            case FRAME_TYPE_PONG:
                uint32_t pong_ack_num = ntohl(frame->payload.ping_pong.ack_num);
                if(pong_ack_num > client.last_pong_ack_num && pong_ack_num <= client.last_ping_ack_num){
                    client.last_pong_ack_num = pong_ack_num;
                    client.last_server_active_time = time(NULL);
                }
                break;
            case FRAME_TYPE_CONNECT_REQUEST: {
                break;
            }                
            default:
                break;
        }
        #ifdef ENABLE_LOGGING
            if(&client != NULL && strlen(client.log_file_path) > 0){
                log_frame(LOG_FRAME_RECV, frame, src_addr, client.log_file_path);
            }
        #endif
    }
    fprintf(stdout, "Process frame thread exiting...\n");
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Send ping-pong ---
unsigned int WINAPI ping_pong_thread_func(void* ptr){

    while(client.session_status == SESSION_CONNECTED){
        if(time(NULL) < (time_t)(client.last_server_active_time + client.session_timeout / 3)){
            continue;
        }
        send_ping_pong(FRAME_TYPE_PING, ++client.last_ping_ack_num ,client.session_id, client.socket, &client.server_addr, client.log_file_path);
        client.last_client_active_time = time(NULL);
        if(time(NULL) > (time_t)(client.last_server_active_time + client.session_timeout * 2)){
            client.session_status = SESSION_DISCONNECTED;
        }
        Sleep(500);
    }
    fprintf(stdout, "Send ping-pong thread exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Send Ack ---
unsigned int WINAPI ack_nak_thread_func(void* ptr){

    SeqNumEntry seq_num_entry;   
    while(client.client_status == CLIENT_READY){
        memset(&seq_num_entry, 0, sizeof(SeqNumEntry));
        if(pop_seq_num(&queue_sqn, &seq_num_entry) == -1){
            Sleep(10);
            continue;
        }
        send_ack_nak(seq_num_entry.type, seq_num_entry.seq_num, client.session_id, client.socket, &seq_num_entry.addr, client.log_file_path);
        client.last_client_active_time = time(NULL);
    }
    fprintf(stdout, "Exit ack/nak thread...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Process command ---
unsigned int WINAPI command_thread_func(void* ptr) {

     char command_str[255];
    uint32_t command_num = 0;

    while(client.client_status == CLIENT_READY){

            fprintf(stdout,"Waiting for command...\n");
            command_str[0] = '\0';
            command_num = 0;
            fgets(command_str, sizeof(command_str), stdin);
            command_str[strcspn(command_str, "\n")] = '\0';
            if(strcmp(command_str, "c") == 0 || strcmp(command_str, "C") == 0) command_num = 1;
            if(strcmp(command_str, "d") == 0 || strcmp(command_str, "D") == 0) command_num = 2;
            if(strcmp(command_str, "q") == 0 || strcmp(command_str, "Q") == 0) command_num = 3;
            if(strcmp(command_str, "t") == 0 || strcmp(command_str, "T") == 0) command_num = 4;
            if(strcmp(command_str, "f") == 0 || strcmp(command_str, "F") == 0) command_num = 5;
            switch(command_num) {
                case 1:
                    // if(client.session_status == SESSION_CONNECTED){
                    //     fprintf(stdout, "Already connected to server...\n");
                    //     break;
                    // }
                    send_connect_request(client.session_id, client.client_id, client.flags, client.client_name, client.socket, &client.server_addr);
                    client.session_status = SESSION_DISCONNECTED;
                    client.last_client_active_time = time(NULL);                   
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
                    Sleep(1000);
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Connection to server failed...\n");
                        client.session_status = SESSION_DISCONNECTED;
                    } else {
                        fprintf(stdout, "Connection to server success...\n");
                    }                    
                    break;

                case 2:
                    send_disconnect(DISCONNECT_REQUEST, client.session_id, client.socket, &client.server_addr, client.log_file_path);
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Disconnecting from server...\n");
                    break;

                case 3:
                    client.client_status = CLIENT_STOP;
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Shutting down...\n");
                    break;

                case 4:                    
                    fprintf(stdout,"Text message to send:");
                    char text_buffer[255] = {0};
                    fgets(text_buffer, sizeof(text_buffer), stdin);
                    text_buffer[strcspn(text_buffer, "\n")] = '\0';
                    send_text_message(get_new_seq_num(), client.session_id, text_buffer, strlen(text_buffer), client.socket, &client.server_addr);
                    break;
                
                case 5:
                    char *file_path = "f:\\vscode\\test_long_text.txt";
                    long text_size = get_text_file_size(file_path);
                    char *text = (char *)malloc(text_size + 1);
                    read_text_file(file_path, text, text_size);

                    uint32_t max_fragment_size = MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 4);
                    uint32_t fragment_offset = 0;
                    uint32_t message_id = 0xBB;
                    uint32_t fragment_len;
                    
                    int bytes_to_send = text_size;
                    while (bytes_to_send > 0){
                        if(bytes_to_send > max_fragment_size){
                            fragment_len = max_fragment_size;
                        } else {
                        fragment_len = bytes_to_send;
                        }
                        send_long_text_fragment(get_new_seq_num(), client.session_id, message_id, text, text_size, fragment_len, fragment_offset, client.socket, &client.server_addr);
                        fragment_offset += fragment_len;
                        bytes_to_send -= fragment_len;
                    }

                    free(text); // Free allocated memory

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

