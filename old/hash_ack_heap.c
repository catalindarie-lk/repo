#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <windows.h> // For sleep function

// --- Configuration ---
#define MAX_BUFFER_SIZE 1024 // Max number of unacknowledged packets
#define HASH_TABLE_SIZE 1024 // Size of the hash map (should be prime)
#define RTO_MILLISECONDS 500 // Retransmission Timeout (example: 500ms)

// --- Memory Pool Configuration ---
// Define max capacity for each type of object in their pools
#define MAX_PACKET_INFO_POOL_SIZE MAX_BUFFER_SIZE
#define MAX_HASH_ENTRY_POOL_SIZE (MAX_BUFFER_SIZE * 2) // More hash entries than packets for collisions

// --- Packet Structures ---
typedef struct PacketInfo {
    unsigned int sequence_number;
    time_t last_sent_time;
    int retransmission_count;
    bool is_acknowledged;
    char* data; // Pointer to actual packet data (from a separate buffer pool)
    size_t data_len;
} PacketInfo;

typedef struct HashEntry {
    unsigned int sequence_number;
    PacketInfo* packet_info_ptr;
    struct HashEntry* next;
} HashEntry;

typedef struct HeapNode {
    time_t retransmit_deadline;
    unsigned int sequence_number;
} HeapNode;

// --- Memory Pool Definitions ---

// Generic Node for a free list in the pool
typedef struct FreeListNode {
    struct FreeListNode* next;
} FreeListNode;

// Generic Memory Pool structure
typedef struct MemoryPool {
    void* buffer; // Raw memory block
    size_t element_size;
    size_t capacity;
    FreeListNode* free_list_head; // Head of the linked list of free elements
} MemoryPool;

// PacketInfo Pool
PacketInfo packet_info_pool_data[MAX_PACKET_INFO_POOL_SIZE];
MemoryPool packet_info_pool;

// HashEntry Pool
HashEntry hash_entry_pool_data[MAX_HASH_ENTRY_POOL_SIZE];
MemoryPool hash_entry_pool;

// Raw data buffer pool (conceptual, not fully implemented here)
// Example: char raw_data_pool_data[MAX_BUFFER_SIZE][MAX_PACKET_DATA_SIZE];
// MemoryPool raw_data_buffer_pool;


// --- Memory Pool Functions ---

void pool_init(MemoryPool* pool, void* buffer, size_t element_size, size_t capacity) {
    pool->buffer = buffer;
    pool->element_size = element_size;
    pool->capacity = capacity;
    pool->free_list_head = NULL;

    // Initialize free list: link all elements in the buffer
    for (size_t i = 0; i < capacity; ++i) {
        FreeListNode* node = (FreeListNode*)((char*)pool->buffer + i * element_size);
        node->next = pool->free_list_head;
        pool->free_list_head = node;
    }
}

void* pool_alloc(MemoryPool* pool) {
    if (!pool->free_list_head) {
        fprintf(stderr, "Error: Memory pool exhausted!\n");
        return NULL; // Or handle error appropriately (e.g., expand pool)
    }
    void* allocated_mem = pool->free_list_head;
    pool->free_list_head = pool->free_list_head->next;
    return allocated_mem;
}

void pool_free(MemoryPool* pool, void* element) {
    if (!element) return;
    FreeListNode* node = (FreeListNode*)element;
    node->next = pool->free_list_head;
    pool->free_list_head = node;
}


// --- Global Data Structures for Sender (using pools) ---

PacketInfo* packet_buffer[MAX_BUFFER_SIZE]; // Now stores pointers to pooled PacketInfo
int buffer_head = 0;
int buffer_tail = 0;

HashEntry* hash_table[HASH_TABLE_SIZE];

HeapNode retransmit_heap[MAX_BUFFER_SIZE + 1];
int heap_size = 0;

unsigned int last_acknowledged_seq_nr = -1;
unsigned int next_sequence_number_to_send = 0;


