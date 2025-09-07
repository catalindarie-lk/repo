#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdint.h>
#include <commctrl.h> // Required for progress bar and InitCommonControlsEx

#include "include/client_statistics.h"
#include "include/client.h" // Assuming this defines 'Client' and 'Buffers' structs

// --- Global Data and Synchronization (DEFINITIONS) ---
ClientStatistics g_clientStats; // The shared data
CRITICAL_SECTION g_statsLock;   // To protect g_clientStats

HWND hMainWindow = NULL;
HANDLE hGuiThread;
HANDLE hMainLogicThread;

// --- Global Handles to Edit Controls (DEFINITIONS) ---
HWND g_hClientSessionID = NULL;

// --- CHANGED: Define arrays for Fstream edit controls and progress bars ---
HWND g_hFstreamEdit[5] = {0}; // Initialize all to NULL
HWND g_hFstreamProgressBar[5] = {0}; // Initialize all to NULL

HWND g_hFilesPendingEdit = NULL;

HWND g_hQSFPendingEdit = NULL;
HWND g_hQSPFPendingEdit = NULL;
HWND g_hQSCFPendingEdit = NULL;
HWND g_hHTSFPendingEdit = NULL;

HWND g_hQRFPendingEdit = NULL;
HWND g_hQRPFPendingEdit = NULL;

HWND g_hPSFFreeEdit = NULL;
HWND g_hPSFTotalEdit = NULL;
HWND g_hPIOCPSendFreeEdit = NULL;
HWND g_hPIOCPSendTotalEdit = NULL;

HWND g_hPIOCPRecvFreeEdit = NULL;
HWND g_hPIOCPRecvTotalEdit = NULL;


// --- Function Prototypes (already in header, but good to have here too for internal use) ---
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
DWORD WINAPI GuiThread(LPVOID lpParam);
DWORD WINAPI MainLogicThread(LPVOID lpParam);

int labelWidth = 180;
int valueWidth = 100;
int progressBarWidth = 150; // Smaller width for horizontal layout
int controlHeight = 15;     // Common height for labels, edits, and progress bars
int spacing = 3;            // Small spacing between elements


