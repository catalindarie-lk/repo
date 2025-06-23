#include <stdio.h>
#include <stdlib.h>

#define HASH_SIZE 10

typedef struct Node {
    int key;
    int value;
    struct Node *next;
} Node;

Node *hashTable[HASH_SIZE] = {NULL};

int hashFunction(int key) {
    return key % HASH_SIZE;
}

void insert(int key, int value) {
    int index = hashFunction(key);
    Node *newNode = (Node *)malloc(sizeof(Node));
    newNode->key = key;
    newNode->value = value;
    newNode->next = hashTable[index];  // Insert at the head (linked list)
    hashTable[index] = newNode;
}

Node *search(int key) {
    int index = hashFunction(key);
    Node *ptr = hashTable[index];
    while (ptr) {
        if (ptr->key == key) return ptr;
        ptr = ptr->next;
    }
    return NULL;
}

void delete(int key) {
    int index = hashFunction(key);
    Node **ptr = &hashTable[index];
    while (*ptr) {
        if ((*ptr)->key == key) {
            Node *temp = *ptr;
            *ptr = temp->next;
            free(temp);
            return;
        }
        ptr = &((*ptr)->next);
    }
}

void printTable() {
    for (int i = 0; i < HASH_SIZE; i++) {
        printf("Bucket %d: ", i);
        Node *ptr = hashTable[i];
        while (ptr) {
            printf("(%d, %d) -> ", ptr->key, ptr->value);
            ptr = ptr->next;
        }
        printf("NULL\n");
    }
}

int main() {
    insert(10, 100);
    insert(20, 200);
    insert(30, 300);
    insert(11, 110);  // Collision with bucket 1
    printTable();

    Node *found = search(20);
    if (found) printf("Found key %d with value %d\n", found->key, found->value);

    delete(20);
    printTable();

    return 0;
}