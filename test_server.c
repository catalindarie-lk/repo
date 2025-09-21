
#include <stdio.h>                      // For printf, fprintf
#include <string.h>                     // For memset, memcpy
#include <stdint.h>                     // For fixed-width integer types    
#include <time.h>                       // For time functions
#include <process.h>                    // For _beginthreadex  
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#include "include/server.h"             // For server data structures and definitions
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/resources.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/crc32.h"           // For checksum validation
#include "include/sha256.h"
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management
#include "include/file_handler.h"       // For frame handling functions
#include "include/message_handler.h"    // For frame handling functions
#include "include/server_frames.h"
#include "include/server_statistics.h"

ServerData Server;
ClientListData ClientList;
ServerBuffers Buffers;
ServerThreads Threads;

const char *server_ip = "192.168.0.241";
// const char *server_ip = "127.0.0.1";

static void get_network_config(){
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

static int init_server_session(){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    memset(client_list, 0, sizeof(ClientListData));

    server->server_status = STATUS_NONE;
    server->session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    server->session_id_counter = 0;
    server->file_block_count = 0;
    snprintf(server->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, SERVER_NAME);

    return RET_VAL_SUCCESS;
}
static int init_server_config(){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    WSADATA wsaData;

    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }

    server->socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (server->socket == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        closesocket(server->socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_port = _htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server->server_addr.sin_addr);
 
    if (bind(server->socket, (SOCKADDR*)&server->server_addr, sizeof(server->server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server->socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    server->iocp_socket_handle = CreateIoCompletionPort((HANDLE)server->socket, NULL, 0, 0);
    if (!server->iocp_socket_handle || server->iocp_socket_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateIoCompletionPort failed socket_handle: %lu\n", GetLastError());
        return RET_VAL_ERROR;
    }

    server->iocp_file_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!server->iocp_file_handle) {
        fprintf(stderr, "CreateIoCompletionPort failed server_file_handle: %lu\n", GetLastError());
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;

}
static int init_server_buffers(){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    for(int i = 0; i < MAX_CLIENTS; i++){
        Client *client = &client_list->client[i];
        memset(client, 0, sizeof(Client));
        client->slot_status = SLOT_FREE;
        client->connection_status = STATUS_NONE;
        client->last_activity_time = time(NULL);
    }
    // Initialize clients locks
    for(int i = 0; i < MAX_CLIENTS; i++){
        Client *client = &client_list->client[i];
        InitializeSRWLock(&client->sack_ctx.lock);
        InitializeSRWLock(&client->lock);
    }
    // Initialize client_list lock
    InitializeCriticalSection(&client_list->lock);    

    init_fstream_pool(pool_fstreams, MAX_SERVER_ACTIVE_FSTREAMS);
    init_mstream_pool(pool_mstreams, MAX_SERVER_ACTIVE_MSTREAMS);
    init_client_pool(pool_clients, MAX_CLIENTS);

    init_pool(pool_recv_udp_frame, sizeof(PoolEntryRecvFrame), SERVER_POOL_SIZE_RECV);
    init_pool(pool_iocp_recv_context, sizeof(SocketContext), SERVER_POOL_SIZE_IOCP_RECV);
    init_pool(pool_file_block, SERVER_FILE_BLOCK_SIZE, SERVER_POOL_SIZE_FILE_BLOCK);
        
    init_queue_ptr(queue_recv_udp_frame, SERVER_QUEUE_SIZE_RECV_FRAME);
    init_queue_ptr(queue_recv_prio_udp_frame, SERVER_QUEUE_SIZE_RECV_PRIO_FRAME);

    init_queue_ptr(queue_process_fstream, MAX_SERVER_ACTIVE_FSTREAMS);
    init_table_id(table_file_id, 1024, 1048576);
    init_table_id(table_message_id, 1024, 32768);

    init_table_fblock(table_file_block, SERVER_POOL_SIZE_FILE_BLOCK, SERVER_POOL_SIZE_FILE_BLOCK);
    
    init_queue_slot(queue_client_slot, SERVER_QUEUE_SIZE_CLIENT_SLOT);

    init_pool(pool_send_udp_frame, sizeof(PoolEntrySendFrame), SERVER_POOL_SIZE_SEND);
    init_pool(pool_iocp_send_context, sizeof(SocketContext), SERVER_POOL_SIZE_IOCP_SEND);
    init_queue_ptr(queue_send_udp_frame, SERVER_QUEUE_SIZE_SEND_FRAME);
    init_queue_ptr(queue_send_prio_udp_frame, SERVER_QUEUE_SIZE_SEND_PRIO_FRAME);
    init_queue_ptr(queue_send_ctrl_udp_frame, SERVER_QUEUE_SIZE_SEND_CTRL_FRAME);
    for(int i = 0; i < MAX_CLIENTS; i++){
        Client *client = &client_list->client[i];
        init_queue_seq(&client->queue_ack_seq, SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ);
    }

    for (int i = 0; i < SERVER_POOL_SIZE_IOCP_RECV; ++i) {
        SocketContext* recv_context = (SocketContext*)pool_alloc(pool_iocp_recv_context);
        if (recv_context == NULL) {
            fprintf(stderr, "Failed to allocate receive context from pool %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
        init_socket_context(recv_context, OP_RECV); // Initialize the context

        if (udp_recv_from(server->socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "Failed to post initial receive operation %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
    }
    printf("Server: All initial receive operations posted.\n");

    server->server_status = STATUS_READY;
    return RET_VAL_SUCCESS;

}
static int start_threads() {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    // Create threads for receiving and processing frames
    for(int i = 0; i < SERVER_MAX_THREADS_RECV_SEND_FRAME; i++){
        threads->recv_send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_recv_send_frame, NULL, 0, NULL);
        if (threads->recv_send_frame[i] == NULL) {
            fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(threads->recv_send_frame[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }

    for(int i = 0; i < SERVER_MAX_THREADS_PROCESS_FRAME; i++){
        threads->process_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_process_frame, NULL, 0, NULL);
        if (threads->process_frame[i] == NULL) {
            fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(threads->process_frame[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }

    for(int i = 0; i < SERVER_MAX_THREADS_WRITE_FILE_BLOCK; i++){
        threads->file_block_written[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_file_block_written, NULL, 0, NULL);
        if (threads->file_block_written[i] == NULL) {
            fprintf(stderr, "Failed to create 'file_block_written' thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(threads->file_block_written[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }

    for(int i = 0; i < SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES; i++){
        threads->send_sack_frame[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_send_sack_frame, NULL, 0, NULL);
        if (threads->send_sack_frame[i] == NULL) {
            fprintf(stderr, "Failed to create send ack frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(threads->send_sack_frame[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }

    threads->scan_for_trailing_sack = (HANDLE)_beginthreadex(NULL, 0, fthread_scan_for_trailing_sack, NULL, 0, NULL);
    if (threads->scan_for_trailing_sack == NULL) {
        fprintf(stderr, "Failed to create scan_for_trailing_sack. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    SetThreadPriority(threads->scan_for_trailing_sack, THREAD_PRIORITY_ABOVE_NORMAL);

    // threads->send_ack_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_ack_frame, NULL, 0, NULL);
    // if (threads->send_ack_frame == NULL) {
    //     fprintf(stderr, "Failed to create send ack frame thread. Error: %d\n", GetLastError());
    //     return RET_VAL_ERROR;
    // }
    // SetThreadPriority(threads->send_ack_frame, THREAD_PRIORITY_ABOVE_NORMAL);

    threads->send_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_frame, NULL, 0, NULL);
    if (threads->send_frame == NULL) {
        fprintf(stderr, "Failed to create hthread_pop_send_frame. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    threads->send_prio_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_prio_frame, NULL, 0, NULL);
    if (threads->send_prio_frame == NULL) {
        fprintf(stderr, "Failed to create hthread_pop_send_frame. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    
    threads->send_ctrl_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_ctrl_frame, NULL, 0, NULL);
    if (threads->send_ctrl_frame == NULL) {
        fprintf(stderr, "Failed to create hthread_pop_send_frame. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    threads->client_timeout = (HANDLE)_beginthreadex(NULL, 0, fthread_client_timeout, NULL, 0, NULL);
    if (threads->client_timeout == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
    //     threads->file_stream[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_process_file_stream, NULL, 0, NULL);
    //     if (threads->file_stream[i] == NULL) {
    //         fprintf(stderr, "Failed to file stream thread. Error: %d\n", GetLastError());
    //         return RET_VAL_ERROR;
    //     }
    // }
    threads->server_command = (HANDLE)_beginthreadex(NULL, 0, fthread_server_command, NULL, 0, NULL);
    if (threads->server_command == NULL) {
        fprintf(stderr, "Failed to create server command thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    server->server_status = STATUS_READY;
    return RET_VAL_SUCCESS;
}
static void shutdown_server() {
    printf("Server shut down!\n");
}

// Find client by session ID
static Client* find_client(const uint32_t session_id) {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    // Search for a client within the provided ClientList that matches the given session ID.
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        Client *client = &client_list->client[slot];
        AcquireSRWLockShared(&client->lock);
        if(client->slot_status == SLOT_FREE){
            ReleaseSRWLockShared(&client->lock);
            continue; // Move to the next slot in the loop.
        }
        if(client->sid == session_id){
            ReleaseSRWLockShared(&client->lock);
            return client;
        }
        ReleaseSRWLockShared(&client->lock);
    }
    return NULL;
}
// Add a new client
static Client* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
       
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    uint32_t free_slot = 0;

    EnterCriticalSection(&client_list->lock);
    while(free_slot < MAX_CLIENTS){
        if(client_list->client[free_slot].slot_status == SLOT_FREE) {
            break;
        }
        free_slot++;
    }

    if(free_slot >= MAX_CLIENTS){
        fprintf(stderr, "\nMax clients reached. Cannot add new client.\n");
        LeaveCriticalSection(&client_list->lock);
        return NULL;
    }
   
    Client *new_client = &client_list->client[free_slot]; 
    
    AcquireSRWLockExclusive(&new_client->lock);

    new_client->slot = free_slot;
    new_client->slot_status = SLOT_BUSY;
    memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
    new_client->sid = InterlockedIncrement(&server->session_id_counter);
    new_client->flags = recv_frame->payload.connection_request.flags;

    snprintf(new_client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    new_client->port = _ntohs(client_addr->sin_port);

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->sid);

    ReleaseSRWLockExclusive(&new_client->lock);
    LeaveCriticalSection(&client_list->lock);
    return new_client;
}
// Remove a client
static int remove_client(const uint32_t slot) {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(client_list == NULL){
        fprintf(stderr, "\nInvalid client pointer!\n");
        return RET_VAL_ERROR;
    }
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "\nInvalid client slot nr:  %d", slot); 
        return RET_VAL_ERROR;
    }
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", client_list->client[slot].sid, client_list->client[slot].slot);   
    
    EnterCriticalSection(&client_list->lock);    
    cleanup_client(&client_list->client[slot]);
    LeaveCriticalSection(&client_list->lock);

    return RET_VAL_SUCCESS;
}
// Release client resources
static void cleanup_client(Client *client){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(client == NULL){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }

    AcquireSRWLockExclusive(&client->lock);

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &pool_fstreams->fstream[i];
        AcquireSRWLockExclusive(&fstream->lock);
        if(fstream->sid == client->sid){
            close_fstream(fstream);
        }
        ReleaseSRWLockExclusive(&fstream->lock);
    }

    ht_remove_all_sid(table_file_id, client->sid);
    ht_remove_all_sid(table_message_id, client->sid);

    for(int i = 0; i < MAX_SERVER_ACTIVE_MSTREAMS; i++){
        ServerMessageStream *mstream = &pool_mstreams->mstream[i];
        AcquireSRWLockExclusive(&mstream->lock);
        if(mstream->sid == client->sid){
            close_mstream(mstream);
        }
        ReleaseSRWLockExclusive(&mstream->lock);
    }

    AcquireSRWLockExclusive(&client->sack_ctx.lock);
    client->sack_ctx.ack_pending = 0;
    client->sack_ctx.start_recorded = 0;
    memset(&client->sack_ctx.payload, 0, sizeof(SAckPayload));
    ReleaseSRWLockExclusive(&client->sack_ctx.lock);

    memset(&client->client_addr, 0, sizeof(struct sockaddr_in));
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    client->port = 0;
    client->cid = 0;
    memset(&client->name, 0, MAX_NAME_SIZE);
    client->flags = 0;
    client->connection_status = CLIENT_DISCONNECTED;
    client->last_activity_time = time(NULL);
    client->slot = 0;
    client->slot_status = SLOT_FREE;
    client->sid = 0;
    ReleaseSRWLockExclusive(&client->lock);
    return;
}
// Compare received hash with calculated hash
static BOOL validate_file_hash(ServerFileStream *fstream){

    // fprintf(stdout, "File hash received: ");
    // for(int i = 0; i < 32; i++){
    //     fprintf(stdout, "%02x", (uint8_t)fstream->received_sha256[i]);
    // }
    // fprintf(stdout, "\n");
    // fprintf(stdout, "File hash calculated: ");
    // for(int i = 0; i < 32; i++){
    //     fprintf(stdout, "%02x", (uint8_t)fstream->calculated_sha256[i]);
    // }
    // fprintf(stdout, "\n");

    for(int i = 0; i < 32; i++){
        if((uint8_t)fstream->calculated_sha256[i] != (uint8_t)fstream->received_sha256[i]){
            fprintf(stdout, "ERROR: SHA256 MISSMATCH\n");
            return FALSE;
        }
    }

    return TRUE;
}
// Check for any open file streams across all clients.
static void check_open_file_stream(){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    // for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
    //     if(pool_fstreams->fstream[i].fstream_busy == TRUE){
    //         fprintf(stdout, "File stream still open: %d\n", i);
    //     }
    // }
    fprintf(stdout, "Completed checking opened file streams\n");
    return;
}
// --- Receive frame thread function ---
static DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    HANDLE CompletitionPort = server->iocp_socket_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;
    char ip_string_buffer[INET_ADDRSTRLEN];
    FILETIME ft;
    
    while (server->server_status == STATUS_READY) {

        BOOL getqcompl_result = GetQueuedCompletionStatus(
            CompletitionPort,
            &NrOfBytesTransferred,
            &lpCompletitionKey,
            &lpOverlapped,
            INFINITE// WSARECV_TIMEOUT_MS
        );

        if (lpOverlapped == NULL) {
            fprintf(stderr, "Warning: NULL pOverlapped received. IOCP may be shutting down.\n");
            continue;
        }

        SocketContext* socket_context = (SocketContext*)lpOverlapped;

         // --- Handle GetQueuedCompletionStatus failures (non-NULL lpOverlapped) ---
        if (!getqcompl_result) {
            int wsa_error = WSAGetLastError();
            if (wsa_error == GETQCOMPL_TIMEOUT) {
                // Timeout, no completion occurred. Continue looping.
                continue;
            } else {
                fprintf(stderr, "GetQueuedCompletionStatus failed with error: %d\n", wsa_error);
                // If it's a real error on a specific operation
                if (socket_context->type == OP_SEND) {
                    pool_free(pool_iocp_send_context, socket_context);
                } else if (socket_context->type == OP_RECV) {
                    // Critical error on a receive socket_context -"retire" this socket_context from the pool.
                    fprintf(stderr, "Server: Error in RECV operation, attempting re-post socket_context %p...\n", (void*)socket_context);
                    pool_free(pool_iocp_recv_context, socket_context);
                }
                continue; // Continue loop to get next completion
            }
        }
        
        switch(socket_context->type){
            case OP_RECV:
                // Validate and dispatch frame
                if (NrOfBytesTransferred > 0 && NrOfBytesTransferred <= sizeof(UdpFrame)) {

                    PoolEntryRecvFrame *recv_frame_entry = (PoolEntryRecvFrame*)pool_alloc(pool_recv_udp_frame);
                    if (recv_frame_entry == NULL) {
                        fprintf(stderr, "Critical: Failed to allocate memory for received frame entry.\n");
                        break;
                    }
                    memset(recv_frame_entry, 0, sizeof(PoolEntryRecvFrame));
                    memcpy(&recv_frame_entry->frame, socket_context->buffer, NrOfBytesTransferred);
                    memcpy(&recv_frame_entry->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                    recv_frame_entry->frame_size = NrOfBytesTransferred;
                    
                    GetSystemTimePreciseAsFileTime(&ft);
                    recv_frame_entry->timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

                    uint8_t frame_type = recv_frame_entry->frame.header.frame_type;
                    BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                                    frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                                    frame_type == FRAME_TYPE_FILE_METADATA ||
                                                    frame_type == FRAME_TYPE_DISCONNECT);
 
                    if (is_high_priority_frame) {
                        if (push_ptr(queue_recv_prio_udp_frame, (uintptr_t)recv_frame_entry) == RET_VAL_ERROR) {
                            fprintf(stderr, "CRITICAL ERROR: Dropping priority recv frame - Failed to push priority recv frame to queue.\n");
                            pool_free(pool_recv_udp_frame, recv_frame_entry);
                            // continue;
                        }
                    } else {
                        if (push_ptr(queue_recv_udp_frame, (uintptr_t)recv_frame_entry) == RET_VAL_ERROR) {
                            fprintf(stderr, "CRITICAL ERROR: Dropping recv frame - Failed to push recv frame to queue.\n");
                            pool_free(pool_recv_udp_frame, recv_frame_entry);
                            // continue;
                        }
                    }

                    // if (inet_ntop(AF_INET, &(socket_context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                    //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                    // }
                    // printf("Server: Received %lu bytes from %s:%d. Type: %u\n",
                    //        NrOfBytesTransferred, ip_string_buffer, ntohs(socket_context->addr.sin_port), frame_entry.frame.header.frame_type);

                } else {
                    // 0 bytes transferred (e.g., graceful shutdown, empty packet)
                    fprintf(stdout, "Server: Receive operation completed with 0 bytes for socket_context %p. Re-posting.\n", (void*)socket_context);
                }

                // *** CRITICAL: Re-post the receive operation using the SAME socket_context ***
                // This ensures the buffer is continuously available for incoming data.
                if (udp_recv_from(server->socket, socket_context) == RET_VAL_ERROR){
                    fprintf(stderr, "Critical: WSARecvFrom re-issue failed for socket_context %p: %d. Freeing.\n", (void*)socket_context, WSAGetLastError());
                    // This is a severe problem. Retire the socket_context from the pool.
                    pool_free(pool_iocp_recv_context, socket_context); // Return to pool if it fails
                    break;
                }
                // refill the recv socket_context mem pool when it drops bellow half
                if(pool_iocp_recv_context->free_blocks < (pool_iocp_recv_context->block_count / 2)){
                    refill_recv_iocp_pool(server->socket, pool_iocp_recv_context);
                }
                break; // End of OP_RECV case

            case OP_SEND:
                // For send completions, simply free the socket_context
                if (NrOfBytesTransferred > 0) {
                    // if (inet_ntop(AF_INET, &(socket_context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                    //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                    // }
                    // printf("Server: Sent %lu bytes to %s:%d (Message: '%s')\n",
                    //        NrOfBytesTransferred, ip_string_buffer, ntohs(socket_context->addr.sin_port), socket_context->buffer);
                } else {
                    fprintf(stderr, "Server: Send operation completed with 0 bytes or error.\n");
                }
                pool_free(pool_iocp_send_context, socket_context);
                break;

            default:
                fprintf(stderr, "Server: Unknown operation type in completion.\n");
                // Free socket_context if it's unknown and shouldn't be re-used, to prevent leak
                pool_free(pool_iocp_send_context, socket_context);
                break;
        } // end of switch(socket_context->type)
    } // end of while (server->server_status == STATUS_READY)
           
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}
// --- Processes a received frame ---
static DWORD WINAPI func_thread_process_frame(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    uint16_t recv_delimiter;      // Stores the extracted start delimiter from the frame header.
    uint8_t  recv_frame_type;     // Stores the extracted frame type from the frame header.
    uint64_t recv_seq_num;        // Stores the extracted sequence number from the frame header.
    uint32_t recv_session_id;     // Stores the extracted session ID from the frame header.


    UdpFrame *frame;                // A pointer to the UDP frame data within frame_entry.
    struct sockaddr_in *src_addr;   // A pointer to the source address of the received UDP frame.
    uint32_t frame_bytes_received = 0;  // The actual number of bytes received for the current UDP frame.

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    // PoolEntryAckFrame *pool_ack_entry = NULL;
    PoolEntrySendFrame *pool_send_entry = NULL;
    int res = RET_VAL_ERROR;

    HANDLE events[2] = {queue_recv_prio_udp_frame->push_semaphore, 
                        queue_recv_udp_frame->push_semaphore};

    PoolEntryRecvFrame recv_frame_entry;
    PoolEntryRecvFrame *entry_recv_frame = NULL;

    while(server->server_status == STATUS_READY) {
        DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            entry_recv_frame = (PoolEntryRecvFrame*)pop_ptr(queue_recv_prio_udp_frame);
            if(!entry_recv_frame){
                fprintf(stderr,"CRITICAL ERROR: Poped empty pointer from queue_recv_prio_frame?\n");
                continue;
            }
            memcpy(&recv_frame_entry, entry_recv_frame, sizeof(PoolEntryRecvFrame));
            pool_free(pool_recv_udp_frame, (void*)entry_recv_frame);
        } else if (result == WAIT_OBJECT_0 + 1) {
            entry_recv_frame = (PoolEntryRecvFrame*)pop_ptr(queue_recv_udp_frame);
            if(!entry_recv_frame){
                fprintf(stderr,"CRITICAL ERROR: Poped empty pointer from queue_recv_frame?\n");
                continue;
            }
            memcpy(&recv_frame_entry, entry_recv_frame, sizeof(PoolEntryRecvFrame));
            pool_free(pool_recv_udp_frame, (void*)entry_recv_frame);
        } else {
            fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Unexpected result wait semaphore frame queues: %lu\n", result);
            continue;
        }  

        frame = &recv_frame_entry.frame;
        src_addr = &recv_frame_entry.src_addr;
        frame_bytes_received = recv_frame_entry.frame_size;

        recv_delimiter = _ntohs(frame->header.start_delimiter);
        recv_frame_type = frame->header.frame_type;
        recv_seq_num = _ntohll(frame->header.seq_num);
        recv_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (recv_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "DEBUF: Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", 
                                            src_ip, src_port, recv_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "DEBUF: Received frame from %s:%d with checksum mismatch. Discarding.\n", 
                                            src_ip, src_port);
            continue;
        }

        Client *client = NULL;
        // time_t now = time(NULL);

        if(recv_frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                                recv_session_id == DEFAULT_CONNECT_REQUEST_SID &&
                                recv_seq_num == DEFAULT_CONNECT_REQUEST_SEQ){
            client = add_client(frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
                continue;
            }
            AcquireSRWLockExclusive(&client->lock);
            client->last_activity_time = time(NULL);
            ReleaseSRWLockExclusive(&client->lock);

            pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            if(!pool_send_entry){
                fprintf(stderr, "ERROR: Failed to allocate memory in the pool for connect response frame\n");
                continue;
            }
            res = construct_connect_response_frame(pool_send_entry,
                                    DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, &client->client_addr);

            if(res == RET_VAL_ERROR){
                fprintf(stderr, "CRITICAL ERROR: construct_file_metadata() returned RET_VAL_ERROR. \
                                        Should not happen since inputs are validated before calling");
                continue;
            }
            if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
                pool_free(pool_send_udp_frame, pool_send_entry);
            }

            fprintf(stdout, "DEBUG: Client %s:%d Requested connection. Responding to connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;

        } else if (recv_frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                        (recv_session_id != DEFAULT_CONNECT_REQUEST_SID || recv_seq_num != DEFAULT_CONNECT_REQUEST_SEQ)) {
            // This is a re-connect request from an existing client.
            fprintf(stdout, "DEBUG: Client %s:%d requested re-connection with Session ID: %u\n", 
                                                    src_ip, src_port, recv_session_id);
            client = find_client(recv_session_id);
            if(client == NULL){
                fprintf(stderr, "ERROR: Unknown client (invalid DEFAULT_CONNECT_REQUEST_SID)\n");
                continue;
            }
            AcquireSRWLockExclusive(&client->lock);
            client->last_activity_time = time(NULL);
            ReleaseSRWLockExclusive(&client->lock);

            pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            if(!pool_send_entry){
                fprintf(stderr, "ERROR: failed to allocate memory for connect response frame\n");
                continue;
            }
            res = construct_connect_response_frame(pool_send_entry,
                                    DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, &client->client_addr);

            if(res == RET_VAL_ERROR){
                fprintf(stderr, "CRITICAL ERROR: construct_file_metadata() returned RET_VAL_ERROR. \
                                        Should not happen since inputs are validated before calling");
                continue;
            }
            if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
                pool_free(pool_send_udp_frame, pool_send_entry);
            }

            fprintf(stdout, "DEBUG: Client %s:%d Requested re-connection. Responding to re-connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;
        } else {
            client = find_client(recv_session_id);
            if(client == NULL){
                fprintf(stderr, "Received frame from unknown client\n");
                continue;
            }
        }

        switch (recv_frame_type) {

            case FRAME_TYPE_ACK:
                // TODO: Implement the full ACK processing logic here. This typically involves:
                //   - Removing acknowledged packets from the sender's retransmission queue.
                //   - Updating window sizes for flow and congestion control.
                //   - Advancing sequence numbers to indicate successfully received data.
                break;

            case FRAME_TYPE_KEEP_ALIVE:
                AcquireSRWLockExclusive(&client->lock);
                client->last_activity_time = time(NULL);
                ReleaseSRWLockExclusive(&client->lock);

                pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
                if(!pool_send_entry){
                    fprintf(stderr, "ERROR: Failed to allocate memory in the pool for keep_alive ack frame\n");
                    break;
                }
                construct_ack_frame(pool_send_entry, recv_seq_num, recv_session_id, STS_KEEP_ALIVE, server->socket, &client->client_addr);
                if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                    pool_free(pool_send_udp_frame, pool_send_entry);
                    fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
                }
                break;

            case FRAME_TYPE_FILE_METADATA:
                handle_file_metadata(client, frame);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                if(handle_file_fragment(client, frame) == RET_VAL_ERROR){
                    // fprintf(stderr, "ERROR: handle_file_fragment() returned RET_VAL_ERROR\n");
                }
                break;

            case FRAME_TYPE_FILE_END:
                handle_file_end(client, frame);
                break;

            case FRAME_TYPE_TEXT_MESSAGE:
                handle_message_fragment(client, frame);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "DEBUG: Client %s:%d with session ID: %d requested disconnect...\n", client->ip, client->port, client->sid);
                pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
                if(!pool_send_entry){
                    fprintf(stderr, "ERROR: Failed to allocate memory in the pool for disconnect ack frame\n");
                    break;
                }
                construct_ack_frame(pool_send_entry, recv_seq_num, recv_session_id, STS_CONFIRM_DISCONNECT, server->socket, &client->client_addr);
                if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                    pool_free(pool_send_udp_frame, pool_send_entry);
                    fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
                }
                remove_client(client->slot);
                break;

            default:
                fprintf(stderr, "Received unknown frame type: %u from %s:%d (Session ID: %u). Discarding.\n",
                        recv_frame_type, src_ip, src_port, recv_session_id);
                break;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- IOCP Write file block ---
static DWORD WINAPI func_thread_file_block_written(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    DWORD NrOfBytesWritten;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;
    
    while (server->server_status == STATUS_READY) {

        BOOL getqcompl_result = GetQueuedCompletionStatus(
            server->iocp_file_handle,
            &NrOfBytesWritten,
            &lpCompletitionKey,
            &lpOverlapped,
            INFINITE// WSARECV_TIMEOUT_MS
        );

        if (!getqcompl_result) {
            DWORD err = GetLastError();
            fprintf(stderr, "I/O failed with error: %lu\n", err);
            continue;
        }
        if (!lpOverlapped) {
            fprintf(stderr, "ERROR: NULL pOverlapped received. IOCP may be shutting down.\n");
            continue;
        }
        
        NodeTableFileBlock *node = (NodeTableFileBlock*)lpOverlapped;
        ServerFileStream *fstream = (ServerFileStream*)lpCompletitionKey;

        if (!node) {
            fprintf(stderr, "ERROR: NULL node received in file write completion. Skipping.\n");
            continue;
        }
        if (!fstream) {
            fprintf(stderr, "ERROR: NULL fstream received in file write completion. Skipping.\n");
            continue;
        }
        
        if(NrOfBytesWritten > 0 && NrOfBytesWritten <= SERVER_FILE_BLOCK_SIZE){
            AcquireSRWLockExclusive(&fstream->lock);
            // if(!ht_search_fblock(table_file_block, node->key)){
            //     fprintf(stdout,"ERROR: Key %llu not found in hash table!\n", node->key);
            // }
            uint64_t block_offset = ((uint64_t)node->overlapped.Offset) | (((uint64_t)node->overlapped.OffsetHigh) << 32);
            uint64_t block_nr = block_offset / SERVER_FILE_BLOCK_SIZE;
            ht_remove_fblock(table_file_block, node->key, pool_file_block);
            pool_free(pool_file_block, fstream->file_block[block_nr]);

            fstream->file_block[block_nr] = NULL;
            fstream->recv_block_bytes[block_nr] = 0;
            // *(fstream->recv_block_bytes + block_nr) = 0;
            fstream->recv_block_status[block_nr] = BLOCK_STATUS_RECEIVED;

            fstream->written_bytes_count += NrOfBytesWritten;
 
            check_file_completition(fstream);
            ReleaseSRWLockExclusive(&fstream->lock);
        }
    }
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}
// --- File SAck Thread functions ---
static DWORD WINAPI fthread_send_sack_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    PoolEntrySendFrame *sack_entry = NULL;

    uint32_t slot = 0;
    Client *client = NULL;

    FILETIME ft;
    uint64_t current_timestamp;
    BOOL sack_ready_timeout;

    uint64_t ack_seq = 0;
    
    while (server->server_status == STATUS_READY) {
        

        DWORD result = WaitForSingleObject(queue_client_slot->push_semaphore, INFINITE); // Wait for a client slot to be available
        if(result ==  WAIT_OBJECT_0) {
            // slot available
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore queue client slot: %lu\n", result);
            continue;
        }

        slot = pop_slot(queue_client_slot);
        if(slot == UINT32_MAX){
            fprintf(stdout, "Poped invalid slot from 'queue_client_slot'\n");
            continue;
        }
        client = &client_list->client[slot];

        AcquireSRWLockShared(&client->lock);
        if(client->slot_status == SLOT_FREE || client->slot != slot){
            ReleaseSRWLockShared(&client->lock);
            fprintf(stderr, "ERROR: Client slot %d is not valid or has been freed.\n", slot);
            client = NULL;
            continue;
        }
        ReleaseSRWLockShared(&client->lock);

        AcquireSRWLockExclusive(&client->sack_ctx.lock);
        GetSystemTimePreciseAsFileTime(&ft);
        current_timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if(!client->sack_ctx.start_recorded && client->queue_ack_seq.pending > 0){
            // Combine the high and low parts of the FILETIME to get a 64-bit value
            client->sack_ctx.start_timestamp = current_timestamp;
            client->sack_ctx.start_recorded = TRUE;
        }
        if(client->queue_ack_seq.pending >= MAX_SACK_COUNT){
            client->sack_ctx.ack_pending = MAX_SACK_COUNT;
            for(int i = 0; i < client->sack_ctx.ack_pending; i++){
                ack_seq = pop_seq(&client->queue_ack_seq);
                if(ack_seq == 0) {
                    memset(&client->sack_ctx.payload, 0, sizeof(SAckPayload));
                    ReleaseSRWLockExclusive(&client->sack_ctx.lock);
                    client = NULL;
                    fprintf(stderr,"CRITICAL ERROR: pop_seq() returned seq with value 0 from queue_ack_seq\n");
                    continue;
                }
                client->sack_ctx.payload.seq_num[i] = ack_seq;
            }
            client->sack_ctx.payload.ack_count = client->sack_ctx.ack_pending;
            sack_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            construct_sack_frame(sack_entry, client->sid, &client->sack_ctx.payload ,server->socket, &client->client_addr);           

            if(push_ptr(queue_send_udp_frame, (uintptr_t)sack_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push sack frame to queue.\n");
                pool_free(pool_send_udp_frame, sack_entry);
            }
            memset(&client->sack_ctx.payload, 0, sizeof(SAckPayload));
            client->sack_ctx.ack_pending = 0;
            client->sack_ctx.start_recorded = FALSE;
        } 
        ReleaseSRWLockExclusive(&client->sack_ctx.lock);
        client = NULL;

    }
    _endthreadex(0);
    return 0;
}
static DWORD WINAPI fthread_scan_for_trailing_sack(LPVOID lpParam){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    PoolEntrySendFrame *sack_entry = NULL;
 
    uint32_t slot = 0;
    Client *client = NULL;

    FILETIME ft;
    uint64_t current_timestamp;
    BOOL sack_ready_timeout;

    uint64_t ack_seq = 0;
    
    while (server->server_status == STATUS_READY) {
    
        for(int i = 0; i < MAX_CLIENTS; i++){
            client = &client_list->client[i];
            if(client->slot_status == SLOT_FREE){
                Sleep(1);
                continue;
            }

            if(client->queue_ack_seq.pending == 0){
                Sleep(1);
                continue;
            }
            
            AcquireSRWLockExclusive(&client->sack_ctx.lock);            
            GetSystemTimePreciseAsFileTime(&ft);
            current_timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            sack_ready_timeout = client->sack_ctx.start_recorded && ((current_timestamp - client->sack_ctx.start_timestamp) > (SACK_READY_FRAME_TIMEOUT_MS * 10000));
            if(sack_ready_timeout && client->queue_ack_seq.pending > 0 && client->queue_ack_seq.pending < MAX_SACK_COUNT){
                
                client->sack_ctx.ack_pending = client->queue_ack_seq.pending;

                for(int i = 0; i < client->sack_ctx.ack_pending; i++){
                    ack_seq = pop_seq(&client->queue_ack_seq);
                    if(ack_seq == 0) {
                        memset(&client->sack_ctx.payload, 0, sizeof(SAckPayload));
                        ReleaseSRWLockExclusive(&client->sack_ctx.lock);
                        fprintf(stderr,"CRITICAL ERROR: pop_seq() returned seq with value 0 from queue_ack_seq\n");
                        continue;
                    }
                    client->sack_ctx.payload.seq_num[i] = ack_seq;
                }
                client->sack_ctx.payload.ack_count = client->sack_ctx.ack_pending;
                sack_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
                construct_sack_frame(sack_entry, client->sid, &client->sack_ctx.payload ,server->socket, &client->client_addr);           

                if(push_ptr(queue_send_udp_frame, (uintptr_t)sack_entry) == RET_VAL_ERROR) {
                    fprintf(stderr, "ERROR: Failed to push sack frame to queue.\n");
                    pool_free(pool_send_udp_frame, sack_entry);
                }
                memset(&client->sack_ctx.payload, 0, sizeof(SAckPayload));
                client->sack_ctx.ack_pending = 0;
                client->sack_ctx.start_recorded = FALSE;
            } 
            ReleaseSRWLockExclusive(&client->sack_ctx.lock);
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Send frame thread functions ---
static DWORD WINAPI fthread_send_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    while (server->server_status == STATUS_READY) {
        DWORD result = WaitForSingleObject(queue_send_udp_frame->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            PoolEntrySendFrame *entry = (PoolEntrySendFrame*)pop_ptr(queue_send_udp_frame);
            if(!entry){
                fprintf(stderr,"CRITICAL ERROR: Poped empty pointer from tx_frame?\n");
                continue;
            }
            send_pool_frame(entry, pool_iocp_send_context);
            pool_free(pool_send_udp_frame, (void*)entry);
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result 'queue_send_frame->push_semaphore': %lu\n", result);
            continue;
        }
   }
    _endthreadex(0);    
    return 0;
}
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    while (server->server_status == STATUS_READY) {
        DWORD result = WaitForSingleObject(queue_send_prio_udp_frame->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            PoolEntrySendFrame *entry = (PoolEntrySendFrame*)pop_ptr(queue_send_prio_udp_frame);
            if(!entry){
                fprintf(stderr,"CRITICAL ERROR: Poped empty pointer from tx_frame?\n");
                continue;
            }
            send_pool_frame(entry, pool_iocp_send_context);
            pool_free(pool_send_udp_frame, (void*)entry);
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result 'queue_send_prio_frame->push_semaphore': %lu\n", result);
            continue;
        }
   }
    _endthreadex(0);    
    return 0;
}
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    while (server->server_status == STATUS_READY) {
        DWORD result = WaitForSingleObject(queue_send_ctrl_udp_frame->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            PoolEntrySendFrame *entry = (PoolEntrySendFrame*)pop_ptr(queue_send_ctrl_udp_frame);
            if(!entry){
                fprintf(stderr,"CRITICAL ERROR: Poped empty pointer from tx_frame?\n");
                continue;
            }
            send_pool_frame(entry, pool_iocp_send_context);
            pool_free(pool_send_udp_frame, (void*)entry);
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result 'queue_send_ctrl_frame->push_semaphore': %lu\n", result);
            continue;
        }
   }
    _endthreadex(0);    
    return 0;
}
// --- Client timeout thread function ---
static DWORD WINAPI fthread_client_timeout(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    time_t time_now;
    while(server->server_status == STATUS_READY) {
        time_now = time(NULL);
        for(int slot = 0; slot < MAX_CLIENTS; slot++){
            if(client_list->client[slot].slot_status == SLOT_FREE){
                continue; // Skip to the next client slot.
            }
            if(time_now - (time_t)client_list->client[slot].last_activity_time < (time_t)server->session_timeout){
                continue; // Skip to the next client slot.
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", client_list->client[slot].sid);
            remove_client(slot);
        }
        Sleep(1000);
    }

    // After the `while` loop condition (`server.status == SERVER_READY`) becomes false,
    _endthreadex(0);
    return 0; // Return 0 to indicate that the thread terminated successfully.
}
// --- Process server command ---
static DWORD WINAPI fthread_server_command(LPVOID lpParam){
    
    char cmd;

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

    while (server->server_status == STATUS_READY){

        fprintf(stdout,"Waiting for command...\n");
        cmd = getchar();
        switch(cmd) {
            case 's':
            case 'S':
            //check what file streams are still open
                check_open_file_stream();
                break;

            case 'l':
            case 'L':
                refill_recv_iocp_pool(server->socket, pool_iocp_recv_context);
                break;

            case 'p':
                ReadSessionIDsInRoot(SERVER_ROOT_FOLDER, SERVER_SID_FOLDER_NAME_FOR_CLIENT);
            default:
                break;
            }
        Sleep(200);
    }

    _endthreadex(0);
    return 0;
}



void init_client_pool(ServerClientPool* pool, const uint64_t block_count) {

    pool->block_count = block_count;

    // Allocate memory for 'next' array
    pool->next = (uint64_t*)_aligned_malloc(sizeof(uint64_t) * pool->block_count, 64);
    if (!pool->next) {
        fprintf(stderr, "Memory allocation failed for next indices in init_client_pool().\n");
        return;
    }
    memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)_aligned_malloc(sizeof(uint8_t) * pool->block_count, 64);
    if (!pool->used) {
        fprintf(stderr, "Memory allocation failed for used flags in init_client_pool().\n");
        _aligned_free(pool->next);
        return;
    }
    memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)

    // Allocate the main memory buffer for the pool
    pool->client = (Client*)_aligned_malloc(sizeof(Client) * pool->block_count, 64);
    if (!pool->client) {
        fprintf(stderr, "Memory allocation failed for fstream in init_client_pool().\n");
        _aligned_free(pool->next);
        _aligned_free(pool->used);
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(pool->client, 0, sizeof(Client) * pool->block_count);

    // Initialize the free list: all blocks are initially free
    pool->free_head = 0;                                        // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t index = 0; index < pool->block_count - 1; index++) {
        pool->next[index] = index + 1;                          // Link to the next block
        pool->used[index] = FREE_BLOCK;                         // Mark as unused
        InitializeSRWLock(&pool->client[index].lock);
    }
    // The last block points to END_BLOCK, indicating the end of the free list
    pool->next[pool->block_count - 1] = END_BLOCK;              // Use END_BLOCK to indicate end of list
    pool->used[pool->block_count - 1] = FREE_BLOCK;             // Last block is also unused
    InitializeSRWLock(&pool->client[pool->block_count - 1].lock);
    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}


int main() {
    // get_network_config();
    init_server_session();
    init_server_config();
    init_server_buffers();
    start_threads();
    init_server_statistics_gui();
    // Main server loop for general management, timeouts, and state updates
    while (Server.server_status == STATUS_READY) {

        Sleep(250); // Prevent busy-waiting
   
        fprintf(stdout, "\r\033[2K-- Hash_fID_Count: %llu; FrRcvFr: %llu; FrSndFr: %llu; PendingAck: %llu; PoolFBlk: %llu; TBFB: %llu : %llu", 
                            Buffers.table_file_id.count,
                            Buffers.pool_recv_udp_frame.free_blocks,
                            Buffers.pool_send_udp_frame.free_blocks,
                            ClientList.client[0].queue_ack_seq.pending,
                            Buffers.pool_file_block.free_blocks,
                            Buffers.table_file_block.count,
                            Buffers.table_file_block.pool_nodes.free_blocks
                            );

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}




