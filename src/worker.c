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

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <msqid> <shmid_clock> <index>\n", argv[0]);
        return 1;
    }

    int msqid = atoi(argv[1]);
    int shmid_clock = atoi(argv[2]);
    int local_index = atoi(argv[3]);
    int *shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    int pid = getpid();

    srand(time(NULL) ^ pid);

    for (int i = 0; i < 5; i++) {
        Message msg;
        msg.mtype = 1;
        msg.pid = local_index;
        msg.resource = rand() % MAX_RESOURCES;
        msg.quantity = (rand() % MAX_INSTANCES) + 1;
        msg.request = rand() % 2;

        msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0);

        if (msg.request == 1)
            printf("User %d requesting %d of R%d\n", local_index, msg.quantity, msg.resource);
        else
            printf("User %d releasing %d of R%d\n", local_index, msg.quantity, msg.resource);

        usleep(100000);
    }

    shmdt(shared_clock);
    return 0;
}
