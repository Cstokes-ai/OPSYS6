// user.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>

#define PAGE_COUNT 32
#define PAGE_SIZE 1024
#define MAX_ADDR (PAGE_COUNT * PAGE_SIZE)

typedef struct {
    long mtype;
    int pid;
    int address;
    int write; // 1 = write, 0 = read
} Message;

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <msqid> <shmid_clock> <shmid_pcbs> <index>\n", argv[0]);
        exit(1);
    }

    int msqid = atoi(argv[1]);
    int *clock = (int *)shmat(atoi(argv[2]), NULL, 0);
    int index = atoi(argv[4]);

    srand(time(NULL) ^ getpid());
    int requests = 0;

    while (requests++ < 5) { // Limit to 5 requests for faster testing
        Message msg;
        msg.mtype = 1;
        msg.pid = index;
        msg.address = rand() % MAX_ADDR; // Generate random address
        msg.write = (rand() % 100 < 20); // 20% write, 80% read

        printf("[DEBUG] P%d sending %s request for address %d\n",
               index, msg.write ? "write" : "read", msg.address);

        msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0);
        msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, 0); // Wait for OSS reply

        usleep(1000 + rand() % 5000); // Simulate delay
    }

    shmdt(clock);
    return 0;
}
