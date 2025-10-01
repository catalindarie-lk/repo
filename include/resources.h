#ifndef RESOURCES_H
#define RESOURCES_H

#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions
#include "include/mem_pool.h"
#include "include/queue.h"
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

// __declspec(align(64))typedef struct{
//     union{
//         PoolEntrySendFrame send;
//         PoolEntryRecvFrame recv;
//     } entry;
// }FRAME_CONTEXT;
 
// Enumeration for operation type within IOCP_CONTEXT

__declspec(align(64))typedef struct {
    OVERLAPPED overlapped; // Must be the first member for easy casting
    WSABUF wsaBuf;
    CHAR buffer[sizeof(UdpFrame)];
    struct sockaddr_in addr;  // Source/Destination address
    int addr_len;
    uint8_t type;      // To distinguish between send and receive operations
} SocketContext;





typedef struct {
    uint32_t fid;
    uint32_t sid;
    size_t block_offset;
    size_t block_size;
    uint8_t block_data[SERVER_FILE_BLOCK_SIZE];
    uint8_t op_type;
} ServerFileBlock;

typedef struct{
    struct sockaddr_in client_addr;

    uint32_t sid;                       // Session ID associated with this file stream.
    uint32_t fid;                       // File ID, unique identifier for the file associated with this file stream.
    uint64_t file_size;                     // Total size of the file being transferred.

    uint64_t *received_file_bitmap;     // Pointer to an array of uint64_t, where each bit represents a file fragment.
    uint64_t *written_file_bitmap;      // Pointer to an array of uint64_t, where each bit represents a file fragment.
                                        // A bit set to 1 means the fragment has been received.
    uint64_t file_bitmap_size;          // Number of uint64_t entries in the bitmap array.

    ServerFileBlock **file_block;
    uint64_t *recv_block_bytes;         // Total bytes received for this file so far.
    uint64_t *recv_block_status;

    uint64_t fragment_count;            // Total number of fragments in the entire file.
    uint64_t block_count;

    uint64_t written_bytes_count;       // Total bytes written to disk for this file so far.

    uint8_t received_sha256[32];        // Buffer for sha256 received from the client
    BOOL end_of_file;                   // Flag indicating if the end of the file has been reached

    uint8_t calculated_sha256[32];      // Buffer for sha256 calculated by the server

    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];               // Array to store the file name+path.
    uint32_t fname_len;

    char ansi_path[MAX_PATH];
    wchar_t unicode_path[MAX_PATH];
    char temp_ansi_path[MAX_PATH];
    wchar_t temp_unicode_path[MAX_PATH];

    HANDLE iocp_file_handle;

    SRWLOCK lock;                       // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.

}ServerFileStream;



typedef struct {
    SAckPayload payload; // SACK payload for this client
    uint32_t ack_pending;
    uint64_t start_timestamp;
    BOOL start_recorded;
    SRWLOCK lock;
}ClientSAckBuffer;

typedef struct {
    struct sockaddr_in client_addr;            // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;

    uint32_t cid;                 // Unique ID received from the client
    uint32_t sid;                // Unique ID assigned by the server for this clients's session
    char name[MAX_NAME_SIZE];               // Optional: human-readable identifier received from the client
    uint8_t flags;                       // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;

    volatile time_t last_activity_time; // Last time the client sent a frame (for timeout checks)             

    QueueSeq queue_ack_seq;
    ClientSAckBuffer sack_buff; // SACK context for this client

    SRWLOCK lock;
}ServerClient;

typedef enum {
    IO_FSTREAM_INIT = 1,
    IO_FSTREAM_CLOSE = 3,
    IO_FILE_BLOCK_FLUSH = 5,
    IO_FILE_END_FRAME = 11,

    IO_DATA_FRAME = 201
    
} IoType;

typedef struct {
    OVERLAPPED overlapped;
    IoType io_type;
} IocpOperation;




// __declspec(align(64))typedef struct NodeTableFileBlock{
//     OVERLAPPED overlapped;
//     uint64_t key;
//     uint32_t fid;
//     uint32_t sid;
//     size_t block_size;
//     char* block_data;
//     uint8_t op_type;
//     struct NodeTableFileBlock *next;
// }NodeTableFileBlock;

// typedef enum {
//     COMPLETION_STREAM,
//     COMPLETION_SHUTDOWN,
//     COMPLETION_TRANSFER
// } CompletionType;

// typedef struct {
//     CompletionType type;
// } CompletionPacket;

// typedef struct {
//     CompletionPacket packet;
//     NodeTableFileBlock *block_ctx;
// } FileBlockPacket;

// typedef struct {
//     CompletionPacket packet;
//     PoolEntryRecvFrame *frame_ctx;
// } FramePacket;

// typedef struct {
//     CompletionPacket packet;
    
// } ErrorPacket;

//--------------------------------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------------------------------
void init_socket_context(SocketContext *socket_context, uint8_t type);
int udp_recv_from(const SOCKET src_socket, SocketContext *socket_context);
int udp_send_to(const char *data, size_t data_len, const SOCKET src_socket, const struct sockaddr_in *dest_addr, MemPool *mem_pool);
void refill_recv_iocp_pool(const SOCKET src_socket, MemPool *mem_pool);
int send_pool_frame(PoolEntrySendFrame *entry, MemPool *mem_pool);
void wait_usec(const long long time_usec);
uint64_t calculate_period_usec(const double speed_mbps, const int bytes_per_packet);


#endif