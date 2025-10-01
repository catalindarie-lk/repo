#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif


void init_mstream_pool(ServerMstreamPool* pool, const uint64_t block_count);
ServerMessageStream* find_mstream(ServerMstreamPool* pool, const uint32_t sid, const uint32_t mid);
ServerMessageStream* alloc_mstream(ServerMstreamPool* pool);
static uint8_t init_mstream(ServerMessageStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len);
void free_mstream(ServerMstreamPool* pool, ServerMessageStream* mstream);
void close_mstream(ServerMessageStream *mstream);
void destroy_mstream_pool(ServerMstreamPool* pool);

static uint8_t validate_fragment(ServerMessageStream *mstream, UdpFrame *frame);
static void attach_fragment(ServerMessageStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len);
static uint8_t check_completion_and_record(ServerMessageStream *mstream);



// HANDLE received message fragment frame
int handle_message_fragment(ServerClient *client, UdpFrame *frame);

#endif // MESSAGE_HANDLER_H