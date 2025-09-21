
#include <stdint.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include "include/protocol_frames.h"
#include "include/resources.h"
#include "include/crc32.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/mem_pool.h"


int construct_connect_response_frame(PoolEntrySendFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, const struct sockaddr_in *dest_addr){
    
    UdpFrame *frame = (UdpFrame*)&entry->frame;
    // Set the header fields
    if(!server_name){
        fprintf(stderr, "ERROR: send_connect_response() - invalid server_name pointer\n");
        return RET_VAL_ERROR;
    }
    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id); // Use client's session ID

    frame->payload.connection_response.session_timeout = _htonl(session_timeout);
    frame->payload.connection_response.server_status = status;

    snprintf(frame->payload.connection_response.server_name, sizeof(frame->payload.connection_response.server_name), "%s", server_name);

    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    return RET_VAL_SUCCESS;
}

int construct_ack_frame(PoolEntrySendFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame *frame = (UdpFrame*)&entry->frame;

    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_ACK;
    frame->header.seq_num = _htonll(seq_num);
    frame->header.session_id = _htonl(session_id);
    frame->payload.ack.op_code = op_code;
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(AckPayload)));
    
    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    return RET_VAL_SUCCESS;
}

int construct_sack_frame(PoolEntrySendFrame *entry,
                    const uint32_t session_id,
                    const SAckPayload *sack_payload, 
                    const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame *frame = (UdpFrame*)&entry->frame;

    frame->header.start_delimiter = _htons(FRAME_DELIMITER);
    frame->header.frame_type = FRAME_TYPE_SACK;
    frame->header.seq_num = _htonll(DEFAULT_SACK_SEQ);
    frame->header.session_id = _htonl(session_id); // Use session ID from the entry
    memcpy(&frame->payload.sack, sack_payload, sizeof(SAckPayload));
    frame->header.checksum = _htonl(calculate_crc32(frame, sizeof(FrameHeader) + sizeof(SAckPayload)));

    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    return RET_VAL_SUCCESS;
}