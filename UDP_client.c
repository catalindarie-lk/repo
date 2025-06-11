#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "UDP_lib.h"

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

// --- Constants ---
#define SERVER_PORT         12345       // Port the server listens on
#define MAX_PAYLOAD_SIZE    1024        // Max size of data within a frame payload (adjust as needed)
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
    struct sockaddr_in server_addr;
    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];
    ClientStatus client_status;         
    SessionStatus session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint32_t frame_seq_num;       // this will be sent as seq_num


    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[NAME_SIZE];       // Human readable server name

    QueueAck queue_ack;            // Queue for frames to be ack by the client
    QueueFrame queue_frame;

    HANDLE recieve_frame_thread;
    HANDLE process_frame_thread;
    HANDLE send_ack_thread;

    HANDLE ping_pong_thread;
    
    uint32_t last_ping_ack_num;
    uint32_t last_pong_ack_num;
    time_t last_server_active_time;
    time_t last_client_active_time;

    HANDLE command_thread;
 
} StructClient;

StructClient client;

const char *server_ip = "127.0.0.1"; // loopback address

// --- Function prototypes ---
void send_connect_request(const uint32_t client_id, const uint32_t flag, const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
void send_text_message(const uint32_t seq_nr, const uint32_t session_id, const char* text, const uint32_t len, const SOCKET src_socket, const struct sockaddr_in *dest_addr);

unsigned int WINAPI command_thread_func(LPVOID lpParam);
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);
unsigned int WINAPI ping_pong_thread_func(LPVOID lpParam);

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

    client->client_status = CLIENT_BUSY;

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
    client->queue_frame.head = 0;
    client->queue_frame.tail = 0;
    InitializeCriticalSection(&client->queue_frame.mutex);

    uint32_t len = sizeof(CLIENT_NAME) - 1;
    strncpy(client->client_name, CLIENT_NAME, len);
    client->client_name[len] = '\0'; // Ensure null termination
    client->client_id = CLIENT_ID;
 
    client->client_status = CLIENT_READY;
    client->session_status = SESSION_DISCONNECTED;
    return;
}

