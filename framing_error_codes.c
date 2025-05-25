#include <winsock2.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library

#define PORT 25000 // Port number to connect to
#define FRAME_PAYLOAD_SIZE 1400

#pragma pack(push, 1)
typedef struct packet_header {
    uint32_t type;
    uint32_t ID;
    uint32_t payload_size;
    uint32_t packet_size; // Total size of the packet (header + payload)
} packet_header_t;

typedef struct frame {
    uint32_t sequence_number;
    uint32_t payload_size; // Actual size of payload in *this frame*
    uint32_t frame_size; // Total size of this frame to be sent
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
} error_code;

void* create_packet(uint32_t type, uint32_t ID, const void* payload, uint32_t payload_size, error_code* out_error);
error_code send_packet_in_frames(SOCKET clientSocket, const void* packet);

// Helper functions (remain void as they are for display/utility)
void print_packet(const void* packet);
void print_frame(frame_t* frame);

// Helper function to get a human-readable message for an error code
const char* get_error_message(error_code error) { 
    switch (error) {
        case NET_ERR_SUCCESS: return "Success";
        case NET_ERR_NULL_POINTER: return "Error: A required pointer was NULL.";
        case NET_ERR_INVALID_PACKET_STRUCTURE: return "Error: Packet structure is invalid (e.g., payload size, total size).";
        case NET_ERR_MEMORY_ALLOCATION_FAILED: return "Error: Memory allocation failed.";
        case NET_ERR_EMPTY_MESSAGE: return "Error: Input message was empty.";
        case NET_ERR_WSA_STARTUP_FAILED: return "Error: Winsock startup failed.";
        case NET_ERR_SEND_FAILED: return "Error: Network send operation failed.";
        default: return "Error: Unknown error code.";
    }
}

void* create_packet(uint32_t type, uint32_t ID, const void* payload, uint32_t payload_size, error_code* error) {
    if (error == NULL) {
        return NULL; // Cannot report error if pointer is NULL
    }
    *error = NET_ERR_SUCCESS; // Assume success initially

    // Validate the input parameters
    if (payload_size == 0 || payload == NULL || sizeof(packet_header_t) + payload_size > UINT32_MAX) {
        *error = NET_ERR_INVALID_PACKET_STRUCTURE;
        return NULL;
    }

    // Allocate memory for the packet header and payload
    void* packet = malloc(sizeof(packet_header_t) + payload_size);
    if (packet == NULL) {
        *error = NET_ERR_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }

    // Fill in the header
    packet_header_t* header = (packet_header_t*)packet;
    
    // Convert header fields to network byte order (big-endian)
    header->type = htonl(type);
    header->ID = htonl(ID);
    header->payload_size = htonl(payload_size);
    header->packet_size = htonl(sizeof(packet_header_t) + payload_size); // Calculate then convert

    // Copy payload data
    memcpy((char*)packet + sizeof(packet_header_t), payload, payload_size);

    return packet; // Return the newly created packet
}

error_code send_packet_in_frames(SOCKET clientSocket, const void* packet) {
    // Validate the packet
    if (packet == NULL) {
        return NET_ERR_NULL_POINTER;
    }

    // Cast the packet to its header type for easier access
    const packet_header_t* header = (const packet_header_t*)packet;
    // Remember to convert from network byte order if you plan to use this 'header'
    // for logic that depends on original host values.
    // Here, we're just using it to get total_bytes_to_frame for framing.
    // If you need the *host* value of packet_size for framing, you'd do:
    // uint32_t total_bytes_to_frame = ntohl(header->packet_size);
    // For this sending loop, we can just use the value from the header as is for memcpy offsets,
    // assuming it was already in network byte order when created.
    // For calculating remaining bytes etc., it's safer to work with host byte order.
    uint32_t total_packet_size_host_order = ntohl(header->packet_size); // Convert to host byte order for calculations

    uint32_t bytes_remaining_to_frame = total_packet_size_host_order;
    uint32_t current_offset = 0;

    frame_t frame = {0}; // Initialize the frame
    uint32_t sequence_nr = 0; // Initialize frame count (using uint32_t for consistency)

    while (bytes_remaining_to_frame > 0) {
        sequence_nr++; // Increment sequence number
        frame.sequence_number = htonl(sequence_nr); // Convert to network byte order

        uint32_t current_frame_payload_size;
        if (bytes_remaining_to_frame < FRAME_PAYLOAD_SIZE) {
            current_frame_payload_size = bytes_remaining_to_frame;
        } else {
            current_frame_payload_size = FRAME_PAYLOAD_SIZE;
        }
        
        frame.payload_size = htonl(current_frame_payload_size); // Convert to network byte order
        
        // Calculate the total frame size *before* converting to network byte order for accuracy
        uint32_t total_frame_struct_size_host_order = sizeof(frame.sequence_number) + sizeof(frame.payload_size) + sizeof(frame.frame_size) + current_frame_payload_size;
        frame.frame_size = htonl(total_frame_struct_size_host_order); // Convert to network byte order

        // Copy actual payload data for this frame
        memcpy(frame.payload, (const char*)packet + current_offset, current_frame_payload_size);

        // Send the frame over the network
        // int bytes_sent = send(clientSocket, (char*)&frame, total_frame_struct_size_host_order, 0); // Use host order size for send()
        // if (bytes_sent == SOCKET_ERROR) {
        //     return NET_ERR_SEND_FAILED;
        // }

        // For debugging, print frame content (converting back to host order for display)
        frame_t print_dbg_frame = {0};
        print_dbg_frame.sequence_number = ntohl(frame.sequence_number);
        print_dbg_frame.payload_size = ntohl(frame.payload_size);
        print_dbg_frame.frame_size = ntohl(frame.frame_size);
        memcpy(print_dbg_frame.payload, frame.payload, current_frame_payload_size);
        print_frame(&print_dbg_frame); 
        
        bytes_remaining_to_frame -= current_frame_payload_size;
        current_offset += current_frame_payload_size;
    }
    return NET_ERR_SUCCESS;
}

