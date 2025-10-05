#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server_frames.h"
#include "include/resources.h"
#include "include/queue.h"
#include "include/mem_pool.h"
#include "include/hash.h"
#include "include/sha256.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS                             0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR                               -1
#endif

////////////////////////////////////////////////////

#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC                 30
#endif

// --- Constants 
#define SERVER_NAME                                 "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                                 10
#define SACK_READY_FRAME_TIMEOUT_MS                 10

#define SERVER_PARTITION_DRIVE                      "H:\\"
#define SERVER_ROOT_FOLDER                          "H:\\_test\\server_root\\"
#define SERVER_MESSAGE_TEXT_FILES_FOLDER            "H:\\_test\\messages_root\\"
#define SERVER_SID_FOLDER_NAME_FOR_CLIENT           "SID_"


//---------------------------------------------------------------------------------------------------------
// --- Server Stream Configuration ---
#define MAX_SERVER_ACTIVE_FSTREAMS                  (10) // 5 file streams per client + 5 extra for safety
#define MAX_SERVER_ACTIVE_MSTREAMS                  10

// --- Server Worker Thread Configuration ---
#define SERVER_MAX_THREADS_RECV_SEND_FRAME          1
#define SERVER_MAX_THREADS_PROCESS_FSTREAM          1

#define SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES    1
#define SERVER_MAX_THREADS_SEND_MESSAGE_ACK_FRAMES  1

//---------------------------------------------------------------------------------------------------------
// --- Server SEND Buffer Sizes ---
#define SERVER_QUEUE_SIZE_SEND_FRAME                (4096 + 256 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_SEND_PRIO_FRAME           1024
#define SERVER_QUEUE_SIZE_SEND_CTRL_FRAME           128

#define SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ            (SERVER_QUEUE_SIZE_SEND_FRAME)
#define SERVER_QUEUE_SIZE_CLIENT_PTR                (SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ * MAX_SERVER_ACTIVE_FSTREAMS)
// --- Server SEND Memory Pool Sizes ---
#define SERVER_POOL_SIZE_SEND                       (SERVER_QUEUE_SIZE_SEND_FRAME + \
                                                    SERVER_QUEUE_SIZE_SEND_PRIO_FRAME + \
                                                    SERVER_QUEUE_SIZE_SEND_CTRL_FRAME)
#define SERVER_POOL_SIZE_IOCP_SEND                  (SERVER_POOL_SIZE_SEND * 2) // Total size for IOCP send contexts
// --- SERVER RECV Buffer Sizes ---
#define SERVER_QUEUE_SIZE_RECV_FRAME                (4096 + 256 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_RECV_PRIO_FRAME           128
// --- Server RECV Memory Pool Sizes ---
#define SERVER_POOL_SIZE_RECV                       (SERVER_QUEUE_SIZE_RECV_FRAME + \
                                                    SERVER_QUEUE_SIZE_RECV_PRIO_FRAME)
#define SERVER_POOL_SIZE_IOCP_RECV                  (SERVER_POOL_SIZE_RECV * 2)
#define SERVER_POOL_SIZE_FILE_BLOCK                 (512) // Number of file blocks in the pool

// --- Macro to Parse Global Data to Threads ---
// This macro simplifies passing pointers to global client data structures into thread functions.
// It creates local pointers within the thread function's scope, pointing to the global instances.
#define PARSE_SERVER_GLOBAL_DATA(server_obj, buffers_obj, threads_obj) \
    ServerData *server = &(server_obj); \
    ServerBuffers *buffers = &(buffers_obj); \
    ServerThreads *threads = &(threads_obj); \
    MemPool *_pool_file_block = &((buffers_obj)._pool_file_block); \
    MemPool *pool_iocp_send_context = &((buffers_obj).pool_iocp_send_context); \
    MemPool *pool_iocp_recv_context = &((buffers_obj).pool_iocp_recv_context); \
    TableIDs *table_file_id = &((buffers_obj).table_file_id); \
    TableIDs *table_message_id = &((buffers_obj).table_message_id); \
    MemPool *pool_send_udp_frame = &((buffers_obj).pool_send_udp_frame); \
    QueuePtr *queue_send_udp_frame = &((buffers_obj).queue_send_udp_frame); \
    MemPool *pool_send_prio_udp_frame = &((buffers_obj).pool_send_prio_udp_frame); \
    QueuePtr *queue_send_prio_udp_frame = &((buffers_obj).queue_send_prio_udp_frame); \
    MemPool *pool_send_ctrl_udp_frame = &((buffers_obj).pool_send_ctrl_udp_frame); \
    QueuePtr *queue_send_ctrl_udp_frame = &((buffers_obj).queue_send_ctrl_udp_frame); \
    QueuePtr *queue_client_ptr = &((buffers_obj).queue_client_ptr); \
    ServerFstreamPool *pool_fstreams = &((server_obj).pool_fstreams); \
    ServerMstreamPool *pool_mstreams = &((server_obj).pool_mstreams); \
    ServerClientPool *pool_clients = &((server_obj).pool_clients); \
    ServerStreamProcessingUnit *sspu = &((server_obj).sspu); \
// end of #define PARSE_GLOBAL_DATA // End marker for the macro definition
// MemPool *pool_file_block = &((buffers_obj).pool_file_block); 
enum Status{
    STATUS_NONE = 0,
    STATUS_BUSY = 1,
    STATUS_READY = 2,
    STATUS_ERROR = 3
};

enum ClientConnection {
    CLIENT_DISCONNECTED = 0,
    CLIENT_CONNECTED = 1
};

