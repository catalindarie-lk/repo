#ifndef CLIENT_H
#define CLIENT_H

#include <stdint.h>                                             // For fixed-width integer types like uint8_t, uint32_t, uint64_t
#include <windows.h>                                            // For Windows-specific types like BOOL, HANDLE, CRITICAL_SECTION, SOCKET, MAX_PATH, etc.
#include "include/protocol_frames.h"                            // Includes definitions related to network frame structures and types
#include "include/resources.h"
#include "include/queue.h"                                      // Includes definitions for queue data structures
#include "include/mem_pool.h"                                   // Includes definitions for memory pool management
#include "include/hash.h"                                       // Includes definitions for hash table structures

// --- Standard Return Value Definitions ---
#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif
#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC                 120
#endif

// --- Client-Specific Constants ---

#define CLIENT_ID                                   (0xFF)                // Example client ID, can be set dynamically by the application
#define CLIENT_NAME                                 "lkdc UDP Text/File Transfer Client"
#define MIN_CONNECTION_TIMEOUT_SEC                  15                     // Minimum timeout for a connection in seconds

#define CLIENT_ROOT_FOLDER                          "H:\\_test\\client_root\\"
#define CLIENT_1_ROOT_FOLDER                        "H:\\_test\\client1_root\\"
#define CLIENT_2_ROOT_FOLDER                        "H:\\_test\\client2_root\\"
#define CLIENT_3_ROOT_FOLDER                        "H:\\_test\\client3_root\\"
#define CLIENT_4_ROOT_FOLDER                        "H:\\_test\\client4_root\\"
#define CLIENT_5_ROOT_FOLDER                        "H:\\_test\\client5_root\\"

#define CLIENT_ERROR_LOG_FILE                       "H:\\_test\\client_error_log.txt" // Path to the client log file
#define CLIENT_DEBUG_LOG_FILE                       "H:\\_test\\client_debug_log.txt" // Path to the client debug log file

#define CLIENT_LOG_MESSAGE_LEN                      256

#define CONNECT_REQUEST_TIMEOUT_MS                  2500                  // Timeout for a connection request in milliseconds
#define DISCONNECT_REQUEST_TIMEOUT_MS               2500                  // Timeout for a disconnect request in milliseconds

#define TEXT_CHUNK_SIZE                             (TEXT_FRAGMENT_SIZE * 64) // Size of a text data chunk (derived from protocol_frames.h)
// #define FILE_CHUNK_SIZE                             (FILE_FRAGMENT_SIZE * 512) // Size of a file data chunk (derived from protocol_frames.h)

#define MAX_MESSAGE_SIZE_BYTES                      (4 * 1024 * 1024)     // Maximum size for a single large text message (4 MB)

//---------------------------------------------------------------------------------------------------------
// --- Client Stream Configuration ---
#define CLIENT_MAX_ACTIVE_MSTREAMS                  1                     // Maximum number of concurrent message streams (e.g., long text messages)
#define CLIENT_MAX_ACTIVE_FSTREAMS                  5

// --- Client Worker Thread Configuration ---
#define CLIENT_MAX_THREADS_RECV_SEND_FRAME          1                     // Number of threads dedicated to receiving and sending frames
#define CLIENT_MAX_TREADS_PROCESS_FRAME             4                     // Number of threads dedicated to processing received frames
#define CLIENT_MAX_THREADS_SEND_FRAME               1                     // Number of threads for popping normal send frames from queue
//---------------------------------------------------------------------------------------------------------
// --- Client SEND Buffer Sizes ---
#define CLIENT_QUEUE_SIZE_SEND_FRAME                256
#define CLIENT_QUEUE_SIZE_SEND_PRIO_FRAME           32
#define CLIENT_QUEUE_SIZE_SEND_CTRL_FRAME           4
// --- Client SEND Memory Pool Sizes ---
#define CLIENT_POOL_SIZE_SEND                       (CLIENT_QUEUE_SIZE_SEND_FRAME + \
                                                    CLIENT_QUEUE_SIZE_SEND_PRIO_FRAME + \
                                                    CLIENT_QUEUE_SIZE_SEND_CTRL_FRAME)
#define CLIENT_POOL_SIZE_IOCP_SEND                  CLIENT_POOL_SIZE_SEND * 2

// --- Client RECV Buffer Sizes ---
#define CLIENT_QUEUE_SIZE_RECV_FRAME                (1024 + 128 * CLIENT_MAX_ACTIVE_FSTREAMS) // Size of the queue for received frames
#define CLIENT_QUEUE_SIZE_RECV_PRIO_FRAME           32
// --- Client RECV Memory Pool Sizes ---
#define CLIENT_POOL_SIZE_RECV                       (CLIENT_QUEUE_SIZE_RECV_FRAME + \
                                                    CLIENT_QUEUE_SIZE_RECV_PRIO_FRAME)
