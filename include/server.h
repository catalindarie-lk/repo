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
#define DEFAULT_SESSION_TIMEOUT_SEC                 60
#endif

// --- Constants 
#define SERVER_NAME                                 "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                                 10
#define SACK_READY_FRAME_TIMEOUT_MS                 100

#define SERVER_PARTITION_DRIVE                      "H:\\"
#define SERVER_ROOT_FOLDER                          "H:\\_test\\server_root\\"
#define SERVER_MESSAGE_TEXT_FILES_FOLDER            "H:\\_test\\messages_root\\"
#define SERVER_SID_FOLDER_NAME_FOR_CLIENT           "SID_"

#define CHUNK_TRAILING                              (1u << 7) // 0b10000000
#define CHUNK_BODY                                  (1u << 6) // 0b01000000
#define CHUNK_HASHED                                (1u << 5) // 0b00100000
#define CHUNK_WRITTEN                               (1u << 0) // 0b00000001
#define CHUNK_NONE                                  (0)       // 0b00000000

//---------------------------------------------------------------------------------------------------------
// --- Server Stream Configuration ---
#define MAX_SERVER_ACTIVE_FSTREAMS                  20
#define MAX_SERVER_ACTIVE_MSTREAMS                  10

// --- Server Worker Thread Configuration ---
#define SERVER_MAX_THREADS_RECV_SEND_FRAME          1
#define SERVER_MAX_THREADS_PROCESS_FRAME            20
#define SERVER_MAX_THREADS_WRITE_FILE_BLOCK         1

#define SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES    1
#define SERVER_MAX_THREADS_SEND_MESSAGE_ACK_FRAMES  1

//---------------------------------------------------------------------------------------------------------
// --- Server SEND Buffer Sizes ---
#define SERVER_QUEUE_SIZE_SEND_FRAME                (4096 + 256 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_SEND_PRIO_FRAME           128
#define SERVER_QUEUE_SIZE_SEND_CTRL_FRAME           16

#define SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ            (SERVER_QUEUE_SIZE_SEND_FRAME)
#define SERVER_QUEUE_SIZE_CLIENT_SLOT               (SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ * MAX_SERVER_ACTIVE_FSTREAMS)
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



// --- Macro to Parse Global Data to Threads ---
// This macro simplifies passing pointers to global client data structures into thread functions.
// It creates local pointers within the thread function's scope, pointing to the global instances.
#define PARSE_SERVER_GLOBAL_DATA(server_obj, client_list_obj, buffers_obj, threads_obj) \
    ServerData *server = &(server_obj); \
    ClientListData *client_list = &(client_list_obj); \
    ServerBuffers *buffers = &(buffers_obj); \
    ServerThreads *threads = &(threads_obj); \
    MemPool *pool_file_block = &((buffers_obj).pool_file_block); \
    MemPool *pool_iocp_send_context = &((buffers_obj).pool_iocp_send_context); \
    MemPool *pool_iocp_recv_context = &((buffers_obj).pool_iocp_recv_context); \
    MemPool *pool_recv_udp_frame = &((buffers_obj).pool_recv_udp_frame); \
    QueuePtr *queue_recv_udp_frame = &((buffers_obj).queue_recv_udp_frame); \
    QueuePtr *queue_recv_prio_udp_frame = &((buffers_obj).queue_recv_prio_udp_frame); \
    QueuePtr *queue_process_fstream = &((buffers_obj).queue_process_fstream); \
    TableIDs *table_file_id = &((buffers_obj).table_file_id); \
    TableIDs *table_message_id = &((buffers_obj).table_message_id); \
    TableFileBlock *table_file_block = &((buffers_obj).table_file_block); \
    MemPool *pool_send_udp_frame = &((buffers_obj).pool_send_udp_frame); \
    QueuePtr *queue_send_udp_frame = &((buffers_obj).queue_send_udp_frame); \
    QueuePtr *queue_send_prio_udp_frame = &((buffers_obj).queue_send_prio_udp_frame); \
    QueuePtr *queue_send_ctrl_udp_frame = &((buffers_obj).queue_send_ctrl_udp_frame); \
    QueueClientSlot *queue_client_slot = &((buffers_obj).queue_client_slot); \
    ServerFstreamPool *pool_fstreams = &((server_obj).pool_fstreams); \
    ServerMstreamPool *pool_mstreams = &((server_obj).pool_mstreams); \
    ServerClientPool *pool_clients = &((server_obj).pool_clients); \
// end of #define PARSE_GLOBAL_DATA // End marker for the macro definition

enum Status{
    STATUS_NONE = 0,
    STATUS_BUSY = 1,
    STATUS_READY = 2,
    STATUS_ERROR = 3
};

typedef uint8_t ClientConnection;
enum ClientConnection {
    CLIENT_DISCONNECTED = 0,
    CLIENT_CONNECTED = 1
};

typedef uint8_t ClientSlotStatus;
enum ClientSlotStatus {
    SLOT_FREE = 0,
    SLOT_BUSY = 1
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
} ServerMessageStream;

