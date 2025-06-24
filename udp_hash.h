#ifndef _UDP_HASH_H
#define _UDP_HASH_H

#include "UDP_lib.h"

#define HASH_SIZE 65536

typedef struct AckHashNode{
    UdpFrame frame;
    time_t time;
    uint16_t counter;
    struct AckHashNode *next;
}AckHashNode;

typedef struct SeqNumNode{
    uint64_t seq_num;
    uint32_t id;
    struct SeqNumNode *next;
}SeqNumNode;


uint16_t get_hash(uint64_t seq_num){
    return (seq_num % HASH_SIZE);
}

void insert_frame(AckHashNode *hash_table[], UdpFrame *frame, uint32_t *count) {
    uint64_t seq_num = ntohll(frame->header.seq_num);
    uint16_t index = get_hash(seq_num);
//    fprintf(stdout, "SeqNum: %d inserted at index: %d\n", seq_num, index);
    AckHashNode *node = (AckHashNode *)malloc(sizeof(AckHashNode));
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->counter = 1;

    node->next = (AckHashNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    (*count)++;
    return;
}

void remove_frame(AckHashNode *hash_table[], uint64_t seq_num, uint32_t *count) {
    uint16_t index = get_hash(seq_num);
    AckHashNode *curr = hash_table[index];
    AckHashNode *prev = NULL;
    while (curr) {      
        if (ntohll(curr->frame.header.seq_num) == seq_num) {
            //fprintf(stdout, "Removing frame with seq num: %zu from index: %d\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;
            }
            free(curr);
            (*count)--;
            //fprintf(stdout, "Hash count: %d\n", *count);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void clean_frame_hash_table(AckHashNode *hash_table[], uint32_t *count){
    AckHashNode *head = NULL;
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){       
            AckHashNode *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    free(head);
                    (*count)--;
            }
            free(ptr);
            hash_table[i] = NULL;
        }     
    }
//    fprintf(stdout, "Frame hash table clean\n");
    return;
}


// void printTable(AckHashNode *hash_table[]) {
//     for (int i = 0; i < HASH_SIZE; i++) {
//         if(hash_table[i]){
//             printf("BUCKET %d: \n", i);           
//             AckHashNode *ptr = hash_table[i];
//             while (ptr) {     
//                     log_ack_hash_frame(&ptr->frame, ptr->time);                    
//                     ptr = ptr->next;
//             }
//         }     
//     }
// }

//--------------------------------------------------------------------------------------------------------------------------
void insert_seq_num(SeqNumNode *hash_table[], uint64_t seq_num, uint32_t id) {
    uint16_t index = get_hash(seq_num);
    //fprintf(stdout, "SeqNum: %d inserted at index: %d\n", seq_num, index);
    SeqNumNode *node = (SeqNumNode *)malloc(sizeof(SeqNumNode));
    node->seq_num = seq_num;
    node->id = id;
 
    node->next = (SeqNumNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    return;
}

void remove_seq_num(SeqNumNode *hash_table[], uint64_t seq_num) {
    uint16_t index = get_hash(seq_num); 
    SeqNumNode *curr = hash_table[index];
    SeqNumNode *prev = NULL;
    while (curr) {     
        if (curr->seq_num == seq_num) {
//            fprintf(stdout, "Removing seq num: %d from index: %d\n", seq_num, index);
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    return;
}

SeqNumNode *search_seq_num(SeqNumNode *hash_table[], uint64_t seq_num, uint32_t id) {
    uint16_t index = get_hash(seq_num);
    SeqNumNode *ptr = hash_table[index];
    while (ptr) {
        if (ptr->seq_num == seq_num && ptr->id == id){
            fprintf(stdout, "Received double SeqNum: %zu for ID: %d\n", ptr->seq_num, ptr->id);
            return ptr;
        }           
        ptr = ptr->next;
    }
    return NULL;
}

void print_seq_num_table(SeqNumNode *hash_table[]) {
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){
            printf("BUCKET %d: \n", i);           
            SeqNumNode *ptr = hash_table[i];
            while (ptr) {     
                    fprintf(stdout, "Bucket: %d - SeqNum: %zu\n", i, ptr->seq_num);                   
                    ptr = ptr->next;
            }
        }     
    }
    return;
}

void clean_seq_num_hash_table(SeqNumNode *hash_table[]){
    SeqNumNode *head = NULL;
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){       
            SeqNumNode *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    free(head);
            }
            free(ptr);
            hash_table[i] = NULL;
        }     
    }
    return;
}

//--------------------------------------------------------------------------------------------------------------------------











#endif