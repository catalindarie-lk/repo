#ifndef FRAMES_H
#define FRAMES_H

#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS                     0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR                       -1
#endif
#ifndef MAX_PATH
#define MAX_PATH                            260
#endif

#define SERVER_PORT                         53567               // Port the server listens on
#define MAX_FRAME_SIZE                      1400
#define FRAME_DELIMITER                     0xAABB             // A magic number to identify valid frames



#define MAX_PAYLOAD_SIZE                    (MAX_FRAME_SIZE - sizeof(FrameHeader))
#define TEXT_FRAGMENT_SIZE                  (MAX_PAYLOAD_SIZE - sizeof(uint32_t) * 4)
#define FILE_FRAGMENT_SIZE                  (MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 2) - sizeof(uint64_t))


#define MAX_SACK_COUNT                      ((MAX_PAYLOAD_SIZE - sizeof(uint8_t)) / sizeof(uint64_t)) // Maximum number of sequence numbers in a SACK frame                      
#define MAX_NAME_SIZE                       255                 // Maximum size for client/server names

#define DEFAULT_DISCONNECT_REQUEST_SEQ      (UINT64_MAX - 1)
#define DEFAULT_CONNECT_REQUEST_SEQ         (UINT64_MAX - 2)
#define DEFAULT_KEEP_ALIVE_SEQ              (UINT64_MAX - 3)
#define DEFAULT_SACK_SEQ                    (UINT64_MAX - 4)
#define DEFAULT_RTT_SEQ                     (UINT64_MAX - 5)

#define DEFAULT_CONNECT_REQUEST_SID         (UINT32_MAX - 1)

#define WSARECV_TIMEOUT_MS                  100         // Timeout in milliseconds in the receive frame thread
#define GETQCOMPL_TIMEOUT                   258L

#define SERVER_FILE_BLOCK_SIZE              (FILE_FRAGMENT_SIZE * 64ULL * 4ULL)
#define CLIENT_FILE_BLOCK_SIZE              (FILE_FRAGMENT_SIZE * 64ULL * 4ULL)

#define OP_RECV ((uint8_t)(1))
#define OP_SEND ((uint8_t)(2))

#define OP_WR ((uint8_t)(3))
#define OP_RD ((uint8_t)(4))

// --- Frame Types ---
typedef uint8_t FrameType;
enum FrameType{

    FRAME_TYPE_CONNECT_REQUEST = 1,             // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 2,            // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 3,                 // Client requests to disconnect

    FRAME_TYPE_ACK = 4,                         // Acknowledgment for a received frame
    FRAME_TYPE_SACK = 5,                        // Selective Acknowledgment for multiple frames
    FRAME_TYPE_KEEP_ALIVE = 6,

    FRAME_TYPE_FILE_METADATA = 20,       // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_METADATA_RESPONSE = 21,
    FRAME_TYPE_FILE_FRAGMENT = 22,                   // File data fragment
    FRAME_TYPE_FILE_END = 23,
    FRAME_TYPE_FILE_COMPLETE = 24,
    FRAME_TYPE_TEXT_MESSAGE = 30      // Fragment of a long text message
};

typedef uint8_t AckErrorCode;
enum AckErrorCode {
    // STS_FRAME_DATA_ACK = 11,

    
    STS_KEEP_ALIVE = 12,  
    STS_CONFIRM_FILE_METADATA = 21,
    STS_CONFIRM_FILE_END = 22,
    STS_CONFIRM_DISCONNECT = 23,
    STS_CONFIRM_MESSAGE_FRAGMENT = 24,

    ERR_EXISTING_FILE = 100,       // Server has completed this transfer
    ERR_DUPLICATE_FRAME = 101,     // Frame was already received
    ERR_MISSING_METADATA = 102,
    ERR_EXISTING_MESSAGE = 103,
    ERR_MESSAGE_FINAL_CHECK = 104,
    ERR_MEMORY_ALLOCATION = 105,
    ERR_STREAM_INIT = 106,    
    ERR_MALFORMED_FRAME = 107,     // Frame structure or size invalid
    ERR_RESOURCE_LIMIT = 108,      // Server ran out of memory or slots
    ERR_CONFIRM_DUPLICATE_FILE_METADATA = 109, // Client sent duplicate metadata

    ERR_INVALID_SESSION = 120,     // Session ID not recognized
    ERR_TIMEOUT = 121,             // Session timed out due to inactivity
    ERR_UNSUPPORTED_FRAME = 122,   // Frame type not supported
    ERR_UNAUTHORIZED = 123,        // Authentication/authorization failed
    ERR_INTERNAL_ERROR = 124,       // Catch-all for unexpected server fault
    ERR_UNKNOWN_ERROR = 125
};