// ------------------------------------------------------------------
// Window Procedure for the main window
// ------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            // int labelWidth = 180;
            // int valueWidth = 100;
            // int progressBarWidth = 150; // Smaller width for horizontal layout
            // int controlHeight = 15;     // Common height for labels, edits, and progress bars
            // int spacing = 3;            // Small spacing between elements

            int startX = 10;
            int currentY = 10;

            // Client Session ID
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Client Session ID:");
            g_hClientSessionID = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 100, hFont);
            currentY += controlHeight + spacing * 2; // Move down after the ID block

            // Fstream Progress Blocks (Label | Value Edit | Progress Bar)
            // --- CHANGED: Use a for loop for fstream creation ---
            for (int i = 0; i < 5; ++i) {
                char labelText[64];
                sprintf_s(labelText, sizeof(labelText), "Fstream %d Progress:", i);

                // Label
                CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, labelText);

                // Value Edit (next to label)
                g_hFstreamEdit[i] = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 101 + i, hFont);

                // Progress Bar (next to value edit)
                g_hFstreamProgressBar[i] = CREATE_PROGRESS_BAR(hwnd, startX + labelWidth + spacing + valueWidth + spacing, currentY, progressBarWidth, controlHeight, 200 + i);
                SendMessage(g_hFstreamProgressBar[i], PBM_SETRANGE32, 0, 100); // Set range 0-100
                SendMessage(g_hFstreamProgressBar[i], PBM_SETPOS, 0, 0);       // Initial position

                currentY += controlHeight + spacing; // Move Y down for next block
            }

            // --- Remaining sections (Files Pending, Send Queues, etc.) ---
            currentY += spacing * 2; // Extra spacer after fstream blocks

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Files Pending:");
            g_hFilesPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 106, hFont);
            currentY += controlHeight + spacing;

            currentY += spacing; // Spacer

            // Send Queues
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Queue Send Frames:");
            g_hQSFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 107, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Queue Send Prio Frames:");
            g_hQSPFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 108, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Queue Send Ctrl Frames:");
            g_hQSCFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 109, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Hash Table Send Frames:");
            g_hHTSFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 110, hFont);
            currentY += controlHeight + spacing;

            currentY += spacing; // Spacer

            // Receive Queues
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Queue Recv Frames:");
            g_hQRFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 111, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Queue Recv Prio Frames:");
            g_hQRPFPendingEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 112, hFont);
            currentY += controlHeight + spacing;

            currentY += spacing; // Spacer

            // Pool Send Blocks
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool Send Frames Free:");
            g_hPSFFreeEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 113, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool Send Frames Total:");
            g_hPSFTotalEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 114, hFont);
            currentY += controlHeight + spacing;

            currentY += spacing; // Spacer

            // Pool IOCP Send Context
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool IOCP Send Free:");
            g_hPIOCPSendFreeEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 115, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool IOCP Send Total:");
            g_hPIOCPSendTotalEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 116, hFont);
            currentY += controlHeight + spacing;

            currentY += spacing; // Spacer

            // Pool IOCP Recv Context
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool IOCP Recv Free:");
            g_hPIOCPRecvFreeEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 117, hFont);
            currentY += controlHeight + spacing;

            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Pool IOCP Recv Total:");
            g_hPIOCPRecvTotalEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, valueWidth, controlHeight, 118, hFont);
            currentY += controlHeight + spacing;

            // Initial display update
            PostMessage(hwnd, WM_UPDATE_DISPLAY, 0, 0);
            break;
        }

        case WM_UPDATE_DISPLAY: {
            // Acquire lock to read shared data safely
            EnterCriticalSection(&g_statsLock);
            char buffer[64];

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.client_session_ID);
            SetWindowTextA(g_hClientSessionID, buffer);

            // Update Fstream Progress Bars and Edit Controls
            // --- CHANGED: Use a for loop for fstream updates ---
            for (int i = 0; i < 5; ++i) {
                double current_progress = g_clientStats.fstream_progress[i];
                sprintf_s(buffer, _countof(buffer), "%.2f %%", current_progress);
                SetWindowTextA(g_hFstreamEdit[i], buffer);
                SendMessage(g_hFstreamProgressBar[i], PBM_SETPOS, (WPARAM)(int)current_progress, 0);
            }

            // ... (rest of WM_UPDATE_DISPLAY for other controls remains the same) ...
            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.files_pending_for_streaming);
            SetWindowTextA(g_hFilesPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.queue_send_frames_pending);
            SetWindowTextA(g_hQSFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.queue_send_prio_frames_pending);
            SetWindowTextA(g_hQSPFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.queue_send_ctrl_frames_pending);
            SetWindowTextA(g_hQSCFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.hash_table_send_frames_pending);
            SetWindowTextA(g_hHTSFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.queue_recv_frames_pending);
            SetWindowTextA(g_hQRFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.queue_recv_prio_frames_pending);
            SetWindowTextA(g_hQRPFPendingEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_send_frames_free_blocks);
            SetWindowTextA(g_hPSFFreeEdit, buffer);
            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_send_frames_total_blocks);
            SetWindowTextA(g_hPSFTotalEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_iocp_send_context_free_blocks);
            SetWindowTextA(g_hPIOCPSendFreeEdit, buffer);
            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_iocp_send_context_total_blocks);
            SetWindowTextA(g_hPIOCPSendTotalEdit, buffer);

            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_iocp_recv_context_free_blocks);
            SetWindowTextA(g_hPIOCPRecvFreeEdit, buffer);
            sprintf_s(buffer, _countof(buffer), "%llu", g_clientStats.pool_iocp_recv_context_total_blocks);
            SetWindowTextA(g_hPIOCPRecvTotalEdit, buffer);

            LeaveCriticalSection(&g_statsLock); // Release lock
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