#define CLIENT_POOL_SIZE_IOCP_RECV                  (CLIENT_POOL_SIZE_RECV * 2)
//---------------------------------------------------------------------------------------------------------
#define CLIENT_QUEUE_SIZE_SEND_FILE                 65536                  // Size of the queue for file stream commands
#define CLIENT_QUEUE_SIZE_SEND_MESSAGE              65536                  // Size of the queue for message stream commands
#define CLIENT_POOL_SIZE_SEND_COMMAND               (CLIENT_QUEUE_SIZE_SEND_FILE + \
                                                    CLIENT_QUEUE_SIZE_SEND_MESSAGE + 16)
#define CLIENT_QUEUE_SIZE_LOG                       65536
#define CLIENT_POOL_SIZE_LOG                        (CLIENT_QUEUE_SIZE_LOG + 16)
//---------------------------------------------------------------------------------------------------------

// --- Macro to Parse Global Data to Threads ---
// This macro simplifies passing pointers to global client data structures into thread functions.
// It creates local pointers within the thread function's scope, pointing to the global instances.
#define PARSE_CLIENT_GLOBAL_DATA(client_obj, queues_obj, buffers_obj, threads_obj) \
    ClientData *client = &(client_obj); /* Pointer to the global ClientData structure */ \
    ClientQueues *queues = &(queues_obj); /* Pointer to queues struct queue */ \
    ClientBuffers *buffers = &(buffers_obj); /* Pointer to the global ClientBuffers structure */ \
    ClientThreads *threads = &(threads_obj); /* Pointer to threads struct queue */ \
    MemPool *pool_send_iocp_context = &((buffers_obj).pool_send_iocp_context); /* Pointer to IOCP send context memory pool */ \
    MemPool *pool_recv_iocp_context = &((buffers_obj).pool_recv_iocp_context); /* Pointer to IOCP recv context memory pool */ \
    MemPool *pool_recv_udp_frame = &((buffers_obj).pool_recv_udp_frame); \
    QueuePtr *queue_recv_udp_frame = &((queues_obj).queue_recv_udp_frame); \
    QueuePtr *queue_recv_prio_udp_frame = &((queues_obj).queue_recv_prio_udp_frame); \
    s_MemPool *pool_send_udp_frame = &((buffers_obj).pool_send_udp_frame); /* Pointer to send frame memory pool */ \
    s_QueuePtr *queue_send_udp_frame = &((queues_obj).queue_send_udp_frame); /* Pointer to send queue for normal frames */ \
    s_QueuePtr *queue_send_prio_udp_frame = &((queues_obj).queue_send_prio_udp_frame); /* Pointer to send queue for priority frames */ \
    s_QueuePtr *queue_send_ctrl_udp_frame = &((queues_obj).queue_send_ctrl_udp_frame); /* Pointer to send queue for control frames */ \
    TableSendFrame *table_send_udp_frame = &((buffers_obj).table_send_udp_frame); /* Pointer to hash table for sent frames awaiting ACK */ \
    s_MemPool *pool_send_command = &((buffers_obj).pool_send_command); /* Pointer commands memory pool */ \
    s_QueuePtr *queue_send_file_command = &((queues_obj).queue_send_file_command); /* Pointer to file stream command queue */ \
    s_QueuePtr *queue_send_message_command = &((queues_obj).queue_send_message_command); /* Pointer to message stream command queue */ \
    MemPool *pool_error_log = &((buffers_obj).pool_error_log); /* Pointer to error log memory pool */ \
    QueuePtr *queue_error_log = &((queues_obj).queue_error_log); /* Pointer to error log queue */ \

    // end of #define PARSE_GLOBAL_DATA // End marker for the macro definition


// --- Enumerations ---

// General status enumeration for a client entity
enum Status{
    STATUS_CLOSED = 0,                          // Client is closed or inactive
    STATUS_BUSY = 1,                            // Client is currently performing an operation
    STATUS_READY = 2,                           // Client is ready for operations
    STATUS_ERROR = 3                            // Client is in an error state
};

// Session status enumeration for a client's connection
typedef uint8_t SessionStatus;                  // Define SessionStatus as an 8-bit unsigned integer
enum SessionStatus{
    CONNECTION_CLOSED = 0,                      // Connection is not established
    CONNECTION_PENDING = 1,                     // Connection request sent, awaiting response
    CONNECTION_ESTABLISHED = 2,                 // Connection is active and established
};

// --- Structure Definitions ---

