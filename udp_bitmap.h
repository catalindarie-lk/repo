#ifndef _UDP_BITMAP_H
#define _UDP_BITMAP_H

#include "UDP_lib.h"

// To mark a fragment as received
void mark_fragment_received(uint32_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
    uint64_t bitmap_index = fragment_offset / fragment_size;
    
    bitmap[bitmap_index / 32] |= (1 << (bitmap_index % 32));
//    fprintf(stdout, "offset: %d, size: %d, index: %d - bitmapped: %X\n", fragment_offset, fragment_size, bitmap_index, bitmap[bitmap_index / 32]);
    return;
 
}

// To check if already received
BOOL check_fragment_received(uint32_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
  
    uint64_t bitmap_index = fragment_offset / fragment_size;
    return (bitmap[bitmap_index / 32] & (1 << (bitmap_index % 32))) != 0;
}

// Check if bitmap is full (all fragments received)
BOOL check_bitmap(uint32_t bitmap[], uint32_t fragment_count){

    uint32_t bitmap_index;
    bitmap_index = fragment_count / 32;
    if(fragment_count % 32 > 0){
        bitmap_index++;
    }
    uint32_t mask = 0xFFFFFFFF;
    for(uint32_t i = 0; i < bitmap_index - 1; i++){
        if(bitmap[i] != mask){
            fprintf(stdout, "Bitmap NOK\n");
            return FALSE;
        }             
    }

    mask = 0x0;
    if (fragment_count % 32 == 0) {
        mask = 0x0;
        return TRUE;
    } else {
        mask = (1U << fragment_count % 32) - 1;
        if(bitmap[bitmap_index - 1] != mask){
            fprintf(stdout, "Bitmap NOK\n");
            return FALSE;
        } 
        
    }
//    fprintf(stdout, "Bitmap OK\n");
    return(TRUE);    
}

#endif