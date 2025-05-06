#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <signal.h>
#include <time.h>

#define MAX_MEMORY_REQUESTS 1000

typedef struct {
    long mtype;
    int pid;
    int address;
    int is_write; // 1 = write, 0 = read
} Message;

int msqid;
int *shared_clock;
int shmid_clock;
int total_requests = 0;
int total_reads = 0;
int total_writes = 0;

// Signal handler for cleanup
void handle_signal(int sig) {
    shmdt(shared_clock); // Detach shared memory
    exit(0);
}

// Function to generate memory requests
void generate_memory_request(Message *msg, int pid) {
    int page_number = rand() % 32; // Random page number (0-31)
    int offset = rand() % 1024;   // Random offset (0-1023)
    int address = (page_number * 1024) + offset; // Calculate memory address
    int is_write = (rand() % 10) < 3 ? 1 : 0;    // 30% chance of write

    msg->mtype = 1;
    msg->pid = pid;
    msg->address = address;
    msg->is_write = is_write;

    // Track statistics
    total_requests++;
    if (is_write) {
        total_writes++;
    } else {
        total_reads++;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        exit(1);
    }

    int pid = atoi(argv[1]); // Get process ID from command-line arguments
    srand(time(NULL) ^ pid); // Seed randomness with process ID

    // Attach to shared memory
    shmid_clock = shmget(ftok("oss.c", 1), sizeof(int) * 2, 0666);
    if (shmid_clock == -1) {
        perror("shmget");
        exit(1);
    }
    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    if (shared_clock == (void *)-1) {
        perror("shmat");
        exit(1);
    }

    // Attach to message queue
    msqid = msgget(ftok("oss.c", 2), 0666);
    if (msqid == -1) {
        perror("msgget");
        shmdt(shared_clock); // Detach shared memory before exiting
        exit(1);
    }

    // Set up signal handling
    signal(SIGINT, handle_signal);

    // Main loop for generating memory requests
    while (1) {
        Message msg;
        generate_memory_request(&msg, pid);

        // Send memory request to oss
        if (msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
            perror("msgsnd");
            break;
        }

        // Wait for response from oss
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), pid + 1, 0) == -1) {
            perror("msgrcv");
            break;
        }

        // Check for termination condition
        if (total_requests >= (1000 + (rand() % 200 - 100))) {
            // Inform oss of termination
            msg.mtype = 2; // Termination message type
            msg.pid = pid;
            if (msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                perror("msgsnd");
            }
            break;
        }
    }

    // Cleanup
    shmdt(shared_clock); // Detach shared memory
    return 0;
}