typedef struct {
    char log_message[CLIENT_LOG_MESSAGE_LEN];
    unsigned long long timestamp_64bit; // 64-bit timestamp
} PoolErrorLogEntry;

// Structure to hold a file's SHA256 hash
typedef struct{
    uint8_t sha256[32];                         // 32 bytes for SHA256 hash
}FileHash;

// Structure representing a single client message stream
typedef struct{
    BOOL mstream_busy;                          // Flag indicating if the message stream is active/busy
    
    char *message_buffer;                       // Pointer to the buffer holding the message content
    uint32_t message_len;                       // Length of the message in bytes
    
    uint32_t message_id;                        // Unique identifier for this message
    uint32_t remaining_bytes_to_send;           // Bytes yet to be sent for this message
    BOOL throttle;                              // Flag to indicate if sending should be throttled

    CRITICAL_SECTION lock;                      // Critical section for protecting access to this stream's data
}ClientMessageStream;

// Structure representing a single client file stream
typedef struct{
    BOOL fstream_busy;                          // Flag indicating if the file stream is active/busy
    FILE *fp;                                   // File pointer for the open file
    
    uint32_t fid;                               // File ID unique to this transfer
    long long fsize;                            // Total size of the file in bytes
    char dirpath[MAX_PATH];                       // Full path to the file on the client
    uint32_t fpath_len;                         // Length of the file path string
    char rpath[MAX_PATH];                       // Relative path for the file (for server-side storage)
    uint32_t rpath_len;                         // Length of the relative path string
    char fname[MAX_PATH];                       // File name (without path)
    uint32_t fname_len;                         // Length of the file name string
    // FileHash fhash;                             // SHA256 hash of the file

    uint8_t calculated_sha256[32];      // Buffer for sha256 calculated by the server

    uint64_t pending_bytes;                     // Remaining bytes of the file data to be sent/received
    // uint64_t pending_metadata_seq_num;          // Sequence number for pending metadata frames
    uint8_t *chunk_buffer;                      // Buffer to hold chunks of file data for reading/writing

    HANDLE hevent_metadata_response_ok;         // Event handle for successful metadata response
    HANDLE hevent_metadata_response_nok;        // Event handle for unsuccessful metadata response

    HANDLE hevent_file_end_response_ok;         // Event handle for successful metadata response
    HANDLE hevent_file_end_response_nok;        // Event handle for unsuccessful metadata response
    HANDLE hevent_file_end_transfer_err;

    SRWLOCK lock;                      // Critical section for protecting access to this stream's data
}ClientFileStream;

// Main structure holding global client data and state
typedef struct{
    SOCKET socket;                              // The UDP socket for communication
    struct sockaddr_in client_addr;             // Client's local address information
    struct sockaddr_in server_addr;             // Server's address information

    uint8_t client_status;                      // Current status of the client (from Status enum)
    uint8_t session_status;                     // Current status of the session (from SessionStatus enum)
    uint32_t cid;                               // Client ID
    uint8_t flags;                              // Various flags for client state
    char client_name[MAX_NAME_SIZE];            // Human readable client name
    time_t last_active_time;                    // Timestamp of last activity, used for session timeout
   
    volatile uint64_t frame_count;              // Monotonically increasing counter for frame sequence numbers
    volatile uint32_t fid_count;                // Counter for generating new unique file IDs
    volatile uint32_t mid_count;                // Counter for generating new unique message IDs

    uint32_t sid;                               // Session ID assigned by the server upon successful connection
    uint8_t server_status;                      // Server's reported status (e.g., 0-NOK; 1-OK)
    uint32_t session_timeout;                   // Session timeout value received from the server (for keep-alive)
    char server_name[MAX_NAME_SIZE];            // Human readable server name
    
    HANDLE hevent_connection_pending;           // Event handle for connection pending state
    HANDLE hevent_connection_established;       // Event handle for connection established state
    HANDLE hevent_connection_closed;            // Event handle for connection closed state

    SocketContext iocp_context;                  // IOCP context structure
    HANDLE iocp_handle;                         // Handle to the IOCP port

    ClientFileStream fstream[CLIENT_MAX_ACTIVE_FSTREAMS]; // Array of active file streams
    HANDLE fstreams_semaphore;                  // Semaphore to control access to file stream slots
    SRWLOCK fstreams_lock;             // Critical section for protecting access to the fstream array

    ClientMessageStream mstream[CLIENT_MAX_ACTIVE_MSTREAMS]; // Array of active message streams
    HANDLE mstreams_semaphore;                  // Semaphore to control access to message stream slots

    double send_speed_mbps;                     //Mbps
    uint64_t send_period_usec;                  //microsec
    // CRITICAL_SECTION send_lock;

} ClientData;

