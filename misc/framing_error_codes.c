#include <winsock2.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

#define PORT 25000 // Port number to connect to
#define FRAME_PAYLOAD_SIZE 8

#pragma pack(push, 1)
typedef struct packet_header {
    uint32_t type;
    uint32_t ID;
    uint32_t payload_size;
    uint32_t size; // Total size of the packet (header + payload)
} packet_header_t;

typedef struct frame {
    uint32_t sequence_number;
    uint32_t CRC;
    uint32_t payload_size; // Actual size of payload in *this frame*
    uint32_t size; // Total size of this frame to be sent
    char payload[FRAME_PAYLOAD_SIZE];
} frame_t;
#pragma pack(pop)

// --- Define Custom Error Codes with NET_ERR_ prefix ---
typedef enum {
    NET_ERR_SUCCESS = 0,
    NET_ERR_NULL_POINTER = -1,
    NET_ERR_INVALID_PACKET_STRUCTURE = -2,
    NET_ERR_MEMORY_ALLOCATION_FAILED = -3,
    NET_ERR_EMPTY_MESSAGE = -4,
    NET_ERR_WSA_STARTUP_FAILED = -5,
    NET_ERR_SEND_FAILED = -6, // For actual network send errors
    // Add more specific errors as your project grows, e.g., NET_ERR_CONNECTION_FAILED, NET_ERR_RECV_FAILED
} err_code_t;

typedef enum{
    LOG_INFO, 
    LOG_DEBUG, 
    LOG_ERROR 
}event_type_t;

void log_event(event_type_t event_type, const char* event_message) {

    time_t current_time;
    time(&current_time);
    struct tm* utc_time = gmtime(&current_time);

    const char* event_str = NULL;
    if (event_type == LOG_INFO){
        event_str = "INFO";
    } else if(event_type == LOG_DEBUG){
        event_str = "DEBUG";
    } else if(event_type == LOG_ERROR){
        event_str = "ERROR";
    } else {
        event_str = "UNKNOWN";
    }
   
    fprintf(stderr,"\n[%s] [UTC %04d-%02d-%02d %02d:%02d:%02d] - %s",
        event_str,
        utc_time->tm_year + 1900,
        utc_time->tm_mon + 1,
        utc_time->tm_mday,
        utc_time->tm_hour,
        utc_time->tm_min,
        utc_time->tm_sec,
        event_message);
}

uint32_t crc32(const void* payload, uint32_t payload_size);
void* create_packet(uint32_t type, uint32_t ID, const void* payload, uint32_t payload_size, err_code_t* out_error);
err_code_t send_packet_in_frames(SOCKET clientSocket, const void* packet);
// Helper functions (remain void as they are for display/utility)
void print_packet(const void* packet);
void print_frame(frame_t* frame);

uint32_t crc32(const void* payload, uint32_t payload_size){
    uint32_t CRC = 0xFFFFFFFF; // Initial CRC value
    // Initialize polynomial for CRC-32
    uint32_t polynomial = 0xEDB88320;
    const uint8_t* byte = (const uint8_t* )payload;

    for (uint32_t i = 0; i < payload_size; i++) {
        CRC ^= byte[i];
        for (int j = 0; j < 8; j++) {
            // Check if the least significant bit of crc is set
            if (CRC & 1) {
                // If it is set, shift crc right by one position and XOR with polynomial
                CRC = (CRC >> 1) ^ polynomial;
            } else {
                // If it is not set, just shift crc right by one position without XOR
                CRC = CRC >> 1;
            }
        }
    }   
    return ~CRC; // Final CRC value
}

