#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define SERVER_IP "127.0.0.1"
#define PORT 8080
#define BUFFER_SIZE 1024
#define TIMEOUT 2
#define WINDOW_SIZE 5  // Number of unacknowledged packets before pausing

typedef struct {
    int sequence_num;
    char data[BUFFER_SIZE];
} Packet;

SOCKET client_socket;
struct sockaddr_in server_addr;
int ack_buffer[10000] = {0}; // Tracks received ACKs
HANDLE ackThread;
CRITICAL_SECTION lock; // Synchronization for ACK buffer

DWORD WINAPI receive_acks(LPVOID param) {
    int ack;
    int addr_len = sizeof(server_addr);
    while (1) {
        if (recvfrom(client_socket, (char*)&ack, sizeof(ack), 0, (struct sockaddr*)&server_addr, &addr_len) > 0) {
            EnterCriticalSection(&lock);
            ack_buffer[ack] = 1; // Mark packet as acknowledged
            LeaveCriticalSection(&lock);
            printf("ACK received for packet #%d\n", ack);
        }
    }
    return 0;
}

void send_file(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("File open failed");
        return;
    }

    Packet packet;
    int sequence = 0;

    while (fread(packet.data, 1, BUFFER_SIZE, file)) {
        packet.sequence_num = sequence;
        sendto(client_socket, (char*)&packet, sizeof(packet), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        printf("Sent packet #%d\n", sequence);

        Sleep(TIMEOUT * 1000); // Simulate delay

        EnterCriticalSection(&lock);
        if (ack_buffer[sequence] == 0) {
            printf("Timeout! Resending packet #%d\n", sequence);
            sendto(client_socket, (char*)&packet, sizeof(packet), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        }
        LeaveCriticalSection(&lock);

        sequence++;
    }

    fclose(file);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    InitializeCriticalSection(&lock);
    ackThread = CreateThread(NULL, 0, receive_acks, NULL, 0, NULL);

    send_file("file.txt");

    DeleteCriticalSection(&lock);
    closesocket(client_socket);
    WSACleanup();
    return 0;
}