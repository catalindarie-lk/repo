
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <stdbool.h>            // For BOOL type
#include <string.h>             // For memset
#include <windows.h>            // Required for CRITICAL_SECTION and related functions
#include "include/bitmap.h"

// To mark a fragment as received
void mark_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
    uint64_t bitmap_index = fragment_offset / (uint64_t)fragment_size;
    bitmap[bitmap_index / 64ULL] |= (1ULL << (bitmap_index % 64ULL));
    //fprintf(stdout, "Marking fragment received %llu, bitmap index: %llu\n", fragment_offset, bitmap_index);
    //fprintf(stdout, "bitmap[%llu] = %llX\n", bitmap_index / 64ULL, bitmap[bitmap_index / 64ULL]);
    return;
}

// To check if already received
BOOL check_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
  
    uint64_t index = fragment_offset / (uint64_t)fragment_size;  
    //fprintf(stdout, "Check fragment received %llu, bitmap index: %llu\n", fragment_offset, bitmap_index);
    return (bitmap[index / 64ULL] & (1ULL << (index % 64ULL))) != 0ULL;
}

// Check if bitmap is full (all fragments received)
BOOL check_bitmap(uint64_t bitmap[], uint64_t fragment_count){

    // Edge case: An empty file has no fragments, so it's considered complete.
    if (fragment_count == 0) {
        fprintf(stderr, "CRITICAL ERROR: Attempting to check bitmap for zero fragments!\n");
        return FALSE;
    }

    // Calculate the total number of uint64_t entries needed for the bitmap
    // This is equivalent to bitmap_entries_count in file_init_recv_slot
    uint64_t num_bitmap_entries =((fragment_count - 1ULL) / 64ULL) + 1ULL;

    uint64_t full_chunk_mask = ~0ULL; // Mask for a completely full 64-bit chunk

    // 1. Check all bitmap entries EXCEPT the very last one.
    // These entries represent full blocks of 64 fragments and must be all ones.
    for(uint64_t i = 0; i < num_bitmap_entries - 1ULL; i++){
        if(bitmap[i] != full_chunk_mask){
            fprintf(stderr, "Bitmap NOK: Missing fragments in full chunk entry %llu\n", i);
            return FALSE;
        }
    }

    // 2. Handle the last bitmap entry.
    // This entry might be full (~0ULL) or partial.
    uint64_t last_entry_index = num_bitmap_entries - 1ULL;
    uint64_t expected_last_mask;
    
    // Calculate how many actual fragments are in the last 64-bit entry
    uint32_t fragments_in_last_entry = fragment_count % 64ULL;

    if (fragments_in_last_entry == 0) {
        // If fragment_count is a perfect multiple of 64, the last entry should also be full.
        //return TRUE;
        expected_last_mask = full_chunk_mask;
    } else {
        // If it's a partial last entry, create a mask with only the relevant bits set.
        // E.g., if 3 fragments, mask is 0b...0111 (2^3 - 1)
        expected_last_mask = (1ULL << fragments_in_last_entry) - 1ULL;
    }

    // Compare the actual last bitmap entry with the expected mask
    if(bitmap[last_entry_index] != expected_last_mask){
        fprintf(stderr, "Bitmap NOK: Mismatch in last entry %llu. Expected 0x%llX, Got 0x%llX\n",
                last_entry_index, expected_last_mask, bitmap[last_entry_index]);
        return FALSE;
    }
    
    // If all checks pass, the bitmap indicates all fragments are received.
    return TRUE;
}

// BOOL check_flag(uint8_t flag[], uint32_t fragment_count){

//     // Edge case: An empty file has no fragments, so it's considered complete.
//     if (fragment_count == 0) {
//         return TRUE;
//     }

//     // Calculate the total number of uint64_t entries needed for the bitmap
//     // This is equivalent to bitmap_entries_count in file_init_recv_slot
//     uint64_t num_bitmap_entries = (uint64_t)(fragment_count + 64ULL - 1ULL) / 64ULL;

//     for(uint64_t i = 0; i < num_bitmap_entries - 1ULL; i++){
//         if(flag[i] != 1){
//             fprintf(stderr, "Bitmap NOK: Flag missing %llu\n", i);
//             return FALSE;
//         }
//     }
    
//     // If all checks pass, the bitmap indicates all fragments are received.
//     return TRUE;
// }