enum FileBlockStatus{
    BLOCK_STATUS_NONE = 0,
    BLOCK_STATUS_RECEIVEING = 1,
    BLOCK_STATUS_RECEIVED = 2,
    BLOCK_STATUS_WRITTEN = 3,
};

typedef struct{
    FILETIME ft;
    ULARGE_INTEGER prev_uli;
    unsigned long long prev_microseconds;
    ULARGE_INTEGER crt_uli;
    unsigned long long crt_microseconds;
    float file_transfer_speed;
    float file_transfer_progress;

    float crt_bytes_received;
    float prev_bytes_received;
}Statistics;

typedef struct {
    BOOL busy;                          // Indicates if this message stream is currently in use.
    uint8_t stream_err;                 // Stores an error code if something goes wrong with the stream.
    char *buffer;                       // Pointer to the buffer holding the message data.
    uint64_t *bitmap;                   // Pointer to a bitmap used for tracking received fragments. Each bit represents a fragment.
    uint32_t sid;                       // Session ID: Unique identifier for the sender of the message.
    uint32_t mid;                       // Message ID: Unique identifier for the message itself.
    uint32_t mlen;                      // Message Length: Total expected length of the complete message in bytes.
    uint32_t chars_received;            // Characters Received: Current number of characters (bytes) received so far for this message.
    uint64_t fragment_count;            // Fragment Count: Total number of expected fragments for the complete message.
    uint64_t bitmap_entries_count;      // Bitmap Entries Count: The number of uint64_t entries in the bitmap array.

    char fname[MAX_NAME_SIZE];      // File Name: Buffer to store the name of the file associated with this message stream.

    SRWLOCK lock;              // Lock: Spinlock/Mutex to protect access to this MsgStream structure in multithreaded environments.
}ServerMessageStream;

typedef struct {
    ServerFileStream *fstream;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
}ServerFstreamPool;

typedef struct {
    ServerMessageStream *mstream;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
}ServerMstreamPool;

typedef struct {
    ServerClient *client;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
}ServerClientPool;

typedef struct {
    // HANDLE thread_process_frame[SERVER_MAX_THREADS_PROCESS_FRAME];
    
    s_MemPool pool_iocp_operation;

    HANDLE thread_process_ctrl_frame;
    HANDLE thread_process_data_frame[SERVER_MAX_THREADS_PROCESS_FSTREAM];
    
    MemPool pool_ctrl_frame;
    QueuePtr queue_ctrl_frame;
    
    MemPool pool_data_frame;
    QueuePtr queue_data_frame;

    HANDLE thread_process_fstream[SERVER_MAX_THREADS_PROCESS_FSTREAM];
    size_t process_fstream_uid[SERVER_MAX_THREADS_PROCESS_FSTREAM];

} ServerStreamProcessingUnit;


typedef struct {
    SOCKET socket;
    struct sockaddr_in server_addr;            // Server address structure
    uint8_t server_status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    volatile uint32_t session_id_counter;   // Global counter for unique session IDs
    volatile uint64_t file_block_count;   // Global counter for unique file block IDs
    char name[MAX_NAME_SIZE];               // Human-readable server name
    
    HANDLE iocp_socket_handle;
    HANDLE iocp_process_fstream_handle;

    ServerFstreamPool pool_fstreams;
    ServerMstreamPool pool_mstreams;
    ServerClientPool pool_clients;

    ServerStreamProcessingUnit sspu;

    HANDLE iocp_ctrl_frame_handle;
    // HANDLE iocp_data_frame_handle[SERVER_MAX_THREADS_PROCESS_FRAME];

}ServerData;

typedef struct {
    // MemPool pool_file_block;
    MemPool _pool_file_block;
    MemPool pool_iocp_send_context;
    MemPool pool_iocp_recv_context;


    MemPool pool_send_udp_frame;
    QueuePtr queue_send_udp_frame;          // For SACK frames

    MemPool pool_send_prio_udp_frame;
    QueuePtr queue_send_prio_udp_frame;     // For ACK frames
    
    MemPool pool_send_ctrl_udp_frame;
    QueuePtr queue_send_ctrl_udp_frame;     // For Request/Keep alive frames

    QueuePtr queue_client_ptr;

    TableIDs table_file_id;
    TableIDs table_message_id;

}ServerBuffers;

typedef struct {
    HANDLE recv_send_frame[SERVER_MAX_THREADS_RECV_SEND_FRAME];
 
    // HANDLE process_frame[SERVER_MAX_THREADS_PROCESS_FRAME];

    HANDLE send_sack_frame[SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES];
    HANDLE scan_for_trailing_sack;
   
    HANDLE send_frame;
    HANDLE send_prio_frame;
    HANDLE send_ctrl_frame;

    HANDLE client_timeout;
    HANDLE file_stream[MAX_SERVER_ACTIVE_FSTREAMS];
    HANDLE server_command;
} ServerThreads;

//--------------------------------------------------------------------------------------------------------------------------

extern ServerData Server;
extern ServerBuffers Buffers;
//extern ClientListData ClientList;
extern ServerThreads Threads;

// Thread functions
static DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam);
// static DWORD WINAPI func_thread_process_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_process_ctrl_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_process_data_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_process_fstream(LPVOID lpParam);

static DWORD WINAPI fthread_send_sack_frame(LPVOID lpParam);
static DWORD WINAPI fthread_scan_for_trailing_sack(LPVOID lpParam);

static DWORD WINAPI fthread_send_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam);

static DWORD WINAPI fthread_client_timeout(LPVOID lpParam);
static DWORD WINAPI fthread_server_command(LPVOID lpParam);

// Client management functions

static BOOL validate_file_hash(ServerFileStream *fstream);
static void check_open_file_stream();

 

 
#endif