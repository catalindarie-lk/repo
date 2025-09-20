// --- udp_client.c ---
#include <stdio.h>                      // For printf, fprintf
#include <string.h>
#include <tchar.h>                     // For memset, memcpy
#include <stdint.h>                     // For fixed-width integer types
#include <time.h>                       // For time functions
#include <process.h>                    // For _beginthreadex
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#include "include/client.h"
#include "include/client_frames.h"
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/resources.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/checksum.h"           // For checksum validation
#include "include/sha256.h"
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management
#include "include/client_api.h"
#include "include/client_statistics.h"

ClientData Client;
ClientQueues Queues;
ClientBuffers Buffers;
ClientThreads Threads;

const char *server_ip = "192.168.0.241";
const char *client_ip = "192.168.0.240";

// const char *server_ip = "127.0.0.1";
// const char *client_ip = "127.0.0.1";


int init_client_session(){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    memset(client, 0, sizeof(ClientData));
    
    client->client_status = STATUS_BUSY;
    client->session_status = CONNECTION_CLOSED;

    client->cid = CLIENT_ID;
    
    client->flags = 0;
    snprintf(client->client_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, CLIENT_NAME);
    client->last_active_time = time(NULL);

    client->frame_count = 1;
    client->fid_count = 0;
    client->mid_count = 0;

    client->sid = DEFAULT_CONNECT_REQUEST_SID;
    client->server_status = STATUS_CLOSED;
    client->session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client->server_name, 0, MAX_NAME_SIZE);
    
    return RET_VAL_SUCCESS;     
}
int reset_client_session(){
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    client->session_status = CONNECTION_CLOSED;
    
    client->frame_count = 1;
    client->fid_count = 0;
    client->mid_count = 0;

    client->sid = DEFAULT_CONNECT_REQUEST_SID;
    client->server_status = STATUS_CLOSED;
    client->session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client->server_name, 0, MAX_NAME_SIZE);
    return RET_VAL_SUCCESS;
}
static int init_client_config(){
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    WSADATA wsaData;

    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        return RET_VAL_ERROR;
    }

    client->socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (client->socket == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        return RET_VAL_ERROR;
    }

    client->client_addr.sin_family = AF_INET;
    client->client_addr.sin_port = _htons(0); // Let OS choose port
    client->client_addr.sin_addr.s_addr = inet_addr(client_ip);

    if (bind(client->socket, (struct sockaddr *)&client->client_addr, sizeof(client->client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        return RET_VAL_ERROR;
    }
   
    client->iocp_handle = CreateIoCompletionPort((HANDLE)client->socket, NULL, 0, 0);
    if (client->iocp_handle == NULL || client->iocp_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateIoCompletionPort failed: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // InitializeCriticalSection(&client->send_lock);
    client->send_speed_mbps = 0; //Mbps
    client->send_period_usec = calculate_period_usec(client->send_speed_mbps, FILE_FRAGMENT_SIZE); //microsec

    return RET_VAL_SUCCESS;
}
static int init_client_buffers(){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    init_pool(pool_send_iocp_context, sizeof(SocketContext), CLIENT_POOL_SIZE_IOCP_SEND);
    s_init_pool(pool_send_udp_frame, sizeof(PoolEntrySendFrame), CLIENT_POOL_SIZE_SEND);

    s_init_queue_ptr(queue_send_udp_frame, CLIENT_QUEUE_SIZE_SEND_FRAME);
    s_init_queue_ptr(queue_send_prio_udp_frame, CLIENT_QUEUE_SIZE_SEND_PRIO_FRAME);
    s_init_queue_ptr(queue_send_ctrl_udp_frame, CLIENT_QUEUE_SIZE_SEND_CTRL_FRAME);
    init_table_send_frame(table_send_udp_frame, CLIENT_POOL_SIZE_SEND, CLIENT_POOL_SIZE_SEND * 4);

    init_pool(pool_recv_iocp_context, sizeof(SocketContext), CLIENT_POOL_SIZE_IOCP_RECV);
    init_pool(pool_recv_udp_frame, sizeof(PoolEntryRecvFrame), CLIENT_POOL_SIZE_RECV);
    init_queue_ptr(queue_recv_udp_frame, CLIENT_QUEUE_SIZE_RECV_FRAME);
    init_queue_ptr(queue_recv_prio_udp_frame, CLIENT_QUEUE_SIZE_RECV_PRIO_FRAME);
        
    s_init_pool(pool_send_command, sizeof(PoolEntryCommand), CLIENT_POOL_SIZE_SEND_COMMAND);
    s_init_queue_ptr(queue_send_file_command, CLIENT_QUEUE_SIZE_SEND_FILE);
    s_init_queue_ptr(queue_send_message_command, CLIENT_QUEUE_SIZE_SEND_MESSAGE);

    init_pool(pool_error_log, sizeof(PoolErrorLogEntry), CLIENT_POOL_SIZE_LOG);
    init_queue_ptr(queue_error_log, CLIENT_QUEUE_SIZE_LOG);
   
    for(int i = 0; i < CLIENT_MAX_ACTIVE_FSTREAMS; i++){
        memset(&client->fstream[i], 0, sizeof(ClientFileStream));
        client->fstream[i].chunk_buffer = _aligned_malloc(CLIENT_FILE_BLOCK_SIZE, 64);
        if(!client->fstream[i].chunk_buffer){
            fprintf(stderr, "CRITICAL ERROR: Failed to pre-allocate memory for fstream (chunk_buffer).\n");
            return RET_VAL_ERROR;
        }
    }
    for(int i = 0; i < CLIENT_MAX_ACTIVE_MSTREAMS; i++){
        memset(&client->mstream[i], 0, sizeof(ClientMessageStream));
        client->mstream[i].message_buffer = malloc(MAX_MESSAGE_SIZE_BYTES);
        if(!client->mstream[i].message_buffer){
            fprintf(stderr, "CRITICAL ERROR: Failed to pre-allocate memory for mstream (message_buffer).\n");
            return RET_VAL_ERROR;
        }
    }

    for (int i = 0; i < CLIENT_POOL_SIZE_IOCP_RECV; ++i) {
        SocketContext* recv_context = (SocketContext*)pool_alloc(pool_recv_iocp_context);
        if (recv_context == NULL) {
            fprintf(stderr, "CRITICAL ERROR: Failed to allocate receive context from pool %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
        init_socket_context(recv_context, OP_RECV); // Initialize the context

        if (udp_recv_from(client->socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "CRITICAL ERROR: Failed to post initial receive operation %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
    }

    // Initialize connection request event
    client->hevent_connection_pending = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (client->hevent_connection_pending == NULL) {
        fprintf(stdout, "CRITICAL ERROR: CreateEvent listen failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    // Initialize connection successfull event
    client->hevent_connection_established = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (client->hevent_connection_established == NULL) {
        fprintf(stdout, "CRITICAL ERROR: CreateEvent established failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client->hevent_connection_closed = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (client->hevent_connection_closed == NULL) {
        fprintf(stdout, "CRITICAL ERROR: CreateEvent disconnect failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // Initialize fstreams
    for(int index = 0; index < CLIENT_MAX_ACTIVE_FSTREAMS; index++){
        client->fstream[index].hevent_metadata_response_ok = CreateEvent(NULL, FALSE, FALSE, NULL);
        client->fstream[index].hevent_metadata_response_nok = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (client->fstream[index].hevent_metadata_response_ok == NULL || 
            client->fstream[index].hevent_metadata_response_nok == NULL) {
            fprintf(stderr, "CRITICAL ERROR: Failed to create fstream metadata events. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
        InitializeSRWLock(&client->fstream[index].lock);
    }
    InitializeSRWLock(&client->fstreams_lock);
    // Initialize mstreams
    for(int index = 0; index < CLIENT_MAX_ACTIVE_MSTREAMS; index++){
        InitializeCriticalSection(&client->mstream[index].lock);
    }

    // CLIENT_READY
    client->client_status = STATUS_READY;
    return RET_VAL_SUCCESS;

}
static int start_threads(){
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    //-------------------------------------------------------------------------------------------------------------------
    for(int i = 0; i < CLIENT_MAX_THREADS_RECV_SEND_FRAME; i++){
        threads->recv_send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_recv_send_frame, NULL, 0, NULL);
        if (threads->recv_send_frame[i] == NULL) {
            fprintf(stderr, "CRITICAL ERROR: Failed to create thread (recv_send_frame). Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    //-------------------------------------------------------------------------------------------------------------------
    for(int i = 0; i < CLIENT_MAX_TREADS_PROCESS_FRAME; i++){
        threads->process_frame[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_process_frame, NULL, 0, NULL);
        if (threads->process_frame[i] == NULL) {
            fprintf(stderr, "CRITICAL ERROR: Failed to create thread (process_frame). Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    //-------------------------------------------------------------------------------------------------------------------
    threads->resend_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_resend_frame, NULL, 0, NULL);
    if (threads->resend_frame == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (resend_frame). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    threads->keep_alive = (HANDLE)_beginthreadex(NULL, 0, fthread_keep_alive, NULL, 0, NULL);
    if (threads->keep_alive == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (keep_alive). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    for(int i = 0; i < CLIENT_MAX_THREADS_SEND_FRAME; i++){
        threads->send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_send_frame, NULL, 0, NULL);
        if (threads->send_frame[i] == NULL) {
            fprintf(stderr, "CRITICAL ERROR: Failed to create thread (pop_send_frame). Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    //-------------------------------------------------------------------------------------------------------------------
    threads->send_prio_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_prio_frame, NULL, 0, NULL);
    if (threads->send_prio_frame == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (pop_send_prio_frame). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    threads->send_ctrl_frame = (HANDLE)_beginthreadex(NULL, 0, fthread_send_ctrl_frame, NULL, 0, NULL);
    if (threads->send_ctrl_frame == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (pop_send_ctrl_frame). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    // START FSTREAM WORKER THREADS
    client->fstreams_semaphore = CreateSemaphore(NULL, CLIENT_MAX_ACTIVE_FSTREAMS, LONG_MAX, NULL);
    if (client->fstreams_semaphore == NULL) {
        fprintf(stderr, "CreateSemaphore failed (fstreams_semaphore): error %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    for(int i = 0; i < (CLIENT_MAX_ACTIVE_FSTREAMS); i++){
        threads->process_fstream[i] = (HANDLE)_beginthreadex(NULL, 0, fthread_process_fstream, NULL, 0, NULL);
        if (threads->process_fstream[i] == NULL){
            fprintf(stderr, "CRITICAL ERROR: Failed to create thread (fstream). Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    //-------------------------------------------------------------------------------------------------------------------
    // START MSTREAM WORKER THREADS
    // client->mstreams_semaphore = CreateSemaphore(NULL, CLIENT_MAX_ACTIVE_MSTREAMS, LONG_MAX, NULL);
    // if (client->mstreams_semaphore == NULL) {
    //     fprintf(stderr, "CreateSemaphore failed (mstreams_semaphore): error %d\n", GetLastError());
    //     return RET_VAL_ERROR;
    // }
    // for(int index = 0; index < CLIENT_MAX_ACTIVE_MSTREAMS; index++){
    //     threads->process_mstream[index] = (HANDLE)_beginthreadex(NULL, 0, fthread_process_mstream, NULL, 0, NULL);
    //     if (threads->process_mstream[index] == NULL){
    //         fprintf(stderr, "CRITICAL ERROR: Failed to create thread (mstream). Error: %d\n", GetLastError());
    //         return RET_VAL_ERROR;
    //     }
    // }
    //-------------------------------------------------------------------------------------------------------------------
    threads->client_command = (HANDLE)_beginthreadex(NULL, 0, fthread_client_command, NULL, 0, NULL);
    if (threads->client_command == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (client_command). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    threads->error_log_write = (HANDLE)_beginthreadex(NULL, 0, fthread_error_log_write, NULL, 0, NULL);
    if (threads->error_log_write == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Failed to create thread (error_log_write). Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    //-------------------------------------------------------------------------------------------------------------------
    return RET_VAL_SUCCESS;
}
static void client_shutdown(){
    return;
}

// --- log function ---
int log_to_file(const char* log_message) {
    // Get the precise system time
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    if (!log_message) {
        fprintf(stderr, "Invalid log message pointer.\n");
        return RET_VAL_ERROR;
    }
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        fprintf(stderr, "Failed to get precise system time.\n");
        return RET_VAL_ERROR;
    }
    // Combine the high and low parts of the FILETIME to get a 64-bit value
    unsigned long long filetime_64bit = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    PoolErrorLogEntry *error_log_entry = (PoolErrorLogEntry *)pool_alloc(pool_error_log);
    if (!error_log_entry) {
        fprintf(stderr, "CRITICAL ERROR: Failed to allocate memory for error log entry. Dropping log!\n");
        return RET_VAL_ERROR;
    }
    // Initialize the error log entry
    memcpy(error_log_entry->log_message, log_message, sizeof(error_log_entry->log_message) - 1);
    error_log_entry->log_message[sizeof(error_log_entry->log_message) - 1] = '\0'; // Ensure null termination
    error_log_entry->timestamp_64bit = filetime_64bit;
    // Push the error log entry to the queue
    if(push_ptr(queue_error_log, (uintptr_t)error_log_entry) == RET_VAL_ERROR) {
        fprintf(stderr, "Failed to push error log entry to queue - Pool FULL\n");
        pool_free(pool_error_log, (void *)error_log_entry);
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}
// --- Cleanup functions for streams ---
void close_file_stream(ClientFileStream *fstream){
    // EnterCriticalSection(&fstream->lock);
    memset(&fstream->calculated_sha256, 0, 32);
    if(fstream->fp){
        fclose(fstream->fp);
        fstream->fp = NULL;
    }
    fstream->fid = 0;
    fstream->fsize = 0;
    fstream->fpath_len = 0;
    memset(&fstream->dirpath, 0, MAX_PATH);
    fstream->rpath_len = 0;
    memset(&fstream->rpath, 0, MAX_PATH);
    fstream->fname_len = 0;
    memset(&fstream->fname, 0, MAX_PATH);
    fstream->pending_bytes = 0;
    fstream->pending_metadata_seq_num = 0;
    memset(fstream->chunk_buffer, 0, CLIENT_FILE_BLOCK_SIZE);
    fstream->fstream_busy = FALSE;
    // LeaveCriticalSection(&fstream->lock);
    return;
}
void close_message_stream(ClientMessageStream *mstream){
    EnterCriticalSection(&mstream->lock);
    memset(mstream->message_buffer, 0, MAX_MESSAGE_SIZE_BYTES);
    mstream->message_id = 0;
    mstream->message_len = 0;
    mstream->remaining_bytes_to_send = 0;
    mstream->throttle = FALSE;
    mstream->mstream_busy = FALSE;
    LeaveCriticalSection(&mstream->lock);
    return;
}

// --- Receive frame thread function ---
static DWORD WINAPI fthread_recv_send_frame(LPVOID lpParam) {

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)
 
    HANDLE CompletitionPort = client->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;
    char ip_string_buffer[INET_ADDRSTRLEN];

    char log_message[CLIENT_LOG_MESSAGE_LEN];
    
    while(client->client_status == STATUS_READY){
        WaitForSingleObject(client->hevent_connection_pending, INFINITE);       

        client->session_status = CONNECTION_PENDING;
        
        while(1){

            if(client->session_status == CONNECTION_CLOSED){
                fprintf(stderr, "Stopped listening...\n");
                break;
            }

            BOOL getqcompl_result = GetQueuedCompletionStatus(
                CompletitionPort,
                &NrOfBytesTransferred,
                &lpCompletitionKey,
                &lpOverlapped,
                INFINITE //WSARECV_TIMEOUT_MS
            );
                        
            if (lpOverlapped == NULL) {
                snprintf(log_message, sizeof(log_message), "WARNING: NULL pOverlapped received. IOCP may be shutting down.");
                log_to_file(log_message);
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
                    snprintf(log_message, sizeof(log_message), "GetQueuedCompletionStatus failed with error: %d.", wsa_error);
                    log_to_file(log_message);
                    // If it's a real error on a specific operation
                    if (socket_context->type == OP_SEND) {
                        pool_free(pool_send_iocp_context, socket_context);
                    } else if (socket_context->type == OP_RECV) {
                        // Critical error on a receive socket_context -"retire" this socket_context from the pool.
                        snprintf(log_message, sizeof(log_message), "ERROR: RECV operation, attempting re-post socket_context.");
                        log_to_file(log_message);
                        pool_free(pool_recv_iocp_context, socket_context);
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
                            snprintf(log_message, sizeof(log_message), "ERROR: Failed to allocate memory for received iocp frame entry.");
                            log_to_file(log_message);
                            break;
                        }
                        memset(recv_frame_entry, 0, sizeof(PoolEntryRecvFrame));
                        memcpy(&recv_frame_entry->frame, socket_context->buffer, NrOfBytesTransferred);
                        memcpy(&recv_frame_entry->src_addr, &socket_context->addr, sizeof(struct sockaddr_in));
                        recv_frame_entry->frame_size = NrOfBytesTransferred;
                        recv_frame_entry->timestamp = time(NULL);

                        uint8_t frame_type = recv_frame_entry->frame.header.frame_type;
                        BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                                        frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                                        frame_type == FRAME_TYPE_FILE_METADATA ||
                                                        frame_type == FRAME_TYPE_DISCONNECT);
    
                        if (is_high_priority_frame) {
                            if (push_ptr(queue_recv_prio_udp_frame, (uintptr_t)recv_frame_entry) == RET_VAL_ERROR) {
                                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Dropping priority recv frame - Failed to push to 'queue_recv_prio_udp_frame'.");
                                log_to_file(log_message);
                                pool_free(pool_recv_udp_frame, recv_frame_entry); // Free the entry if it fails to push
                            }
                        } else {
                            if (push_ptr(queue_recv_udp_frame, (uintptr_t)recv_frame_entry) == RET_VAL_ERROR) {
                                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Dropping recv frame - Failed to push to 'queue_recv_udp_frame'.");
                                log_to_file(log_message);
                                pool_free(pool_recv_udp_frame, recv_frame_entry); // Free the entry if it fails to push
                            }
                        }

                        // if (inet_ntop(AF_INET, &(socket_context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                        //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                        // }
                        // printf("Server: Received %lu bytes from %s:%d. Type: %u\n",
                        //        NrOfBytesTransferred, ip_string_buffer, ntohs(socket_context->addr.sin_port), frame_entry.frame.header.frame_type);

                    } else {
                        // 0 bytes transferred (e.g., graceful shutdown, empty packet)
                        snprintf(log_message, sizeof(log_message), "ERROR: Receive operation completed with 0 bytes for iocp socket_context. Re-posting.");
                        log_to_file(log_message);
                    }

                    // *** CRITICAL: Re-post the receive operation using the SAME socket_context ***
                    // This ensures the buffer is continuously available for incoming data.
                    if (udp_recv_from(client->socket, socket_context) == RET_VAL_ERROR){
                        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: WSARecvFrom re-issue failed for socket_context %p: errno: %d. Freeing.", (void*)socket_context, WSAGetLastError());
                        log_to_file(log_message);
                        // This is a severe problem. Retire the socket_context from the pool.
                        pool_free(pool_recv_iocp_context, socket_context); // Return to pool if it fails
                    }
                    if(pool_recv_iocp_context->free_blocks > (pool_recv_iocp_context->block_count / 2)){
                        refill_recv_iocp_pool(client->socket, pool_recv_iocp_context);
                    }
                    break; // End of OP_RECV case

                case OP_SEND:
                    // For send completions, simply free the socket_context
                    if (NrOfBytesTransferred > 0) {
                        // if (inet_ntop(AF_INET, &(socket_context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                        //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                        // }
                        // printf("Client: Sent %lu bytes to %s:%d (Message: '%s')\n",
                        //     NrOfBytesTransferred, ip_string_buffer, ntohs(iocp_context->addr.sin_port), iocp_context->buffer);
                    } else {
                        snprintf(log_message, sizeof(log_message), "ERROR: Send operation completed with 0 bytes or error.");
                        log_to_file(log_message);
                    }
                    pool_free(pool_send_iocp_context, socket_context);
                    break;

                default:
                    snprintf(log_message, sizeof(log_message), "ERROR: Unknown operation type in completion.");
                    log_to_file(log_message);
                    pool_free(pool_send_iocp_context, socket_context);
                    break;

            } // end of switch(socket_context->type)
        } // end of while(true)
    } // end of while(client.client_status == STATUS_READY)

    fprintf(stdout,"receive frame thread closed...\n");
    _endthreadex(0);    
    return 0;
}
// --- Processes a received frame ---
static DWORD WINAPI fthread_process_frame(LPVOID lpParam) {

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;
    
    uint32_t frame_bytes_received = 0;

    uint16_t recv_delimiter = 0;
    uint8_t  recv_frame_type = 0;
    uint64_t recv_seq_num = 0;
    uint32_t recv_session_id = 0;    

    uint32_t recv_session_timeout;
    uint8_t recv_server_status;

    char log_message[CLIENT_LOG_MESSAGE_LEN];

    HANDLE events[2] = {queue_recv_prio_udp_frame->push_semaphore, queue_recv_udp_frame->push_semaphore};

    PoolEntryRecvFrame recv_frame_entry;
    PoolEntryRecvFrame *entry_recv_frame = NULL;

    while(client->client_status == STATUS_READY){

        DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            entry_recv_frame = (PoolEntryRecvFrame*)pop_ptr(queue_recv_prio_udp_frame);
            if(!entry_recv_frame){
                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Poped empty pointer from 'queue_recv_prio_udp_frame'.");
                log_to_file(log_message);
                continue;
            }
            memcpy(&recv_frame_entry, entry_recv_frame, sizeof(PoolEntryRecvFrame));
            pool_free(pool_recv_udp_frame, (void*)entry_recv_frame);
        } else if (result == WAIT_OBJECT_0 + 1) {
            entry_recv_frame = (PoolEntryRecvFrame*)pop_ptr(queue_recv_udp_frame);
            if(!entry_recv_frame){
                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Poped empty pointer from 'queue_recv_udp_frame'.");
                log_to_file(log_message);
                continue;
            }
            memcpy(&recv_frame_entry, entry_recv_frame, sizeof(PoolEntryRecvFrame));
            pool_free(pool_recv_udp_frame, (void*)entry_recv_frame);
        } else {
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: WaitForMultipleObjects (queue_recv_udp_frame - semaphore) failed with code: %lu.", result);
            log_to_file(log_message);
            continue;
        }

        frame = &recv_frame_entry.frame;
        src_addr = &recv_frame_entry.src_addr;
        frame_bytes_received = recv_frame_entry.frame_size;

        // Extract header fields   
        recv_delimiter = _ntohs(frame->header.start_delimiter);
        recv_frame_type = frame->header.frame_type;
        recv_seq_num = _ntohll(frame->header.seq_num);
        recv_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (recv_delimiter != FRAME_DELIMITER) {
            snprintf(log_message, sizeof(log_message), "ERROR: Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.", src_ip, src_port, recv_delimiter);
            log_to_file(log_message);
            continue;
        }        
        if (!is_checksum_valid(frame, frame_bytes_received)) {
            snprintf(log_message, sizeof(log_message), "ERROR: Received frame from %s:%d with checksum mismatch. Discarding.", src_ip, src_port);
            log_to_file(log_message);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }
        switch (recv_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE:
                if(recv_seq_num != DEFAULT_CONNECT_REQUEST_SEQ){
                    snprintf(log_message, sizeof(log_message), "ERROR: Connect response seq num invalid. Connection not established...");
                    log_to_file(log_message);
                    fprintf(stderr, "%s\n", log_message);
                    break;
                }
                recv_server_status = frame->payload.connection_response.server_status;
                recv_session_timeout = _ntohl(frame->payload.connection_response.session_timeout);               
                if(recv_session_id == 0 || recv_server_status != STATUS_READY){
                    snprintf(log_message, sizeof(log_message), "ERROR: Session ID invalid or server not ready. Connection not established...");
                    log_to_file(log_message);
                    fprintf(stderr, "%s\n", log_message);
                    break;
                }
                if(recv_session_timeout <= MIN_CONNECTION_TIMEOUT_SEC){
                    snprintf(log_message, sizeof(log_message), "ERROR: Session timeout invalid. Connection not established...");
                    log_to_file(log_message);
                    fprintf(stderr, "%s\n", log_message);
                    break;
                }
                snprintf(log_message, sizeof(log_message), "DEBUG: Received connect response from %s:%d with session ID: %d, timeout: %d seconds, server status: %d.",
                                                        src_ip, src_port, recv_session_id, recv_session_timeout, recv_server_status);
                log_to_file(log_message);
                fprintf(stderr, "%s\n", log_message);
                client->server_status = recv_server_status;
                client->session_timeout = recv_session_timeout;
                client->sid = recv_session_id;
                snprintf(client->server_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, frame->payload.connection_response.server_name);
                client->last_active_time = time(NULL);
                               
                SetEvent(client->hevent_connection_established);
                break;

            case FRAME_TYPE_ACK:
                if(recv_session_id != client->sid){
                    //fprintf(stderr, "Received ACK frame with invalid session ID: %d\n", recv_session_id);
                    //TODO - send ACK frame with error code for invalid session ID
                    break;
                }
                client->last_active_time = time(NULL);
                uint8_t recv_op_code = frame->payload.ack.op_code;

                if(recv_seq_num == DEFAULT_DISCONNECT_REQUEST_SEQ && recv_op_code == STS_CONFIRM_DISCONNECT){
                    SetEvent(client->hevent_connection_closed);
                    snprintf(log_message, sizeof(log_message), "DEBUG: Received ack STS_CONFIRM_DISCONNECT - code: %lu; seq num: %llx.", recv_op_code, recv_seq_num);
                    log_to_file(log_message);
                    fprintf(stderr, "%s\n", log_message);
                    break;
                }

                for(int i = 0; i < CLIENT_MAX_ACTIVE_FSTREAMS; i++){
                    ClientFileStream *fstream = &client->fstream[i];
                    if(recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == STS_CONFIRM_FILE_METADATA) { 
                        fstream->pending_metadata_seq_num = 0; 
                        SetEvent(fstream->hevent_metadata_response_ok);
                    } else if (recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == ERR_CONFIRM_DUPLICATE_FILE_METADATA) {
                        fstream->pending_metadata_seq_num = 0;
                        SetEvent(fstream->hevent_metadata_response_ok);
                        snprintf(log_message, sizeof(log_message), "WARNING: Received ERR_CONFIRM_DUPLICATE_FILE_METADATA for file '%s%s'. Duplicate metadata frame.", fstream->dirpath, fstream->fname);
                        log_to_file(log_message);
                    } else if(recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == ERR_EXISTING_FILE ) {
                        fstream->pending_metadata_seq_num = 0;
                        SetEvent(fstream->hevent_metadata_response_nok);
                        snprintf(log_message, sizeof(log_message), "ERROR: Received ERR_EXISTING_FILE for file '%s%s'. Droping metadata frame.", fstream->dirpath, fstream->fname);
                        log_to_file(log_message);
                    } else if(recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == ERR_STREAM_INIT) {
                        fstream->pending_metadata_seq_num = 0;
                        SetEvent(fstream->hevent_metadata_response_nok);
                        snprintf(log_message, sizeof(log_message), "ERROR: Received ERR_STREAM_INIT for file '%s%s'. Droping metadata frame.", fstream->dirpath, fstream->fname);
                        log_to_file(log_message);
                    } else if (recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == ERR_INTERNAL_ERROR){
                        snprintf(log_message, sizeof(log_message), "ERROR: Received ERR_INTERNAL_ERROR for file '%s%s'.", fstream->dirpath, fstream->fname);
                        log_to_file(log_message);
                    } else if (recv_seq_num == fstream->pending_metadata_seq_num && recv_op_code == ERR_UNKNOWN_ERROR){
                        snprintf(log_message, sizeof(log_message), "ERROR: Received ERR_UNKNOWN_ERROR for file '%s%s'.", fstream->dirpath, fstream->fname);
                        log_to_file(log_message);
                    }
                }

                if(recv_op_code == STS_CONFIRM_MESSAGE_FRAGMENT ||
                        recv_op_code == STS_CONFIRM_FILE_METADATA ||
                        recv_op_code == ERR_CONFIRM_DUPLICATE_FILE_METADATA ||
                        recv_op_code == STS_CONFIRM_FILE_END ||
                        recv_op_code == ERR_DUPLICATE_FRAME || 
                        recv_op_code == ERR_EXISTING_FILE ||
                        recv_op_code == ERR_EXISTING_MESSAGE
                        
                    ){
                    
                    uintptr_t entry = remove_table_send_frame(table_send_udp_frame, recv_seq_num);
                    if(!entry){
                        snprintf(log_message, sizeof(log_message), "WARNING: fail to remove from send frame from hash table? - null pointer returned - most likely double acked frame.");
                        log_to_file(log_message);
                        break;
                    }
                    s_pool_free(pool_send_udp_frame, (void*)entry);
                }

                if(recv_seq_num == DEFAULT_KEEP_ALIVE_SEQ && recv_op_code == STS_KEEP_ALIVE){
                    // snprintf(log_message, sizeof(log_message), "DEBUG: Received ack STS_KEEP_ALIVE - code: %lu; seq num: %llx.", recv_op_code, recv_seq_num);
                    // log_to_file(log_message);
                    break;
                }
                break;

            case FRAME_TYPE_DISCONNECT:
                if(recv_session_id != client->sid){
                    break;                    
                }
                snprintf(log_message, sizeof(log_message), "Received disconnect request frame. Session closed by server...");
                log_to_file(log_message);
                fprintf(stderr, "%s\n", log_message);
                break;

            case FRAME_TYPE_CONNECT_REQUEST:
                break;
                
            case FRAME_TYPE_KEEP_ALIVE:
                break;
            
            case FRAME_TYPE_SACK:
                if(recv_session_id != client->sid){
                    snprintf(log_message, sizeof(log_message), "ERROR: Received SACK frame with invalid session ID: %d. Discarding.", recv_session_id);
                    log_to_file(log_message);
                    break;
                }
                // Process SACK frame logic here
                // snprintf(log_message, sizeof(log_message), "DEBUG: Received SACK frame from %s:%d with seq num: %llu, ack count: %u", src_ip, src_port, recv_seq_num, frame->payload.sack.ack_count);
                // log_to_file(log_message);
                int ack_count = frame->payload.sack.ack_count;
                if(ack_count > 0){
                    for(int i = 0; i < ack_count; i++){
                        uint64_t ack_seq_num = _ntohll(frame->payload.sack.seq_num[i]);
                        uintptr_t entry = remove_table_send_frame(table_send_udp_frame, ack_seq_num);
                        if(!entry){
                            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: fail to remove from send frame from hash table? - null pointer returned - most likely double acked frame.");
                            log_to_file(log_message);
                            continue;
                        }
                        s_pool_free(pool_send_udp_frame, (void*)entry);
                    }
                } else {
                    snprintf(log_message, sizeof(log_message), "ERROR: Received SACK frame with zero ACK count. Discarding.");
                    log_to_file(log_message);
                }
                break;

            default:
                snprintf(log_message, sizeof(log_message), "ERROR: Received frame with unknown type: %u from %s:%d. Discarding.", recv_frame_type, src_ip, src_port);
                log_to_file(log_message);
                break;
        }
    }
    _endthreadex(0);    
    return 0;
}
// --- Re-Send frames not acknowledges within set time ---
static DWORD WINAPI fthread_resend_frame(LPVOID lpParam){
   
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    while(client->client_status == STATUS_READY){ 
        // if(client.session_status == CONNECTION_CLOSED){
        //     ht_clean(&buffers.ht_frame);
        //     Sleep(250);
        //     continue;
        // }
        time_t current_time = time(NULL);
        if(table_send_udp_frame->count == 0){
            Sleep(100);
            continue;
        }
        for(int i = 0; i < CLIENT_MAX_ACTIVE_FSTREAMS; i++){
            AcquireSRWLockExclusive(&table_send_udp_frame->mutex);
            for (int i = 0; i < table_send_udp_frame->size; i++) {
                TableNodeSendFrame *table_node = table_send_udp_frame->node[i];
                while (table_node) {
                    PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)table_node->entry;
                    if(entry_send->frame.header.frame_type == FRAME_TYPE_FILE_METADATA && current_time - table_node->sent_time > (time_t)RESEND_FILE_METADATA_TIMEOUT_SEC){
                        send_pool_frame(entry_send, pool_send_iocp_context);
                        table_node->sent_time = current_time;
                    }
                    if(current_time - table_node->sent_time > (time_t)RESEND_TIMEOUT_SEC){
                        send_pool_frame(entry_send, pool_send_iocp_context);
                        table_node->sent_time = current_time;
                    }
                    table_node = table_node->next;
                }
                
            }
            ReleaseSRWLockExclusive(&table_send_udp_frame->mutex);
        }
        Sleep(100);
    }
    fprintf(stdout,"resend frame thread exiting...\n");
    _endthreadex(0);
    return 0;
}
// --- Send keep alive ---
static DWORD WINAPI fthread_keep_alive(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    time_t now_keep_alive = time(NULL);
    time_t last_keep_alive = time(NULL);

    char log_message[CLIENT_LOG_MESSAGE_LEN];
    
    while(client->client_status == STATUS_READY){
        if(client->session_status == CONNECTION_ESTABLISHED){
            DWORD keep_alive_clock_sec = (DWORD)(client->session_timeout / 5);

            now_keep_alive = time(NULL);
            if(now_keep_alive - last_keep_alive > keep_alive_clock_sec){
                PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
                if(!entry_send){
                    snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for keep alive frame. Should never do since it has semaphore to block when full.");
                    log_to_file(log_message);
                    Sleep(1000);
                    continue;
                }
                construct_keep_alive(entry_send, 
                                        client->sid, 
                                        client->socket, &client->server_addr
                                    );
                if(s_push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
                    snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Failed to push keep alive frame to queue_send_ctrl_udp_frame. Should never happen since queue is blocking on push/pop semaphores.");
                    log_to_file(log_message);
                    s_pool_free(pool_send_udp_frame, (void*)entry_send);
                    continue;
                }
                last_keep_alive = time(NULL);
            }
            if(time(NULL) > (time_t)(client->last_active_time + client->session_timeout * 2)){
                snprintf(log_message, sizeof(log_message), "ERROR: Server connection timeout.");
                log_to_file(log_message);
                fprintf(stderr, "%s\n", log_message);
                client->session_status = CONNECTION_CLOSED;
            }

            Sleep(1000);
        } else {
            now_keep_alive = time(NULL);
            last_keep_alive = time(NULL);
            Sleep(1000);
            continue;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Pop a frame from frame queue for processing ---
static DWORD WINAPI fthread_send_frame(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    char log_message[CLIENT_LOG_MESSAGE_LEN];

    while(client->client_status == STATUS_READY){
        PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pop_ptr(queue_send_udp_frame);
        if(!entry_send){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Poped empty pointer from 'queue_send_udp_frame'. Should not happen queue has push/pop semaphores");
            log_to_file(log_message);
            continue;
        }
        // Enter the critical section to acquire the right to send.
        // EnterCriticalSection(&client->send_lock);
        if(client->send_period_usec > 0){
            wait_usec(client->send_period_usec);
        }
        insert_table_send_frame(table_send_udp_frame, (uintptr_t)entry_send);
        send_pool_frame(entry_send, pool_send_iocp_context);
        // LeaveCriticalSection(&client->send_lock);
   }
    _endthreadex(0);    
    return 0;
}
// --- Pop a frame from priority queue for processing ---
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    char log_message[CLIENT_LOG_MESSAGE_LEN];

    while(client->client_status == STATUS_READY){
        
        PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pop_ptr(queue_send_prio_udp_frame);
        if(!entry_send){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Poped empty pointer from 'queue_send_prio_udp_frame'. Should not happen queue has push/pop semaphores");
            log_to_file(log_message);
        }
        insert_table_send_frame(table_send_udp_frame, (uintptr_t)entry_send);
        send_pool_frame(entry_send, pool_send_iocp_context);
   }
    _endthreadex(0);    
    return 0;
}
// --- Pop a frame from ctrl queue for processing ---
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    char log_message[CLIENT_LOG_MESSAGE_LEN];

    while(client->client_status == STATUS_READY){
        
        PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pop_ptr(queue_send_ctrl_udp_frame);
        if(!entry_send){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Poped empty pointer from 'queue_send_ctrl_udp_frame'. Should not happen queue has push/pop semaphores");
            log_to_file(log_message);
            continue;
        }
        send_pool_frame(entry_send, pool_send_iocp_context);
        s_pool_free(pool_send_udp_frame, (void*)entry_send);
    }
    _endthreadex(0);
    return 0;
}
// --- File transfer thread function ---
static DWORD WINAPI fthread_process_fstream(LPVOID lpParam){
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    SHA256_CTX sha256_ctx;
    uint32_t chunk_bytes_to_send;
    uint32_t chunk_fragment_offset;
    uint32_t frame_fragment_size;
    uint64_t frame_fragment_offset;

    PoolEntryCommand *entry = NULL;
    PoolEntrySendFrame *entry_send = NULL;
    int res;

    char log_message[CLIENT_LOG_MESSAGE_LEN];
 
    while(client->client_status == STATUS_READY){
        // this semaphore is released when a stream finished it's job
        WaitForSingleObject(client->fstreams_semaphore, INFINITE);
        
        entry = (PoolEntryCommand*)s_pop_ptr(queue_send_file_command);
        if(!entry){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: popped invalid pointer from command file queue\n");
            log_to_file(log_message);
            continue;
        }
         
        ClientFileStream *fstream = NULL;
        AcquireSRWLockExclusive(&client->fstreams_lock);
        for(int index = 0; index < CLIENT_MAX_ACTIVE_FSTREAMS; index++){
            fstream = &client->fstream[index];
            if(!fstream->fstream_busy){
                fstream->fstream_busy = TRUE;
                break;
            }
        }
        ReleaseSRWLockExclusive(&client->fstreams_lock);

        if(!fstream){
            snprintf(log_message, sizeof(log_message), "WARNING: All fstreams are busy.");
            log_to_file(log_message);
            continue;
        }
        
        AcquireSRWLockExclusive(&fstream->lock);
       
        // Safely copy paths using the lengths from the queue entry
        // fpath
        int result = snprintf(fstream->dirpath, MAX_PATH, "%.*s",
                                   (int)entry->command.send_file.dirpath_len,
                                   entry->command.send_file.dirpath);
        if (result < 0 || (size_t)result != entry->command.send_file.dirpath_len) {
            snprintf(log_message, sizeof(log_message), "ERROR: fthread_process_fstream - Failed to copy dirpath '%.*s' (truncation or error). Result: %d, Expected: %u.",
                                                    (int)entry->command.send_file.dirpath_len, entry->command.send_file.dirpath, result, entry->command.send_file.dirpath_len);
            log_to_file(log_message);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->fpath_len = entry->command.send_file.dirpath_len;
        // rpath
        result = snprintf(fstream->rpath, MAX_PATH, "%.*s",
                                   (int)entry->command.send_file.rpath_len,
                                   entry->command.send_file.rpath);
        if (result < 0 || (size_t)result != entry->command.send_file.rpath_len) {
            snprintf(log_message, sizeof(log_message), "ERROR: fthread_process_fstream - Failed to copy rpath '%.*s' (truncation or error). Result: %d, Expected: %u.",
                                                    (int)entry->command.send_file.rpath_len, entry->command.send_file.rpath, result, entry->command.send_file.rpath_len);
            log_to_file(log_message);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->rpath_len = entry->command.send_file.rpath_len;

        // fname
        result = snprintf(fstream->fname, MAX_PATH, "%.*s",
                                   (int)entry->command.send_file.fname_len,
                                   entry->command.send_file.fname);
        if (result < 0 || (size_t)result != entry->command.send_file.fname_len) {
            snprintf(log_message, sizeof(log_message), "ERROR: fthread_process_fstream - Failed to copy fname '%.*s' (truncation or error). Result: %d, Expected: %u.",
                                                    (int)entry->command.send_file.fname_len, entry->command.send_file.fname, result, entry->command.send_file.fname_len);
            log_to_file(log_message);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->fname_len = entry->command.send_file.fname_len;

        sha256_init(&sha256_ctx);       
        fstream->fp = NULL;
        
        char full_path[MAX_PATH] = {0};
        snprintf(full_path, MAX_PATH, "%s%s", fstream->dirpath, fstream->fname);

        fstream->fsize = get_file_size(full_path, &fstream->fp);
        if(fstream->fsize == RET_VAL_ERROR){
            goto clean;
        }
        fstream->pending_bytes = fstream->fsize;

        // fstream->fp = fopen(full_path, "rb");
        if(!fstream->fp){
            snprintf(log_message, sizeof(log_message), "ERROR: fthread_process_fstream - failed to open file %s", full_path);
            log_to_file(log_message);
            fprintf(stderr, "%s\n", log_message);
            goto clean;
        }

        fstream->fid = InterlockedIncrement(&client->fid_count);

        fstream->pending_metadata_seq_num = InterlockedIncrement64(&client->frame_count);
        
        entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
        if(!entry_send){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for metadata frame. Should never do since it has semaphore to block when full");
            log_to_file(log_message);
            goto clean;
        }
        res = construct_file_metadata(entry_send,
                                        fstream->pending_metadata_seq_num, 
                                        client->sid, 
                                        fstream->fid, 
                                        fstream->fsize,
                                        fstream->rpath,
                                        fstream->rpath_len, 
                                        fstream->fname, 
                                        fstream->fname_len,
                                        // FILE_FRAGMENT_SIZE,
                                        client->socket, &client->server_addr);
        if(res == RET_VAL_ERROR){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: construct_file_metadata() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling.");
            log_to_file(log_message);
            goto clean;
        }
        if(s_push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Failed to push file metadata frame to 'queue_send_prio_udp_frame'. Should never happen since queue is blocking on push/pop semaphores\n");
            log_to_file(log_message);
            s_pool_free(pool_send_udp_frame, (void*)entry_send);
            goto clean;
        }

        HANDLE events[2] = {fstream->hevent_metadata_response_ok, fstream->hevent_metadata_response_nok};

        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        
        if(wait_result == WAIT_OBJECT_0){
            // metadata response ok event
        } else if (wait_result == WAIT_OBJECT_0 + 1){
            // metadata response nok event 
            snprintf(log_message, sizeof(log_message), "ERROR: Send metadata frame - response nok from server.");
            log_to_file(log_message);
            goto clean;
        } else {
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Unexpected result for wait metadata frame event: %lu.", GetLastError());
            log_to_file(log_message);
            goto clean;
        }
        
        frame_fragment_offset = 0;

        while(fstream->pending_bytes > 0){

            chunk_bytes_to_send = fread(fstream->chunk_buffer, 1, CLIENT_FILE_BLOCK_SIZE, fstream->fp);
            if (chunk_bytes_to_send == 0 && ferror(fstream->fp)) {
                snprintf(log_message, sizeof(log_message), "ERROR: fthread_process_fstream - error reading from file %s.", full_path);
                log_to_file(log_message);
                goto clean;
            }           

            sha256_update(&sha256_ctx, (const uint8_t *)fstream->chunk_buffer, chunk_bytes_to_send);
 
            chunk_fragment_offset = 0;
            
            while (chunk_bytes_to_send > 0){

                if(chunk_bytes_to_send > FILE_FRAGMENT_SIZE){
                    frame_fragment_size = FILE_FRAGMENT_SIZE;
                } else {
                    frame_fragment_size = chunk_bytes_to_send;
                }
                
                // char buffer[FILE_FRAGMENT_SIZE];

                // const char *offset = fstream->chunk_buffer + chunk_fragment_offset;
                // memcpy(buffer, offset, frame_fragment_size);
                // if(frame_fragment_size < FILE_FRAGMENT_SIZE){
                //     memset(buffer + frame_fragment_size, 0, FILE_FRAGMENT_SIZE - frame_fragment_size);
                // }
                
                entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
                if(!entry_send){
                    snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for file fragment frame. Should never happen since queue is blocking on push/pop semaphores.");
                    log_to_file(log_message);
                    goto clean;
                }
                construct_file_fragment(entry_send, 
                                            InterlockedIncrement64(&client->frame_count),
                                            client->sid,
                                            fstream->fid,
                                            frame_fragment_offset,
                                            (fstream->chunk_buffer + chunk_fragment_offset), //buffer, 
                                            frame_fragment_size, 
                                            client->socket, &client->server_addr);

                if(s_push_ptr(queue_send_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
                    snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Failed to push file fragment frame to 'queue_send_udp_frame'. Should never happen since queue is blocking on push/pop semaphores.");
                    log_to_file(log_message);
                    s_pool_free(pool_send_udp_frame, (void*)entry_send);
                    goto clean;
                }
                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                fstream->pending_bytes -= frame_fragment_size;
            }     
        }                  

        sha256_final(&sha256_ctx, (uint8_t *)&fstream->calculated_sha256);

        // entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
        // if(!entry_send){
        //     snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for end frame. Should never happen since queue is blocking on push/pop semaphores.");
        //     log_to_file(log_message);
        //     goto clean;
        // }
        // construct_file_end(entry_send,
        //                     InterlockedIncrement64(&client->frame_count), 
        //                     client->sid, 
        //                     fstream->fid, 
        //                     fstream->fsize, 
        //                     (uint8_t *)&fstream->calculated_sha256,
        //                     client->socket, &client->server_addr);
        // if(s_push_ptr(queue_send_prio_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
        //     snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Failed to push file end frame to 'queue_send_prio_udp_frame'. Should never happen since queue is blocking on push/pop semaphores.");
        //     log_to_file(log_message);
        //     s_pool_free(pool_send_udp_frame, (void*)entry_send);
        //     goto clean;
        // }

    clean:
        s_pool_free(pool_send_command, (void*)entry);
        close_file_stream(fstream);
        ReleaseSRWLockExclusive(&fstream->lock);
        ReleaseSemaphore(client->fstreams_semaphore, 1, NULL);
    }
    _endthreadex(0);
    return 0;               
}
// --- Send message thread function ---
static DWORD WINAPI fthread_process_mstream(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    uint32_t frame_fragment_offset;
    uint32_t frame_fragment_len;

    PoolEntryCommand *entry = NULL;
    int res;

    char log_message[CLIENT_LOG_MESSAGE_LEN];

    while(client->client_status == STATUS_READY){
        // this semaphore is released when a stream finished it's job
        WaitForSingleObject(client->mstreams_semaphore, INFINITE);

        // s_pop_command(queue_send_message_command, &entry);

        entry = (PoolEntryCommand*)s_pop_ptr(queue_send_message_command);
        if(!entry){
            snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: popped invalid pointer from command message queue\n");
            log_to_file(log_message);
            continue;
        }

        if(!entry->command.send_message.message_buffer){
            snprintf(log_message, sizeof(log_message), "ERROR: Queue message buffer invalid pointer.");
            log_to_file(log_message);
            continue;
        }

        if(entry->command.send_message.message_len >= MAX_MESSAGE_SIZE_BYTES){
            snprintf(log_message, sizeof(log_message), "CRITTICAL ERROR: Message size is too big. Can't send.");
            log_to_file(log_message);
            fprintf(stderr, "%s\n", log_message);
            continue;
        }
        if(entry->command.send_message.message_len <= 0){
            snprintf(log_message, sizeof(log_message), "CRITTICAL ERROR: Message size not valid (should be greater than zero).");
            log_to_file(log_message);
            fprintf(stderr, "%s\n", log_message);
            continue;
        }
        
        ClientMessageStream *mstream = NULL;
        for(int index = 0; index < CLIENT_MAX_ACTIVE_MSTREAMS; index++){
            mstream = &client->mstream[index];
            if(!mstream->mstream_busy) {
                EnterCriticalSection(&mstream->lock);
                mstream->mstream_busy = TRUE;
                mstream->message_len = entry->command.send_message.message_len;
                memcpy(mstream->message_buffer, entry->command.send_message.message_buffer, mstream->message_len);
                mstream->message_buffer[mstream->message_len] = '\0';
                free(entry->command.send_message.message_buffer);
                break;
            }
        }

        if(!mstream){
            LeaveCriticalSection(&mstream->lock);
            snprintf(log_message, sizeof(log_message), "WARNING: All mstreams are busy!");
            log_to_file(log_message);
            fprintf(stderr, "%s\n", log_message);
            continue;
        }

        mstream->message_id = InterlockedIncrement(&client->mid_count);
        frame_fragment_offset = 0;

        mstream->remaining_bytes_to_send = mstream->message_len;

        while(mstream->remaining_bytes_to_send > 0){

            if(mstream->remaining_bytes_to_send > TEXT_FRAGMENT_SIZE){
                frame_fragment_len = TEXT_FRAGMENT_SIZE;
            } else {
                frame_fragment_len = mstream->remaining_bytes_to_send;
            }

            char buffer[TEXT_FRAGMENT_SIZE];

            const char *offset = mstream->message_buffer + frame_fragment_offset;
            memcpy(buffer, offset, frame_fragment_len);
            if(frame_fragment_len < TEXT_FRAGMENT_SIZE){
                buffer[frame_fragment_len] = '\0';
            }

            PoolEntrySendFrame *entry_send = s_pool_alloc(pool_send_udp_frame);
            if(!entry_send){
                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for text fragment. Should not happen since queue is blocking on push/pop semaphores.");
                log_to_file(log_message);
                goto clean;
            }

            res = construct_text_fragment(entry_send,
                                            InterlockedIncrement64(&client->frame_count), 
                                            client->sid, 
                                            mstream->message_id, 
                                            mstream->message_len, 
                                            frame_fragment_offset, 
                                            buffer, 
                                            frame_fragment_len,
                                            client->socket, &client->server_addr);

            if(s_push_ptr(queue_send_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
                snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: Failed to push message frame to 'queue_send_udp_frame'. Should not happen since queue is blocking on push/pop semaphores.");
                log_to_file(log_message);
                goto clean;
            }

            frame_fragment_offset += frame_fragment_len;                       
            mstream->remaining_bytes_to_send -= frame_fragment_len;
        }

    clean:
        s_pool_free(pool_send_command, (void*)entry);
        close_message_stream(mstream);
        ReleaseSemaphore(client->mstreams_semaphore, 1, NULL);
        LeaveCriticalSection(&mstream->lock);

    }
    _endthreadex(0);
    return 0; 
}
// --- Process command ---
static DWORD WINAPI fthread_client_command(LPVOID lpParam) {

    char cmd;
    int index;
    int retry_count;
    char _path[MAX_PATH] = {0};

    char nr;
    char client_root_folder[MAX_PATH] = {0};
    
       fprintf(stdout, "Input the client number: ");
        nr = getchar();
        if(nr == '1'){
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_1_ROOT_FOLDER);
        } else if(nr == '2'){
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_2_ROOT_FOLDER);
        } else if(nr == '3'){
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_3_ROOT_FOLDER);
        } else if(nr == '4'){
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_4_ROOT_FOLDER);
        } else if(nr == '5'){
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_5_ROOT_FOLDER);
        } else {
            fprintf(stdout, "Defaulted to 'client_folder'\n");
            snprintf(client_root_folder, MAX_PATH, "%s", CLIENT_ROOT_FOLDER);
        }

    while(Client.client_status == STATUS_READY){
        fprintf(stdout,"Waiting for command...\n");

        cmd = getchar();
        switch(cmd) {
            //--------------------------------------------------------------------------------------------------------------------------
            case 'c':
            case 'C': // connect
                RequestConnect(server_ip);
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'd':
            case 'D': // disconnect
                RequestDisconnect();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'q':
            case 'Q': // shutdown
                // client_shutdown();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'h':
            case 'H':
                if(Client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s%s", client_root_folder, "test_file.txt");
                SendSingleFile(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'f':
            case 'F':
                if(Client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", client_root_folder);
                SendAllFilesInFolder(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'g':
            case 'G':
                if(Client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                // snprintf(_path, MAX_PATH, "%s", CLIENT_ROOT_FOLDER);
                snprintf(_path, MAX_PATH, "%s", client_root_folder);
                SendAllFilesInFolderAndSubfolders(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 't':
            case 'T':
                if(Client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s%s", CLIENT_ROOT_FOLDER, "test_file.txt");
                SendTextInFile(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case '\n':
                break;
            default:
                fprintf(stdout, "Invalid command!\n");
                break;
            
        }
    Sleep(100);
    }
    fprintf(stdout, "client command thread exiting...\n");
    _endthreadex(0);    
    return 0;
}
// --- Thread function to write error logs to a file ---
static DWORD WINAPI fthread_error_log_write(LPVOID lpParam){

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    while(client->client_status == STATUS_READY){
        WaitForSingleObject(queue_error_log->push_semaphore, INFINITE);
        PoolErrorLogEntry *entry_error_log = (PoolErrorLogEntry*)pop_ptr(queue_error_log);
        if(!entry_error_log){
            continue;
        }

        FILETIME ft;
        // Convert FILETIME to a SYSTEMTIME structure for year, month, etc.
        ft.dwHighDateTime = (unsigned long)(entry_error_log->timestamp_64bit >> 32);
        ft.dwLowDateTime = (unsigned long)entry_error_log->timestamp_64bit;

        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);

        char timestamp[64];
        snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        // Write to the log file
        FILE* log_file = fopen(CLIENT_ERROR_LOG_FILE, "a");

        if (log_file != NULL) {
            fprintf(log_file, "[%s] - %s\n", timestamp, entry_error_log->log_message);
            fclose(log_file);
        }
        else {
            fprintf(stderr, "ERROR: Could not open error log file for writing: %s\n", CLIENT_ERROR_LOG_FILE);
        }
        pool_free(pool_error_log, (void*)entry_error_log);
    }
    _endthreadex(0);
    return 0;
}



double get_cpu_time_ms(HANDLE thread) {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
        return -1.0;
    }

    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;

    return (k.QuadPart + u.QuadPart) / 10000.0; // Convert to milliseconds
}

double measure_thread_cpu_usage(HANDLE thread, DWORD interval_ms) {
    double start_cpu = get_cpu_time_ms(thread);
    DWORD start_time = GetTickCount();

    Sleep(interval_ms); // Wait for interval

    double end_cpu = get_cpu_time_ms(thread);
    DWORD end_time = GetTickCount();

    if (start_cpu < 0 || end_cpu < 0) return -1.0;

    double cpu_delta = end_cpu - start_cpu;
    double time_delta = (double)(end_time - start_time);

    return (cpu_delta / time_delta) * 100.0; // CPU usage percentage
}






// --- Main function ---
int main() {
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    init_client_session();
    init_client_config();
    init_client_buffers();
    start_threads();
    init_statistics_gui();

    while(Client.client_status == STATUS_READY){


        // fprintf(stdout, "\r\033[2K-- File: %.2f, CmdPending: %d; FreeRecv: %llu; FreeSend: %llu; TXQueue: %llu; HT_TX: %llu; PoolF: %llu", 
        //                     (float)(Client.fstream[0].fsize - Client.fstream[0].pending_bytes) / (float)Client.fstream[0].fsize * 100.0, 
        //                     Buffers.queue_fstream.pending,
        //                     Buffers.pool_iocp_recv_context.free_blocks,
        //                     Buffers.pool_iocp_send_context.free_blocks,
        //                     Buffers.queue_send_frame.pending,
        //                     Buffers.table_send_frame.count,
        //                     Buffers.pool_send_frame.free_blocks
        //                     );
        // fprintf(stdout, "\r\033[2K-- recv_send_frame: %.2f, process_frame: %.2f, resent_frame: %.2f",                    
        //                         measure_thread_cpu_usage(threads->send_frame[0], 1000),
        //                         measure_thread_cpu_usage(threads->send_ctrl_frame, 1000),
        //                         measure_thread_cpu_usage(threads->send_prio_frame, 1000)
        //                         );
        fflush(stdout);
        Sleep(250); // Simulate some delay between messages        
    }

    return 0;

}