// Helper function to get a human-readable message for an error code
const char* get_error_message(err_code_t err_code) { 
    switch (err_code) {
        case NET_ERR_SUCCESS: return "Success";
        case NET_ERR_NULL_POINTER: return "A required pointer was NULL.";
        case NET_ERR_INVALID_PACKET_STRUCTURE: return "Packet structure is invalid (e.g., payload size, total size).";
        case NET_ERR_MEMORY_ALLOCATION_FAILED: return "Memory allocation failed.";
        case NET_ERR_EMPTY_MESSAGE: return "Input message was empty.";
        case NET_ERR_WSA_STARTUP_FAILED: return "Winsock startup failed.";
        case NET_ERR_SEND_FAILED: return "Network send operation failed.";
        default: return "Unknown error code.";
    }
}

void* create_packet(uint32_t type, uint32_t ID, const void* payload, uint32_t payload_size, err_code_t* err_code) {
    if (err_code == NULL) {
        return NULL; // Cannot report error if pointer is NULL
    }
    *err_code = NET_ERR_SUCCESS; // Assume success initially

    // Validate the input parameters
    if (payload_size == 0 || payload == NULL || sizeof(packet_header_t) + payload_size > UINT32_MAX) {
        *err_code = NET_ERR_INVALID_PACKET_STRUCTURE;
        return NULL;
    }

    // Allocate memory for the packet header and payload
    void* packet = malloc(sizeof(packet_header_t) + payload_size);
    if (packet == NULL) {
        *err_code = NET_ERR_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }

    // Fill in the header
    packet_header_t* header = (packet_header_t*)packet;
    
    // Convert header fields to network byte order (big-endian)
    header->type = htonl(type);
    header->ID = htonl(ID);
    header->payload_size = htonl(payload_size);
    header->size = htonl(sizeof(packet_header_t) + payload_size); // Calculate then convert

    // Copy payload data
    memcpy((char*)packet + sizeof(packet_header_t), payload, payload_size);

    return packet; // Return the newly created packet
}

err_code_t send_packet_in_frames(SOCKET clientSocket, const void* packet) {
    // Validate the packet
    if (packet == NULL) {
        return NET_ERR_NULL_POINTER;
    }

    // Cast the packet to its header type for easier access
    const packet_header_t* header = (const packet_header_t*)packet;
    uint32_t total_packet_size = ntohl(header->size); // Convert to host byte order for calculations

    uint32_t bytes_remaining = total_packet_size;
    uint32_t current_offset = 0;

    frame_t frame = {0}; // Initialize the frame
    uint32_t sequence_nr = 0; // Initialize frame count (using uint32_t for consistency)

    while (bytes_remaining > 0) {
        sequence_nr++; // Increment sequence number
        frame.sequence_number = htonl(sequence_nr); // Convert to network byte order

        uint32_t current_frame_payload_size;
        if (bytes_remaining < FRAME_PAYLOAD_SIZE) {
            current_frame_payload_size = bytes_remaining;
        } else {
            current_frame_payload_size = FRAME_PAYLOAD_SIZE;
        }
        frame.CRC = htonl(crc32(frame.payload, current_frame_payload_size)); // Compute CRC-32

        frame.payload_size = htonl(current_frame_payload_size); // Convert to network byte order
        
        // Calculate the total frame size *before* converting to network byte order for accuracy
        uint32_t current_frame_size = (sizeof(frame_t) - FRAME_PAYLOAD_SIZE) + current_frame_payload_size;
        frame.size = htonl(current_frame_size); // Convert to network byte order

        // Copy actual payload data for this frame
        memcpy(frame.payload, (const char*)packet + current_offset, current_frame_payload_size);
        
        bytes_remaining -= current_frame_payload_size;
        current_offset += current_frame_payload_size;
        // Send the frame over the network
        // int bytes_sent = send(clientSocket, (char*)&frame, total_frame_struct_size_host_order, 0); // Use host order size for send()
        // if (bytes_sent == SOCKET_ERROR) {
        //     return NET_ERR_SEND_FAILED;
        // }

        // For debugging, print frame content (converting back to host order for display)
        frame_t print_dbg_frame = {0};
        print_dbg_frame.sequence_number = ntohl(frame.sequence_number);
        print_dbg_frame.CRC = ntohl(frame.CRC);
        print_dbg_frame.payload_size = ntohl(frame.payload_size);
        print_dbg_frame.size = ntohl(frame.size);
        memcpy(print_dbg_frame.payload, frame.payload, current_frame_payload_size);
        print_frame(&print_dbg_frame); 
           
    }
    return NET_ERR_SUCCESS;
}

