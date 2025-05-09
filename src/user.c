// user.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define MSG_KEY        0x2222
#define PAGE_SIZE      1024
#define REQUEST_TYPE   1

typedef struct {
    long mtype;    // 1=request, pid+1=reply
    int pid;
    int address;
    int is_write;
} Message;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr,"Usage: %s <sim_pid>\n",argv[0]);
        return 1;
    }
    int sim_pid = atoi(argv[1]);
    key_t key = MSG_KEY;
    int msqid = msgget(key,0666);
    if (msqid<0) { perror("msgget"); exit(1); }
    srand(getpid());

    // loop until kernel signals termination or we decide to stop
    for (int i=0; i<1000; i++) {
      Message msg = {
        .mtype   = REQUEST_TYPE,
        .pid     = sim_pid,
        .address = (rand()%PAGE_SIZE)*PAGE_SIZE + rand()%PAGE_SIZE,
        .is_write= (rand()%10)<3
      };
      msgsnd(msqid,&msg,sizeof(msg)-sizeof(long),0);
      // block waiting for reply
      Message rep;
      msgrcv(msqid,&rep,sizeof(rep)-sizeof(long),sim_pid+1,0);
      // simulate work
      usleep((rand()%100+1)*1000);
    }
    // done
    return 0;
}
