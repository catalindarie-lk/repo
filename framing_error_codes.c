#include <winsock2.h>
#include <stdio.h> // Still needed for main's output, but removed from library functions
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
    switch (error) { // Used 'error' here
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

void* create_packet(uint32_t type, uint32_t ID, const void* payload, uint32_t payload_size, error_code* error) { // Parameter name changed to 'error'
    // Always initialize out_error to success, then change it on error
    if (error == NULL) { // Used 'error' here
        return NULL; // Cannot report error if pointer is NULL
    }
    *error = NET_ERR_SUCCESS; // Used 'error' here

    // Validate the input parameters
    if (payload_size == 0 || payload == NULL || sizeof(packet_header_t) + payload_size > UINT32_MAX) {
        *error = NET_ERR_INVALID_PACKET_STRUCTURE; // Used 'error' here
        return NULL;
    }

    // Allocate memory for the packet header and payload
    void* packet = malloc(sizeof(packet_header_t) + payload_size);
    if (packet == NULL) {
        *error = NET_ERR_MEMORY_ALLOCATION_FAILED; // Used 'error' here
        return NULL;
    }

    // Fill in the header
    packet_header_t* header = (packet_header_t*)packet;
    header->type = type;
    header->ID = ID;
    header->payload_size = payload_size;
    header->packet_size = sizeof(packet_header_t) + payload_size;

    // Copy payload data
    memcpy((char*)packet + sizeof(packet_header_t), payload, payload_size);

    return packet; // Return the newly created packet
}

error_code send_packet_in_frames(SOCKET clientSocket, const void* packet) {
    // Validate the packet
    if (packet == NULL) {
        return NET_ERR_NULL_POINTER;
    }

    const packet_header_t* header = (const packet_header_t*)packet;
    uint32_t total_bytes_to_frame = header->packet_size;
    uint32_t bytes_remaining_to_frame = total_bytes_to_frame;
    uint32_t current_offset = 0;

    frame_t frame = {0}; // Initialize the frame
    int sequence_nr = 0; // Initialize frame count

    while (bytes_remaining_to_frame > 0) {
        frame.sequence_number = ++sequence_nr;

        if (bytes_remaining_to_frame < FRAME_PAYLOAD_SIZE) {
            frame.payload_size = bytes_remaining_to_frame;
        } else {
            frame.payload_size = FRAME_PAYLOAD_SIZE;
        }
        
        frame.frame_size = sizeof(frame.sequence_number) + sizeof(frame.payload_size) + sizeof(frame.frame_size) + frame.payload_size;

        memcpy(frame.payload, (const char*)packet + current_offset, frame.payload_size);

        // Send the frame over the network
        // int bytes_sent = send(clientSocket, (char*)&frame, frame.frame_size, 0);
        // if (bytes_sent == SOCKET_ERROR) {
        //     return NET_ERR_SEND_FAILED;
        // }

        print_frame(&frame); // For debugging
        bytes_remaining_to_frame -= frame.payload_size;
        current_offset += frame.payload_size;
    }
    return NET_ERR_SUCCESS;
}

void print_packet(const void* packet) {
    if (packet == NULL) {
        printf("Cannot print NULL packet.\n");
        return;
    }
    const packet_header_t* header = (const packet_header_t*)packet;
    printf("Packet Type: %u\n", header->type);
    printf("Packet ID: %u\n", header->ID);
    printf("Payload Size: %u\n", header->payload_size);
    printf("Packet Size: %u\n", header->packet_size); // Corrected: header->packet_size

    if (header->payload_size > 0) {
        const char* payload = (const char*)packet + sizeof(packet_header_t);
        printf("Payload: ");
        for (uint32_t i = 0; i < header->payload_size; i++) {
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
    printf("\n--- Frame --- \n");
    printf("Frame Sequence Number: %u\n", frame->sequence_number);
    printf("Frame Payload Size: %u\n", frame->payload_size);
    printf("Frame Size: %u\n", frame->frame_size);
    printf("Frame Payload: ");
    for (uint32_t i = 0; i < frame->payload_size; i++) {
        printf("%02X ", (unsigned char)frame->payload[i]);
    }
    printf("\n");
}

int main() {
    error_code status; // Renamed variable for consistency

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