void print_packet(const void* packet) {
    if (packet == NULL) {
        log_event(LOG_INFO, "Cannot print NULL packet.");
        return;
    }
    const packet_header_t* header = (const packet_header_t*)packet;
    
    // Convert fields from network byte order to host byte order for printing
    printf("\nPacket Type: %u", ntohl(header->type));
    printf("\nPacket ID: %u", ntohl(header->ID));
    printf("\nPayload Size: %u", ntohl(header->payload_size));
    printf("\nPacket Size: %u", ntohl(header->size));

    // Access payload directly (it's raw data, no byte order conversion needed)
    // The payload size for memcpy is also converted back to host order for calculation
    uint32_t payload_size_host_order = ntohl(header->payload_size);
    if (payload_size_host_order > 0) {
        const char* payload = (const char*)packet + sizeof(packet_header_t);
        printf("\nPacket Payload: ");
        for (uint32_t i = 0; i < payload_size_host_order; i++) {
            printf("%02X ", (unsigned char)payload[i]);
        }
    }
}

void print_frame(frame_t* frame) {
    if (frame == NULL) {
        log_event(LOG_ERROR, "Cannot print NULL frame.");
        return;
    }
    // Note: This print_frame now expects *host byte order* values.
    // The calling send_packet_in_frames converts them back for printing
    printf("\n----------------------"); 
    printf("\nFrame Sequence Number: %u", frame->sequence_number); 
    printf("\nFrame Payload CRC: %u", frame->CRC);       
    printf("\nFrame Payload Size: %u", frame->payload_size);       
    printf("\nFrame Size: %u", frame->size);                
    printf("\nFrame Payload: ");
    for (uint32_t i = 0; i < frame->payload_size; i++) { // Use host order payload_size for loop
        printf("%02X ", (unsigned char)frame->payload[i]);
    }
}

int main() {
    err_code_t err_code;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_event(LOG_ERROR, get_error_message(NET_ERR_WSA_STARTUP_FAILED));
        return NET_ERR_WSA_STARTUP_FAILED;
    }

    void* packet = NULL;
    char out_message[1024] = {0};

    while(1){
        
        log_event(LOG_INFO, "Enter your message: ");
        fgets(out_message, sizeof(out_message), stdin);
        out_message[strcspn(out_message, "\n")] = '\0';
        if (strlen(out_message) == 0) {
            log_event(LOG_ERROR, get_error_message(NET_ERR_EMPTY_MESSAGE));
            WSACleanup();
            return NET_ERR_EMPTY_MESSAGE;
        }
        if(!strcmp(out_message, "exit")){
            log_event(LOG_INFO, "Exiting.");
            WSACleanup();
            return 1;
        }

        // Pass the address of 'status' to create_packet
        packet = create_packet(1, 1, out_message, strlen(out_message), &err_code);

        if (err_code != NET_ERR_SUCCESS) {
            log_event(LOG_ERROR,get_error_message(err_code));
            WSACleanup();
            return 1;
        }

        log_event(LOG_INFO, "Original Packet");
        // print_packet now takes a packet with fields in network byte order
        print_packet(packet);

        log_event(LOG_INFO, "Sending Packet in Frames");
        err_code = send_packet_in_frames(INVALID_SOCKET, packet); // Placeholder for actual socket

        if (err_code != NET_ERR_SUCCESS) {
            log_event(LOG_ERROR, get_error_message(err_code));
            free(packet);
            WSACleanup();
            return 1;
        }

        log_event(LOG_INFO, "Packet sent in frames successfully.");

        free(packet);

    }
    WSACleanup();
    return 0;
}