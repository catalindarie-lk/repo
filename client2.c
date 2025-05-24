#include <winsock2.h>
#include <stdio.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")  // Link against Winsock library

#define PORT 25000  

int main() {
    WSADATA wsaData;
    SOCKET clientSocket;
    struct sockaddr_in serverAddr;
    char out_message[1024] = {0};
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
    serverAddr.sin_family = AF_INET;
    if (InetPton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) != 1) {
        printf("Invalid address/ Address not supported\n");
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    serverAddr.sin_port = htons(PORT);  // Match the server's port

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

    bytesSent = send(clientSocket,"CONNECT ID = CL1",strlen(out_message), 0);

    while(connected){       
        printf("\nWrite message:");
        memset(out_message, 0, sizeof(out_message));
        fgets(out_message,sizeof(out_message),stdin);
        out_message[strcspn(out_message, "\n")] = '\0';
        if (strcmp(out_message,"disconnect") == 0){
            connected = 0;
            break;
        }        
        bytesSent = send(clientSocket, out_message,strlen(out_message), 0);
        if (bytesSent == -1) {
            printf("\nsend() failed");
            connected = 0;
            break;
        } else {
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