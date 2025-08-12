#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdint.h>
#include <commctrl.h> // Required for progress bar and InitCommonControlsEx

#include "include/server_statistics.h" // Include your new header
#include "include/server.h"

// --- Global Data and Synchronization (DEFINITIONS) ---
ServerStatistics g_serverStats; // The shared server data
CRITICAL_SECTION g_statsLock;   // To protect g_serverStats

HWND hServerMainWindow = NULL;
HANDLE hServerGuiThread;
HANDLE hServerMainLogicThread; // Assuming your server also has a main logic thread

// --- Global Handles to GUI Controls (DEFINITIONS) ---
HWND g_hConnectedClientsEdit = NULL;

// Arrays for Fstream controls (10 entries)
HWND g_hFstreamProgressEdit[10] = {0};
HWND g_hFstreamProgressBar[10] = {0};
HWND g_hFstreamSessionIDEdit[10] = {0};

// --- Function Prototypes ---
LRESULT CALLBACK WndProc_Server(HWND, UINT, WPARAM, LPARAM);
DWORD WINAPI GuiThread_Server(LPVOID lpParam);
DWORD WINAPI MainLogicThread_Server(LPVOID lpParam); // Simulate server's main logic

// ------------------------------------------------------------------
// Window Procedure for the main server statistics window
// ------------------------------------------------------------------
LRESULT CALLBACK WndProc_Server(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            int labelWidth = 150;        // Fstream X Progress:
            int sessionIDWidth = 80;     // For Session ID
            int percentageWidth = 80;    // For XX.YY %
            int progressBarWidth = 120;  // Width of the progress bar
            int controlHeight = 20;      // Common height for all controls
            int spacing = 5;             // Small spacing between elements

            int startX = 10;
            int currentY = 10;

            // Connected Clients
            CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, "Connected Clients:");
            g_hConnectedClientsEdit = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, sessionIDWidth, controlHeight, 100, hFont);
            currentY += controlHeight + spacing * 3; // More spacing before fstream blocks

            // Fstream Progress Blocks (10 entries - all on one row each)
            for (int i = 0; i < 10; ++i) {
                char labelText[64];
                sprintf_s(labelText, sizeof(labelText), "Fstream %d Progress:", i);

                // 1. Progress Label
                CREATE_STATIC_LABEL(hwnd, startX, currentY, labelWidth, controlHeight, labelText);

                // 2. Session ID Value (right after label)
                g_hFstreamSessionIDEdit[i] = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing, currentY, sessionIDWidth, controlHeight, 200 + i, hFont);

                // 3. Progress Percentage Value (right after session ID)
                g_hFstreamProgressEdit[i] = CREATE_VALUE_EDIT(hwnd, startX + labelWidth + spacing + sessionIDWidth + spacing, currentY, percentageWidth, controlHeight, 300 + i, hFont);

                // 4. Progress Bar (right after percentage)
                g_hFstreamProgressBar[i] = CREATE_PROGRESS_BAR(hwnd, startX + labelWidth + spacing + sessionIDWidth + spacing + percentageWidth + spacing, currentY, progressBarWidth, controlHeight, 400 + i);
                SendMessage(g_hFstreamProgressBar[i], PBM_SETRANGE32, 0, 100); // Set range 0-100
                SendMessage(g_hFstreamProgressBar[i], PBM_SETPOS, 0, 0);       // Initial position

                currentY += controlHeight + spacing; // Move Y down for next fstream block
            }

            // Initial display update
            PostMessage(hwnd, WM_UPDATE_SERVER_DISPLAY, 0, 0);
            break;
        }

        case WM_UPDATE_SERVER_DISPLAY: {
            // Acquire lock to read shared data safely
            EnterCriticalSection(&g_statsLock);
            char buffer[64];

            // Update Connected Clients
            sprintf_s(buffer, _countof(buffer), "%llu", g_serverStats.connected_clients);
            SetWindowTextA(g_hConnectedClientsEdit, buffer);

            // Update Fstream Progress and Session IDs
            for (int i = 0; i < 10; ++i) {
                // Update Session ID
                sprintf_s(buffer, _countof(buffer), "%llu", g_serverStats.fstream_session_id[i]);
                SetWindowTextA(g_hFstreamSessionIDEdit[i], buffer);

                // Update Progress Percentage
                double current_progress = g_serverStats.fstream_progress[i];
                // Clamp progress to 0-100 range in case of floating point inaccuracies
                if (current_progress < 0.0) current_progress = 0.0;
                if (current_progress > 100.0) current_progress = 100.0;
                sprintf_s(buffer, _countof(buffer), "%.2f %%", current_progress);
                SetWindowTextA(g_hFstreamProgressEdit[i], buffer);

                // Update Progress Bar
                SendMessage(g_hFstreamProgressBar[i], PBM_SETPOS, (WPARAM)(int)current_progress, 0);
            }

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
// GUI Thread Function for Server Statistics
// ------------------------------------------------------------------
DWORD WINAPI GuiThread_Server(LPVOID lpParam) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    HWND hwnd;
    MSG Msg;

    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc_Server; // Use the server-specific WndProc
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ServerStatsDashboardClass"; // Unique class name
    wc.hIconSm       = LoadIconA(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Server Window Registration Failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    // Adjust window size for 10 fstream entries all on one row
    hwnd = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "ServerStatsDashboardClass",
        "Server Statistics Dashboard",
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        500, // Increased Width: Label (150) + SID (80) + % (80) + PB (120) + 4*spacing + padding = ~460. Set to 500 for comfort.
        350, // Reduced Height: Connected clients (20+15) + 10 fstream blocks (10 * (20+5)) + padding = ~300. Set to 350 for comfort.
        NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Server Window Creation Failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    *(HWND*)lpParam = hwnd; // Pass the HWND back via the parameter

    while (GetMessageA(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    return (DWORD)Msg.wParam;
}

// ------------------------------------------------------------------
// Main Logic Thread Function (simulating your server's work)
// ------------------------------------------------------------------
DWORD WINAPI MainLogicThread_Server(LPVOID lpParam) {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers, Threads) // this macro is defined in server header file (server.h)
    
    HWND hDisplayWnd = (HWND)lpParam;
    uint64_t session_id_counter = 1000; // Dummy counter for session IDs
    int dummy_client_count = 5;

    // Initial dummy data
    EnterCriticalSection(&g_statsLock);
    g_serverStats.connected_clients = dummy_client_count;
    for (int i = 0; i < 10; ++i) {
        g_serverStats.fstream_progress[i] = 0.0;
        g_serverStats.fstream_session_id[i] = 0;
    }
    LeaveCriticalSection(&g_statsLock);

    while (TRUE) {
        // Simulate data updates
        EnterCriticalSection(&g_statsLock);
        g_serverStats.connected_clients = 0;
        for(int i = 0; i < MAX_CLIENTS; ++i){
            if(client_list->client[i].slot_status == SLOT_BUSY){
                g_serverStats.connected_clients++;
            }
        }

        for (int i = 0; i < 10; ++i) {
            if(pool_fstreams->fstream[i].fstream_busy){
                g_serverStats.fstream_progress[i] = (double)((double)pool_fstreams->fstream[i].recv_bytes_count / (double)pool_fstreams->fstream[i].fsize) * 100.0;
                g_serverStats.fstream_session_id[i] = pool_fstreams->fstream[i].sid;
            } else {
                g_serverStats.fstream_progress[i] = (double)((double)pool_fstreams->fstream[i].recv_bytes_count / (double)pool_fstreams->fstream[i].fsize) * 100.0;
                g_serverStats.fstream_session_id[i] = pool_fstreams->fstream[i].sid;
            }
            
        }
        LeaveCriticalSection(&g_statsLock);

        // Tell the GUI thread to update the display
        PostMessage(hDisplayWnd, WM_UPDATE_SERVER_DISPLAY, 0, 0);

        Sleep(500); // Update every 500 milliseconds
    }
    return 0;
}

// ------------------------------------------------------------------
// Initialization function for the server statistics GUI
// This is your main entry point for the server's GUI.
// ------------------------------------------------------------------
int init_server_statistics_gui() {
    // Initialize Common Controls Library
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_PROGRESS_CLASS; // Only need progress bar class
    if (!InitCommonControlsEx(&icc)) {
        MessageBoxA(NULL, "Failed to initialize common controls for server GUI!", "Error!", MB_ICONERROR | MB_OK);
        return -1;
    }

    InitializeCriticalSection(&g_statsLock); // Initialize the critical section

    hServerMainWindow = NULL; // Will store the window handle from the GUI thread

    // Create the GUI thread
    hServerGuiThread = CreateThread(
        NULL,
        0,
        GuiThread_Server, // Use the server-specific GUI thread function
        &hServerMainWindow, // Parameter to pass to thread (to get HWND back)
        0,
        NULL);

    if (hServerGuiThread == NULL) {
        MessageBoxA(NULL, "Failed to create Server GUI thread!", "Error!", MB_ICONERROR | MB_OK);
        return -1;
    }

    // Wait a moment for the GUI thread to create the window and controls
    // Check for at least one known control handle, e.g., the connected clients edit
    while (hServerMainWindow == NULL || g_hConnectedClientsEdit == NULL) {
        Sleep(50);
    }

    // Create your "main logic" thread for the server
    hServerMainLogicThread = CreateThread(
        NULL,
        0,
        MainLogicThread_Server, // Use the server-specific main logic thread function
        hServerMainWindow, // Pass the window handle to the main logic thread
        0,
        NULL);

    if (hServerMainLogicThread == NULL) {
        MessageBoxA(NULL, "Failed to create Server Main Logic thread!", "Error!", MB_ICONERROR | MB_OK);
        // Clean up GUI thread if main logic thread fails to create
        PostThreadMessage(GetThreadId(hServerGuiThread), WM_QUIT, 0, 0);
        CloseHandle(hServerGuiThread);
        DeleteCriticalSection(&g_statsLock);
        return -1;
    }

    return 0; // Success
}