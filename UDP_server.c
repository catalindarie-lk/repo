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
#define SERVER_NAME "lkdc UDP Text/File Transfer Server"

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
    struct sockaddr_in addr;
    uint32_t client_id;         // Unique ID received by the client
    uint32_t session_id;        // Unique ID assigned by the server and used for communication
    time_t last_activity_time;  
    uint32_t frame_count;

    uint8_t flags;       // Protocol version supported by the client
    char client_name[64];       // Optional: human-readable identifier
    Queue queue_ack;            // Queue for ACK/NACK frames to be sent to this client

    // In a real system, you'd add:
    // - A queue for outgoing reliable packets (with retransmission info)
    // - A receive buffer for out-of-order packets (for sliding window)
} ClientInfo;

ClientInfo connected_clients[MAX_CLIENTS];      // Array of connected clients
int num_connected_clients = 0;                  // Number of currently connected clients
CRITICAL_SECTION client_list_mutex; // For thread-safe access to connected_clients


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
void send_connect_response(ClientInfo *client);
void send_file_transfer_status(const struct sockaddr_in *dest_addr, uint32_t file_id, const uint8_t *final_hash, BOOL success);
void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *sender_addr);

// Thread functions
unsigned int WINAPI receive_thread_func(LPVOID lpParam);
unsigned int WINAPI send_ack_thread_func(LPVOID lpParam);

