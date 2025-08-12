
#include <stdint.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "include/protocol_frames.h"
#include "include/resources.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/client.h"

// Custom strnlen (for platforms that might not have it or if you use a custom one)
size_t s_strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len] != '\0') {
        len++;
    }
    return len;
}

/* ************************************************************************************************************************* */
int construct_connect_request(PoolEntrySendFrame *entry,
                            const uint32_t session_id, 
                            const uint32_t client_id, 
                            const uint32_t flags, 
                            const char *client_name,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    char log_message[CLIENT_LOG_MESSAGE_LEN];
    UdpFrame *frame = &entry->frame;

    if(!client_name){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_connect_request - Invalid client_name pointer (NULL).");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }
    
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame->header.seq_num = _htonll(DEFAULT_CONNECT_REQUEST_SEQ);
    frame->header.session_id = _htonl(session_id);
    frame->payload.connection_request.client_id = _htonl(client_id);
    frame->payload.connection_request.flags = flags;

    snprintf(frame->payload.connection_request.client_name, sizeof(frame->payload.connection_request.client_name), "%s", client_name);

    // Calculate the checksum for the frame
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}

int construct_disconnect_request(PoolEntrySendFrame *entry,
                            const uint32_t session_id,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame *frame = &entry->frame;

    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_DISCONNECT;
    frame->header.seq_num = _htonll(DEFAULT_DISCONNECT_REQUEST_SEQ);
    frame->header.session_id = _htonl(session_id);

    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}

int construct_keep_alive(PoolEntrySendFrame *entry,
                            const uint32_t session_id,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    
    UdpFrame *frame = &entry->frame;
    
    // Set the header fields
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_KEEP_ALIVE;
    frame->header.seq_num = _htonll(DEFAULT_KEEP_ALIVE_SEQ);
    frame->header.session_id = _htonl(session_id); // Use the session ID provided  
    // Calculate CRC32 for the frame
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    
    return RET_VAL_SUCCESS;
}

int construct_file_fragment(PoolEntrySendFrame *entry,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    
    UdpFrame *frame = &entry->frame;
    
    // Set the header fields
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_FILE_FRAGMENT;
    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id);

    frame->payload.file_fragment.file_id = _htonl(file_id);
    frame->payload.file_fragment.size = _htonl(fragment_size);
    frame->payload.file_fragment.offset = _htonll(fragment_offset);
    memcpy(frame->payload.file_fragment.bytes, fragment_buffer, fragment_size);
    
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(FileFragmentPayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}

int construct_file_metadata(PoolEntrySendFrame *entry,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *rpath,
                            const uint32_t rpath_len,
                            const char *fname,
                            const uint32_t fname_len,
                            const uint32_t file_fragment_size,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    char log_message[CLIENT_LOG_MESSAGE_LEN];
    UdpFrame *frame = &entry->frame;
    
    // --- Validate rpath ---
    if(rpath == NULL){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Invalid relative file path pointer (NULL).");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }

    // NEW: rpath must be at least 1 character (for '\')
    if(rpath_len == 0){ // If declared length is 0
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Relative file path has a declared length of 0. Minimum is 1 (for '\\').");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }
    if (strlen(rpath) == 0) { // If actual string content is empty
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Relative file path content is empty. Minimum is 1 character (for '\\').");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }

    // Check against the size of the field in UdpFrame payload.
    // rpath_len is *content* length, and receiver expects `content_len < MAX_PATH`.
    // So, `rpath_len >= sizeof(frame.payload.file_metadata.rpath)` means it won't fit WITH a null terminator.
    if(rpath_len >= sizeof(frame->payload.file_metadata.rpath)){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Relative file path content (length %u) is too long for payload buffer (max %zu chars).",
                rpath_len, sizeof(frame->payload.file_metadata.rpath) - 1);
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }
    // Consistency check: declared length vs. actual string length
    if (rpath_len != strlen(rpath)) {
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Declared rpath_len (%u) does not match actual strlen(rpath) (%zu).",
                rpath_len, strlen(rpath));
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }

    // --- Validate fname ---
    if(fname == NULL){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Invalid filename pointer (NULL).");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }
    // Filename should typically never be 0 length
    if(fname_len == 0){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Filename has a declared length of 0.");
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }

    if(fname_len >= sizeof(frame->payload.file_metadata.fname)){
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Filename content (length %u) is too long for payload buffer (max %zu chars).",
                fname_len, sizeof(frame->payload.file_metadata.fname) - 1);
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);
        return RET_VAL_ERROR;
    }
    // Consistency check: declared length vs. actual string length
    if (fname_len != strlen(fname)) {
        snprintf(log_message, sizeof(log_message), "CRITICAL ERROR: send_file_metadata - Declared fname_len (%u) does not match actual strlen(fname) (%zu).\n",
                fname_len, strlen(fname));
        log_to_file(log_message);
        fprintf(stderr, "%s\n", log_message);        
        return RET_VAL_ERROR;
    }

    // Set the header fields
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id);
    // Set the payload fields
    frame->payload.file_metadata.file_id = _htonl(file_id);
    frame->payload.file_metadata.file_size = _htonll(file_size);

    frame->payload.file_metadata.rpath_len = _htonl(rpath_len);
    snprintf(frame->payload.file_metadata.rpath, sizeof(frame->payload.file_metadata.rpath), "%s", rpath);
    frame->payload.file_metadata.fname_len = _htonl(fname_len);
    snprintf(frame->payload.file_metadata.fname, sizeof(frame->payload.file_metadata.fname), "%s", fname);

    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}

int construct_file_end(PoolEntrySendFrame *entry,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const char *file_hash,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    
    UdpFrame *frame = &entry->frame;
    
    // Set the header fields
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_FILE_END;
    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id);
    // Set the payload fields
    frame->payload.file_metadata.file_id = _htonl(file_id);
    frame->payload.file_metadata.file_size = _htonll(file_size);

    memcpy(frame->payload.file_end.file_hash, file_hash, 32);
           
    // Calculate the checksum for the frame
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(FileEndPayload)));
    
    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}



int construct_text_fragment(PoolEntrySendFrame *entry,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t message_id, 
                            const uint32_t message_len, 
                            const uint32_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_len,
                            const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    
    UdpFrame *frame = &entry->frame;
    
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_TEXT_MESSAGE;
    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id);
    // Set the payload fields
    frame->payload.text_fragment.message_id = _htonl(message_id);
    frame->payload.text_fragment.message_len = _htonl(message_len);
    frame->payload.text_fragment.fragment_len = _htonl(fragment_len);
    frame->payload.text_fragment.fragment_offset = _htonl(fragment_offset);
    
    memcpy(frame->payload.text_fragment.chars, fragment_buffer, fragment_len);
   
    // Calculate the checksum for the frame
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(TextPayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));

    return RET_VAL_SUCCESS;
}
