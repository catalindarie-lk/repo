
#include <stdio.h>                      // For printf, fprintf
#include <string.h>                     // For memset, memcpy
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep       

#include "include/resources.h"
#include "include/queue.h"
#include "include/client.h"
#include "include/client_frames.h"
#include "include/client_api.h"
#include "include/netendians.h"         // For network byte order conversions

int RequestConnect(const char *server_ip) {

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    // Define server address
    memset(&client->server_addr, 0, sizeof(client->server_addr));
    client->server_addr.sin_family = AF_INET;
    client->server_addr.sin_port = _htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client->server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        return RET_VAL_ERROR;
    };

    ResetEvent(client->hevent_connection_closed);
    ResetEvent(client->hevent_connection_established);
    SetEvent(client->hevent_connection_pending);

    PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
    if(!entry_send){
        fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for connect request. Should never do since it has semaphore to block when full");
        return RET_VAL_ERROR;
    }

    int res = construct_connect_request(entry_send,
                                        client->sid, 
                                        client->cid, 
                                        client->flags, 
                                        client->client_name,
                                        client->socket, &client->server_addr);
    if(res == RET_VAL_ERROR){
        fprintf(stderr, "CRITICAL ERROR: construct_connect_request() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
        return RET_VAL_ERROR;
    }
    if(s_push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
        fprintf(stderr, "CRITICAL ERROR: Failed to push connect request frame to queue_send_ctrl_frame. Should never happen since queue is blocking on push/pop semaphores\n");
        s_pool_free(pool_send_udp_frame, (void*)entry_send);
        return RET_VAL_ERROR;
    }

    DWORD wait_connection_established = WaitForSingleObject(client->hevent_connection_established, CONNECT_REQUEST_TIMEOUT_MS);
    if (wait_connection_established == WAIT_OBJECT_0) {
        client->session_status = CONNECTION_ESTABLISHED;
        fprintf(stdout, "Connection established...\n");
    } else if (wait_connection_established == WAIT_TIMEOUT) {
        reset_client_session();
        fprintf(stderr, "Connection closed...\n");
        return RET_VAL_ERROR;
    } else {
        reset_client_session();
        fprintf(stderr, "Unexpected error for established event: %lu\n", wait_connection_established);
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}

void RequestDisconnect(){
    
    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)
    
    if(client->session_status == CONNECTION_CLOSED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    // send disconnect frame
    ResetEvent(client->hevent_connection_established);

    PoolEntrySendFrame *entry_send = (PoolEntrySendFrame*)s_pool_alloc(pool_send_udp_frame);
    if(!entry_send){
        fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for disconnect request. Should never do since it has semaphore to block when full");
        return;
    }

    construct_disconnect_request(entry_send, client->sid, client->socket, &client->server_addr);
    if(s_push_ptr(queue_send_ctrl_udp_frame, (uintptr_t)entry_send) == RET_VAL_ERROR){
        fprintf(stderr, "CRITICAL ERROR: Failed to push disconnect request frame to queue_send_ctrl_frame. Should never happen since queue is blocking on push/pop semaphores\n");
        s_pool_free(pool_send_udp_frame, (void*)entry_send);
    }

 
    DWORD wait_connection_closed = WaitForSingleObject(client->hevent_connection_closed, DISCONNECT_REQUEST_TIMEOUT_MS);

    if (wait_connection_closed == WAIT_OBJECT_0) {
        fprintf(stderr, "Connection closed\n"); 
    } else if (wait_connection_closed == WAIT_TIMEOUT) {
        fprintf(stdout, "Connection close timeout - closing connection anyway\n");
    } else {    
        fprintf(stderr, "Unexpected error for disconnect event: %lu\n", wait_connection_closed);
    }  
    reset_client_session();
    return;
}


// Send text
// buffer: pointer to text buffer
// len: The actual nr of bytes of the text buffer, NOT including the null terminator.
void SendTextMessage(const char *buffer, const size_t len) {

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    PoolEntryCommand *entry = NULL;

    if(!buffer){
        fprintf(stderr, "ERROR: Message buffer invalid pointer.\n");
        return;
    }
    if(len <= 0){
        fprintf(stderr, "ERROR: Message length zero or negative lenght.\n");
        return;
    }
    if(len >= MAX_MESSAGE_SIZE_BYTES){
        fprintf(stderr, "ERROR: Message size too long.\n");
        return;
    }

    entry = (PoolEntryCommand*)s_pool_alloc(pool_send_command);
    if(!entry){
        fprintf(stderr, "CRITICAL ERROR: failed to allocate memory for message command from the pool\n");
        return;
    }

    snprintf(entry->command.send_message.text, sizeof("SendTextMessage"), "%s", "SendTextMessage");
    entry->command.send_message.message_len = len;
    entry->command.send_message.message_buffer = malloc(len + 1);
    memcpy(entry->command.send_message.message_buffer, buffer, len);
    entry->command.send_message.message_buffer[len] = '\0';

    if(s_push_ptr(queue_send_message_command, (uintptr_t)entry) == RET_VAL_ERROR){
        s_pool_free(pool_send_command, (void*)entry);
        fprintf(stderr, "CRITICAL ERROR: Failed to push command to queue_process_mstream. Should never happen since queue is blocking on push/pop semaphores\n");
    }
    return;
}

// Send text in file
// fpath: path to text file
// len: The actual length of the string fpath, NOT including the null terminator.
int SendTextInFile(const char *fpath, size_t fpath_len){

    if (!fpath) {
        fprintf(stderr, "ERROR: SendTextInFile - Input file path is NULL.\n");
        return RET_VAL_ERROR;
    }

    if (fpath_len == 0) {
        // An empty string (fpath_len=0) is a valid input if it means current directory or empty filename.
        // However, given the context of sending a single file, an empty path might be an error.
        // The original code treated strlen(fpath) == 0 as an error. We'll keep this logic.
        fprintf(stderr, "ERROR: SendTextInFile - Input file path length is zero (empty path).\n");
        return RET_VAL_ERROR;
    }

    // MAX_PATH typically includes space for the null terminator.
    // So, if fpath_len is MAX_PATH, it means the string *exactly* fills the buffer,
    // leaving no space for the null terminator if the buffer itself is MAX_PATH bytes.
    // Therefore, for a string of 'fpath_len' characters to fit AND be null-terminated
    // in a MAX_PATH buffer, 'fpath_len' must be strictly less than MAX_PATH.
    if (fpath_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendTextInFile - Input file path length (%zu) is too long for MAX_PATH (%d) buffer (requires space for null terminator).\n", fpath_len, MAX_PATH);
        return RET_VAL_ERROR;
    }

    if (fpath_len != strlen(fpath)) {
        fprintf(stderr, "WARNING: SendTextInFile - Provided length (%zu) does not match actual string length (%zu).\n", fpath_len, strlen(fpath));
        return RET_VAL_ERROR;
    }

    char _fpath[MAX_PATH] = {0};
    snprintf(_fpath, MAX_PATH, "%s", fpath);
    
    FILE *fp = NULL;
    
    int message_len = (uint32_t)get_file_size(_fpath);
    if(message_len == RET_VAL_ERROR){
        fprintf(stdout, "Error when reading file size!\n");
        return RET_VAL_ERROR;
    }
    if(message_len > MAX_MESSAGE_SIZE_BYTES){
        fprintf(stdout, "Message too big!\n");
        return RET_VAL_ERROR;
    }
    fp = fopen(_fpath, "rb");
    if(fp == NULL){
        fprintf(stdout, "Error opening file!\n");
        return RET_VAL_ERROR;
    }
    char *message_buffer = malloc(message_len + 1);
    if(message_buffer == NULL){
        fprintf(stdout, "Error allocating memeory buffer for message!\n");
        return RET_VAL_ERROR;
    }
    size_t bytes_read = fread(message_buffer, 1, message_len, fp);
    message_buffer[message_len] = '\0';
    if (bytes_read == 0 && ferror(fp)) {
        fprintf(stdout, "Error reading message file!\n");
        return RET_VAL_ERROR;
    }
    SendTextMessage(message_buffer, message_len);
    free(message_buffer);
    message_buffer = NULL;
 
    return RET_VAL_SUCCESS;
}
//============================================================================================================================================================

// helper function to send a file on the queue
// dirpath_len: The actual content length of fpath, EXCLUDING the null terminator.
// rpath_len: The actual content length of rpath, EXCLUDING the null terminator.
// fname_len: The actual content length of fname, EXCLUDING the null terminator.
static void stream_file(const char *dirpath, const size_t dirpath_len,
                        const char *rpath, const size_t rpath_len,
                        const char *fname, const size_t fname_len) {

    PARSE_CLIENT_GLOBAL_DATA(Client, Queues, Buffers, Threads) // this macro is defined in client header file (client.h)

    PoolEntryCommand *entry = NULL;
    int res; // For checking return values of secure functions
    // memset(&entry, 0, sizeof(QueueCommandEntry));

    // --- Input Validation (now based on content lengths) ---

    // Validate fpath (full actual disk directory path)
    if (!dirpath) { 
        fprintf(stderr, "ERROR: stream_file - Absolute directory path string invalid pointer.\n"); 
        return; 
    }
    // dirpath_len 0 is invalid for a path
    if (dirpath_len == 0) { 
        fprintf(stderr, "ERROR: stream_file - Absolute directory path string zero content length.\n"); 
        return; 
    }
    // A string of 'dirpath_len' characters needs 'dirpath_len + 1' bytes for the null terminator.
    // So, it must be strictly less than MAX_PATH to fit within a MAX_PATH buffer.
    if (dirpath_len >= MAX_PATH) { 
        fprintf(stderr, "ERROR: stream_file - Absolute directory path string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, dirpath_len); 
        return; 
    }
    // (Optional but good check: Verify actual null termination if function might pass to strlen-reliant APIs)
    // if (strnlen_s(fpath, dirpath_len + 1) > dirpath_len) { /* Good, null terminated within expected length */ }
    // else { fprintf(stderr, "WARNING: stream_file - Absolute directory path may not be null-terminated within its claimed length + 1.\n"); }


    // Validate rpath (relative directory path)
    if (!rpath) { 
        fprintf(stderr, "ERROR: stream_file - Relative directory path string invalid pointer.\n"); 
        return; 
    }
    // rpath_len MUST be at least 1 (for '\'), so 0 content length is an error
    if (rpath_len == 0) { // Should not happen with correct logic from recursive function (should always be at least "\")
        fprintf(stderr, "ERROR: stream_file - Relative directory path string zero content length.\n");
        return;
    }
    if (rpath_len >= MAX_PATH) { 
        fprintf(stderr, "ERROR: stream_file - Relative directory path string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, rpath_len); 
        return; 
    }
    // (Optional null termination check)


    // Validate fname (just the filename)
    if (!fname) { 
        fprintf(stderr, "ERROR: stream_file - Filename string invalid pointer.\n"); 
        return; 
    }
    if (fname_len == 0) { fprintf(stderr, "ERROR: stream_file - Filename string zero content length (filename cannot be empty).\n"); 
        return; 
    }
    if (fname_len >= MAX_PATH) { fprintf(stderr, "ERROR: stream_file - Filename string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, fname_len); 
        return; 
    }
    // (Optional null termination check)

    // --- Populate QueueCommandEntry using secure functions ---

    entry = (PoolEntryCommand*)s_pool_alloc(pool_send_command);
    if(!entry){
        fprintf(stderr, "CRITICAL ERROR: failed to allocate memory for file command from the pool\n");
        return;
    }

    // Populate 'text' field for stream_file command type
    // Note: "sendfile" has 8 chars, fits easily in 32 bytes with null.
    res = snprintf(entry->command.send_file.text,
                      sizeof(entry->command.send_file.text),
                      "%s", "sendfile");
    // Check for errors (negative return) or truncation (result >= buffer size)
    if (res < 0 || (size_t)res >= sizeof(entry->command.send_file.text)) { // Cast result to size_t for comparison
        fprintf(stderr, "ERROR: stream_file - Failed to set 'text' field for send_file command (truncation or error). Result: %d, Buffer Size: %zu\n",
                res, sizeof(entry->command.send_file.text));
        return;
    }

    // Populate 'fpath' field with the FULL ACTUAL DISK DIRECTORY PATH
    // Use '%.*s' to copy exactly 'dirpath_len' characters
    res = snprintf(entry->command.send_file.dirpath,
                      sizeof(entry->command.send_file.dirpath),
                      "%.*s", (int)dirpath_len, dirpath); // Cast dirpath_len to int for printf specifier
    // Check if the exact number of characters was copied
    if (res < 0 || (size_t)res != dirpath_len) {
        fprintf(stderr, "ERROR: stream_file - Failed to copy absolute directory path to command entry (truncation or error). Path len: %zu, Result: %d, Buffer Size: %zu\n",
                dirpath_len, res, sizeof(entry->command.send_file.dirpath));
        return;
    }
    entry->command.send_file.dirpath_len = (uint32_t)dirpath_len;

    // Populate 'rpath' field with the RELATIVE DIRECTORY PATH
    // Use '%.*s' to copy exactly 'rpath_len' characters
    res = snprintf(entry->command.send_file.rpath,
                      sizeof(entry->command.send_file.rpath),
                      "%.*s", (int)rpath_len, rpath);
    if (res < 0 || (size_t)res != rpath_len) {
        fprintf(stderr, "ERROR: stream_file - Failed to copy relative directory path to command entry (truncation or error). Path len: %zu, Result: %d, Buffer Size: %zu\n",
                rpath_len, res, sizeof(entry->command.send_file.rpath));
        return;
    }
    entry->command.send_file.rpath_len = (uint32_t)rpath_len;

    // Populate 'fname' field with JUST THE FILENAME
    // Use '%.*s' to copy exactly 'fname_len' characters
    res = snprintf(entry->command.send_file.fname,
                      sizeof(entry->command.send_file.fname),
                      "%.*s", (int)fname_len, fname);
    if (res < 0 || (size_t)res != fname_len) {
        fprintf(stderr, "ERROR: stream_file - Failed to copy filename to command entry (truncation or error). Name len: %zu, Result: %d, Buffer Size: %zu\n",
                fname_len, res, sizeof(entry->command.send_file.fname));
        return;
    }
    entry->command.send_file.fname_len = (uint32_t)fname_len;

    // Push the command to the queue
    if(s_push_ptr(queue_send_file_command, (uintptr_t)entry) == RET_VAL_ERROR){
        s_pool_free(pool_send_command, (void*)entry);
        fprintf(stderr, "CRITICAL ERROR: Failed to push command to queue_process_fstream. Should never happen since queue is blocking on push/pop semaphores\n");
    }
                    
    return;
}

//============================================================================================================================================================

// Send single file
// len: The actual length of the string fpath, NOT including the null terminator.
void SendSingleFile(const char *fpath, size_t len) {
    if (!fpath) {
        fprintf(stderr, "ERROR: SendSingleFile - Input file path is NULL.\n");
        return;
    }

    if (len == 0) {
        // An empty string (len=0) is a valid input if it means current directory or empty filename.
        // However, given the context of sending a single file, an empty path might be an error.
        // The original code treated strlen(fpath) == 0 as an error. We'll keep this logic.
        fprintf(stderr, "ERROR: SendSingleFile - Input file path length is zero (empty path).\n");
        return;
    }

    // MAX_PATH typically includes space for the null terminator.
    // So, if len is MAX_PATH, it means the string *exactly* fills the buffer,
    // leaving no space for the null terminator if the buffer itself is MAX_PATH bytes.
    // Therefore, for a string of 'len' characters to fit AND be null-terminated
    // in a MAX_PATH buffer, 'len' must be strictly less than MAX_PATH.
    if (len >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendSingleFile - Input file path length (%zu) is too long for MAX_PATH (%d) buffer (requires space for null terminator).\n", len, MAX_PATH);
        return;
    }

    if (len != strlen(fpath)) {
        fprintf(stderr, "WARNING: SendSingleFile - Provided length (%zu) does not match actual string length (%zu).\n", len, strlen(fpath));
        return;
    }

    char absoluteDirectoryPath[MAX_PATH]; // This will be the fpath for stream_file
    char fileNameOnly[MAX_PATH];          // This will be the fname for stream_file
    const char *relativeDirectoryPath = "\\"; // Fixed rpath for SendSingleFile

    // --- Extract Filename and Absolute Directory Path ---
    const char *last_slash_win = NULL;
    const char *last_slash_unix = NULL;
    const char *last_slash = NULL;

    // Search within the provided 'len' boundary
    // Iterate up to 'len' characters to find the last slash.
    for (size_t i = 0; i < len; ++i) {
        if (fpath[i] == '\\') {
            last_slash_win = fpath + i;
        } else if (fpath[i] == '/') {
            last_slash_unix = fpath + i;
        }
    }

    // Determine the last actual path separator (Windows-style preference first)
    if (last_slash_win && (!last_slash_unix || last_slash_win > last_slash_unix)) {
        last_slash = last_slash_win;
    } else if (last_slash_unix) {
        last_slash = last_slash_unix;
    }

    if (last_slash) {
        // Filename is after the last slash
        size_t filename_offset = (size_t)(last_slash - fpath) + 1;
        // The length of the filename string itself
        size_t filename_content_len = len - filename_offset;

        if (filename_offset > len || filename_content_len >= MAX_PATH) { // Check if there's actual filename content and if it fits
            fprintf(stderr, "ERROR: SendSingleFile - Path ends with a separator or filename content is too long for buffer: '%s'.\n", fpath);
            return;
        }

        // Use 'filename_content_len' with '%.*s' to copy exactly that many characters
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%.*s", (int)filename_content_len, fpath + filename_offset);
        if (snprintf_res < 0 || (size_t)snprintf_res != filename_content_len) { // Check if all characters were copied
            fprintf(stderr, "ERROR: SendSingleFile - Failed to extract filename '%s' from path (truncation or error). Result: %d, Expected: %zu\n", fpath + filename_offset, snprintf_res, filename_content_len);
            return;
        }

        // Absolute directory path is up to and including the last slash
        size_t dir_content_len = (size_t)(last_slash - fpath) + 1; // +1 to include the slash itself
        if (dir_content_len >= MAX_PATH) { // Needs space for null terminator
            fprintf(stderr, "ERROR: SendSingleFile - Directory path derived from '%.*s' is too long.\n", (int)len, fpath);
            return;
        }
        memcpy(absoluteDirectoryPath, fpath, dir_content_len);
        absoluteDirectoryPath[dir_content_len] = '\0';

    } else {
        // No slash found, assume the entire path is the filename and the directory is current ".\"
        // The entire input 'fpath' (of 'len' characters) is the filename.
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%.*s", (int)len, fpath);
        if (snprintf_res < 0 || (size_t)snprintf_res != len) { // Check if all characters were copied
            fprintf(stderr, "ERROR: SendSingleFile - Failed to copy full path as filename (truncation or error). Result: %d, Expected: %zu\n", snprintf_res, len);
            return;
        }

        // Set absoluteDirectoryPath to current directory indicator
        snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, ".\\");
        if (snprintf_res < 0 || snprintf_res >= MAX_PATH) { // Check if it fits
            fprintf(stderr, "ERROR: SendSingleFile - Failed to set default absolute directory path (truncation or error). Result: %d\n", snprintf_res);
            return;
        }
    }

    // Call the internal stream_file helper
    // fpath_len, rpath_len, fname_len should include the null terminator (+1) as per original calls
    stream_file(absoluteDirectoryPath, strlen(absoluteDirectoryPath),
                relativeDirectoryPath, strlen(relativeDirectoryPath), // rpath is fixed to "\"
                fileNameOnly, strlen(fileNameOnly));

    // printf("SendSingleFile: Processed file: %s\n", full_file_path);
}

//============================================================================================================================================================

// Send all files in a folder
// fd_path: The path to the folder.
// len: The actual length of the fd_path string, NOT including the null terminator.
void SendAllFilesInFolder(const char *fd_path, size_t len) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // For checking return values of snprintf

    if (!fd_path) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path is NULL.\n");
        return;
    }

    if (len == 0) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path length is zero (empty path).\n");
        return;
    }

    // len must be less than MAX_PATH to allow space for the null terminator.
    // Also, we need space for "\\*" which adds 2 characters + null terminator
    // So, fd_path_len + 2 + 1 <= MAX_PATH  =>  fd_path_len <= MAX_PATH - 3
    if (len >= MAX_PATH - 2) { // Need at least space for "\*" and null terminator
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path length (%zu) is too long for MAX_PATH (%d) buffer when adding search wildcard.\n", len, MAX_PATH);
        return;
    }

    // Prepare search path: "C:\Folder\*" or "C:\Folder*"
    // Use '%.*s' to copy exactly 'len' characters from fd_path
    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%.*s\\*", (int)len, fd_path);
    // The expected result length is 'len' (from fd_path) + 2 (for "\*").
    // The snprintf_res is the number of characters *written*, excluding the null terminator.
    if (snprintf_res < 0 || (size_t)snprintf_res != len + 2) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to format search path for '%.*s' (truncation or error). Result: %d, Expected: %zu\n", (int)len, fd_path, snprintf_res, len + 2);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Folder '%.*s' not found.\n", (int)len, fd_path);
        } else {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Could not open folder '%.*s'. System Error Code: %lu\n", (int)len, fd_path, lastError);
        }
        return;
    }

    // --- Prepare 'fpath' for stream_file ---
    // This will be the absolute directory path, ensuring a trailing backslash.
    char absoluteDirectoryPathForSend[MAX_PATH];
    size_t current_abs_dir_len = len; // Start with the provided length of fd_path

    // Copy the original fd_path using the provided length
    snprintf_res = snprintf(absoluteDirectoryPathForSend, MAX_PATH, "%.*s", (int)len, fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != len) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy base directory path '%.*s' for stream_file (truncation or error). Result: %d, Expected: %zu\n", (int)len, fd_path, snprintf_res, len);
        FindClose(hFind);
        return;
    }

    // Append a trailing backslash if not already present, ensuring space
    // Check the last character of the copied string (which is at index len - 1)
    if (current_abs_dir_len > 0 &&
        absoluteDirectoryPathForSend[current_abs_dir_len - 1] != '\\' &&
        absoluteDirectoryPathForSend[current_abs_dir_len - 1] != '/') {

        if (current_abs_dir_len < MAX_PATH - 1) { // -1 for backslash, -1 for null terminator
            absoluteDirectoryPathForSend[current_abs_dir_len] = '\\';
            absoluteDirectoryPathForSend[current_abs_dir_len + 1] = '\0';
            current_abs_dir_len++; // Update the length to include the new backslash
        } else {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Absolute directory path '%.*s' is too long to add trailing backslash. Path may be incomplete.\n", (int)len, fd_path);
            return;
        }
    } else {
        // If it already has a slash or is empty, ensure null termination at 'len'
        absoluteDirectoryPathForSend[current_abs_dir_len] = '\0';
    }


    // --- Prepare 'rpath' for stream_file ---
    // Since this function only sends files from the *top-level* of fd_path
    // (no recursion), the relative path is simply the root relative path.
    // For consistency with your earlier examples, we'll make it "\".
    char relativeDirectoryPathForSend[MAX_PATH];
    snprintf_res = snprintf(relativeDirectoryPathForSend, MAX_PATH, "\\"); // Representing the root relative path
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to set relative directory path to '\\' (truncation or error). Result: %d\n", snprintf_res);
        FindClose(hFind);
        return;
    }


    do {
        // Skip '.' and '..'
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }

        // Only process files, not directories (as this is non-recursive)
        if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // --- Prepare 'fname' for stream_file ---
            // Just the filename from findFileData
            char fileNameOnly[MAX_PATH];
            size_t filename_content_len = strlen(findFileData.cFileName);

            // Check if filename fits
            if (filename_content_len >= MAX_PATH) {
                fprintf(stderr, "ERROR: SendAllFilesInFolder - Filename '%s' is too long (%zu) for buffer. Skipping this file.\n", findFileData.cFileName, filename_content_len);
                continue; // Skip this file and try the next
            }

            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != filename_content_len) {
                fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy filename '%s' (truncation or error). Result: %d, Expected: %zu. Skipping this file.\n", findFileData.cFileName, snprintf_res, filename_content_len);
                continue; // Skip this file and try the next
            }

            // --- Call stream_file with the three path components ---
            // fpath argument: absoluteDirectoryPathForSend (e.g., "C:\Folder\")
            // rpath argument: relativeDirectoryPathForSend (e.g., "\")
            // fname argument: fileNameOnly (e.g., "MyFile.txt")
            stream_file(absoluteDirectoryPathForSend, current_abs_dir_len, // current_abs_dir_len updated above
                        relativeDirectoryPathForSend, strlen(relativeDirectoryPathForSend),
                        fileNameOnly, filename_content_len); // filename_content_len already calculated

            // Print confirmation
            printf("Send File: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
                   absoluteDirectoryPathForSend, relativeDirectoryPathForSend, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}