typedef struct{
    struct sockaddr_in client_addr;

    uint32_t sid;                       // Session ID associated with this file stream.
    uint32_t fid;                       // File ID, unique identifier for the file associated with this file stream.
    uint64_t file_size;                     // Total size of the file being transferred.

    uint64_t *received_file_bitmap;     // Pointer to an array of uint64_t, where each bit represents a file fragment.
    uint64_t *written_file_bitmap;      // Pointer to an array of uint64_t, where each bit represents a file fragment.
                                        // A bit set to 1 means the fragment has been received.
    uint64_t file_bitmap_size;          // Number of uint64_t entries in the bitmap array.


    char **file_block;
    uint64_t *recv_block_bytes;          // Total bytes received for this file so far.
    uint64_t *recv_block_status;

    uint64_t fragment_count;            // Total number of fragments in the entire file.
    uint64_t block_count;

    uint64_t written_bytes_count;       // Total bytes written to disk for this file so far.
    
    uint8_t received_sha256[32];        // Buffer for sha256 received from the client
    uint8_t calculated_sha256[32];      // Buffer for sha256 calculated by the server

    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];               // Array to store the file name+path.
    uint32_t fname_len;
    
    char iocp_full_path[MAX_PATH];
    HANDLE iocp_file_handle;

    SRWLOCK lock;              // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.

}ServerFileStream;

typedef struct {
    SAckPayload payload; // SACK payload for this client
    uint32_t ack_pending;
    uint64_t start_timestamp;
    BOOL start_recorded;
    SRWLOCK lock;
}ClientSAckContext;

typedef struct {

    struct sockaddr_in client_addr;            // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    
    uint32_t cid;                 // Unique ID received from the client
    char name[MAX_NAME_SIZE];               // Optional: human-readable identifier received from the client
    uint8_t flags;                       // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;
 
    uint32_t sid;                // Unique ID assigned by the server for this clients's session
    volatile time_t last_activity_time; // Last time the client sent a frame (for timeout checks)             

    uint32_t slot;                  // Index of the slot the client is connected to [0..MAX_CLIENTS-1]
    uint8_t slot_status;                // 0->FREE; 1->BUSY
   
    QueueSeq queue_ack_seq;
    ClientSAckContext sack_ctx; // SACK context for this client
 
    SRWLOCK lock;
} Client;

typedef struct {
    Client client[MAX_CLIENTS];     // Array of connected clients
    CRITICAL_SECTION lock;          // For thread-safe access to connected_clients
}ClientListData;

typedef struct {
    ServerFileStream *fstream;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
} ServerFstreamPool;

typedef struct {
    ServerMessageStream *mstream;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
} ServerMstreamPool;

typedef struct {
    Client *client;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
} ServerClientPool;

typedef struct {
    SOCKET socket;
    struct sockaddr_in server_addr;            // Server address structure
    uint8_t server_status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    volatile uint32_t session_id_counter;   // Global counter for unique session IDs
    volatile uint64_t file_block_count;   // Global counter for unique file block IDs
    char name[MAX_NAME_SIZE];               // Human-readable server name
    
    HANDLE iocp_socket_handle;
    HANDLE iocp_file_handle;

    

    ServerFstreamPool pool_fstreams;
    ServerMstreamPool pool_mstreams;
    ServerClientPool pool_clients;

}ServerData;

typedef struct {
    MemPool pool_file_block;
    MemPool pool_iocp_send_context;
    MemPool pool_iocp_recv_context;

    MemPool pool_recv_udp_frame;
    QueuePtr queue_recv_udp_frame;
    QueuePtr queue_recv_prio_udp_frame;

    MemPool pool_send_udp_frame;
    QueuePtr queue_send_udp_frame;          // For SACK frames
    QueuePtr queue_send_prio_udp_frame;     // For ACK frames
    QueuePtr queue_send_ctrl_udp_frame;     // For Request/Keep alive frames

    QueuePtr queue_process_fstream;

    QueueClientSlot queue_client_slot;

    TableIDs table_file_id;
    TableIDs table_message_id;

    TableFileBlock table_file_block;

}ServerBuffers;

typedef struct {

    HANDLE recv_send_frame[SERVER_MAX_THREADS_RECV_SEND_FRAME];
    HANDLE file_block_written[SERVER_MAX_THREADS_WRITE_FILE_BLOCK];
    HANDLE process_frame[SERVER_MAX_THREADS_PROCESS_FRAME];

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
extern ClientListData ClientList;
extern ServerThreads Threads;

// Thread functions
static DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_process_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_file_block_written(LPVOID lpParam);

static DWORD WINAPI fthread_send_sack_frame(LPVOID lpParam);
static DWORD WINAPI fthread_scan_for_trailing_sack(LPVOID lpParam);

// static DWORD WINAPI fthread_send_ack_frame(LPVOID lpParam);

static DWORD WINAPI fthread_send_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam);

static DWORD WINAPI fthread_client_timeout(LPVOID lpParam);
static DWORD WINAPI fthread_server_command(LPVOID lpParam);

// Client management functions
void init_client_pool(ServerClientPool* pool, const uint64_t block_count);
static Client* find_client(const uint32_t session_id);
static Client* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
static int remove_client(const uint32_t slot);

static void cleanup_client(Client *client);
static BOOL validate_file_hash(ServerFileStream *fstream);
static void check_open_file_stream();

 



#endif