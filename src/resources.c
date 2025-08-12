#include <stdio.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#include "include/resources.h"
#include "include/protocol_frames.h"
#include "include/checksum.h"
#include "include/netendians.h"
#include "include/mem_pool.h"


void init_iocp_context(IOCP_CONTEXT *iocp_context, OPERATION_TYPE type) {
    if (!iocp_context) 
        return;
    memset(iocp_context, 0, sizeof(IOCP_CONTEXT));
    iocp_context->overlapped.hEvent = NULL; // Not using event handles for IOCP completions
    iocp_context->wsaBuf.buf = iocp_context->buffer;
    iocp_context->wsaBuf.len = sizeof(UdpFrame);
    iocp_context->addr_len = sizeof(struct sockaddr_in);
    iocp_context->type = type;
    return;
}

int udp_recv_from(const SOCKET src_socket, IOCP_CONTEXT *iocp_context){

    if (!iocp_context) {
        return RET_VAL_ERROR;
    }

    init_iocp_context(iocp_context, OP_RECV);

    DWORD bytes_recv = 0;
    DWORD flags = 0;

    int recvfrom_result = WSARecvFrom(
        src_socket,
        &iocp_context->wsaBuf,
        1,
        &bytes_recv,
        &flags,
        (SOCKADDR*)&iocp_context->addr,
        &iocp_context->addr_len,
        &iocp_context->overlapped,
        NULL
    );

    if (recvfrom_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        int error_code = WSAGetLastError();
        // fprintf(stderr, "WSARecvFrom failed with error: %d\n", error_code);
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}

int udp_send_to(const char *data, size_t data_len, const SOCKET src_socket, const struct sockaddr_in *dest_addr, MemPool *mem_pool) {
    // Allocate a new iocp_context for each send operation
    IOCP_CONTEXT *iocp_context = (IOCP_CONTEXT*)pool_alloc(mem_pool);
    if (iocp_context == NULL) {
        fprintf(stderr, "Failed to allocate IOCP_CONTEXT for send.\n");
        return RET_VAL_ERROR;
    }
    init_iocp_context(iocp_context, OP_SEND); // Initialize as a send iocp_context

    // Copy data to the iocp_context's buffer
    if (data_len > sizeof(UdpFrame)) {
        fprintf(stderr, "Send data larger than sizeof(UdpFrame).\n");
        pool_free(mem_pool, (void*)iocp_context);
        return RET_VAL_ERROR;
    }
    memcpy(iocp_context->buffer, data, data_len);
    iocp_context->wsaBuf.len = (ULONG)data_len; // Set actual data length for send

    // Set destination address
    memcpy(&iocp_context->addr, dest_addr, sizeof(struct sockaddr_in));
    iocp_context->addr_len = sizeof(struct sockaddr_in);

    DWORD bytes_sent = 0;
    int result = WSASendTo(
        src_socket,
        &iocp_context->wsaBuf,
        1,
        &bytes_sent,
        0, // Flags
        (SOCKADDR*)&iocp_context->addr,
        iocp_context->addr_len,
        &iocp_context->overlapped,
        NULL
    );

    if (result == SOCKET_ERROR){
        int error = WSAGetLastError();
        if(error != WSA_IO_PENDING) {
            fprintf(stderr, "WSASendTo %d", WSAGetLastError());
            pool_free(mem_pool, iocp_context); // Free iocp_context immediately if not pending
            return RET_VAL_ERROR;
        } else {
            // pending
        }
    } else {
        // operation succeded
    }
    return (int)bytes_sent;
}

void refill_recv_iocp_pool(const SOCKET src_socket, MemPool *mem_pool){
    uint64_t mem_pool_free_blocks = mem_pool->free_blocks;
    for(int i = 0; i < mem_pool_free_blocks; i++){
        IOCP_CONTEXT* recv_context = (IOCP_CONTEXT*)pool_alloc(mem_pool);
        if (recv_context == NULL) {
            fprintf(stderr, "Failed to allocate receive context from mem_pool %d. Exiting.\n", i);
            continue;
        }
        init_iocp_context(recv_context, OP_RECV);
        if (udp_recv_from(src_socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "Failed to re-post receive operation %d. Exiting.\n", i);
            pool_free(mem_pool, recv_context);
            continue;
        }
    }
    return;
}

int send_pool_frame(PoolEntrySendFrame *pool_entry, MemPool *mem_pool){
    
    UdpFrame *frame = &pool_entry->frame;
    SOCKET src_socket = pool_entry->src_socket;
    struct sockaddr_in *dest_addr = &pool_entry->dest_addr;
    
    size_t frame_size = 0;
    switch (frame->header.frame_type) {
        case FRAME_TYPE_FILE_METADATA:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_METADATA_RESPONSE:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataResponsePayload);
            break;
        case FRAME_TYPE_FILE_FRAGMENT:
            frame_size = sizeof(FrameHeader) + sizeof(FileFragmentPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_END:
            frame_size = sizeof(FrameHeader) + sizeof(FileEndPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_COMPLETE:
            frame_size = sizeof(FrameHeader) + sizeof(FileCompletePayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_TEXT_MESSAGE:
            frame_size = sizeof(FrameHeader) + sizeof(TextPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_ACK:
            frame_size = sizeof(FrameHeader) + sizeof(AckPayload); // Acknowledgment frame
            break;
        case FRAME_TYPE_SACK:
            frame_size = sizeof(FrameHeader) + sizeof(SAckPayload); // Selective Acknowledgment frame
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectRequestPayload);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectResponsePayload);
            break;
        case FRAME_TYPE_DISCONNECT:
            frame_size = sizeof(FrameHeader);
            break;
        case FRAME_TYPE_KEEP_ALIVE:
            frame_size = sizeof(FrameHeader);
            break;
        default:
            frame_size = sizeof(UdpFrame); // Fallback to max size
            break;
    }
    return udp_send_to((const char*)frame, frame_size, src_socket, dest_addr, mem_pool);
}

// int send_pool_ack_frame(PoolEntryAckFrame *pool_ack_entry, MemPool *mem_pool){
    
//     AckUdpFrame *frame = &pool_ack_entry->frame;
//     SOCKET src_socket = pool_ack_entry->src_socket;
//     struct sockaddr_in *dest_addr = &pool_ack_entry->dest_addr;
    
//     return udp_send_to((const char*)frame, sizeof(AckUdpFrame), src_socket, dest_addr, mem_pool);

// }




void wait_usec(const long long time_usec){

    FILETIME time;
    uint64_t current_time;
    uint64_t timestamp;

    if(time_usec <= 0) return;

    GetSystemTimePreciseAsFileTime(&time);
    timestamp = (uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime;
    // current_time = timestamp;

    do{
        GetSystemTimePreciseAsFileTime(&time);
        current_time = (uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime;
    } while ((current_time - timestamp) < time_usec * 10);

    return;

}

uint64_t calculate_period_usec(const double speed_mbps, const int bytes_per_packet) {
    if (speed_mbps <= 0.0) {
        return 0; // Avoid division by zero
    }
    if(bytes_per_packet <= 0){
        return 0;
    }
    // Calculate the total number of bits per packet.
    // 1 byte = 8 bits
    double bits_per_packet = (double)bytes_per_packet * 8.0;

    // Calculate the speed in bits per second.
    // 1 Megabit = 1000000 bits
    double speed_bps = speed_mbps * 1000000.0;
    
    // Calculate the time to send one packet in seconds.
    // Time = Size / Speed
    double time_per_packet_sec = bits_per_packet / speed_bps;
    
    // Convert the time to microseconds.
    // 1 second = 1,000,000 microseconds
    uint64_t time_per_packet_usec = (uint64_t)(time_per_packet_sec * 1000000.0);
    
    return time_per_packet_usec;
}
