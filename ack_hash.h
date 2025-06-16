#ifndef _ACK_HASH_H
#define _ACK_HASH_H

#include "UDP_lib.h"
#define HASH_SIZE 65535


typedef struct AckHashNode{
    UdpFrame frame;
    time_t time;
    uint16_t counter;
    struct AckHashNode *next;
}AckHashNode;

//void log_ack_hash_frame(UdpFrame *frame);
void log_ack_hash_frame(UdpFrame *frame, time_t time);
uint16_t get_hash(uint32_t seq_num);
void remove_frame(AckHashNode *hash_table[], uint32_t seq_num);
void printTable(AckHashNode *hash_table[]);

uint16_t get_hash(uint32_t seq_num){
    return seq_num % HASH_SIZE;
}

void insert_frame(AckHashNode *hash_table[], UdpFrame *frame) {
    uint32_t seq_num = ntohl(frame->header.seq_num);
    uint16_t index = get_hash(seq_num);
//    fprintf(stdout, "SeqNum: %d inserted at index: %d\n", seq_num, index);
    AckHashNode *node = (AckHashNode *)malloc(sizeof(AckHashNode));
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->counter = 1;

    node->next = (AckHashNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;

}

void remove_frame(AckHashNode *hash_table[], uint32_t seq_num) {
    uint16_t index = get_hash(seq_num);
    
    AckHashNode *curr = hash_table[index];
    AckHashNode *prev = NULL;

    while (curr) {
        
        if (ntohl(curr->frame.header.seq_num) == seq_num) {
//            fprintf(stdout, "Removing seq num: %d from index: %d\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;  // Removing head
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    // Optional: log if not found
    // printf("Frame with seq %u not found\n", seq_num);
}

void printTable(AckHashNode *hash_table[]) {
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){
            printf("BUCKET %d: \n", i);           
            AckHashNode *ptr = hash_table[i];
            while (ptr) {     
                    log_ack_hash_frame(&ptr->frame, ptr->time);
                     
                    ptr = ptr->next;
            }
        }     
    }
}

// void log_ack_hash_frame(UdpFrame *frame){
   
//     switch(frame->header.frame_type){
 
//         case FRAME_TYPE_LONG_TEXT_MESSAGE:
//             fprintf(stdout, "   FRAME_TYPE_LONG_TEXT_MESSAGE\n   Seq Num: %d\n   Session ID: %d\n   Checksum: %d\n   Message ID: %d\n   Total Length: %d\n   Fragment Length: %d\n   Fragment Offset: %d\n   Fragment Text: %s\n", 
//                                                     ntohl(frame->header.seq_num), 
//                                                     ntohl(frame->header.session_id), 
//                                                     ntohl(frame->header.checksum),
//                                                     ntohl(frame->payload.long_text_msg.message_id), 
//                                                     ntohl(frame->payload.long_text_msg.total_text_len),
//                                                     ntohl(frame->payload.long_text_msg.fragment_len),
//                                                     ntohl(frame->payload.long_text_msg.fragment_offset), 
//                                                     frame->payload.long_text_msg.fragment_text);
//             break;

//         default:
//             break;
//     }
//     return;

// }

void log_ack_hash_frame(UdpFrame *frame, time_t time){
   
    switch(frame->header.frame_type){
 
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            fprintf(stdout, "Seq Num: %d - time: %ul\n", ntohl(frame->header.seq_num), (unsigned long)time);
            break;

        default:
            break;
    }
    return;

}

#endif