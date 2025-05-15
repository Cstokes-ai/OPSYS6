#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <time.h>

#define PAGE_COUNT 32   // Pages per process
#define REQUEST 1       // method type for read/write requests
#define TERMINATE 3     // method type for termination

typedef struct {
    long mtype;
    int pid;
    int page_number;
    int method; // 1 = Read, 2 = Write, 3 = Terminate
} Message;

int main(int argc, char *argv[]) {
    int user_index = 0;
    if (argc > 1) {
        user_index = atoi(argv[1]);
    }

    int msgid;
    key_t key = ftok("oss.c", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);

    srand(getpid()); // Seed random number generator with PID for randomness
    int pid = getpid();
    int runtime = 0;

    printf("{DEBUG} USER P%d started\n", user_index);

    while (runtime < 5000) { 
        runtime += rand() % 100 + 1; //runtime random increment 1-100 ms
        
        // Generate a random page number to request
        int page_number = rand() % PAGE_COUNT;
        int offset = rand() % 1024;
        int address = page_number * 1024 + offset;

        // Randomly decide whether to read or write
        int is_write = rand() % 2;
        Message msg;
        msg.mtype = 1; // Messages to OSS are of type 1
        msg.pid = pid;
        msg.page_number = page_number;
        msg.method = is_write ? REQUEST + 1 : REQUEST; // 1 = Read, 2 = Write

        // Send the request message to OSS
        msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0);
        printf("{DEBUG} USER P%d %s address %d (page %d, offset %d)\n",
            user_index, is_write ? "writing to" : "reading from", address, page_number, offset);

        // Simulate a random delay before the next request
        struct timespec ts = {0, (rand() % 100 + 1) * 1000000}; // Random delay 1-100 ms
        nanosleep(&ts, NULL);

        // Randomly decide whether to terminate (10% chance)
        if (rand() % 100 < 10) {
            msg.method = TERMINATE;
            msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0);
            printf("{DEBUG} USER P%d terminating\n", user_index);
            break;
        }
    }

    printf("{DEBUG} USER P%d Done\n", user_index);
    return 0;
}