#ifndef SERVER_STATISTICS_H
#define SERVER_STATISTICS_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>       // For _countof
#include <stdio.h>       // For sprintf_s
#include <stdint.h>      // For uint64_t
#include <commctrl.h>    // For progress bar control (PROGRESS_CLASSA, PBM_ messages)

// --- ServerStatistics Struct ---
typedef struct {
    uint64_t connected_clients;
    double fstream_progress[10];
    uint64_t fstream_session_id[10];
} ServerStatistics;

// --- Custom Message ID for Server GUI ---
#define WM_UPDATE_SERVER_DISPLAY (WM_USER + 2) // Using +2 to avoid conflict if WM_USER+1 is used elsewhere

// Helper macros (copied from client_statistics.h for self-containment)
#define CREATE_STATIC_LABEL(parent, x, y, width, height, text) \
    CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, \
                 x, y, width, height, parent, NULL, GetModuleHandle(NULL), NULL)

#define CREATE_VALUE_EDIT(parent, x, y, width, height, id, hFont) \
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", \
                   WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, \
                   x, y, width, height, parent, (HMENU)id, GetModuleHandle(NULL), NULL); \
    if(hFont) SendMessage(GetDlgItem(parent, id), WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

#define CREATE_PROGRESS_BAR(parent, x, y, width, height, id) \
    CreateWindowExA(0, PROGRESS_CLASSA, NULL, \
                   WS_CHILD | WS_VISIBLE | PBS_SMOOTH, \
                   x, y, width, height, parent, (HMENU)id, GetModuleHandle(NULL), NULL);

// --- Global Data and Synchronization (declarations for variables defined in server_statistics.c) ---
extern ServerStatistics g_serverStats; // The shared server data
extern CRITICAL_SECTION g_statsLock;   // To protect g_serverStats

extern HWND hServerMainWindow;
extern HANDLE hServerGuiThread;
extern HANDLE hServerMainLogicThread; // Assuming your server also has a main logic thread

// --- Global Handles to GUI Controls ---
extern HWND g_hConnectedClientsEdit;

// Arrays for Fstream controls (10 entries)
extern HWND g_hFstreamProgressEdit[10];
extern HWND g_hFstreamProgressBar[10];
extern HWND g_hFstreamSessionIDEdit[10];

// Function to initialize the server statistics GUI
int init_server_statistics_gui();

#endif // SERVER_STATISTICS_H