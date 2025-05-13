#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>

#define MAX_PROCESSES 18
#define MAX_FRAMES 256
#define PAGE_SIZE 1024
#define PAGES_PER_PROCESS 32
#define DISK_IO_TIME_NS 14000000

typedef struct {
    long mtype;
    int pid;
    int address;
    int write; // 1 = write, 0 = read
} Message;

typedef struct {
    int occupied;
    int pid;
    int page;
    int dirty;
    int lastRefSec;
    int lastRefNano;
} FrameTableEntry;

typedef struct {
    int pageTable[PAGES_PER_PROCESS];
} PCB;

int shmid_clock, shmid_pcbs;
int *sim_clock;
PCB *pcbs;
FrameTableEntry frameTable[MAX_FRAMES];
int msqid;
FILE *log_file;
pid_t child_pids[MAX_PROCESSES] = {0};
int launched = 0, total_created = 0;
int max_children = 100, simul_limit = 5, launch_interval_ms = 1000;
char log_filename[256] = "oss.log";

void print_usage(const char *progname) {
    printf("Usage: %s [-h] [-n proc] [-s simul] [-i intervalMs] [-f logfile]\n", progname);
}

void advance_clock(int sec, int ns) {
    sim_clock[1] += ns;
    sim_clock[0] += sec + sim_clock[1] / 1000000000;
    sim_clock[1] %= 1000000000;
}

void handle_sigint(int sig) {
    fprintf(stderr, "Master: Terminating on signal %d.\n", sig);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    while (wait(NULL) > 0);
    if (log_file) fclose(log_file);
    shmdt(sim_clock); shmctl(shmid_clock, IPC_RMID, NULL);
    shmdt(pcbs); shmctl(shmid_pcbs, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
    exit(0);
}

void init_shared_resources() {
    shmid_clock = shmget(IPC_PRIVATE, sizeof(int) * 2, IPC_CREAT | 0666);
    sim_clock = (int *)shmat(shmid_clock, NULL, 0);
    sim_clock[0] = sim_clock[1] = 0;

    shmid_pcbs = shmget(IPC_PRIVATE, sizeof(PCB) * MAX_PROCESSES, IPC_CREAT | 0666);
    pcbs = (PCB *)shmat(shmid_pcbs, NULL, 0);
    for (int i = 0; i < MAX_PROCESSES; i++)
        for (int j = 0; j < PAGES_PER_PROCESS; j++)
            pcbs[i].pageTable[j] = -1;

    msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
}

int find_free_frame() {
    for (int i = 0; i < MAX_FRAMES; i++)
        if (!frameTable[i].occupied) return i;
    return -1;
}

int find_lru_frame() {
    int lru = -1;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (!frameTable[i].occupied) continue;
        if (lru == -1 ||
            frameTable[i].lastRefSec < frameTable[lru].lastRefSec ||
            (frameTable[i].lastRefSec == frameTable[lru].lastRefSec &&
             frameTable[i].lastRefNano < frameTable[lru].lastRefNano)) {
            lru = i;
        }
    }
    return lru;
}

void print_memory_map() {
    fprintf(log_file, "\nCurrent memory layout at time %d:%d is:\n", sim_clock[0], sim_clock[1]);
    fprintf(log_file, "Occupied DirtyBit LastRefS LastRefNano\n");
    for (int i = 0; i < MAX_FRAMES; i++) {
        FrameTableEntry *f = &frameTable[i];
        if (f->occupied)
            fprintf(log_file, "Frame %d: Yes %d %d %d\n", i, f->dirty, f->lastRefSec, f->lastRefNano);
        else
            fprintf(log_file, "Frame %d: No  0 0\n", i);
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] > 0) {
            fprintf(log_file, "P%d page table: [ ", i);
            for (int j = 0; j < PAGES_PER_PROCESS; j++)
                fprintf(log_file, "%d ", pcbs[i].pageTable[j]);
            fprintf(log_file, "]\n");
        }
    }
    fflush(log_file);
}

