#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 25000
#define MAX_THREADS 5   // Number of worker threads
#define MAX_QUEUE_SIZE 5  // Max pending clients in the queue
#define BUFFER_SIZE 10;


// Mutex and condition variable for client queue
HANDLE mutex;
HANDLE clientSemaphore;

// Simple user database
typedef struct {
    char username[50];
    char password[50];
} User;

// Struct for client context
typedef struct {
    SOCKET clientSocket;
    User user; // User information
    int authenticated;
} ClientContext;



User users[] = {
    {"user1", "password123"},
    {"admin", "securepass"},
    {"guest", "guest123"}
};

// Client queue
ClientContext* clientQueue[MAX_QUEUE_SIZE];
int queueCount = 0;

// Function to authenticate user
int authenticate(const char* username, const char* password) {
    for (int i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
        if (strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, password) == 0) {
            return 1; // Success
        }
    }
    return 0; // Failed
}

// Worker thread function
DWORD WINAPI WorkerThread(void* arg) {
    (void)arg; // Unused parameter

    while (1) {
        WaitForSingleObject(clientSemaphore, INFINITE); // Wait for a client
        WaitForSingleObject(mutex, INFINITE);

        // Check if there are clients in the queue
        if (queueCount < 0) {
            queueCount = 0; // Reset queue count if it was less than 0
            printf("No clients in queue.\n");
            // If no clients, release mutex and continue
            ReleaseMutex(mutex);
            continue;}

        // Get the client from the queue
        if (clientQueue[queueCount] == NULL) {
            printf("No valid client in queue.\n");
            ReleaseMutex(mutex);
            continue;
        }
        ClientContext* context = clientQueue[queueCount];
        clientQueue[queueCount] = NULL; // Clear the slot
        queueCount--; // Decrease queue count
        if (queueCount < 0) {
            queueCount = 0; // Reset queue count if it was less than 0
        }
        ReleaseMutex(mutex);

        if (!context){            
            printf("No valid client in queue.\n");
            ReleaseMutex(mutex);
            continue;
        }
        // Process the client
        
        SOCKET client = context->clientSocket;
        char buffer[512];

        // Receive username
        int bytesReceived = recv(client, context->user.username, sizeof(context->user.username) - 1, 0);
        if (bytesReceived <= 0) {
            printf("Client disconnected before authentication.\n");
            closesocket(client);
            free(context);
            continue;
        }
        context->user.username[bytesReceived] = '\0';

        // Receive password
        bytesReceived = recv(client, context->user.password, sizeof(context->user.password) - 1, 0);
        if (bytesReceived <= 0) {
            printf("Client disconnected before authentication.\n");
            closesocket(client);
            free(context);
            continue;
        }
        context->user.password[bytesReceived] = '\0';

        // Authenticate user
        if (!authenticate(context->user.username, context->user.password)) {
            printf("Authentication failed for user: %s\n", context->user.username);
            send(client, "Authentication failed\n", 22, 0);
            closesocket(client);
            free(context);
            continue;
        }

        context->authenticated = 1;
        send(client, "Authentication successful\n", 26, 0);
        printf("User authenticated: %s\n", context->user.username);

        int value_received = 0;
        int nr_bytes_received = 0;
        char bytes_buffer[5] = {0}; // Clear buffer
        // Message loop
        while (1) {
            memset(bytes_buffer, 0, sizeof(bytes_buffer)); // Clear buffer
            // Receive message from client
            // recv() receives data from the connected socket
            nr_bytes_received = recv(client, bytes_buffer, sizeof(bytes_buffer) - 1, 0);
            printf("Nr of bytes received: %d\n", nr_bytes_received);
            if (nr_bytes_received <= 0) {
                printf("%s disconnected.\n", context->user.username);
                break;
            }
            if (nr_bytes_received >= sizeof(bytes_buffer)) {
                printf("Received data too large for buffer, truncating.\n");
                nr_bytes_received = sizeof(bytes_buffer) - 1; // Prevent overflow
            }
            bytes_buffer[nr_bytes_received] = '\0'; // Null-terminate the string
            value_received = atoi(bytes_buffer); // Convert string to integer
            printf("Received value: %d from %s\n", value_received, context->user.username);
        }

        closesocket(client);        
        free(context);
    }
    return 0;
}