// Structure holding handles to all client-related threads
typedef struct {
    HANDLE recv_send_frame[CLIENT_MAX_THREADS_RECV_SEND_FRAME]; // Handles for threads managing frame receive/send
    HANDLE process_frame[CLIENT_MAX_TREADS_PROCESS_FRAME];     // Handles for threads processing frames
    HANDLE resend_frame;                                       // Handle for the resend management thread
    HANDLE keep_alive;                                         // Handle for the keep-alive thread
    HANDLE send_frame[CLIENT_MAX_THREADS_SEND_FRAME];  // Handles for threads popping normal send frames
    HANDLE send_prio_frame;                                // Handle for thread popping priority send frames
    HANDLE send_ctrl_frame;                                // Handle for thread popping control send frames

    HANDLE process_fstream[CLIENT_MAX_ACTIVE_FSTREAMS]; // Handles for individual file stream threads (if any)
    HANDLE process_mstream[CLIENT_MAX_ACTIVE_MSTREAMS]; // Handles for individual message stream threads (if any)   

    HANDLE client_command;                      // Handle for the client command processing thread
    HANDLE error_log_write;                     // Handle for the error log writing thread
}ClientThreads;

// Structure holding all client-related buffer and memory pool structures
typedef struct {
    MemPool pool_send_iocp_context;             // Memory pool for IOCP send contexts
    MemPool pool_recv_iocp_context;             // Memory pool for IOCP receive contexts
    MemPool pool_recv_udp_frame;                    // Memory pool for received frames
    s_MemPool pool_send_udp_frame;                  // Memory pool for normal frames to be sent
    TableSendFrame table_send_udp_frame;              // Hash table to track sent frames awaiting acknowledgment (ACK)
    s_MemPool pool_send_command;
    MemPool pool_error_log;                     // Memory pool for error log entries    
}ClientBuffers;

typedef struct {
    QueuePtr queue_recv_udp_frame;
    QueuePtr queue_recv_prio_udp_frame;
    s_QueuePtr queue_send_udp_frame;            // Queue for normal frames awaiting transmission
    s_QueuePtr queue_send_prio_udp_frame;       // Queue for priority frames awaiting transmission
    s_QueuePtr queue_send_ctrl_udp_frame;       // Queue for control frames awaiting transmission
    s_QueuePtr queue_send_file_command;         // Queue for file stream commands
    s_QueuePtr queue_send_message_command;         // Queue for message stream commands
    QueuePtr queue_error_log;                // Queue for normal frames to be sent
}ClientQueues;

// --- Global Data Declarations (defined in a corresponding .c file) ---
extern ClientData Client;                       // Global instance of client data
extern ClientQueues Queues;                   // Global instance of client queues
extern ClientBuffers Buffers;                   // Global instance of client buffers and pools
extern ClientThreads Threads;                   // Global instance of client thread handles

// --- Function Prototypes ---
// Functions related to sequence number generation and session management
int init_client_session();                      // Initializes the client session, sockets, pools, queues, etc.
int reset_client_session();                     // Resets the client session to a clean state

// Functions for cleaning up stream-specific resources
void close_file_stream(ClientFileStream *fstream);          // Cleans up resources associated with a file stream
void close_message_stream(ClientMessageStream *mstream);    // Cleans up resources associated with a message stream

int log_to_file(const char* log_message);

// Thread entry point functions
static DWORD WINAPI fthread_recv_send_frame(LPVOID lpParam);       // Thread for receiving and dispatching frames
static DWORD WINAPI fthread_process_frame(LPVOID lpParam);         // Thread for processing received frames
static DWORD WINAPI fthread_resend_frame(LPVOID lpParam);          // Thread for managing retransmissions
static DWORD WINAPI fthread_keep_alive(LPVOID lpParam);            // Thread for sending keep-alive messages to maintain session
static DWORD WINAPI fthread_send_frame(LPVOID lpParam);        // Thread for popping normal frames from send queue and initiating send
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam);   // Thread for popping priority frames from send queue
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam);   // Thread for popping control frames from send queue
static DWORD WINAPI fthread_process_fstream(LPVOID lpParam);       // Thread for managing a specific file stream operation
static DWORD WINAPI fthread_process_mstream(LPVOID lpParam);       // Thread for managing a specific message stream operation
static DWORD WINAPI fthread_client_command(LPVOID lpParam);        // Thread for processing client commands (e.g., from UI or other modules)
static DWORD WINAPI fthread_error_log_write(LPVOID lpParam);       // Thread for writing error logs to a file

#endif // CLIENT_H // End of header guard