// --- Hash Map Functions (Adapted for Pools) ---

unsigned int hash_function(unsigned int seq_nr) {
    return seq_nr % HASH_TABLE_SIZE;
}

void hash_insert(unsigned int seq_nr, PacketInfo* info_ptr) {
    unsigned int index = hash_function(seq_nr);
    HashEntry* new_entry = (HashEntry*)pool_alloc(&hash_entry_pool);
    if (!new_entry) {
        perror("Failed to allocate hash entry from pool");
        exit(EXIT_FAILURE);
    }
    new_entry->sequence_number = seq_nr;
    new_entry->packet_info_ptr = info_ptr;
    new_entry->next = hash_table[index];
    hash_table[index] = new_entry;
}

PacketInfo* hash_lookup(unsigned int seq_nr) {
    unsigned int index = hash_function(seq_nr);
    HashEntry* current = hash_table[index];
    while (current) {
        if (current->sequence_number == seq_nr) {
            return current->packet_info_ptr;
        }
        current = current->next;
    }
    return NULL;
}

void hash_remove(unsigned int seq_nr) {
    unsigned int index = hash_function(seq_nr);
    HashEntry* current = hash_table[index];
    HashEntry* prev = NULL;

    while (current) {
        if (current->sequence_number == seq_nr) {
            if (prev) {
                prev->next = current->next;
            } else {
                hash_table[index] = current->next;
            }
            pool_free(&hash_entry_pool, current); // Return to pool
            return;
        }
        prev = current;
        current = current->next;
    }
}

// --- Min-Heap Functions (Same as before, not pool-dependent) ---

void swap_heap_nodes(int i, int j) {
    HeapNode temp = retransmit_heap[i];
    retransmit_heap[i] = retransmit_heap[j];
    retransmit_heap[j] = temp;
}

void heapify_down(int idx) {
    int smallest = idx;
    int left = 2 * idx + 1;
    int right = 2 * idx + 2;

    if (left < heap_size && retransmit_heap[left].retransmit_deadline < retransmit_heap[smallest].retransmit_deadline) {
        smallest = left;
    }
    if (right < heap_size && retransmit_heap[right].retransmit_deadline < retransmit_heap[smallest].retransmit_deadline) {
        smallest = right;
    }

    if (smallest != idx) {
        swap_heap_nodes(idx, smallest);
        heapify_down(smallest);
    }
}

void heapify_up(int idx) {
    int parent = (idx - 1) / 2;
    while (idx > 0 && retransmit_heap[parent].retransmit_deadline > retransmit_heap[idx].retransmit_deadline) {
        swap_heap_nodes(idx, parent);
        idx = parent;
        parent = (idx - 1) / 2;
    }
}

void heap_insert(time_t deadline, unsigned int seq_nr) {
    if (heap_size >= MAX_BUFFER_SIZE) {
        fprintf(stderr, "Heap is full, cannot insert.\n");
        return;
    }
    retransmit_heap[heap_size].retransmit_deadline = deadline;
    retransmit_heap[heap_size].sequence_number = seq_nr;
    heap_size++;
    heapify_up(heap_size - 1);
}

HeapNode heap_extract_min() {
    if (heap_size <= 0) {
        return (HeapNode){ .retransmit_deadline = -1, .sequence_number = -1 };
    }
    HeapNode min_node = retransmit_heap[0];
    retransmit_heap[0] = retransmit_heap[heap_size - 1];
    heap_size--;
    heapify_down(0);
    return min_node;
}

void heap_remove_seq_nr(unsigned int seq_nr) {
    int i;
    for (i = 0; i < heap_size; ++i) {
        if (retransmit_heap[i].sequence_number == seq_nr) {
            break;
        }
    }
    if (i < heap_size) {
        retransmit_heap[i] = retransmit_heap[heap_size - 1];
        heap_size--;
        if (i < heap_size) {
            heapify_down(i);
            heapify_up(i);
        }
    }
}