//============================================================================================================================================================

// --- Recursive Helper Function ---
// root_path_to_replace: The initial absolute path provided by the user. Used to calculate relative paths.
// root_len: The length of root_path_to_replace (excluding null terminator).
// current_fd_path: The absolute path of the folder currently being processed.
// current_len: The length of current_fd_path (excluding null terminator).
static void send_all_files_in_folder_recursive(const char *root_path_to_replace, size_t root_len,
                                          const char *current_fd_path, size_t current_len) {

    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // Result for snprintf calls

    // Validation for incoming lengths (can be removed if confident about caller's validation)
    if (!current_fd_path || current_len == 0 || current_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Invalid current folder path or length (path: %p, len: %zu).\n", current_fd_path, current_len);
        return;
    }
    if (!root_path_to_replace || root_len == 0 || root_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Invalid root path or length (path: %p, len: %zu).\n", root_path_to_replace, root_len);
        return;
    }


    // Construct the search path using the actual current_fd_path for OS calls
    // current_len (for current_fd_path) + '\' + '*' + '\0'  =>  current_len + 3 bytes needed
    if (current_len >= MAX_PATH - 2) { // Need space for "\*" and null terminator
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Current folder path '%.*s' is too long for search wildcard (len: %zu).\n",
                (int)current_len, current_fd_path, current_len);
        return;
    }

    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%.*s\\*", (int)current_len, current_fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != current_len + 2) { // Expected written chars: current_len + '\' + '*'
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Failed to format search path for '%.*s' (truncation or error). Result: %d, Expected: %zu\n",
                (int)current_len, current_fd_path, snprintf_res, current_len + 2);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Folder '%.*s' not found.\n", (int)current_len, current_fd_path);
        } else {
            fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Could not open folder '%.*s'. System Error Code: %lu\n", (int)current_len, current_fd_path, lastError);
        }
        return;
    }

    // Prepare the current ACTUAL disk directory path with a trailing backslash.
    // This will be passed as the 'fpath' argument to stream_file.
    char absoluteDirectoryPath[MAX_PATH]; // Full absolute path to current directory
    size_t abs_dir_path_len = current_len; // Start with the provided length

    // Copy the current_fd_path using its length
    snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, "%.*s", (int)current_len, current_fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != current_len) {
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Failed to copy current actual directory path '%.*s' (truncation or error). Result: %d, Expected: %zu\n",
                (int)current_len, current_fd_path, snprintf_res, current_len);
        FindClose(hFind);
        return;
    }

    // Append a trailing backslash if it's not already present.
    if (abs_dir_path_len > 0 &&
        absoluteDirectoryPath[abs_dir_path_len - 1] != '\\' &&
        absoluteDirectoryPath[abs_dir_path_len - 1] != '/')
    {
        if (abs_dir_path_len < MAX_PATH - 1) { // Need space for '\' and '\0'
            absoluteDirectoryPath[abs_dir_path_len] = '\\';
            absoluteDirectoryPath[abs_dir_path_len + 1] = '\0';
            abs_dir_path_len++; // Update length to include the new backslash
        } else {
            fprintf(stderr, "WARNING: send_all_files_in_folder_recursive - Current actual directory path '%.*s' is too long to add trailing backslash. Path may be incomplete.\n", (int)abs_dir_path_len, absoluteDirectoryPath);
            // Decide if this is a fatal error or just a warning for your application
        }
    } else {
        absoluteDirectoryPath[abs_dir_path_len] = '\0'; // Ensure null termination
    }


    // Prepare the RELATIVE DIRECTORY PATH (for rpath argument of stream_file)
    char relativeDirectoryPath[MAX_PATH]; // Will hold "relative\path\" or "\"
    size_t relative_path_content_start_idx; // Index in current_fd_path where relative path starts

    // Determine the part of current_fd_path that comes after root_path_to_replace
    if (current_len >= root_len && _strnicmp(current_fd_path, root_path_to_replace, root_len) == 0)
    {
        relative_path_content_start_idx = root_len;
        // Skip any leading path separators immediately after the root path, unless the root itself is just a drive letter "C:\"
        // This handles cases like root="C:\Folder", current="C:\Folder\Sub" or root="C:", current="C:\Folder"
        if (relative_path_content_start_idx < current_len &&
            (current_fd_path[relative_path_content_start_idx] == '\\' || current_fd_path[relative_path_content_start_idx] == '/')) {
            relative_path_content_start_idx++;
        }

        size_t relative_sub_path_len = current_len - relative_path_content_start_idx;

        if (relative_sub_path_len == 0) { // Current directory IS the root itself (or its path with just a trailing slash)
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
        } else { // Current directory is a subfolder
            // Add leading backslash and trailing backslash
            // "\\", content, "\\" + NULL => relative_sub_path_len + 3 bytes
            if (relative_sub_path_len + 2 >= MAX_PATH) { // +2 for leading/trailing slashes, +1 for null
                fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Calculated relative path '%.*s' is too long. Skipping this folder.\n",
                        (int)relative_sub_path_len, current_fd_path + relative_path_content_start_idx);
                FindClose(hFind);
                return;
            }
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\%.*s\\", (int)relative_sub_path_len, current_fd_path + relative_path_content_start_idx);
        }
    } else {
        // Fallback: This case should ideally not be hit with correct recursion logic.
        // If for some reason current_fd_path isn't under the root,
        // use a default of "\" for consistency.
        fprintf(stderr, "WARNING: send_all_files_in_folder_recursive - Current path '%.*s' is not a subpath of root '%.*s'. Using default relative path.\n",
                (int)current_len, current_fd_path, (int)root_len, root_path_to_replace);
        snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
    }

    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Failed to prepare relative directory path for '%.*s' (truncation or error). Result: %d. Skipping this folder.\n",
                (int)current_len, current_fd_path, snprintf_res);
        FindClose(hFind);
        return;
    }


    // --- Loop through all files and directories found ---
    do {
        // Skip special directory entries: "." and ".."
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }

        size_t found_filename_len = strlen(findFileData.cFileName);
        // Sanity check on found filename length
        if (found_filename_len == 0 || found_filename_len >= MAX_PATH) {
            fprintf(stderr, "WARNING: send_all_files_in_folder_recursive - Invalid filename length for '%s' (len: %zu). Skipping.\n", findFileData.cFileName, found_filename_len);
            continue;
        }


        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // It's a subdirectory, recurse into it.
            // Construct the actual full path to the subfolder for the recursive call.
            char subFolderPathActual[MAX_PATH];
            // current_len + '\\' + found_filename_len + '\0' => current_len + 1 + found_filename_len + 1 bytes needed
            size_t sub_path_len = current_len + 1 + found_filename_len;

            if (sub_path_len >= MAX_PATH) { // Check if the new path will exceed MAX_PATH
                fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Constructed subfolder path '%.*s\\%s' is too long (len: %zu). Skipping recursion.\n",
                        (int)current_len, current_fd_path, findFileData.cFileName, sub_path_len);
                continue;
            }

            snprintf_res = snprintf(subFolderPathActual, MAX_PATH, "%.*s\\%s", (int)current_len, current_fd_path, findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != sub_path_len) {
                fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Failed to construct actual subfolder path for '%.*s\\%s' (truncation or error). Result: %d. Skipping recursion.\n",
                        (int)current_len, current_fd_path, findFileData.cFileName, snprintf_res);
                continue;
            }

            // Recursive call with the original root and its length, and the new actual path and its length.
            send_all_files_in_folder_recursive(root_path_to_replace, root_len,
                                          subFolderPathActual, sub_path_len);

        } else {
            // It's a file, prepare its paths for stream_file.

            // 1. Prepare JUST THE FILENAME for `fname` argument of stream_file
            char fileNameOnly[MAX_PATH]; // Will hold "MyFile.txt"
            // We already have found_filename_len from above
            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != found_filename_len) {
                fprintf(stderr, "ERROR: send_all_files_in_folder_recursive - Failed to copy raw filename '%s' (truncation or error). Result: %d, Expected: %zu. Skipping this file.\n", findFileData.cFileName, snprintf_res, found_filename_len);
                continue;
            }

            // 2. Call stream_file with the three path components
            //    fpath argument: absoluteDirectoryPath (e.g., "C:\Folder\Subfolder\")
            //    rpath argument: relativeDirectoryPath (e.g., "Subfolder\" or "\")
            //    fname argument: fileNameOnly (e.g., "MyFile.txt")
            stream_file(absoluteDirectoryPath, abs_dir_path_len,
                        relativeDirectoryPath, strlen(relativeDirectoryPath), // strlen here is fine as relativeDirectoryPath is null-terminated
                        fileNameOnly, found_filename_len);

            // Print confirmation with all paths for clarity
            // fprintf(stdout, "Sent: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
            //        absoluteDirectoryPath, relativeDirectoryPath, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}

// --- Public Entry Function for Directory Traversal ---
// fd_path: The path to the folder.
// len: The actual length of the fd_path string, NOT including the null terminator.
void SendAllFilesInFolderAndSubfolders(const char *fd_path, size_t len) {
    if (!fd_path) {
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path is NULL.\n");
        return;
    }

    if (len == 0) {
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path length is zero (empty path).\n");
        return;
    }

    // `MAX_PATH` usually implies space for null terminator. So `len` must be strictly less than `MAX_PATH`.
    // Also consider the `\*\0` for the search path in recursive call. If len = MAX_PATH - 3, it still fits.
    // If len = MAX_PATH - 2, then MAX_PATH - 2 + 2 = MAX_PATH. This will exactly fill `folderPathSearch` and leave no space for null.
    // So `len` must be <= MAX_PATH - 3 for `folderPathSearch` to guarantee null termination.
    if (len >= MAX_PATH - 2) { // Need at least space for "\*" and null terminator
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path length (%zu) exceeds MAX_PATH (%d) for initial root path (requires space for search wildcard and null terminator).\n", len, MAX_PATH);
        return;
    }

    // Start the recursive traversal, passing the initial fd_path as the root to replace
    send_all_files_in_folder_recursive(fd_path, len, fd_path, len);
}