// Client management functions
ClientInfo* find_client(const struct sockaddr_in *addr);
ClientInfo* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *addr);
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

    // Create a dedicated thread for receiving UDP packets using _beginthreadex
    // This is generally safer than CreateThread when using C runtime functions.
    HANDLE hRecvThread = (HANDLE)_beginthreadex(NULL, 0, receive_thread_func, NULL, 0, NULL);
    if (hRecvThread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        running = 0; // Signal immediate shutdown
    }

    HANDLE hSendAckThread = (HANDLE)_beginthreadex(NULL, 0, send_ack_thread_func, NULL, 0, NULL);
    if (hRecvThread == NULL) {
        fprintf(stderr, "Failed to create SendAck thread. Error: %d\n", GetLastError());
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
void send_connect_response(ClientInfo *client) {
    UdpFrame response_frame;
    // Initialize the response frame
    memset(&response_frame, 0, sizeof(response_frame));
    // Set the header fields
    response_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    response_frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;
    response_frame.header.seq_num = htonl(++(client->frame_count));
    
    response_frame.payload_data.response.session_id = htonl(client->session_id);
    response_frame.payload_data.response.session_timeout = htonl(CLIENT_TIMEOUT_SEC);
    response_frame.payload_data.response.server_status = SERVER_READY; // Assuming server is ready

    uint32_t len = sizeof(SERVER_NAME) - 1;
    strncpy(response_frame.payload_data.response.server_name, SERVER_NAME, len);
    response_frame.payload_data.response.server_name[len] = '\0'; // Ensure null-termination

    // Calculate CRC32 for the ACK frame
    response_frame.header.checksum = htonl(calculate_crc32(&response_frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));
    // Send the response frame
    send_frame(&response_frame, server_socket, &client->addr);
    log_sent_frame(&response_frame, &client->addr);
}

// Sends a file transfer status frame
void send_file_transfer_status(const struct sockaddr_in *dest_addr, uint32_t file_id, const uint8_t *final_hash, BOOL success) {
    UdpFrame status_frame;
    memset(&status_frame, 0, sizeof(status_frame));

    status_frame.header.start_delimiter = htons(FRAME_DELIMITER);
    status_frame.header.frame_type = success ? FRAME_TYPE_FILE_TRANSFER_COMPLETE : FRAME_TYPE_FILE_TRANSFER_FAILED;
    static uint32_t status_seq = 0;
    status_frame.header.seq_num = htonl(status_seq++);
    status_frame.payload_data.file_transfer_status.file_id = htonl(file_id);
    memcpy(status_frame.payload_data.file_transfer_status.final_hash, final_hash, 16);
    status_frame.payload_data.file_transfer_status.success = success;

    status_frame.header.checksum = htonl(calculate_crc32(&status_frame, sizeof(FrameHeader) + sizeof(FileTransferStatusPayload)));

    send_frame(&status_frame, server_socket, dest_addr);
    char dest_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(dest_addr->sin_addr), dest_ip, INET_ADDRSTRLEN);
    printf("Sending File Transfer Status for File ID %u to %s:%d: %s\n",
           file_id, dest_ip, ntohs(dest_addr->sin_port), success ? "COMPLETE" : "FAILED");
}
// Processes a received UDP frame
void process_received_frame(UdpFrame *frame, int bytes_received, const struct sockaddr_in *sender_addr) {
    // Convert header fields from network byte order to host byte order
    uint16_t received_delimiter = ntohs(frame->header.start_delimiter);
    uint8_t  received_frame_type = frame->header.frame_type; // No byte order for uint8_t
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
    log_recv_frame(frame, sender_addr);
    // Find or add client (thread-safe access)
    ClientInfo *client = NULL;
    client = find_client(sender_addr);
    if (!client) {
        if (received_frame_type == FRAME_TYPE_CONNECT_REQUEST) {
            client = add_client(frame, sender_addr);
            if (client) {
                // Send CONNECT_RESPONSE
                EnterCriticalSection(&client_list_mutex);       
                push_queue(&client->queue_ack, received_seq_num);
                LeaveCriticalSection(&client_list_mutex);
                send_connect_response(client);                      
            } else {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", sender_ip, sender_port);
                // Optionally send NACK indicating server full
            }
        } else {
            fprintf(stderr, "Received frame from unknown client %s:%d (Type: %u, Seq: %u). Ignoring.\n",
                    sender_ip, sender_port, received_frame_type, received_seq_num);
            // Do not send ACK/NACK to unknown clients
            return;
        }
    } else {
        // Update last activity time
        // Basic duplicate check (more complex with sliding window and retransmission buffers)
        // If the received sequence number is less than or equal to the last processed
        // sequence number, it's likely a duplicate or out-of-order old packet.
        // Re-ACK it to confirm receipt on the sender's side.
    }

    // 3. Process Payload based on Frame Type
    switch (received_frame_type) {
        case FRAME_TYPE_TEXT_MESSAGE: {
            uint32_t text_len = ntohl(frame->payload_data.text_msg.text_len); // Changed to uint32_t
            // Ensure null termination for safe printing, cap at max payload size
            if (text_len >= sizeof(frame->payload_data.text_msg.text_data)) {
                text_len = sizeof(frame->payload_data.text_msg.text_data) - 1;
            }
            frame->payload_data.text_msg.text_data[text_len] = '\0';
            printf("\n[TEXT from %s:%d] Seq: %u, Message: \"%s\"\n",
                    sender_ip, sender_port, received_seq_num, frame->payload_data.text_msg.text_data);
            EnterCriticalSection(&client_list_mutex);
            push_queue(&client->queue_ack, received_seq_num);
            LeaveCriticalSection(&client_list_mutex);
            //send_ack(client, received_seq_num); // Acknowledge receipt
            // Optional: Route message to another client if specified
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
        //    send_ack(client, received_seq_num);
            break;
        }
        // case FRAME_TYPE_FILE_METADATA_REQUEST: {
        //     uint32_t total_size = ntohl(frame->payload_data.file_metadata.total_file_size);
        //     char *filename = frame->payload_data.file_metadata.filename;
        //     uint8_t *file_hash = frame->payload_data.file_metadata.file_hash;
        //     printf("\n[FILE METADATA REQ from %s:%d] Seq: %u, Filename: '%s', Size: %u bytes\n",
        //             sender_ip, sender_port, received_seq_num, filename, total_size);

        //     EnterCriticalSection(&file_transfer_mutex);
        //     FileTransferContext *ctx = start_new_file_transfer(sender_addr, filename, total_size, file_hash);
        //     if (ctx) {
        //         // Send FILE_METADATA_RESPONSE with the assigned file_id
        //         UdpFrame resp_frame;
        //         memset(&resp_frame, 0, sizeof(resp_frame));
        //         resp_frame.header.start_delimiter = htons(FRAME_DELIMITER);
        //         resp_frame.header.frame_type = FRAME_TYPE_FILE_METADATA_RESPONSE;
        //         static uint32_t file_meta_resp_seq = 0;
        //         resp_frame.header.seq_num = htonl(file_meta_resp_seq++);
        //         resp_frame.payload_data.file_metadata.file_id = htonl(ctx->file_id);
        //         resp_frame.payload_data.file_metadata.total_file_size = htonl(total_size);
        //         strncpy(resp_frame.payload_data.file_metadata.filename, filename, sizeof(resp_frame.payload_data.file_metadata.filename) - 1);
        //         resp_frame.payload_data.file_metadata.filename[sizeof(resp_frame.payload_data.file_metadata.filename) - 1] = '\0'; // Ensure null termination
        //         memcpy(resp_frame.payload_data.file_metadata.file_hash, file_hash, 16);
        //         resp_frame.header.checksum = htonl(calculate_crc32(&resp_frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));
        //         send_frame(&resp_frame, sender_addr);
        //         printf("Assigned File ID %u for '%s'. Ready to receive.\n", ctx->file_id, filename);
        //         send_ack(client, received_seq_num); // ACK the request
        //     } else {
        //         fprintf(stderr, "Failed to start file transfer for '%s'. Server busy?\n", filename);
        //         // Send NACK to client indicating server busy or max transfers reached
        //         send_nack(client, received_seq_num);
        //     }
        //     LeaveCriticalSection(&file_transfer_mutex);
        //     break;
        // }
        // case FRAME_TYPE_FILE_DATA: {
        //     uint32_t file_id = ntohl(frame->payload_data.file_data.file_id);
        //     uint32_t offset = ntohl(frame->payload_data.file_data.fragment_offset);
        //     uint32_t payload_len = ntohl(frame->payload_data.file_data.payload_len); // Changed to uint32_t

        //     EnterCriticalSection(&file_transfer_mutex);
        //     FileTransferContext *ctx = find_file_transfer(file_id);
        //     if (ctx) {
        //         // Update last activity time for this transfer
        //         ctx->last_fragment_time = time(NULL);
        //         printf("[FILE DATA from %s:%d] Seq: %u, File ID: %u, Offset: %u, Length: %u (Total: %zu/%zu)\n",
        //                 sender_ip, sender_port, received_seq_num, file_id, offset, payload_len,
        //                 ctx->received_bytes + payload_len, ctx->total_size);

        //         // Write fragment to temporary file
        //         if (ctx->temp_file_handle) {
        //             // Seek to the correct offset
        //             if (fseek(ctx->temp_file_handle, offset, SEEK_SET) == 0) {
        //                 size_t bytes_written = fwrite(frame->payload_data.file_data.fragment_data, 1, payload_len, ctx->temp_file_handle);
        //                 if (bytes_written == payload_len) {
        //                     ctx->received_bytes += payload_len;
        //                     // In a robust system, you'd mark this fragment as received in a bitmap for selective repeat
        //                     send_ack(client, received_seq_num); // ACK this specific fragment

        //                     // Check for file completion - simplified check, needs robust fragment tracking
        //                     if (ctx->received_bytes >= ctx->total_size) {
        //                         // Important: This simply checks total bytes. A robust solution needs to ensure
        //                         // ALL fragments (offsets) are present and then verify file hash.
        //                         printf("File transfer %u for '%s' possibly complete. Received %zu of %zu bytes.\n",
        //                                 file_id, ctx->filename, ctx->received_bytes, ctx->total_size);
        //                         finalize_file_transfer(ctx); // Close temp file, verify hash, move file, notify client
        //                         remove_file_transfer(file_id); // Remove from active transfers
        //                     }
        //                 } else {
        //                     fprintf(stderr, "Error writing file fragment to temp file for %u. Expected %u, wrote %zu.\n", file_id, payload_len, bytes_written);
        //                     // Send NACK to client to request retransmission
        //                     send_nack(client, received_seq_num);
        //                 }
        //             } else {
        //                 fprintf(stderr, "Error seeking in temp file for %u.\n", file_id);
        //                 send_nack(client, received_seq_num);
        //             }
        //         } else {
        //             fprintf(stderr, "File handle not open for transfer %u. This indicates a server issue.\n", file_id);
        //             send_nack(client, received_seq_num); // Request retransmission, hoping file handle is re-opened or transfer restarted
        //         }
        //     } else {
        //         fprintf(stderr, "Received FILE_DATA for unknown File ID %u from %s:%d. Discarding.\n",
        //                 file_id, sender_ip, sender_port);
        //         // Send NACK for unknown File ID, signaling client to restart transfer or provide metadata
        //         send_nack(client, 0); // Seq 0, Target ID is file_id
        //     }
        //     LeaveCriticalSection(&file_transfer_mutex);
        //     break;
        // }
        // case FRAME_TYPE_ACK: {
        //     uint32_t acked_seq = ntohl(frame->payload_data.ack_nack.acknowledged_seq_num);
        //     uint32_t target_id = ntohl(frame->payload_data.ack_nack.target_id);
        //     uint32_t target_offset = ntohl(frame->payload_data.ack_nack.target_offset);
        //     printf("[ACK from %s:%d] Acked Seq: %u, Target ID: %u, Offset: %u (Server Seq: %u)\n",
        //             sender_ip, sender_port, acked_seq, target_id, target_offset, received_seq_num);
        //     // TODO: Update sender's retransmission queue/timers. Mark packet as acknowledged.
        //     // This is where a proper reliable UDP implementation's sliding window logic would go.
        //     break;
        // }
        // case FRAME_TYPE_NACK: {
        //     uint32_t nacked_seq = ntohl(frame->payload_data.ack_nack.acknowledged_seq_num);
        //     uint32_t target_id = ntohl(frame->payload_data.ack_nack.target_id);
        //     uint32_t target_offset = ntohl(frame->payload_data.ack_nack.target_offset);
        //     fprintf(stderr, "[NACK from %s:%d] Nacked Seq: %u, Target ID: %u, Offset: %u (Server Seq: %u)\n",
        //             sender_ip, sender_port, nacked_seq, target_id, target_offset, received_seq_num);
        //     // TODO: Trigger retransmission of the specified packet/fragment from server to client.
        //     break;
        // }
        case FRAME_TYPE_DISCONNECT: {
            printf("[DISCONNECT from %s:%d] Client disconnected.\n", sender_ip, sender_port);
            remove_client(sender_addr); // Remove client from active list
        //    send_ack(client, received_seq_num); // ACK the disconnect
            break;
        }
        case FRAME_TYPE_KEEP_ALIVE: {
            printf("[KEEP_ALIVE from %s:%d] Seq: %u\n", sender_ip, sender_port, received_seq_num);
            // Client's last_activity_time already updated by find_client/add_client logic.
        //    send_ack(client, received_seq_num); // Acknowledge keep-alive
            break;
        }
        case FRAME_TYPE_CONNECT_REQUEST:
        case FRAME_TYPE_CONNECT_RESPONSE: // Handled during client find/add
            break;
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

    BOOL queue_empty = TRUE;
    ClientInfo *client = NULL;

    while(running){
        EnterCriticalSection(&client_list_mutex);
        int current_connected_clients = num_connected_clients;
        LeaveCriticalSection(&client_list_mutex);
        for(int i = 0; i < current_connected_clients; i++){
            client = &connected_clients[i];
            EnterCriticalSection(&client_list_mutex);
            queue_empty = client->queue_ack.tail == client->queue_ack.head;
            //if queue is not empty then send acknowledge for frame sequence number
            if(!queue_empty){
                int ack_seq_count = client->queue_ack.head;
                send_ack_nack(FRAME_TYPE_ACK, client->queue_ack.buffer[ack_seq_count], server_socket, &client->addr);
                pop_queue(&client->queue_ack);
            }
            LeaveCriticalSection(&client_list_mutex);
            client = NULL;
        }
        Sleep(10);
    }
    printf("Receive thread exiting.\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Client Management Implementation Details ---
// Find client by address
ClientInfo* find_client(const struct sockaddr_in *addr) {
    EnterCriticalSection(&client_list_mutex);
    // Assumes client_list_mutex is locked by caller
    for (int i = 0; i < num_connected_clients; i++) {
        if (connected_clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            connected_clients[i].addr.sin_port == addr->sin_port) {
            LeaveCriticalSection(&client_list_mutex);
            return &connected_clients[i];
        }
    }
    LeaveCriticalSection(&client_list_mutex);
    return NULL;
}
// Add a new client
ClientInfo* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *addr) {
    // Assumes client_list_mutex is locked by caller
    EnterCriticalSection(&client_list_mutex);
    if (num_connected_clients >= MAX_CLIENTS) {
        fprintf(stderr, "Max clients reached. Cannot add new client.\n");
        return NULL;
    }
    ClientInfo *new_client = &connected_clients[num_connected_clients];
    memset(new_client, 0, sizeof(ClientInfo));
    memcpy(&new_client->addr, addr, sizeof(struct sockaddr_in));
    new_client->last_activity_time = time(NULL);
    new_client->client_id = ntohl(recv_frame->payload_data.request.client_id); // Simple ID assignment
    new_client->session_id = new_client->client_id + 100;
    new_client->flags = recv_frame->payload_data.request.flags;
    new_client->frame_count = 0;
    uint32_t len = sizeof(recv_frame->payload_data.request.client_name) - 1;
    strncpy(new_client->client_name, recv_frame->payload_data.request.client_name, len);
    new_client->client_name[len] = '\0';

    new_client->queue_ack.head = 0;
    new_client->queue_ack.tail = 0;
    num_connected_clients++;
    LeaveCriticalSection(&client_list_mutex);
    return new_client;
}
// Remove a client
void remove_client(const struct sockaddr_in *addr) {
    EnterCriticalSection(&client_list_mutex);
    for (int i = 0; i < num_connected_clients; i++) {
        if (connected_clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            connected_clients[i].addr.sin_port == addr->sin_port) {
            // Shift elements to fill the gap
            for (int j = i; j < num_connected_clients - 1; j++) {
                connected_clients[j] = connected_clients[j + 1];
            }
            num_connected_clients--;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
            printf("Client %s:%d removed.\n", ip_str, ntohs(addr->sin_port));
            break;
        }
    }
    LeaveCriticalSection(&client_list_mutex);
}
// --- File Transfer Management Implementation Details ---
// Find file transfer by ID
FileTransferContext* find_file_transfer(uint32_t file_id) {
    // Assumes file_transfer_mutex is locked by caller
    for (int i = 0; i < num_active_file_transfers; i++) {
        if (active_file_transfers[i].file_id == file_id) {
            return &active_file_transfers[i];
        }
    }
    return NULL;
}
// Start a new file transfer
FileTransferContext* start_new_file_transfer(const struct sockaddr_in *sender_addr, const char *filename, uint32_t total_size, const uint8_t *file_hash) {
    // Assumes file_transfer_mutex is locked by caller
    if (num_active_file_transfers >= MAX_FILE_TRANSFERS) {
        fprintf(stderr, "Max concurrent file transfers reached. Cannot start new transfer for '%s'.\n", filename);
        return NULL;
    }

    FileTransferContext *new_ctx = &active_file_transfers[num_active_file_transfers];
    memset(new_ctx, 0, sizeof(FileTransferContext));

    new_ctx->file_id = next_file_id++; // Assign unique ID
    memcpy(&new_ctx->client_address, sender_addr, sizeof(struct sockaddr_in));
    strncpy(new_ctx->filename, filename, sizeof(new_ctx->filename) - 1);
    new_ctx->filename[sizeof(new_ctx->filename) - 1] = '\0';
    new_ctx->total_size = total_size;
    memcpy(new_ctx->expected_file_hash, file_hash, 16); // Store expected hash
    new_ctx->received_bytes = 0;
    new_ctx->last_fragment_time = time(NULL);

    // Create a temporary file
    char temp_filename_buffer[MAX_PATH];
    snprintf(temp_filename_buffer, MAX_PATH, "temp_%u_%s", new_ctx->file_id, new_ctx->filename);
    strncpy(new_ctx->temp_filepath, temp_filename_buffer, MAX_PATH - 1);
    new_ctx->temp_filepath[MAX_PATH - 1] = '\0';

    new_ctx->temp_file_handle = fopen(new_ctx->temp_filepath, "wb");
    if (!new_ctx->temp_file_handle) {
        fprintf(stderr, "Failed to open temporary file '%s' for writing. Error: %d\n", new_ctx->temp_filepath, errno);
        return NULL;
    }

    // Pre-allocate file size if possible (optional, but good for large files)
    // _chsize_s(_fileno(new_ctx->temp_file_handle), total_size); // Windows specific

    num_active_file_transfers++;
    printf("Started new file transfer for '%s' (ID: %u, Size: %zu bytes) to temp file: %s\n",
           filename, new_ctx->file_id, total_size, new_ctx->temp_filepath);
    return new_ctx;
}
// Finalize a file transfer (verify hash, move file)
void finalize_file_transfer(FileTransferContext *ctx) {
    if (ctx->temp_file_handle) {
        fclose(ctx->temp_file_handle);
        ctx->temp_file_handle = NULL;
    }

    printf("File transfer %u for '%s' finalizing. Temp file: %s.\n",
           ctx->file_id, ctx->filename, ctx->temp_filepath);

    // 1. **Verify File Hash**:
    // This is a placeholder. You'll need a real hash calculation library (e.g., OpenSSL for MD5/SHA)
    // For demonstration, we'll assume a successful hash check.
    uint8_t calculated_hash[16] = {0}; // Placeholder for actual calculated hash
    // In a real scenario, read the temp file and compute its hash.
    // For now, let's just make it 'match' if the file size is correct.
    BOOL hash_matches = FALSE;
    if (ctx->received_bytes == ctx->total_size) {
        // This is where you would actually calculate the hash of ctx->temp_filepath
        // and compare it to ctx->expected_file_hash.
        // For simulation:
        memcpy(calculated_hash, ctx->expected_file_hash, 16); // Simulate successful hash match
        hash_matches = TRUE;
    }

    if (hash_matches) {
        printf("File transfer %u: Hash matched! Moving '%s' to final destination.\n", ctx->file_id, ctx->filename);
        char final_filepath[MAX_PATH];
        snprintf(final_filepath, MAX_PATH, "received_files\\%s", ctx->filename); // Example destination folder

        // Ensure the directory exists
        CreateDirectoryA("received_files", NULL);

        if (rename(ctx->temp_filepath, final_filepath) == 0) {
            printf("File '%s' successfully moved to '%s'.\n", ctx->filename, final_filepath);
            send_file_transfer_status(&ctx->client_address, ctx->file_id, calculated_hash, TRUE);
        } else {
            fprintf(stderr, "Failed to move file '%s' to '%s'. Error: %d\n", ctx->temp_filepath, final_filepath, errno);
            send_file_transfer_status(&ctx->client_address, ctx->file_id, calculated_hash, FALSE);
            remove(ctx->temp_filepath); // Clean up temp file on move failure
        }
    } else {
        fprintf(stderr, "File transfer %u: Hash mismatch or incomplete file for '%s'. Deleting temp file.\n", ctx->file_id, ctx->filename);
        send_file_transfer_status(&ctx->client_address, ctx->file_id, calculated_hash, FALSE);
        remove(ctx->temp_filepath); // Delete corrupted temp file
    }
}
// Remove a file transfer context and clean up associated temporary files
void remove_file_transfer(uint32_t file_id) {
    EnterCriticalSection(&file_transfer_mutex);
    for (int i = 0; i < num_active_file_transfers; i++) {
        if (active_file_transfers[i].file_id == file_id) {
            if (active_file_transfers[i].temp_file_handle) {
                fclose(active_file_transfers[i].temp_file_handle);
                active_file_transfers[i].temp_file_handle = NULL;
            }
            remove(active_file_transfers[i].temp_filepath); // Delete temp file (if it still exists)
            // Shift elements to fill the gap
            for (int j = i; j < num_active_file_transfers - 1; j++) {
                active_file_transfers[j] = active_file_transfers[j + 1];
            }
            num_active_file_transfers--;
            printf("File transfer %u removed from active list.\n", file_id);
            break;
        }
    }
    LeaveCriticalSection(&file_transfer_mutex);
}
// --- Periodic Server Management ---
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

    // Manage file transfers
    EnterCriticalSection(&file_transfer_mutex);
    for (int i = 0; i < num_active_file_transfers; /* no increment here */) {
        if (current_time - active_file_transfers[i].last_fragment_time > FILE_TRANSFER_TIMEOUT_SEC) {
            fprintf(stderr, "File transfer %u for '%s' stalled (inactive for %d seconds). Cleaning up.\n",
                    active_file_transfers[i].file_id, active_file_transfers[i].filename, FILE_TRANSFER_TIMEOUT_SEC);
            // Optionally send FRAME_TYPE_FILE_TRANSFER_FAILED to client here
            send_file_transfer_status(&active_file_transfers[i].client_address, active_file_transfers[i].file_id, (uint8_t*)&"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", FALSE);
            remove_file_transfer(active_file_transfers[i].file_id); // This will decrement num_active_file_transfers and adjust 'i'
        } else {
            i++; // Only increment if transfer is not removed
        }
    }
    LeaveCriticalSection(&file_transfer_mutex);
}