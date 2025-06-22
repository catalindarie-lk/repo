#include <winsock2.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")  // Link against Winsock library

#define PORT 25000              // Port number to connect to

#define FRAME_SIZE 255  // Size of the frame
#define FRAME_PAYLOAD_SIZE (FRAME_SIZE - sizeof(frame_header_t)) // Adjusted for header size

#define MAX_USERNAME_SIZE 255   // Maximum size of username
#define MAX_PASSWORD_SIZE 255   // Maximum size of username and password
#define MAX_MESSAGE_SIZE 8192   // Maximum size of a message


typedef struct login_data{
    char username[MAX_USERNAME_SIZE]; // Username for login
    char password[MAX_PASSWORD_SIZE]; // Password for login
    int authenticated; // Authentication status (0 = not authenticated, 1 = authenticated)
}login_data_t;

#pragma pack(push, 1) // Ensure no padding in structures
typedef struct message_header{
    uint32_t type;   // 1 = login, 2 = message
    uint32_t length; // Length of the message in bytes
    uint32_t ID;    // Unique identifier for the message
}message_header_t;

typedef struct frame_header{
    uint32_t type;   // 1 = login, 2 = message
    uint32_t length; // Length of the message in bytes
    uint32_t sequence_number; // Sequence number for message ordering
}frame_header_t;
#pragma pack(pop) // Restore previous packing alignment

typedef struct message{
    message_header_t header; // Header containing metadata about the message
    char payload[MAX_MESSAGE_SIZE]; // Data payload of the message
}message_t;

typedef struct frame{
    frame_header_t header; // Header containing metadata about the frame
    char payload[FRAME_PAYLOAD_SIZE]; // Data payload of the frame
}frame_t;


void send_login(SOCKET clientSocket, login_data_t* client) {
    // Prepare the login frame
    message_t login_message;
    memset(&login_message, 0, sizeof(login_message)); // Clear the message

    while(!client->authenticated) {
        // Prompt user for username and password
        printf("Enter username: ");
        fgets(client->username, sizeof(client->username), stdin);
        client->username[strcspn(client->username, "\n")] = '\0'; // Remove newline

        printf("Enter password: ");
        fgets(client->password, sizeof(client->password), stdin);
        client->password[strcspn(client->password, "\n")] = '\0'; // Remove newline

        // For simplicity, we assume authentication is successful
        client->authenticated = 1; // Set authenticated to true
    } 

    // Set the header values
    login_message.header.type = 1; // Login type
    login_message.header.length = sizeof(message_t);
    login_message.header.ID = 999; // Example sequence number

    // Copy username and password into the payload
    strncpy(login_frame.payload, client->username, MAX_USERNAME_SIZE);
    strncpy(login_frame.payload + MAX_USERNAME_SIZE, client->password, MAX_PASSWORD_SIZE);

        
    // Send the login frame to the server
    int bytesSent = send(clientSocket, (char*)&login_frame, sizeof(login_frame), 0);
    if (bytesSent == SOCKET_ERROR) {
        printf("Failed to send login data: %d\n", WSAGetLastError());
        return;
    }
    
    printf("Login data sent successfully.\n");
}

void send_message(SOCKET clientSocket, const char* message) {
    // Prepare the message frame
    frame_t message_frame;
    memset(&message_frame, 0, sizeof(message_frame)); // Clear the frame

    // Set the header values
    message_frame.header.type = 2; // Message type
    message_frame.header.length = strlen(message);
    message_frame.header.sequence_number = 1; // Example sequence number

    // Copy the message into the payload
    strncpy(message_frame.payload, message, FRAME_PAYLOAD_SIZE);

    // Send the message frame to the server
    int bytesSent = send(clientSocket, (char*)&message_frame, sizeof(message_frame), 0);
    if (bytesSent == SOCKET_ERROR) {
        printf("Failed to send message: %d\n", WSAGetLastError());
        return;
    }
    
    printf("Message sent successfully: %s\n", message);
}


void send_message_frames(SOCKET clientSocket, message_t* message) {
    // Send the frame to the server
    char* buffer = malloc(sizeof(message_t));
    if (buffer == NULL) {
        printf("Memory allocation failed for buffer.\n");
        return;
    }
    memcpy((char* )buffer, (char* )message, sizeof(message_t)); // Copy the message to the buffer 
    frame_t frame = {0}; // Initialize the frame
    int frame_count = 0; // Initialize frame count

    int bytes_to_send = message->header.length;
    while(bytes_to_send > 0){
        // Prepare the frame
        frame.header.type = 123; // Set the type from the message header
        frame.header.length = bytes_to_send < FRAME_PAYLOAD_SIZE ? bytes_to_send : FRAME_PAYLOAD_SIZE; // Set length
        frame.header.sequence_number = ++frame_count; // Sequence number for ordering
        // Copy the payload from the message to the frame
        memcpy(frame.payload, buffer + (message->header.length - bytes_to_send), frame.header.length);
        // Send the frame
        print_frame(&frame);
        bytes_to_send -= frame.header.length; // Decrease remaining bytes to send
    }
}

print_frame(frame_t* frame) {
    printf("\nFrame Type: %u", frame->header.type);
    printf("\nFrame Length: %u", frame->header.length);
    printf("\nFrame Sequence Number: %u", frame->header.sequence_number);
    printf("\nFrame Payload: %.*s\n", frame->header.length, frame->payload);
}



int main() {
    WSADATA wsaData;
    SOCKET clientSocket;
    struct sockaddr_in serverAddr;

    login_data_t client;
    char out_message[MAX_MESSAGE_SIZE] = {0};       

    int connected = 0;
    int bytesSent;
    
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed!\n");
        return 1;
    }

    // Create a socket
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET) {
        printf("Socket creation failed!\n");
        WSACleanup();
        return 1;
    }

    // Configure server address (loopback only)
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);  // Match the server's port
    if (InetPton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) != 1) {  // Connect to localhost
        printf("Invalid address/ Address not supported\n");
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    // Connect to server
    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("Connection to server failed!\n");
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }else{
        printf("Connected to server!\n");
        connected = 1;
    }
  

    while(connected){
        printf("\nWrite message:");
        // Clear the output message buffer
        memset(out_message, 0, sizeof(out_message));
        // Read input from user
        // fgets reads a line from stdin and stores it in out_message
        fgets(out_message,sizeof(out_message),stdin);
        // Remove newline character from the end of the string
        // strcspn finds the first occurrence of any character in the second string
        out_message[strcspn(out_message, "\n")] = '\0';
        // Check if the user wants to disconnect
        // If the user types "disconnect", set connected to 0 to exit the loop
        if (strcmp(out_message,"disconnect") == 0){
            connected = 0;
            break;
        }        
        // Send message to server
        if (strlen(out_message) == 0) {
            printf("Empty message, please enter a valid message.\n");
            continue; // Skip sending if the message is empty
        }
        // Send the message to the server
        // send() sends data to the connected socket
        bytesSent = send(clientSocket, out_message,strlen(out_message), 0);
        if (bytesSent == -1) {
            printf("\nsend() failed");
            connected = 0;
            break;
        } else {
            // If send is successful, print the message and number of bytes sent
            // printf() prints formatted output to the console
            printf("\nMessage sent: \"%s\"",out_message);
            printf(" : Sent %d bytes:\n", bytesSent);
        }
    }

    // Cleanup
    printf("\nClosing connection...");
    closesocket(clientSocket);
    WSACleanup();

    return 0;
}