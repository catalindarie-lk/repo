#ifndef CLIENT_STATISTICS_H
#define CLIENT_STATISTICS_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>       // Still include tchar.h as it defines useful _countof
#include <stdio.h>       // For sprintf_s
#include <stdint.h>      // For uint64_t
#include <commctrl.h>    // NEW: For progress bar control (PROGRESS_CLASSA, PBM_ messages)

// --- Your ClientStatistics Struct ---
typedef struct {
    uint64_t client_session_ID;

    // --- CHANGED: Use an array for fstream progress ---
    double fstream_progress[5];

    uint64_t files_pending_for_streaming;

    uint64_t queue_send_frames_pending;
    uint64_t queue_send_prio_frames_pending;
    uint64_t queue_send_ctrl_frames_pending;
    uint64_t hash_table_send_frames_pending;

    uint64_t queue_recv_frames_pending;
    uint64_t queue_recv_prio_frames_pending;

    uint64_t pool_send_frames_free_blocks;
    uint64_t pool_send_frames_total_blocks;
    uint64_t pool_iocp_send_context_free_blocks;
    uint64_t pool_iocp_send_context_total_blocks;

    uint64_t pool_iocp_recv_context_free_blocks;
    uint64_t pool_iocp_recv_context_total_blocks;
} ClientStatistics;


// --- Custom Message ID ---
#define WM_UPDATE_DISPLAY (WM_USER + 1)


// Helper macro to create static text labels
#define CREATE_STATIC_LABEL(parent, x, y, width, height, text) \
    CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, \
                 x, y, width, height, parent, NULL, GetModuleHandle(NULL), NULL)

// Helper macro to create read-only edit controls for values
#define CREATE_VALUE_EDIT(parent, x, y, width, height, id, hFont) \
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", \
                   WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, \
                   x, y, width, height, parent, (HMENU)id, GetModuleHandle(NULL), NULL); \
    if(hFont) SendMessage(GetDlgItem(parent, id), WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

// --- NEW: Helper macro to create a progress bar ---
#define CREATE_PROGRESS_BAR(parent, x, y, width, height, id) \
    CreateWindowExA(0, PROGRESS_CLASSA, NULL, \
                   WS_CHILD | WS_VISIBLE | PBS_SMOOTH, \
                   x, y, width, height, parent, (HMENU)id, GetModuleHandle(NULL), NULL);


// --- Global Handles (declarations for variables defined in client_statistics.c) ---
extern ClientStatistics g_clientStats; // The shared data
extern CRITICAL_SECTION g_statsLock;   // To protect g_clientStats

extern HWND hMainWindow;
extern HANDLE hGuiThread;
extern HANDLE hMainLogicThread;

extern HWND g_hClientSessionID;

// --- CHANGED: Use arrays for Fstream edit controls and progress bars ---
extern HWND g_hFstreamEdit[5];
extern HWND g_hFstreamProgressBar[5];

extern HWND g_hFilesPendingEdit;

extern HWND g_hQSFPendingEdit;
extern HWND g_hQSPFPendingEdit;
extern HWND g_hQSCFPendingEdit;
extern HWND g_hHTSFPendingEdit;

extern HWND g_hQRFPendingEdit;
extern HWND g_hQRPFPendingEdit;

extern HWND g_hPSFFreeEdit;
extern HWND g_hPSFTotalEdit;
extern HWND g_hPIOCPSendFreeEdit;
extern HWND g_hPIOCPSendTotalEdit;

extern HWND g_hPIOCPRecvFreeEdit;
extern HWND g_hPIOCPRecvTotalEdit;

int init_statistics_gui();

#endif // CLIENT_STATISTICS_H