void start_threads(StructClient *client){

    client->process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, client, 0, NULL);
    if (client->process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client->session_status = SESSION_DISCONNECTED;
        client->client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    client->send_ack_thread = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, client, 0, NULL);
        if (client->send_ack_thread == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        client->session_status = SESSION_DISCONNECTED;
        client->client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    client->command_thread = (HANDLE)_beginthreadex(NULL, 0, command_thread_func, client, 0, NULL);
    if (client->command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        client->session_status = SESSION_DISCONNECTED;
        client->command_thread = CLIENT_STOP; // Signal immediate shutdown
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
    if (client->ping_pong_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client->ping_pong_thread, INFINITE);
        CloseHandle(client->ping_pong_thread);
    }
    if (client->command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client->command_thread, INFINITE);
        CloseHandle(client->command_thread);
    }
    DeleteCriticalSection(&client->queue_frame.mutex);
    DeleteCriticalSection(&client->queue_ack.mutex);
    // Cleanup
    closesocket(client->socket);
    cleanup_winsock();
}
// --- Main function ---
int main() {

    init_winsock();
    init_client(&client);   
    start_threads(&client);

    while(client.client_status == CLIENT_BUSY || client.client_status == CLIENT_READY){
        if(client.session_status == SESSION_DISCONNECTED){
            client.frame_seq_num = 0;       // this will be sent as seq_num
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
void send_connect_request(const uint32_t client_id, const uint32_t flag, const char *client_name, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = 0;
    frame.header.session_id = 0;
    frame.payload.request.client_id = htonl(client_id);
    frame.payload.request.flag = flag;

    strncpy(frame.payload.request.client_name, client_name, NAME_SIZE - 1);
    frame.payload.request.client_name[NAME_SIZE - 1] = '\0';

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
    frame.payload.text_msg.len = htonl(len);
    // Copy the text data into the payload
    strncpy(frame.payload.text_msg.text, text, len);
    frame.payload.text_msg.text[len] = '\0';
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
 
    while (client->session_status == SESSION_CONNECTING || client->session_status == SESSION_CONNECTED) {

        UdpFrame received_frame;
        memset(&received_frame, 0, sizeof(UdpFrame));
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        int src_addr_len = sizeof(src_addr);
    
        DWORD timeout = RECV_TIMEOUT_MS;
        if (setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
            // Do not exit, but log the error
        }

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
            if(push_frame(&client->queue_frame, &frame_data) == -1){
                continue;
            };

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

        if(client->queue_frame.head == client->queue_frame.tail) {
            Sleep(10); // No frames to process, yield CPU
            continue; // Re-check the queue after waking up
        }
          
        FrameData frame_data;
        if(pop_frame(&client->queue_frame, &frame_data) == -1){
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

        // 3. Process Payload based on Frame Type
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE: {
                uint32_t session_id = ntohl(frame->header.session_id);
                uint32_t session_timeout = ntohl(frame->payload.response.session_timeout);
                uint8_t server_status = frame->payload.response.server_status;
                if(session_id == 0 || server_status == 0){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    client->session_status = SESSION_DISCONNECTED;
                    break;
                }
                 if(session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    client->session_status = SESSION_DISCONNECTED;
                    break;
                }
                client->session_id = session_id;
                client->server_status = server_status;
                client->session_timeout = session_timeout;
                uint32_t len = sizeof(frame->payload.response.server_name) - 1;
                strncpy(client->server_name, frame->payload.response.server_name, len);
                client->server_name[len] = '\0';
                client->last_ping_ack_num = 0;
                client->last_pong_ack_num = 0;
                client->last_server_active_time = time(NULL);
                client->session_status = SESSION_CONNECTED;
                
                client->ping_pong_thread = (HANDLE)_beginthreadex(NULL, 0, ping_pong_thread_func, client, 0, NULL);
                if (client->ping_pong_thread == NULL) {
                    fprintf(stderr, "Failed to create ping pong thread. Error: %d\n", GetLastError());
                    client->session_status = SESSION_DISCONNECTED;
                    client->ping_pong_thread = CLIENT_STOP; // Signal immediate shutdown
                }
                //TODO:
                break; 
            }

            case FRAME_TYPE_ACK: {
                uint32_t ack_seq_num = ntohl(frame->payload.ack_nak.seq_num);
                uint8_t flag = frame->payload.ack_nak.flag;
                client->last_server_active_time = time(NULL);
                break;
            }

            case FRAME_TYPE_DISCONNECT: {
                uint8_t flag = frame->payload.disconnect.flag;
                if(flag == DISCONNECT_TIMEOUT){
                    printf("Server closed the session due to incativity...\n");
                } 
                client->session_status = SESSION_DISCONNECTED;
                printf("Session closed...\n");
                break;
            }
            case FRAME_TYPE_PING:
                break;
            case FRAME_TYPE_PONG:
                uint32_t pong_ack_num = ntohl(frame->payload.ping_pong.ack_num);
                if(pong_ack_num > client->last_pong_ack_num && pong_ack_num <= client->last_ping_ack_num){
                    client->last_pong_ack_num = pong_ack_num;
                    client->last_server_active_time = time(NULL);
                }
                break;
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
        uint32_t ack_seq_num = client->queue_ack.seq_num[client->queue_ack.head];
        if(pop_seq_num(&client->queue_ack) == -1) {
            continue; // Nothing to send, skip to next iteration
        }
        send_ack_nack(FRAME_TYPE_ACK, ack_seq_num, client->session_id, client->socket, &client->server_addr);
        client->last_client_active_time = time(NULL);
    }
    fprintf(stdout, "Send ack thread exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}

unsigned int WINAPI ping_pong_thread_func(LPVOID lpParam){

    StructClient *client = (StructClient *)lpParam;

    while(client->session_status == SESSION_CONNECTED){
        if(time(NULL) < (time_t)(client->last_server_active_time + client->session_timeout / 3)){
            continue;
        }
        send_ping_pong(FRAME_TYPE_PING, ++client->last_ping_ack_num ,client->session_id, client->socket, &client->server_addr);
        client->last_client_active_time = time(NULL);
        if(time(NULL) > (time_t)(client->last_server_active_time + client->session_timeout * 2)){
            client->session_status = SESSION_DISCONNECTED;
        }
        Sleep(1000);
    }
    fprintf(stdout, "Send ping-pong thread exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}

unsigned int WINAPI command_thread_func(LPVOID lpParam) {

    StructClient *client = (StructClient *)lpParam;

    char command_str[255];
    uint32_t command_num = 0;

    while(client->client_status == CLIENT_READY){

            fprintf(stdout,"Waiting for command...\n");
//            char c = getchar();
            command_str[0] = '\0';
            command_num = 0;
            fgets(command_str, sizeof(command_str), stdin);
            command_str[strcspn(command_str, "\n")] = '\0';
            if(strcmp(command_str, "c") == 0 || strcmp(command_str, "C") == 0) command_num = 1;
            if(strcmp(command_str, "d") == 0 || strcmp(command_str, "D") == 0) command_num = 2;
            if(strcmp(command_str, "q") == 0 || strcmp(command_str, "Q") == 0) command_num = 3;
            if(strcmp(command_str, "t") == 0 || strcmp(command_str, "T") == 0) command_num = 4;
            switch(command_num) {
                case 1:
                    if(client->session_status == SESSION_CONNECTED){
                        printf("Already connected\n");
                        continue;
                    }
                    send_connect_request(client->client_id, client->flags, client->client_name, client->socket, &client->server_addr);
                    client->session_status = SESSION_CONNECTING;
                    client->last_client_active_time = time(NULL);
                    printf("Attempting to connect to server...\n");
                    client->recieve_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, client, 0, NULL);
                    if (client->recieve_frame_thread == NULL) {
                        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
                        client->session_status = SESSION_DISCONNECTED;
                        client->client_status = CLIENT_STOP; // Signal immediate shutdown
                    }
                    Sleep(1000);
                    if(client->session_status != SESSION_CONNECTED){
                        for(int i = 0; i < 3; i++){
                            client->session_status = SESSION_CONNECTING;
                            send_connect_request(client->client_id, client->flags, client->client_name, client->socket, &client->server_addr);
                            printf("Re-attempting to connect to server...\n");
                            Sleep(1000);
                            if(client->session_status == SESSION_CONNECTED){
                                break;
                            }
                        }    
                        client->session_status = SESSION_DISCONNECTED;
                        printf("Server not responding...\n");
                    } 
                    break;

                case 2:
                    send_disconnect(DISCONNECT_REQUEST, client->session_id, client->socket, &client->server_addr);
                    client->session_status = SESSION_DISCONNECTED;
                    printf("Disconnecting from server...\n");
                    break;

                case 3:
                    client->client_status = CLIENT_STOP;
                    client->session_status = SESSION_DISCONNECTED;
                    printf("Shutting down...\n");
                    break;

                case 4:                    
                    fprintf(stdout,"Text message to send:");
                    char text_buffer[255] = {0};
                    fgets(text_buffer, sizeof(text_buffer), stdin);
                    text_buffer[strcspn(text_buffer, "\n")] = '\0';
                    send_text_message(++client->frame_seq_num, client->session_id, text_buffer, strlen(text_buffer), client->socket, &client->server_addr);
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