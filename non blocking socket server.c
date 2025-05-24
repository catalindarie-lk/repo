#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 25000
#define MAX_CLIENTS 10

WSAEVENT eventArray[MAX_CLIENTS];
SOCKET clientSockets[MAX_CLIENTS];

void HandleClient(SOCKET client, int index) {
    char buffer[512];
    int bytesReceived = recv(client, buffer, sizeof(buffer), 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        printf("Client %d says: %s\n", index, buffer);
    } 
    else if (bytesReceived == 0) {
        printf("Client disconnected.\n");
        closesocket(client);
        clientSockets[index] = 0;
    } 
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        // No data yet, wait for FD_READ event again
        return;
    } 
    else {
        printf("recv failed, error: %d\n", WSAGetLastError());
    }
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET listening = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");  // Loopback only

    bind(listening, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listening, SOMAXCONN);

    WSAEVENT listenEvent = WSACreateEvent();
    WSAEventSelect(listening, listenEvent, FD_ACCEPT);

    while (1) {
        WSAWaitForMultipleEvents(1, &listenEvent, FALSE, INFINITE, FALSE);
        
        SOCKET client = accept(listening, NULL, NULL);
        if (client != INVALID_SOCKET) {
            printf("New client connected!\n");

            WSAEVENT clientEvent = WSACreateEvent();
            WSAEventSelect(client, clientEvent, FD_READ | FD_CLOSE);

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clientSockets[i] == 0) {
                    clientSockets[i] = client;
                    eventArray[i] = clientEvent;
                    break;
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clientSockets[i] != 0) {
                WSAWaitForMultipleEvents(1, &eventArray[i], FALSE, 0, FALSE);
                HandleClient(clientSockets[i], i);
            }
        }
    }

    closesocket(listening);
    WSACleanup();
    return 0;
}