// ------------------------------------------------------------------
// GUI Thread Function
// ------------------------------------------------------------------
DWORD WINAPI GuiThread(LPVOID lpParam) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    HWND hwnd;
    MSG Msg;

    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ClientStatsDashboardClass";
    wc.hIconSm       = LoadIconA(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }


    int windowWidth = 50 + labelWidth + valueWidth + progressBarWidth;
    int windowHeight = 100 + (CLIENT_MAX_ACTIVE_FSTREAMS * (controlHeight + spacing)); // Adjust height for 5 fstream entries

    // Window dimensions (same as before for the compact horizontal layout)
    hwnd = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "ClientStatsDashboardClass",
        "Client Statistics Dashboard",
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth, // Width
        windowHeight, // Height
        NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    *(HWND*)lpParam = hwnd;

    while (GetMessageA(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    return (DWORD)Msg.wParam;
}

// ------------------------------------------------------------------
// Main Logic Thread Function (simulating your program's work)
// ------------------------------------------------------------------
DWORD WINAPI MainLogicThread(LPVOID lpParam) {
    HWND hDisplayWnd = (HWND)lpParam;

    // Initialize g_clientStats with some initial dummy data
    EnterCriticalSection(&g_statsLock);
    g_clientStats.client_session_ID = 0; // Initialize with 0 or a sensible default
    // --- CHANGED: Initialize fstream_progress array ---
    for (int i = 0; i < 5; ++i) {
        g_clientStats.fstream_progress[i] = 0.0;
    }

    g_clientStats.files_pending_for_streaming = 0;

    g_clientStats.queue_send_frames_pending = 0;
    g_clientStats.queue_send_prio_frames_pending = 0;
    g_clientStats.queue_send_ctrl_frames_pending = 0;
    g_clientStats.hash_table_send_frames_pending = 0;

    g_clientStats.queue_recv_frames_pending = 0;
    g_clientStats.queue_recv_prio_frames_pending = 0;

    g_clientStats.pool_send_frames_free_blocks = 0;
    g_clientStats.pool_send_frames_total_blocks = 0;

    g_clientStats.pool_iocp_send_context_free_blocks = 0;
    g_clientStats.pool_iocp_send_context_total_blocks = 0;

    g_clientStats.pool_iocp_recv_context_free_blocks = 0;
    g_clientStats.pool_iocp_recv_context_total_blocks = 0;
    LeaveCriticalSection(&g_statsLock);


    while (TRUE) {
        EnterCriticalSection(&g_statsLock);
        g_clientStats.client_session_ID = Client.sid;

        // Calculate and update fstream progress, handling division by zero
        // --- CHANGED: Use a for loop for fstream data updates ---
        for (int i = 0; i < CLIENT_MAX_ACTIVE_FSTREAMS; ++i) {
            double progress = 0.0;
            // Ensure fsize is not zero to prevent division by zero
            if (Client.fstream[i].fsize > 0) {
                progress = (double)(Client.fstream[i].fsize - Client.fstream[i].pending_bytes) / (double)Client.fstream[i].fsize * 100.0;
            }
            // Clamp progress to 0-100 range in case of floating point inaccuracies or initial states
            if (progress < 0.0) progress = 0.0;
            if (progress > 100.0) progress = 100.0;

            g_clientStats.fstream_progress[i] = progress;
        }

        for (int i = CLIENT_MAX_ACTIVE_FSTREAMS; i < 5; ++i) {
            double progress = 0.0;
            g_clientStats.fstream_progress[i] = progress;
        }

        g_clientStats.files_pending_for_streaming = Queues.queue_send_file_command.pending;

        g_clientStats.queue_send_frames_pending = Queues.queue_send_udp_frame.pending;
        g_clientStats.queue_send_prio_frames_pending = Queues.queue_recv_prio_udp_frame.pending;
        g_clientStats.queue_send_ctrl_frames_pending = Queues.queue_send_ctrl_udp_frame.pending;
        g_clientStats.hash_table_send_frames_pending = Buffers.table_send_udp_frame.count;

        g_clientStats.queue_recv_frames_pending = Queues.queue_recv_udp_frame.pending;
        g_clientStats.queue_recv_prio_frames_pending = Queues.queue_recv_prio_udp_frame.pending;

        g_clientStats.pool_send_frames_free_blocks = Buffers.pool_send_udp_frame.free_blocks;
        g_clientStats.pool_send_frames_total_blocks = Buffers.pool_send_udp_frame.block_count;

        g_clientStats.pool_iocp_send_context_free_blocks = Buffers.pool_send_iocp_context.free_blocks;
        g_clientStats.pool_iocp_send_context_total_blocks = Buffers.pool_send_iocp_context.block_count;

        g_clientStats.pool_iocp_recv_context_free_blocks = Buffers.pool_recv_iocp_context.free_blocks;
        g_clientStats.pool_iocp_recv_context_total_blocks = Buffers.pool_recv_iocp_context.block_count;

        LeaveCriticalSection(&g_statsLock);

        // Tell the GUI thread to update the display
        PostMessage(hDisplayWnd, WM_UPDATE_DISPLAY, 0, 0);

        Sleep(500); // Update every 500 milliseconds
    }
    return 0;
}


int init_statistics_gui(){
    // Initialize Common Controls Library
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_PROGRESS_CLASS; // Only need progress bar class for now
    if (!InitCommonControlsEx(&icc)) {
        MessageBoxA(NULL, "Failed to initialize common controls!", "Error!", MB_ICONERROR | MB_OK);
        return -1;
    }

    InitializeCriticalSection(&g_statsLock); // Initialize the critical section

    hMainWindow = NULL; // Will store the window handle from the GUI thread

    // Create the GUI thread
    hGuiThread = CreateThread(
        NULL,
        0,
        GuiThread,
        &hMainWindow, // Parameter to pass to thread (to get HWND back)
        0,
        NULL);

    if (hGuiThread == NULL) {
        MessageBoxA(NULL, "Failed to create GUI thread!", "Error!", MB_ICONERROR | MB_OK);
        return -1;
    }

    // Wait a moment for the GUI thread to create the window and controls
    // --- CHANGED: Check for one of the new array-based handles ---
    while (hMainWindow == NULL || g_hFstreamEdit[0] == NULL) {
        Sleep(50);
    }

    // Create your "main logic" thread
    hMainLogicThread = CreateThread(
        NULL,
        0,
        MainLogicThread,
        hMainWindow, // Pass the window handle to the main logic thread
        0,
        NULL);

    if (hMainLogicThread == NULL) {
        MessageBoxA(NULL, "Failed to create Main Logic thread!", "Error!", MB_ICONERROR | MB_OK);
        // Clean up GUI thread if main logic thread fails to create
        PostThreadMessage(GetThreadId(hGuiThread), WM_QUIT, 0, 0);
        CloseHandle(hGuiThread);
        DeleteCriticalSection(&g_statsLock);
        return -1;
    }

    return 0;
}