void handle_memory_request(Message msg) {
    int pid = msg.pid;
    int addr = msg.address;
    int page = addr / PAGE_SIZE;
    int offset = addr % PAGE_SIZE;
    int frame = pcbs[pid].pageTable[page];

    fprintf(log_file, "oss: P%d requesting %s of address %05d at time %d:%d\n",
            pid, msg.write ? "write" : "read", addr, sim_clock[0], sim_clock[1]);

    if (frame != -1) {
        frameTable[frame].lastRefSec = sim_clock[0];
        frameTable[frame].lastRefNano = sim_clock[1];
        if (msg.write) frameTable[frame].dirty = 1;

        fprintf(log_file, "oss: Address %05d in frame %d, %s data to P%d at time %d:%d\n",
                addr, frame, msg.write ? "writing" : "giving", pid, sim_clock[0], sim_clock[1]);

        advance_clock(0, 100);
        msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0);
    } else {
        fprintf(log_file, "oss: Address %05d is not in a frame, pagefault\n", addr);

        int f = find_free_frame();
        if (f == -1) {
            f = find_lru_frame();
            FrameTableEntry *victim = &frameTable[f];
            fprintf(log_file, "oss: Clearing frame %d and swapping in P%d page %d\n", f, pid, page);
            if (victim->dirty) {
                fprintf(log_file, "oss: Dirty bit of frame %d set, adding additional time to the clock\n", f);
                advance_clock(0, DISK_IO_TIME_NS);
            }
            pcbs[victim->pid].pageTable[victim->page] = -1;
        }

        frameTable[f] = (FrameTableEntry){
            .occupied = 1, .pid = pid, .page = page, .dirty = msg.write,
            .lastRefSec = sim_clock[0], .lastRefNano = sim_clock[1]
        };
        pcbs[pid].pageTable[page] = f;

        advance_clock(0, DISK_IO_TIME_NS);
        fprintf(log_file, "oss: Indicating to P%d that %s has happened to address %05d\n",
                pid, msg.write ? "write" : "read", addr);
        msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0);
    }
    fflush(log_file);
}

void launch_child(int i) {
    pid_t pid = fork();
    if (pid == 0) {
        char msq_str[16], clk_str[16], pcb_str[16], idx_str[8];
        sprintf(msq_str, "%d", msqid);
        sprintf(clk_str, "%d", shmid_clock);
        sprintf(pcb_str, "%d", shmid_pcbs);
        sprintf(idx_str, "%d", i);
        execl("./user", "./user", msq_str, clk_str, pcb_str, idx_str, NULL);
        perror("execl");
        exit(1);
    } else {
        child_pids[i] = pid;
        launched++;
        total_created++;
        fprintf(log_file, "oss: Launched child P%d (PID %d) at %d:%d\n", i, pid, sim_clock[0], sim_clock[1]);
        fflush(log_file);
        printf("[DEBUG] Launched P%d (PID %d)\n", i, pid); // Console debug
    }
}

void check_children() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (child_pids[i] == pid) {
                fprintf(log_file, "oss: P%d (PID %d) terminated\n", i, pid);
                for (int j = 0; j < PAGES_PER_PROCESS; j++) {
                    int f = pcbs[i].pageTable[j];
                    if (f != -1) {
                        frameTable[f].occupied = 0;
                        pcbs[i].pageTable[j] = -1;
                    }
                }
                child_pids[i] = 0;
                launched--;
                break;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'n': max_children = atoi(optarg); break;
            case 's': simul_limit = atoi(optarg); break;
            case 'i': launch_interval_ms = atoi(optarg); break;
            case 'f': strncpy(log_filename, optarg, 255); break;
        }
    }

    srand(time(NULL));
    signal(SIGINT, handle_sigint);
    log_file = fopen(log_filename, "w");
    if (!log_file) { perror("fopen"); exit(1); }

    init_shared_resources();

    int loop = 0;
    int max_loops = 5000;

    while ((total_created < max_children || launched > 0) && loop < max_loops) {
        check_children();

        if (total_created < max_children) {
            for (int i = 0; i < MAX_PROCESSES && launched < simul_limit && total_created < max_children; i++) {
                if (child_pids[i] == 0) {
                    launch_child(i);
                }
            }
        }

        Message msg;
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            printf("[DEBUG] Received message from P%d: addr=%d %s\n", msg.pid, msg.address, msg.write ? "write" : "read");
            handle_memory_request(msg);
        }

        if (loop % 100 == 0)
            print_memory_map();

        advance_clock(0, 1000);
        usleep(launch_interval_ms * 1000);
        loop++;
    }

    handle_sigint(SIGINT);
    return 0;
}