// Accept thread function
DWORD WINAPI AcceptThread(void* arg) {
    SOCKET listening = *(SOCKET* )arg;

    while (1) {
        SOCKET client = accept(listening, NULL, NULL);
        if (client != INVALID_SOCKET) {
            printf("New client attempting to connect...\n");

            WaitForSingleObject(mutex, INFINITE);
            if (queueCount >= MAX_QUEUE_SIZE) {
                printf("Queue full! Rejecting connection.\n");
                closesocket(client);
            } else {
                // Allocate context and add to queue
                ClientContext* context = (ClientContext *)malloc(sizeof(ClientContext));
                if (!context) {
                    printf("Memory allocation failed!\n");
                    closesocket(client);
                } else {
                    if (queueCount < 0) {
                        queueCount = 0; // Reset queue count if it was less than 0
                    }
                    // Initialize context
                    memset(context, 0, sizeof(ClientContext));
                    context->clientSocket = INVALID_SOCKET; // Initialize to invalid socket
                    context->authenticated = 0;
                    memset(context->user.username,0, sizeof(context->user.username)); // Initialize username                  
                    // Assign client socket
                    context->clientSocket = client;
                    context->authenticated = 0;
                    clientQueue[++queueCount] = context;                    
                    ReleaseSemaphore(clientSemaphore, 1, NULL); // Signal worker thread
                }
            }
            ReleaseMutex(mutex);
        }
    }
    return 0;
}

int main() {
    // Initialize Winsock
    // Initialize Winsock library
    WSADATA wsaData;
    // Check if WSAStartup was successful
    // If WSAStartup fails, print an error message and exit
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    // Check if Winsock version is supported
    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        printf("Winsock version not supported!\n");
        WSACleanup();
        return 1;
    }

    // Initialize mutex and semaphore
    mutex = CreateMutex(NULL, FALSE, NULL);
    clientSemaphore = CreateSemaphore(NULL, 0, MAX_QUEUE_SIZE, NULL);
    // Check if mutex and semaphore were created successfully
    // If either mutex or semaphore creation fails, print an error message and exit
    if (mutex == NULL || clientSemaphore == NULL) {
        printf("Mutex or semaphore creation failed!\n");
        WSACleanup();
        return 1;
    }
    // Create a listening socket
    // Create a socket for listening to incoming connections
    // If the socket creation fails, print an error message and exit
    SOCKET listening = socket(AF_INET, SOCK_STREAM, 0);
    if (listening == INVALID_SOCKET) {
        printf("Socket creation failed!\n");
        WSACleanup();
        return 1;
    }

    struct sockaddr_in serverAddr = {0};
    // Configure server address (loopback only)    
    // Set the address family to IPv4
    serverAddr.sin_family = AF_INET;
    // Set the port number
    serverAddr.sin_port = htons(PORT);
    // Convert IPv4 and IPv6 addresses from text to binary form
    // Use InetPton to convert the address from string to binary form
    InetPton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    // Bind and listen
    // Bind the socket to the address and port
    bind(listening, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    // Listen for incoming connections
    // Set the socket to listen for incoming connections
    listen(listening, SOMAXCONN);
    if (listening == INVALID_SOCKET) {
        printf("Socket binding failed!\n");
        WSACleanup();
        return 1;
    }
    // Print server listening message
    printf("Server listening on port %d...\n", PORT);

    // Create worker threads
    HANDLE workerThreads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++) {
        workerThreads[i] = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    }

    // Start accept thread
    HANDLE acceptThread = CreateThread(NULL, 0, AcceptThread, (void* )&listening, 0, NULL);
    if (acceptThread != NULL) {
        CloseHandle(acceptThread);
    }

    // Keep server running
    while (1) {
        Sleep(1000);
    }

    // Cleanup
    CloseHandle(mutex);
    CloseHandle(clientSemaphore);
    closesocket(listening);
    WSACleanup();
    return 0;
}