// --- Sender Buffer Logic (Adapted for Pools) ---

void sender_buffer_init() {
    // Initialize memory pools
    pool_init(&packet_info_pool, packet_info_pool_data, sizeof(PacketInfo), MAX_PACKET_INFO_POOL_SIZE);
    pool_init(&hash_entry_pool, hash_entry_pool_data, sizeof(HashEntry), MAX_HASH_ENTRY_POOL_SIZE);

    memset(hash_table, 0, sizeof(hash_table));
    memset(packet_buffer, 0, sizeof(packet_buffer)); // Clear pointers
    heap_size = 0;
    last_acknowledged_seq_nr = -1;
    next_sequence_number_to_send = 0;
    buffer_head = 0;
    buffer_tail = 0;
}

void send_packet_to_network(PacketInfo* info) {
//    printf("SENDING Packet (Seq: %u, Data: '%.*s') at time %ld\n",
//           info->sequence_number, (int)info->data_len, info->data, time(NULL));
}

void handle_new_packet_to_send(const char* data, size_t data_len) {
    // Allocate PacketInfo from pool
    PacketInfo* new_packet = (PacketInfo*)pool_alloc(&packet_info_pool);
    if (!new_packet) {
        printf("PACKET_INFO_POOL FULL. Cannot send packet (Seq: %u).\n", next_sequence_number_to_send);
        return;
    }

    // In a real system, you'd also allocate raw packet data from a dedicated pool
    // new_packet->data = (char*)pool_alloc(&raw_data_buffer_pool, data_len);
    // if (!new_packet->data) {
    //    pool_free(&packet_info_pool, new_packet); // Free PacketInfo if data allocation fails
    //    printf("RAW_DATA_BUFFER_POOL FULL. Cannot send packet (Seq: %u).\n", next_sequence_number_to_send);
    //    return;
    // }
    // For this example, we'll use malloc for data for simplicity, but avoid it for struct metadata
    new_packet->data = (char*)malloc(data_len);
    if (!new_packet->data) {
        perror("Failed to allocate packet data (using malloc for example)");
        pool_free(&packet_info_pool, new_packet);
        exit(EXIT_FAILURE);
    }
    memcpy(new_packet->data, data, data_len);
    new_packet->data_len = data_len;


    new_packet->sequence_number = next_sequence_number_to_send;
    new_packet->last_sent_time = time(NULL);
    new_packet->retransmission_count = 0;
    new_packet->is_acknowledged = false;

    // Store pointer in circular buffer
    packet_buffer[buffer_tail] = new_packet; // Store pointer to pooled object
    buffer_tail = (buffer_tail + 1) % MAX_BUFFER_SIZE;

    // Add to hash map for quick lookup
    hash_insert(new_packet->sequence_number, new_packet);

    // Add to min-heap for retransmission scheduling
    heap_insert(new_packet->last_sent_time + (RTO_MILLISECONDS / 1000), new_packet->sequence_number);

    send_packet_to_network(new_packet);

    next_sequence_number_to_send++;
}

