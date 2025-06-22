// cl.exe udp_threadpool.c ws2_32.lib
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 12345
#define BUFFER_SIZE 512
#define WORKER_COUNT 4

typedef struct {
    SOCKET sock;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_len;
} Task;

CRITICAL_SECTION cs;
CONDITION_VARIABLE cv;
Task task_queue[100];
int queue_front = 0, queue_back = 0;
volatile int queue_count = 0;

void enqueue(Task task) {
    EnterCriticalSection(&cs);
    task_queue[queue_back] = task;
    queue_back = (queue_back + 1) % 100;
    queue_count++;
    LeaveCriticalSection(&cs);
    WakeConditionVariable(&cv);
}

Task dequeue() {
    EnterCriticalSection(&cs);
    while (queue_count == 0) {
        SleepConditionVariableCS(&cv, &cs, INFINITE);
    }
    Task task = task_queue[queue_front];
    queue_front = (queue_front + 1) % 100;
    queue_count--;
    LeaveCriticalSection(&cs);
    return task;
}

DWORD WINAPI worker_thread(LPVOID lpParam) {
    while (1) {
        Task task = dequeue();
        printf("Worker: Received '%s'\n", task.buffer);
        sendto(task.sock, task.buffer, strlen(task.buffer), 0,
               (struct sockaddr*)&task.client_addr, task.client_len);
    }
    return 0;
}

int main() {
    WSADATA wsaData;
    SOCKET udp_sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    WSAStartup(MAKEWORD(2, 2), &wsaData);

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(udp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    printf("UDP server listening on port %d...\n", PORT);

    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv);

    // Start thread pool
    for (int i = 0; i < WORKER_COUNT; i++) {
        CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    while (1) {
        Task task;
        task.sock = udp_sock;
        task.client_len = sizeof(task.client_addr);
        int len = recvfrom(udp_sock, task.buffer, BUFFER_SIZE - 1, 0,
                           (struct sockaddr*)&task.client_addr, &task.client_len);
        if (len > 0) {
            task.buffer[len] = '\0';
            enqueue(task);
        }
    }

    DeleteCriticalSection(&cs);
    closesocket(udp_sock);
    WSACleanup();
    return 0;
}