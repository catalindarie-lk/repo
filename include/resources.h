#ifndef RESOURCES_H
#define RESOURCES_H

#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions
#include "include/mem_pool.h"
#include "include/protocol_frames.h"

//--------------------------------------------------------------------------------------------------------------------------
#define MAX_ENTRY_SIZE (MAX_PATH + MAX_PATH + 32)
#pragma pack(push, 1)
typedef struct{
    char text[32];
    char dirpath[MAX_PATH];
    uint32_t dirpath_len;
    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];
    uint32_t fname_len;
}CommandSendFile;

typedef struct{
    char text[32];
    char *message_buffer;
    uint32_t message_len;
}CommandSendMessage;
 
__declspec(align(64))typedef struct{
    union{
        CommandSendFile send_file;
        CommandSendMessage send_message;
        uint8_t max_bytes[MAX_ENTRY_SIZE];
    } command;
}PoolEntryCommand;
#pragma pack(pop)

__declspec(align(64))typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    SOCKET src_socket;
    struct sockaddr_in dest_addr; // Destination address for the frame
}PoolEntrySendFrame;

__declspec(align(64))typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t frame_size; // Size of the frame in bytes
    uint64_t timestamp; // Timestamp when the frame was received
}PoolEntryRecvFrame;

__declspec(align(64))typedef struct{
    union{
        PoolEntrySendFrame send;
        PoolEntryRecvFrame recv;
    } entry;
}FRAME_CONTEXT;
 
// Enumeration for operation type within IOCP_CONTEXT

__declspec(align(64))typedef struct {
    OVERLAPPED overlapped; // Must be the first member for easy casting
    WSABUF wsaBuf;
    CHAR buffer[sizeof(UdpFrame)];
    struct sockaddr_in addr;  // Source/Destination address
    int addr_len;
    uint8_t type;      // To distinguish between send and receive operations
} IOCP_CONTEXT;
//--------------------------------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------------------------------
void init_iocp_context(IOCP_CONTEXT *iocp_context, uint8_t type);
int udp_recv_from(const SOCKET src_socket, IOCP_CONTEXT *iocp_context);
int udp_send_to(const char *data, size_t data_len, const SOCKET src_socket, const struct sockaddr_in *dest_addr, MemPool *mem_pool);

void refill_recv_iocp_pool(const SOCKET src_socket, MemPool *mem_pool);

int send_pool_frame(PoolEntrySendFrame *entry, MemPool *mem_pool);

void wait_usec(const long long time_usec);

uint64_t calculate_period_usec(const double speed_mbps, const int bytes_per_packet);


#endif