void print_packet(const void* packet) {
    if (packet == NULL) {
        printf("Cannot print NULL packet.\n");
        return;
    }
    const packet_header_t* header = (const packet_header_t*)packet;
    
    // Convert fields from network byte order to host byte order for printing
    printf("Packet Type: %u\n", ntohl(header->type));
    printf("Packet ID: %u\n", ntohl(header->ID));
    printf("Payload Size: %u\n", ntohl(header->payload_size));
    printf("Packet Size: %u\n", ntohl(header->packet_size));

    // Access payload directly (it's raw data, no byte order conversion needed)
    // The payload size for memcpy is also converted back to host order for calculation
    uint32_t payload_size_host_order = ntohl(header->payload_size);
    if (payload_size_host_order > 0) {
        const char* payload = (const char*)packet + sizeof(packet_header_t);
        printf("Payload: ");
        for (uint32_t i = 0; i < payload_size_host_order; i++) {
            printf("%02X ", (unsigned char)payload[i]);
        }
        printf("\n");
    }
}

void print_frame(frame_t* frame) {
    if (frame == NULL) {
        printf("Cannot print NULL frame.\n");
        return;
    }
    // Note: This print_frame now expects *host byte order* values.
    // The calling send_packet_in_frames converts them back for printing.
    printf("\n--- Frame --- \n");
    printf("Frame Sequence Number: %u\n", frame->sequence_number); // Already host order from send_packet_in_frames debug copy
    printf("Frame Payload Size: %u\n", frame->payload_size);       // Already host order
    printf("Frame Size: %u\n", frame->frame_size);                 // Already host order
    printf("Frame Payload: ");
    for (uint32_t i = 0; i < frame->payload_size; i++) { // Use host order payload_size for loop
        printf("%02X ", (unsigned char)frame->payload[i]);
    }
    printf("\n");
}

int main() {
    error_code status;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "%s\n", get_error_message(NET_ERR_WSA_STARTUP_FAILED));
        return NET_ERR_WSA_STARTUP_FAILED;
    }

    void* packet = NULL;
    char out_message[1024] = {0};

    printf("\nEnter your message: ");
    fgets(out_message, sizeof(out_message), stdin);
    out_message[strcspn(out_message, "\n")] = '\0';

    if (strlen(out_message) == 0) {
        fprintf(stderr, "%s\n", get_error_message(NET_ERR_EMPTY_MESSAGE));
        WSACleanup();
        return NET_ERR_EMPTY_MESSAGE;
    }

    // Pass the address of 'status' to create_packet
    packet = create_packet(1, 1, out_message, strlen(out_message), &status);

    if (status != NET_ERR_SUCCESS) {
        fprintf(stderr, "Failed to create packet: %s\n", get_error_message(status));
        WSACleanup();
        return status;
    }

    printf("\n--- Original Packet --- \n");
    // print_packet now takes a packet with fields in network byte order
    print_packet(packet);

    printf("\n--- Sending Packet in Frames --- \n");
    status = send_packet_in_frames(INVALID_SOCKET, packet); // Placeholder for actual socket

    if (status != NET_ERR_SUCCESS) {
        fprintf(stderr, "Failed to send packet in frames: %s\n", get_error_message(status));
        free(packet);
        WSACleanup();
        return status;
    }

    printf("\nPacket sent in frames successfully.\n");

    free(packet);
    WSACleanup();
    return NET_ERR_SUCCESS;
}