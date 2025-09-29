
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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    // memset(client_list, 0, sizeof(ClientListData));

    server->server_status = STATUS_NONE;
    server->session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    server->session_id_counter = 0;
    server->file_block_count = 0;
    snprintf(server->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, SERVER_NAME);

    return RET_VAL_SUCCESS;
}
static int init_server_config(){
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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

    server->iocp_ctrl_frame = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!server->iocp_ctrl_frame) {
        fprintf(stderr, "CreateIoCompletionPort failed iocp_ctrl_frame: %lu\n", GetLastError());
        return RET_VAL_ERROR;
    }
    for(size_t i = 0; i < SERVER_MAX_THREADS_PROCESS_FRAME; i++){
        server->iocp_data_frame[i] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!server->iocp_data_frame[i]) {
            fprintf(stderr, "CreateIoCompletionPort failed iocp_data_frame: %lu\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }

    return RET_VAL_SUCCESS;

}
static int init_server_buffers(){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    init_fstream_pool(pool_fstreams, MAX_SERVER_ACTIVE_FSTREAMS);
    init_mstream_pool(pool_mstreams, MAX_SERVER_ACTIVE_MSTREAMS);
    init_client_pool(pool_clients, MAX_CLIENTS);

    init_pool(pool_iocp_recv_context, sizeof(SocketContext), SERVER_POOL_SIZE_IOCP_RECV);
    init_pool(pool_file_block, SERVER_FILE_BLOCK_SIZE, SERVER_POOL_SIZE_FILE_BLOCK);

    init_table_id(table_file_id, 1024, 1048576);
    init_table_id(table_message_id, 1024, 32768);

    init_table_fblock(table_file_block, SERVER_POOL_SIZE_FILE_BLOCK, SERVER_POOL_SIZE_FILE_BLOCK);

    init_queue_ptr(queue_client_ptr, SERVER_QUEUE_SIZE_CLIENT_PTR);

    init_pool(pool_send_udp_frame, sizeof(PoolEntrySendFrame), SERVER_POOL_SIZE_SEND);
    init_pool(pool_iocp_send_context, sizeof(SocketContext), SERVER_POOL_SIZE_IOCP_SEND);
    init_queue_ptr(queue_send_udp_frame, SERVER_QUEUE_SIZE_SEND_FRAME);
    init_queue_ptr(queue_send_prio_udp_frame, SERVER_QUEUE_SIZE_SEND_PRIO_FRAME);
    init_queue_ptr(queue_send_ctrl_udp_frame, SERVER_QUEUE_SIZE_SEND_CTRL_FRAME);

    init_pool(&sspu->pool_ctrl_frame, sizeof(PoolEntryRecvFrame), 64);
    init_queue_ptr(&sspu->queue_ctrl_frame, SERVER_QUEUE_SIZE_RECV_FRAME);
    init_pool(&sspu->pool_data_frame, sizeof(PoolEntryRecvFrame), SERVER_POOL_SIZE_RECV);
    for(int i = 0; i < SERVER_MAX_THREADS_PROCESS_FRAME; i++){
        init_queue_ptr(&sspu->queue_data_frame[i], SERVER_POOL_SIZE_RECV);
        init_queue_ptr(&sspu->queue_prio_data_frame[i], 64);
        init_queue_ptr(&sspu->queue_stream_completition[i], MAX_SERVER_ACTIVE_FSTREAMS * 2);
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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    // Create threads for receiving and processing frames
    for(int i = 0; i < SERVER_MAX_THREADS_RECV_SEND_FRAME; i++){
        threads->recv_send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_recv_send_frame, NULL, 0, NULL);
        if (threads->recv_send_frame[i] == NULL) {
            fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(threads->recv_send_frame[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }

    for(size_t i = 0; i < SERVER_MAX_THREADS_PROCESS_FRAME; i++){
        sspu->id[i] = i;
        sspu->thread_process_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_process_frame, (LPVOID)&sspu->id[i], 0, NULL);
        if (sspu->thread_process_frame[i] == NULL) {
            fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        SetThreadPriority(sspu->thread_process_frame[i], THREAD_PRIORITY_ABOVE_NORMAL);
    }
    sspu->thread_process_ctrl_frame = (HANDLE)_beginthreadex(NULL, 0, func_thread_process_ctrl_frame, NULL, 0, NULL);
    if (sspu->thread_process_ctrl_frame == NULL) {
        fprintf(stderr, "Failed to create thread_process_ctrl_frame. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    SetThreadPriority(sspu->thread_process_ctrl_frame, THREAD_PRIORITY_NORMAL);

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
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)
    
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

                    PoolEntryRecvFrame *frame_buff = NULL;
                    uint32_t session_id = 0;
                    uint32_t file_id = 0;
                    size_t thread_id = 0;

                    uint8_t frame_type = ((UdpFrame*)socket_context->buffer)->header.frame_type;
                    switch (frame_type) {
                        case FRAME_TYPE_CONNECT_REQUEST:
                        case FRAME_TYPE_DISCONNECT:
                        case FRAME_TYPE_KEEP_ALIVE:
                            
                            frame_buff = (PoolEntryRecvFrame*)pool_alloc(&sspu->pool_ctrl_frame);
                            if (!frame_buff) {
                                fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for received frame entry.\n");
                                break;
                            }
                            
                            memset(frame_buff, 0, sizeof(PoolEntryRecvFrame));
                            memcpy(&frame_buff->frame, socket_context->buffer, NrOfBytesTransferred);
                            memcpy(&frame_buff->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                            frame_buff->frame_size = NrOfBytesTransferred;
                            GetSystemTimePreciseAsFileTime(&ft);
                            frame_buff->timestamp = FILETIME_TO_UINT64(ft);

                            PostQueuedCompletionStatus(server->iocp_ctrl_frame, 0, (uintptr_t)frame_buff, NULL);

                            break;

                        case FRAME_TYPE_FILE_METADATA:

                            frame_buff = (PoolEntryRecvFrame*)pool_alloc(&sspu->pool_data_frame);
                            if (!frame_buff) {
                                fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for received frame entry.\n");
                                break;
                            }

                            memset(frame_buff, 0, sizeof(PoolEntryRecvFrame));
                            memcpy(&frame_buff->frame, socket_context->buffer, NrOfBytesTransferred);
                            memcpy(&frame_buff->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                            frame_buff->frame_size = NrOfBytesTransferred;
                            GetSystemTimePreciseAsFileTime(&ft);
                            frame_buff->timestamp = FILETIME_TO_UINT64(ft);

                            session_id = _ntohl(frame_buff->frame.header.session_id);
                            file_id = _ntohl(frame_buff->frame.payload.file_metadata.file_id);
                            thread_id = hash_stream(session_id, file_id, SERVER_MAX_THREADS_PROCESS_FRAME);                            
                            PostQueuedCompletionStatus(server->iocp_data_frame[thread_id], 0, (uintptr_t)frame_buff, NULL);
                            // fprintf(stdout, "Pushing FRAME_TYPE_FILE_METADATA to queue[%llu]", thread_id);
                            break;
                        
                        case FRAME_TYPE_FILE_FRAGMENT:

                            frame_buff = (PoolEntryRecvFrame*)pool_alloc(&sspu->pool_data_frame);
                            if (!frame_buff) {
                                fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for received frame entry.\n");
                                break;
                            }

                            memset(frame_buff, 0, sizeof(PoolEntryRecvFrame));
                            memcpy(&frame_buff->frame, socket_context->buffer, NrOfBytesTransferred);
                            memcpy(&frame_buff->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                            frame_buff->frame_size = NrOfBytesTransferred;
                            GetSystemTimePreciseAsFileTime(&ft);
                            frame_buff->timestamp = FILETIME_TO_UINT64(ft);

                            session_id = _ntohl(frame_buff->frame.header.session_id);
                            file_id = _ntohl(frame_buff->frame.payload.file_fragment.file_id);
                            thread_id = hash_stream(session_id, file_id, SERVER_MAX_THREADS_PROCESS_FRAME);
                            PostQueuedCompletionStatus(server->iocp_data_frame[thread_id], 0, (uintptr_t)frame_buff, NULL);
                            // fprintf(stdout, "Pushing FRAME_TYPE_FILE_FRAGMENT to queue[%llu]", thread_id);
                            break;

                        case FRAME_TYPE_FILE_END:
                                                    
                            frame_buff = (PoolEntryRecvFrame*)pool_alloc(&sspu->pool_data_frame);
                            if (!frame_buff) {
                                fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for received frame entry.\n");
                                break;
                            }

                            memset(frame_buff, 0, sizeof(PoolEntryRecvFrame));
                            memcpy(&frame_buff->frame, socket_context->buffer, NrOfBytesTransferred);
                            memcpy(&frame_buff->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                            frame_buff->frame_size = NrOfBytesTransferred;
                            GetSystemTimePreciseAsFileTime(&ft);
                            frame_buff->timestamp = FILETIME_TO_UINT64(ft);

                            session_id = _ntohl(frame_buff->frame.header.session_id);
                            file_id = _ntohl(frame_buff->frame.payload.file_end.file_id);
                            thread_id = hash_stream(session_id, file_id, SERVER_MAX_THREADS_PROCESS_FRAME);

                            PostQueuedCompletionStatus(server->iocp_data_frame[thread_id], 0, (uintptr_t)frame_buff, NULL);
                            // fprintf(stdout, "Pushing FRAME_TYPE_FILE_END to queue[%llu]", thread_id);
                            break;

                        default:
                            fprintf(stderr, "CRITICAL ERROR: Invalid frame type %u.\n", frame_type);
                            break;
                    }

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
                } else {
                    fprintf(stderr, "Server: Send operation completed with 0 bytes or error.\n");
                }
                pool_free(pool_iocp_send_context, socket_context);
                break;

            default:
                fprintf(stderr, "Server: Unknown operation type in completion.\n");
                pool_free(pool_iocp_send_context, socket_context);
                break;
        } // end of switch(socket_context->type)
    } // end of while (server->server_status == STATUS_READY)
           
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}

// --- Process a control frame ---
static DWORD WINAPI func_thread_process_ctrl_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    PoolEntrySendFrame *pool_send_entry = NULL;

    DWORD dummyBytesTransferred;
    uintptr_t lpCompletionKey;
    LPOVERLAPPED dummyOverlapped;
    
    while (server->server_status == STATUS_READY) {

        BOOL getqcompl_result = GetQueuedCompletionStatus(
            server->iocp_ctrl_frame,
            &dummyBytesTransferred,
            &lpCompletionKey,
            &dummyOverlapped,
            INFINITE
        );

        if (!getqcompl_result) {
            DWORD err = GetLastError();
            fprintf(stderr, "I/O failed with error: %lu\n", err);
            continue;
        }
        
        if (lpCompletionKey == 0) {
            fprintf(stderr, "ERROR: NULL lpCompletitionKey from server->iocp_ctrl_frame. Skipping.\n");
            continue;
        }

        PoolEntryRecvFrame* recv_ptr = (PoolEntryRecvFrame*)lpCompletionKey;
        PoolEntryRecvFrame frame_buff = {0};
        
        memcpy(&frame_buff, recv_ptr, sizeof(PoolEntryRecvFrame));
        pool_free(&sspu->pool_ctrl_frame, (void*)recv_ptr);

        UdpFrame *frame = &frame_buff.frame;
        struct sockaddr_in *src_addr = &frame_buff.src_addr;
        uint32_t frame_size = frame_buff.frame_size;

        uint16_t start_delimiter = _ntohs(frame->header.start_delimiter);
        uint8_t frame_type = frame_buff.frame.header.frame_type;
        uint64_t seq_num = _ntohll(frame->header.seq_num);
        uint32_t session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (start_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "DEBUG: Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", 
                                            src_ip, src_port, start_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_size)) {
            fprintf(stderr, "DEBUG: Received frame from %s:%d with checksum mismatch. Discarding.\n", 
                                            src_ip, src_port);
            continue;
        }

        Client *client = NULL;

        if(frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                                session_id == DEFAULT_CONNECT_REQUEST_SID &&
                                seq_num == DEFAULT_CONNECT_REQUEST_SEQ){
            client = alloc_client(pool_clients);
            if (!client) {
                fprintf(stderr, "Failed to alloc_client() from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
                continue;
            }
            init_client(client, InterlockedIncrement(&server->session_id_counter), frame, src_addr);
            AcquireSRWLockExclusive(&client->lock);
            client->last_activity_time = time(NULL);
            ReleaseSRWLockExclusive(&client->lock);

            pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            if(!pool_send_entry){
                fprintf(stderr, "ERROR: Failed to allocate memory in the pool for connect response frame\n");
                continue;
            }
            construct_connect_response_frame(pool_send_entry,
                                    DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, &client->client_addr);

            if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
                pool_free(pool_send_udp_frame, pool_send_entry);
            }

            fprintf(stdout, "DEBUG: Client %s:%d Requested connection. Responding to connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;

        } else if (frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                        (session_id != DEFAULT_CONNECT_REQUEST_SID || seq_num != DEFAULT_CONNECT_REQUEST_SEQ)) {
            // This is a re-connect request from an existing client.
            fprintf(stdout, "DEBUG: Client %s:%d requested re-connection with Session ID: %u\n", 
                                                    src_ip, src_port, session_id);
            client = find_client(pool_clients, session_id);
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
            construct_connect_response_frame(pool_send_entry,
                                    DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, &client->client_addr);

            if(push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push connect response frame to queue.\n");
                pool_free(pool_send_udp_frame, pool_send_entry);
            }

            fprintf(stdout, "DEBUG: Client %s:%d Requested re-connection. Responding to re-connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;
        } else {
            client = find_client(pool_clients, session_id);
            if(!client){
                // fprintf(stderr, "Received frame from unknown client\n");
                continue;
            }
        }

        switch (frame_type) {

        case FRAME_TYPE_KEEP_ALIVE:
            AcquireSRWLockExclusive(&client->lock);
            client->last_activity_time = time(NULL);
            ReleaseSRWLockExclusive(&client->lock);

            pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            if(!pool_send_entry){
                fprintf(stderr, "ERROR: Failed to allocate memory in the pool for keep_alive ack frame\n");
                break;
            }

            uint32_t file_id = _ntohl(frame->payload.ack.file_id);

            construct_ack_frame(pool_send_entry, seq_num, session_id, file_id, STS_KEEP_ALIVE, server->socket, &client->client_addr);
            if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                pool_free(pool_send_udp_frame, pool_send_entry);
                fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
            }
            break;

        case FRAME_TYPE_DISCONNECT:
            fprintf(stdout, "DEBUG: Client %s:%d with session ID: %d requested disconnect...\n", client->ip, client->port, client->sid);
            pool_send_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            if(!pool_send_entry){
                fprintf(stderr, "ERROR: Failed to allocate memory in the pool for disconnect ack frame\n");
                break;
            }
            construct_ack_frame(pool_send_entry, seq_num, session_id, 0, STS_CONFIRM_DISCONNECT, server->socket, &client->client_addr);
            if (push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)pool_send_entry) == RET_VAL_ERROR) {
                pool_free(pool_send_udp_frame, pool_send_entry);
                fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
            }
            // TODO: if client is closed immediately then the server can crash since another worker thread will try to access
            // clear client memory in the pool. it is safe to close the client after no more frames are being send (timeout).
            // need to think of a strategy to safely close the client with frame request!
            // One ideea is to send a disconnect response and stop processing frames from this client and 
            // let the timeout disconnect the client naturally (probably need to introduce separate timeout with lower time value)
            AcquireSRWLockExclusive(&client->lock);
            client->connection_status = CLIENT_DISCONNECTED;
            ReleaseSRWLockExclusive(&client->lock);
            // close_client(client);
            break;
        }
    }
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}

static DWORD WINAPI func_thread_process_frame(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    size_t thread_id = *((size_t *)lpParam);

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    DWORD dummyBytesTransferred;
    uintptr_t lpCompletionKey;
    LPOVERLAPPED dummyOverlapped;
    
    while (server->server_status == STATUS_READY) {

        BOOL getqcompl_result = GetQueuedCompletionStatus(
            server->iocp_data_frame[thread_id],
            &dummyBytesTransferred,
            &lpCompletionKey,
            &dummyOverlapped,
            INFINITE
        );

        if (!getqcompl_result) {
            DWORD err = GetLastError();
            fprintf(stderr, "I/O failed with error: %lu\n", err);
            continue;
        }
        
        if (lpCompletionKey == 0) {
            fprintf(stderr, "ERROR: NULL lpCompletitionKey from server->iocp_ctrl_frame. Skipping.\n");
            continue;
        }

        PoolEntryRecvFrame* recv_ptr = (PoolEntryRecvFrame*)lpCompletionKey;
        PoolEntryRecvFrame frame_buff = {0};
        
        memcpy(&frame_buff, recv_ptr, sizeof(PoolEntryRecvFrame));
        pool_free(&sspu->pool_data_frame, (void*)recv_ptr);

        UdpFrame *frame = &frame_buff.frame;
        struct sockaddr_in *src_addr = &frame_buff.src_addr;
        uint32_t frame_size = frame_buff.frame_size;

        uint16_t start_delimiter = _ntohs(frame->header.start_delimiter);
        uint8_t frame_type = frame_buff.frame.header.frame_type;
        uint64_t seq_num = _ntohll(frame->header.seq_num);
        uint32_t session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (start_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "DEBUG: Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", 
                                            src_ip, src_port, start_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_size)) {
            fprintf(stderr, "DEBUG: Received frame from %s:%d with checksum mismatch. Discarding.\n", 
                                            src_ip, src_port);
            continue;
        }

        Client *client = find_client(pool_clients, session_id);
        if(!client){
            fprintf(stderr, "Received frame from unknown client\n");
            continue;
        }

        switch (frame_type) {

            case FRAME_TYPE_ACK:
                // TODO: Implement the full ACK processing logic here. This typically involves:
                //   - Removing acknowledged packets from the sender's retransmission queue.
                //   - Updating window sizes for flow and congestion control.
                //   - Advancing sequence numbers to indicate successfully received data.
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

            default:
                fprintf(stderr, "Received unknown frame type: %u from %s:%d (Session ID: %u). Discarding.\n",
                        frame_type, src_ip, src_port, session_id);
                break;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- IOCP Write file block ---
static DWORD WINAPI func_thread_file_block_written(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)
    
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
        
        if(node->op_type == 1 && NrOfBytesWritten > 0 && NrOfBytesWritten <= SERVER_FILE_BLOCK_SIZE){
       
            AcquireSRWLockExclusive(&fstream->lock);
            // if(!ht_search_fblock(table_file_block, node->key)){
            //     fprintf(stdout,"ERROR: Key %llu not found in hash table!\n", node->key);
            // }
            uint64_t block_offset = ((uint64_t)node->overlapped.Offset) | (((uint64_t)node->overlapped.OffsetHigh) << 32);
            uint64_t block_nr = block_offset / SERVER_FILE_BLOCK_SIZE;
            ht_remove_fblock(table_file_block, node->key);
            pool_free(pool_file_block, fstream->file_block[block_nr]);

            fstream->file_block[block_nr] = NULL;
            fstream->recv_block_bytes[block_nr] = 0;
            // *(fstream->recv_block_bytes + block_nr) = 0;
            fstream->recv_block_status[block_nr] = BLOCK_STATUS_RECEIVED;

            fstream->written_bytes_count += NrOfBytesWritten;

            if(fstream->end_of_file && (fstream->written_bytes_count == fstream->file_size)){
                if(fstream->written_bytes_count == 0){
                    fprintf(stderr, "CRITICAL ERROR: File write finished with zero bytes written: %s\n", fstream->ansi_path);
                    continue;
                }
                if(check_bitmap(fstream->received_file_bitmap, fstream->fragment_count)){
                    ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_RECV_COMPLETE);
                }
                close_fstream(fstream);
                free_fstream(pool_fstreams, fstream);
            }
            ReleaseSRWLockExclusive(&fstream->lock);
        } else if (node->op_type == 2 && NrOfBytesWritten == 0){

                    AcquireSRWLockExclusive(&fstream->lock);
                    fstream->end_of_file = TRUE;
                    ht_remove_fblock(table_file_block, node->key);
                    if(fstream->end_of_file && (fstream->written_bytes_count == fstream->file_size)){
                        if(fstream->written_bytes_count == 0){
                            fprintf(stderr, "CRITICAL ERROR: File write finished with zero bytes written: %s\n", fstream->ansi_path);
                            continue;
                        }
                        if(check_bitmap(fstream->received_file_bitmap, fstream->fragment_count)){
                            ht_update_id_status(table_file_id, fstream->sid, fstream->fid, ID_RECV_COMPLETE);
                        }
                        close_fstream(fstream);
                        free_fstream(pool_fstreams, fstream);
                    }
                    ReleaseSRWLockExclusive(&fstream->lock);

        } else if (node->op_type == 3 && NrOfBytesWritten == 0){

                    AcquireSRWLockExclusive(&fstream->lock);
                    ht_remove_fblock(table_file_block, node->key);
                    close_fstream(fstream);
                    free_fstream(pool_fstreams, fstream);
                    ReleaseSRWLockExclusive(&fstream->lock);

        }
    }
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}
// --- File SAck Thread functions ---
static DWORD WINAPI fthread_send_sack_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    PoolEntrySendFrame *sack_entry = NULL;

    // uint32_t slot = 0;
    Client *client = NULL;

    FILETIME ft;
    uint64_t current_timestamp;
    BOOL sack_ready_timeout;

    uint64_t ack_seq = 0;
    
    while (server->server_status == STATUS_READY) {
        

        DWORD result = WaitForSingleObject(queue_client_ptr->push_semaphore, INFINITE); // Wait for a client slot to be available
        if(result ==  WAIT_OBJECT_0) {
            // slot available
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore queue client slot: %lu\n", result);
            continue;
        }

        client = (Client*)pop_ptr(queue_client_ptr);
        if(!client){
            fprintf(stdout, "Poped invalid client from 'queue_client_ptr'\n");
            continue;
        }
        
        AcquireSRWLockExclusive(&client->sack_buff.lock);
        GetSystemTimePreciseAsFileTime(&ft);
        current_timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if(!client->sack_buff.start_recorded && client->queue_ack_seq.pending > 0){
            // Combine the high and low parts of the FILETIME to get a 64-bit value
            client->sack_buff.start_timestamp = current_timestamp;
            client->sack_buff.start_recorded = TRUE;
        }
        if(client->queue_ack_seq.pending >= MAX_SACK_COUNT){
            client->sack_buff.ack_pending = MAX_SACK_COUNT;
            for(int i = 0; i < client->sack_buff.ack_pending; i++){
                ack_seq = pop_seq(&client->queue_ack_seq);
                if(ack_seq == 0) {
                    memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
                    ReleaseSRWLockExclusive(&client->sack_buff.lock);
                    client = NULL;
                    fprintf(stderr,"CRITICAL ERROR: pop_seq() returned seq with value 0 from queue_ack_seq\n");
                    continue;
                }
                client->sack_buff.payload.seq_num[i] = ack_seq;
            }
            client->sack_buff.payload.ack_count = client->sack_buff.ack_pending;
            sack_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
            construct_sack_frame(sack_entry, client->sid, &client->sack_buff.payload ,server->socket, &client->client_addr);

            if(push_ptr(queue_send_udp_frame, (uintptr_t)sack_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR: Failed to push sack frame to queue.\n");
                pool_free(pool_send_udp_frame, sack_entry);
            }
            memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
            client->sack_buff.ack_pending = 0;
            client->sack_buff.start_recorded = FALSE;
        } 
        ReleaseSRWLockExclusive(&client->sack_buff.lock);
        client = NULL;

    }
    _endthreadex(0);
    return 0;
}
static DWORD WINAPI fthread_scan_for_trailing_sack(LPVOID lpParam){
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    PoolEntrySendFrame *sack_entry = NULL;
 
    uint32_t slot = 0;
    Client *client = NULL;

    FILETIME ft;
    uint64_t current_timestamp;
    BOOL sack_ready_timeout;

    uint64_t ack_seq = 0;
    
    while (server->server_status == STATUS_READY) {
    
        for(int i = 0; i < MAX_CLIENTS; i++){
            AcquireSRWLockShared(&pool_clients->lock);
            if(!pool_clients->used[i]){
                ReleaseSRWLockShared(&pool_clients->lock);
                Sleep(10);
                continue;
            }
            client = &pool_clients->client[i];
            ReleaseSRWLockShared(&pool_clients->lock);
            
            if(client->queue_ack_seq.pending == 0){
                Sleep(10);
                continue;
            }
            
            AcquireSRWLockExclusive(&client->sack_buff.lock);            
            GetSystemTimePreciseAsFileTime(&ft);
            current_timestamp = FILETIME_TO_UINT64(ft);
            sack_ready_timeout = client->sack_buff.start_recorded && ((current_timestamp - client->sack_buff.start_timestamp) > (SACK_READY_FRAME_TIMEOUT_MS * 10000));
            if(sack_ready_timeout && client->queue_ack_seq.pending > 0 && client->queue_ack_seq.pending < MAX_SACK_COUNT){
                
                client->sack_buff.ack_pending = client->queue_ack_seq.pending;

                for(int i = 0; i < client->sack_buff.ack_pending; i++){
                    ack_seq = pop_seq(&client->queue_ack_seq);
                    if(ack_seq == 0) {
                        memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
                        ReleaseSRWLockExclusive(&client->sack_buff.lock);
                        fprintf(stderr,"CRITICAL ERROR: pop_seq() returned seq with value 0 from queue_ack_seq\n");
                        continue;
                    }
                    client->sack_buff.payload.seq_num[i] = ack_seq;
                }
                client->sack_buff.payload.ack_count = client->sack_buff.ack_pending;
                sack_entry = (PoolEntrySendFrame*)pool_alloc(pool_send_udp_frame);
                construct_sack_frame(sack_entry, client->sid, &client->sack_buff.payload ,server->socket, &client->client_addr);           

                if(push_ptr(queue_send_udp_frame, (uintptr_t)sack_entry) == RET_VAL_ERROR) {
                    fprintf(stderr, "ERROR: Failed to push sack frame to queue.\n");
                    pool_free(pool_send_udp_frame, sack_entry);
                }
                memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
                client->sack_buff.ack_pending = 0;
                client->sack_buff.start_recorded = FALSE;
            } 
            ReleaseSRWLockExclusive(&client->sack_buff.lock);
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Send frame thread functions ---
static DWORD WINAPI fthread_send_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    time_t time_now;
    while(server->server_status == STATUS_READY) {
        time_now = time(NULL);
        for(int i = 0; i < MAX_CLIENTS; i++){

            AcquireSRWLockShared(&pool_clients->lock);
            if(!pool_clients->used[i]){
                ReleaseSRWLockShared(&pool_clients->lock);
                continue;
            }
            Client *client = &pool_clients->client[i];
            ReleaseSRWLockShared(&pool_clients->lock);

            if(time_now - (time_t)client->last_activity_time < (time_t)server->session_timeout){
                continue; // Skip to the next client slot.
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", client->sid);
            close_client(client);
            client = NULL;
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

    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

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
    uint64_t last_block = pool->block_count - 1;
    for (uint64_t index = 0; index < last_block; index++) {
        pool->next[index] = index + 1;                          // Link to the next block
        pool->used[index] = FREE_BLOCK;                         // Mark as unused

        pool->client[index].connection_status = CLIENT_DISCONNECTED;
        pool->client[index].last_activity_time = time(NULL);

        InitializeSRWLock(&pool->client[index].lock);
        InitializeSRWLock(&pool->client[index].sack_buff.lock);
        init_queue_seq(&pool->client[index].queue_ack_seq, SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ);   
    }
    // The last block points to END_BLOCK, indicating the end of the free list
    pool->next[last_block] = END_BLOCK;              // Use END_BLOCK to indicate end of list
    pool->used[last_block] = FREE_BLOCK;             // Last block is also unused

    pool->client[last_block].connection_status = CLIENT_DISCONNECTED;
    pool->client[last_block].last_activity_time = time(NULL);

    InitializeSRWLock(&pool->client[last_block].lock);
    InitializeSRWLock(&pool->client[last_block].sack_buff.lock);
    init_queue_seq(&pool->client[last_block].queue_ack_seq, SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ);

    pool->free_blocks = pool->block_count;
    // Initialize the critical section for thread safety
    InitializeSRWLock(&pool->lock);
    return;
}
Client* alloc_client(ServerClientPool* pool) {
    // Enter critical section to protect shared pool data
    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to alloc_client() in an unallocated pool!\n");
        return NULL;
    }
    AcquireSRWLockExclusive(&pool->lock);
    // Check if the pool is exhausted
    if (pool->free_head == END_BLOCK) { // Check against END_BLOCK
        ReleaseSRWLockExclusive(&pool->lock);
        return NULL; // Pool exhausted
    }
    // Get the index of the first free block
    uint64_t index = pool->free_head;
    // Update the free head to the next free block
    pool->free_head = pool->next[index];
    // Mark the allocated block as used
    pool->used[index] = USED_BLOCK;
    pool->free_blocks--;
    ReleaseSRWLockExclusive(&pool->lock);
    return &pool->client[index];
}
Client* find_client(ServerClientPool* pool, const uint32_t sid) {

    Client *client = NULL;

    if(!pool) {
        fprintf(stderr, "ERROR: Attempt to find_client() in an unallocated pool!\n");
        return NULL;
    }

    AcquireSRWLockShared(&pool->lock);
    for(uint64_t index = 0; index < pool->block_count; index++){
        if(!pool->used[index]) {
            continue;
        }
        client = &pool->client[index];
        if(client->sid == sid && client->connection_status == CLIENT_CONNECTED){
            ReleaseSRWLockShared(&pool->lock);
            return client;
        }
    }
    ReleaseSRWLockShared(&pool->lock);
    return NULL;
}
int init_client(Client *client, const uint32_t sid, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr){

    if(!client){
        fprintf(stderr, "Invalid client pointer passed for init!\n");
        return RET_VAL_ERROR;
    }

    AcquireSRWLockExclusive(&client->lock);

    memcpy(&client->client_addr, client_addr, sizeof(struct sockaddr_in));
    client->connection_status = CLIENT_CONNECTED;
    client->last_activity_time = time(NULL);

    client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
    client->sid = sid;
    client->flags = recv_frame->payload.connection_request.flags;

    snprintf(client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, client->ip, INET_ADDRSTRLEN);
    client->port = _ntohs(client_addr->sin_port);

    memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
    client->sack_buff.ack_pending = 0;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    client->sack_buff.start_timestamp = FILETIME_TO_UINT64(ft);
    client->sack_buff.start_recorded = FALSE;

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", client->ip, client->port, client->sid);

    ReleaseSRWLockExclusive(&client->lock);

    return RET_VAL_SUCCESS;

}
void free_client(ServerClientPool* pool, Client* client) {
    
    if (!pool) {
        fprintf(stderr, "ERROR: Attempt to free_client() in an unallocated pool!\n");
        return;
    }
    if (!client) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in client pool!\n");
        return;
    }
    AcquireSRWLockExclusive(&pool->lock);
    // Calculate the index of the block to be freed
    uint64_t index = (uint64_t)(((char*)client - (char*)pool->client) / sizeof(Client));
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || pool->used[index] == FREE_BLOCK) {       
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid client from pool!\n");
        ReleaseSRWLockExclusive(&pool->lock);
        return;
    }
    // Add the freed block back to the head of the free list
    pool->next[index] = pool->free_head;
    pool->free_head = index;
    // Mark the block as unused
    pool->used[index] = FREE_BLOCK;
    pool->free_blocks++;
    ReleaseSRWLockExclusive(&pool->lock);
    return;
}
static void close_client(Client *client){
    
    PARSE_SERVER_GLOBAL_DATA(Server, Buffers, Threads) // this macro is defined in server header file (server.h)

    if(!client){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }

    AcquireSRWLockExclusive(&client->lock);

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &pool_fstreams->fstream[i];
        AcquireSRWLockExclusive(&fstream->lock);
        if(fstream->sid == client->sid){
            close_fstream(fstream);
            free_fstream(pool_fstreams, fstream);
        }
        ReleaseSRWLockExclusive(&fstream->lock);
    }

    ht_remove_all_sid(table_file_id, client->sid);
 
    AcquireSRWLockExclusive(&client->sack_buff.lock);
    client->sack_buff.ack_pending = 0;
    client->sack_buff.start_recorded = 0;
    memset(&client->sack_buff.payload, 0, sizeof(SAckPayload));
    ReleaseSRWLockExclusive(&client->sack_buff.lock);

    memset(&client->client_addr, 0, sizeof(struct sockaddr_in));
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    client->port = 0;
    client->cid = 0;
    memset(&client->name, 0, MAX_NAME_SIZE);
    client->flags = 0;
    client->connection_status = CLIENT_DISCONNECTED;
    client->last_activity_time = time(NULL);
    client->sid = 0;
    free_client(pool_clients, client);
    ReleaseSRWLockExclusive(&client->lock);
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
   
        fprintf(stdout, "\r\033[2K-- Hash_fID_Count: %llu; FrSndFr: %llu; PendingAck: %llu; PoolFBlk: %llu; TBFB: %llu : %llu", 
                            Buffers.table_file_id.count,
                            Buffers.pool_send_udp_frame.free_blocks,
                            Server.pool_clients.client[0].queue_ack_seq.pending,
                            Buffers.pool_file_block.free_blocks,
                            Buffers.table_file_block.count,
                            Buffers.table_file_block.pool.free_blocks
                            );

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}



// Find client by session ID
// static Client* find_client(const uint32_t session_id) {
    
//     PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

//     // Search for a client within the provided ClientList that matches the given session ID.
//     for (int slot = 0; slot < MAX_CLIENTS; slot++) {
//         Client *client = &client_list->client[slot];
//         AcquireSRWLockShared(&client->lock);
//         if(client->slot_status == SLOT_FREE){
//             ReleaseSRWLockShared(&client->lock);
//             continue; // Move to the next slot in the loop.
//         }
//         if(client->sid == session_id){
//             ReleaseSRWLockShared(&client->lock);
//             return client;
//         }
//         ReleaseSRWLockShared(&client->lock);
//     }
//     return NULL;
// }
// // Add a new client
// static Client* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
       
//     PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

//     uint32_t free_slot = 0;

//     EnterCriticalSection(&client_list->lock);
//     while(free_slot < MAX_CLIENTS){
//         if(client_list->client[free_slot].slot_status == SLOT_FREE) {
//             break;
//         }
//         free_slot++;
//     }

//     if(free_slot >= MAX_CLIENTS){
//         fprintf(stderr, "\nMax clients reached. Cannot add new client.\n");
//         LeaveCriticalSection(&client_list->lock);
//         return NULL;
//     }
   
//     Client *new_client = &client_list->client[free_slot]; 
    
//     AcquireSRWLockExclusive(&new_client->lock);

//     new_client->slot = free_slot;
//     new_client->slot_status = SLOT_BUSY;
//     memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
//     new_client->connection_status = CLIENT_CONNECTED;
//     new_client->last_activity_time = time(NULL);

//     new_client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
//     new_client->sid = InterlockedIncrement(&server->session_id_counter);
//     new_client->flags = recv_frame->payload.connection_request.flags;

//     snprintf(new_client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

//     inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
//     new_client->port = _ntohs(client_addr->sin_port);

//     fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->sid);

//     ReleaseSRWLockExclusive(&new_client->lock);
//     LeaveCriticalSection(&client_list->lock);
//     return new_client;
// }
// // Remove a client
// static int remove_client(const uint32_t slot) {
    
//     PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)

//     if(client_list == NULL){
//         fprintf(stderr, "\nInvalid client pointer!\n");
//         return RET_VAL_ERROR;
//     }
//     if (slot < 0 || slot >= MAX_CLIENTS) {
//         fprintf(stderr, "\nInvalid client slot nr:  %d", slot); 
//         return RET_VAL_ERROR;
//     }
//     fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", client_list->client[slot].sid, client_list->client[slot].slot);   
    
//     EnterCriticalSection(&client_list->lock);    
//     cleanup_client(&client_list->client[slot]);
//     LeaveCriticalSection(&client_list->lock);

//     return RET_VAL_SUCCESS;
// }

