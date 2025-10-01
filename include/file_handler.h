#ifndef PROTOCOL_FRAME_HANDLERS_H
#define PROTOCOL_FRAME_HANDLERS_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

void init_fstream_pool(ServerFstreamPool* pool, const uint64_t block_count);
ServerFileStream* find_fstream(ServerFstreamPool* pool, const uint32_t sid, const uint32_t fid);
ServerFileStream* alloc_fstream(ServerFstreamPool* pool);
void free_fstream(ServerFstreamPool* pool, ServerFileStream* fstream);
void close_fstream(ServerFileStream *fstream);
void destroy_fstream_pool(ServerFstreamPool* pool);
uint8_t init_fstream(ServerFileStream *fstream, UdpFrame *frame, const struct sockaddr_in *client_addr);

void handle_file_metadata(ServerClient *client, UdpFrame *frame);
void handle_file_fragment(ServerClient *client, UdpFrame *frame);
void handle_file_end(ServerClient *client, UdpFrame *frame);

#endif // FRAME_HANDLERS_H