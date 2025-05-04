#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>

#define MAX_RESOURCES 5
#define MAX_INSTANCES 10

typedef struct {
    long mtype;
    int pid;
    int resource;
    int quantity;
    int request; // 1 = request, 0 = release
} Message;

void memory_request(int pid, int address, int is_write){
    //gnenerate random page number between 0 and 31 (32 pages per processes)
    int page_number = rand() % 32;
    //random offset between 0 and 1023(1kb page size)
    int offset = rand() % 1024;
    // calc memory address by combining the page number and offset
    int memory_address = (page_number * 1024) + offset;
}

void read_write(int pid, int address, int is_write) {
    //use randomness to decide if the request is a read or a write
    int random = rand() % 2;
    if random == 0) {
        is_ write = 0; // read
    } else {
        is_write = 1; // write
    }
    //also need bias
}