#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <windows.h>            // For BOOL type

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

// To mark a fragment as received
void mark_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size);

// To check if already received
BOOL check_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size);

// Check if bitmap is full (all fragments received)
BOOL check_bitmap(uint64_t bitmap[], uint32_t fragment_count);

// Check if a flag is set in a bitmap
BOOL check_flag(uint8_t flag[], uint32_t fragment_count);

#endif