void handle_ack_received(unsigned int acked_seq_nr) {
    printf("RECEIVED ACK for Seq: %u\n", acked_seq_nr);

    PacketInfo* acked_packet_info = hash_lookup(acked_seq_nr);

    if (acked_packet_info && !acked_packet_info->is_acknowledged) {
        acked_packet_info->is_acknowledged = true;
        printf("Packet (Seq: %u) marked as acknowledged.\n", acked_seq_nr);

        hash_remove(acked_seq_nr);
        heap_remove_seq_nr(acked_seq_nr);

        // Free the actual packet data (if allocated with malloc for this example)
        if (acked_packet_info->data) {
            free(acked_packet_info->data);
            acked_packet_info->data = NULL;
        }
        // In a real system, return raw data buffer to its pool:
        // pool_free(&raw_data_buffer_pool, acked_packet_info->data);

        // Return PacketInfo struct to its pool
        pool_free(&packet_info_pool, acked_packet_info);

        // Advance LARS and slide window
        // Note: For a true circular buffer and efficient windowing, you'd explicitly
        // check packet_buffer[buffer_head] and advance buffer_head if it's acknowledged.
        // This simple example's LARS update is mostly conceptual.
        while ((last_acknowledged_seq_nr + 1) < next_sequence_number_to_send) {
            // Need a way to quickly check if packet_buffer[next_seq % MAX_BUFFER_SIZE] is acknowledged.
            // Lookup in hash map is simplest for this example, but implies it's still there.
            PacketInfo* potential_next_acked = hash_lookup(last_acknowledged_seq_nr + 1);
            if (!potential_next_acked && (last_acknowledged_seq_nr + 1) <= next_sequence_number_to_send) {
                // If not in hash map, it means it was acknowledged and removed.
                // This logic is simplified; a robust windowing would require
                // checking status directly from packet_buffer based on indices.
                last_acknowledged_seq_nr++;
            } else {
                break;
            }
        }
        printf("Last acknowledged sequence number advanced to: %u\n", last_acknowledged_seq_nr);

    } else if (acked_packet_info && acked_packet_info->is_acknowledged) {
        printf("Packet (Seq: %u) already acknowledged (duplicate ACK).\n", acked_seq_nr);
    } else {
        printf("ACK for unknown or already cleaned-up packet (Seq: %u). Note: If data was free'd, pointer is invalid.\n", acked_seq_nr);
    }
}

void check_for_retransmissions() {
    time_t current_time = time(NULL);
//    printf("Checking for retransmissions at time %ld...\n", current_time);

    while (heap_size > 0 && retransmit_heap[0].retransmit_deadline <= current_time) {
        HeapNode expired_node = heap_extract_min();
        unsigned int seq_nr_to_retransmit = expired_node.sequence_number;

        PacketInfo* packet_to_retransmit = hash_lookup(seq_nr_to_retransmit);

        if (packet_to_retransmit && !packet_to_retransmit->is_acknowledged) {
            packet_to_retransmit->retransmission_count++;
            packet_to_retransmit->last_sent_time = current_time;
            printf("RETRANSMITTING Packet (Seq: %u, Count: %d)\n",
                   packet_to_retransmit->sequence_number, packet_to_retransmit->retransmission_count);
            send_packet_to_network(packet_to_retransmit);

            heap_insert(packet_to_retransmit->last_sent_time + (RTO_MILLISECONDS / 1000),
                        packet_to_retransmit->sequence_number);
        } else {
            printf("Packet (Seq: %u) was acknowledged before retransmission check.\n", seq_nr_to_retransmit);
        }
    }
}

// --- Main Simulation ---

int main() {
    sender_buffer_init();

    // Simulate sending some packets
    handle_new_packet_to_send("Hello 0", 7);
    handle_new_packet_to_send("World 1", 7);
    handle_new_packet_to_send("Packet 2", 8);
    handle_new_packet_to_send("Data 3", 6);

    printf("\n-- Simulating some time passing --\n\n");
    Sleep(1);

    check_for_retransmissions();

    printf("\n-- Simulating some ACKs arriving --\n\n");
    handle_ack_received(1);
    handle_ack_received(3);

    printf("\n-- Simulating more time passing --\n\n");
    Sleep(1);

    check_for_retransmissions();

    printf("\n-- Simulating final ACKs --\n\n");
    handle_ack_received(0);
    handle_ack_received(2);

    printf("\n-- Final check for retransmissions (should be none) --\n\n");
    check_for_retransmissions();

    // In a real application, a proper shutdown would involve ensuring all pools are truly empty
    // or freeing the underlying `pool->buffer` if it was heap-allocated initially.
    // For this example, pools use static arrays, so no explicit free needed at end of main.
    // But data allocated with malloc (new_packet->data) still needs to be freed explicitly
    // if not returned to a data buffer pool. The current logic handles this.

    return 0;
}