typedef uint8_t StreamErrorCode;
enum StreamErrorCode {
    STS_STREAM_UNDEFINED = 0,
    STS_STREAM_SUCCESS = 10,

    ERR_STREAM_MEMORY_ALLOCATION = 100,
    ERR_STREAM_INVALID_PARAMETERS = 101,
    ERR_STREAM_INVALID_PAYLOAD_DATA = 102,
    ERR_STREAM_PATH_CREATE = 103,
    ERR_STREAM_FILE_EXIST = 104,
    ERR_STREAM_TEMP_FILE_EXIST = 105,
    ERR_STREAM_TABLE_INSERT = 106,
    ERR_STREAM_FILE_HANDLE_INIT = 107,
    ERR_STREAM_FILE_HANDLE_ASSOCIATE = 108

};

#pragma pack(push, 1) 
// Common Header for all frames
typedef struct {
    uint16_t start_delimiter;                               // Magic number (e.g., 0xAABB)
    uint8_t frame_type;                                    // Discriminator: what kind of payload is in the union
    uint8_t padding_0;
    uint64_t seq_num;                                       // Global sequence number for this frame from the sender
    uint32_t session_id;                                    // Unique identifier for the session (e.g., client ID or session ID)
    uint32_t checksum;                                      // Checksum for this frame's header + active union member (CRC32 recommended)
} FrameHeader;
// Payload Structures for different frame types
typedef struct {
    uint32_t client_id;                                     // Unique identifier of the sender
    uint8_t  flags;                                         // Protocol the client supports
    char     client_name[MAX_NAME_SIZE];                    // Optional: human-readable identifier
} ConnectRequestPayload;

typedef struct {
    uint32_t session_timeout;                               // Suggested timeout period for client inactivity
    uint8_t  server_status;                                 // BUSY (0) READY (1) or ERR (x), etc
    char     server_name[MAX_NAME_SIZE];                    // Optional: human-readable identifier
} ConnectResponsePayload;

typedef struct {
    uint8_t op_code;
} AckPayload;

typedef struct {                                 // Session ID for which this SACK is valid
    uint8_t ack_count;
    uint64_t seq_num[MAX_SACK_COUNT];
} SAckPayload;

typedef struct {
    uint64_t timestamp;
} RttPayload;

typedef struct {
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint64_t file_size;                                     // Total size of the file being transferred
    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];                                   // Max filename length
    uint32_t fname_len;
}FileMetadataPayload;

typedef struct {
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint8_t  op_code;                                       //
}FileMetadataResponsePayload;

typedef struct {
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint64_t offset;                                        // Offset of this fragment within the file
    uint32_t size;                                          // Length of actual data in 'fragment_data'
    uint8_t  bytes[FILE_FRAGMENT_SIZE];                        // Adjusted size
}FileFragmentPayload;

typedef struct{
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint64_t file_size;                                     // Total size of the file being transferred
    uint8_t file_hash[32];                                  // For SHA256 hash
}FileEndPayload;

typedef struct{
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint8_t op_code;
}FileCompletePayload;

typedef struct {
    uint32_t message_id;                                    // Unique ID for this specific long message
    uint32_t message_len;                                   // Total length of the original message
    uint32_t fragment_len;                                  // Length of actual text data in 'fragment_data'
    uint32_t fragment_offset;                               // Offset of this fragment within the long message
    char     chars[TEXT_FRAGMENT_SIZE];             // Adjusted size
} TextPayload;

// Main UDP Frame Structure
typedef struct {
    FrameHeader header;
    union {
        ConnectRequestPayload connection_request;                      // Client's connect request
        ConnectResponsePayload connection_response;                    // Server's response to client connect
        AckPayload ack;
        RttPayload rtt;
        SAckPayload sack;
        FileMetadataPayload file_metadata;                  // File metadata request/response
        FileMetadataResponsePayload file_metadata_response;                  // File metadata request/response
        FileFragmentPayload file_fragment;                  // File data fragment
        FileEndPayload file_end;
        FileCompletePayload file_complete;
        TextPayload text_fragment;                      // Fragment of a long text message
        uint8_t raw_payload[MAX_PAYLOAD_SIZE];              // For generic access or padding
    } payload;
} UdpFrame;

// typedef struct {
//     FrameHeader header;
//     AckPayload payload;
// } AckUdpFrame;

#pragma pack(pop)

#endif 