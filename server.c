#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <process.h>  // For _beginthread and _endthread

#pragma comment(lib, "ws2_32.lib")

#define PORT 25000
#define BUFFER_SIZE 1024
#define NAME_SIZE 50
#define PASSWORD_SIZE 50

volatile int server_running = 1;
volatile int client_counter = 0;

typedef struct {
    SOCKET clientSocket;
    int clientID;
    char name[NAME_SIZE];
    char password[PASSWORD_SIZE];
    int sts;
}s_clientData;

void userAuthenticateHandler(void* clientData);
void serverCloseHandler(void *serverSocketPtr);
void userMessageHandler(void* clientData);

void startServer(void* serverSocket){

    
    WSADATA wsaData;
    struct sockaddr_in serverAddr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        exit(1);
    }

    *(SOCKET *)serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (*(SOCKET *)serverSocket == INVALID_SOCKET) {
        printf("Socket creation failed.\n");
        WSACleanup();
        exit(0);
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");  // Loopback only
    serverAddr.sin_port = htons(PORT);

    if (bind(*(SOCKET *)serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("Bind failed.\n");
        closesocket(*(SOCKET *)serverSocket);
        WSACleanup();
        exit(0);
    }

    if (listen(*(SOCKET *)serverSocket, 5) == SOCKET_ERROR) {
        printf("Listen failed.\n");
        closesocket(*(SOCKET *)serverSocket);
        WSACleanup();
        exit(0);
    }
    printf("Server listening on port %d...\n", PORT);
    _beginthread(serverCloseHandler, 0, (SOCKET *)serverSocket); 
    

}


void userAuthenticateHandler(void* clientData){
    s_clientData* data = (s_clientData* )clientData; 

    char client_name[NAME_SIZE] = {0};
    char client_password[PASSWORD_SIZE] = {0};

    strcpy(client_name,data->name);
    strcpy(client_password,data->password);
    printf("\n%s\n",client_name);
    printf("\n%s\n",client_password);
    while(1){
    if ((strcmp(client_name,"ABC") == 0) && (strcmp(client_password,"abc") == 0)){
        ((s_clientData*)clientData)->sts = 1;
        printf("\nClient OK\n");
        break;
    } else {
        ((s_clientData*)clientData)->sts = 0;
        printf("\nClient NOK\n");
        continue;
    } 
    }
    _endthread();

}



void userMessageHandler(void* clientData) {
    
    s_clientData* data = (s_clientData* )clientData; 
    SOCKET clientSocket = data->clientSocket;
    int clientID = (int)data->clientID;

    int connection_open = 1;
    int bytesReceived;
    char in_message[BUFFER_SIZE];
    char out_message[BUFFER_SIZE] = {0};

    printf("\nClient %d connected!", clientID);
    ((s_clientData*)clientData)->sts = 0;

    while (1){
        bytesReceived = recv(clientSocket, in_message, sizeof(in_message) - 1, 0);
        if (bytesReceived > 0) {
            in_message[bytesReceived] = '\0';          
            printf("\nMessage received from client %d: \"%s\"", clientID, in_message);
            printf(" : Received %d bytes",bytesReceived);
            ((s_clientData*)clientData)->sts++;
        } else if (bytesReceived == 0){
            printf("\nClient %d disconnected!", clientID);
            break;
        } else {
            int clientError = WSAGetLastError();
            printf("\nClient %d connection error %d!", clientID, clientError);
            break;
        }
    }
    printf("\nServer closed connection with client %d! ",clientID);
    closesocket(clientSocket);
    _endthread();
}

void serverCloseHandler(void *serverSocketPtr) {
    SOCKET serverSocket = *(SOCKET *)serverSocketPtr;
    char input[BUFFER_SIZE];

    while (server_running) {
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = '\0'; // Remove newline character

        if (strcmp(input, "close") == 0) {
            printf("\nClosing server...\n");
            server_running = 0;
            closesocket(serverSocket);
            break;
        }
    }

    _endthread();
}

int main() {
        
    SOCKET serverSocket;
    SOCKET clientSocket;
    struct sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    
    s_clientData* clientData[100];
   
    startServer(&serverSocket);
  


    while (server_running) {

 

        clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET) {
            if (server_running) {
                printf("Accept failed.\n");
            }
            break;
        }
        client_counter++;
        clientData[client_counter] = (s_clientData* )malloc(sizeof(s_clientData));
        if (clientData[client_counter] == NULL) {
            printf("Memory allocation failed.\n");
            closesocket(clientSocket);
            break;
        }
        clientData[client_counter]->sts = 0;
        clientData[client_counter]->clientSocket = clientSocket;
        clientData[client_counter]->clientID = client_counter;
        strcpy(clientData[client_counter]->name, "ABC");
        strcpy(clientData[client_counter]->password, "abc");
        printf("\nClient %d authenticated!", client_counter);
        _beginthread(userMessageHandler, 0, (void* )clientData[client_counter]);          

        
    }
    printf("\nNR of Messages:%d\n",clientData[1]->sts);
   // free();
    WSACleanup();
    
    printf("Server has been shut down.\n");

    return 0;
}


