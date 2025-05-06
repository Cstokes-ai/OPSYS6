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
#include <limits.h>

#define MAX_PROCESSES 18
#define FRAME_COUNT 256 // 256 frames for 128KB memory
#define PAGE_COUNT 32   // 32 pages per process
#define MAX_REQUESTS 100

typedef struct {
    long mtype;
    int pid;
    int address;
    int is_write; // 1 = write, 0 = read
} Message;

typedef struct {
    int pid;
    int page_number;
    int dirty_bit;
    int last_access_time;      // in seconds
    int last_access_nano_time; // in nanoseconds
} Frame;

int shmid_clock;
int msqid;
int *shared_clock;
FILE *log_file;
pid_t child_pids[MAX_PROCESSES] = {0};

int max_children = 5;
int simul_limit = 5;
int launch_interval_ms = 1000;
char log_filename[256] = "oss.log";

Frame frame_table[FRAME_COUNT];
int page_table[MAX_PROCESSES][PAGE_COUNT];

void print_usage(const char *progname) {
    printf("Usage: %s [-h] [-n proc] [-s simul] [-i intervalMs] [-f logfile]\n", progname);
}

void increment_clock() {
    shared_clock[1] += 14000000; // Increment clock by 14ms
    if (shared_clock[1] >= 1000000000) {
        shared_clock[0]++;
        shared_clock[1] -= 1000000000;
    }
}

void initialize_frame_table() {
    for (int i = 0; i < FRAME_COUNT; i++) {
        frame_table[i].pid = -1; // Mark all frames as free
        frame_table[i].page_number = -1;
        frame_table[i].dirty_bit = 0;
        frame_table[i].last_access_time = 0;
        frame_table[i].last_access_nano_time = 0;
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        for (int j = 0; j < PAGE_COUNT; j++) {
            page_table[i][j] = -1; // Mark all page table entries as invalid
        }
    }
}

int lru_algorithm() {
    int lru_index = -1;
    int oldest_time = INT_MAX;
    int oldest_nano_time = INT_MAX;

    for (int i = 0; i < FRAME_COUNT; i++) {
        if (frame_table[i].pid != -1) { // Only consider frames in use
            if (frame_table[i].last_access_time < oldest_time ||
                (frame_table[i].last_access_time == oldest_time &&
                 frame_table[i].last_access_nano_time < oldest_nano_time)) {
                lru_index = i;
                oldest_time = frame_table[i].last_access_time;
                oldest_nano_time = frame_table[i].last_access_nano_time;
            }
        }
    }

    return lru_index;
}

void handle_memory_request(Message msg) {
    fprintf(log_file, "oss: P%d requesting %s of address %d at time %d:%d\n",
            msg.pid, msg.is_write ? "write" : "read", msg.address, shared_clock[0], shared_clock[1]);

    int page_number = msg.address / 1024;
    int offset = msg.address % 1024;

    int page_in_memory = 0;
    for (int i = 0; i < FRAME_COUNT; i++) {
        if (frame_table[i].pid == msg.pid && frame_table[i].page_number == page_number) {
            page_in_memory = 1;
            frame_table[i].last_access_time = shared_clock[0];
            frame_table[i].last_access_nano_time = shared_clock[1];
            if (msg.is_write) {
                frame_table[i].dirty_bit = 1;
            }
            fprintf(log_file, "oss: Address %d in frame %d, %s data to P%d at time %d:%d\n",
                    msg.address, i, msg.is_write ? "writing" : "giving", msg.pid, shared_clock[0], shared_clock[1]);
            break;
        }
    }

    if (!page_in_memory) {
        fprintf(log_file, "oss: Address %d is not in a frame, pagefault\n", msg.address);

        int free_frame = -1;
        for (int i = 0; i < FRAME_COUNT; i++) {
            if (frame_table[i].pid == -1) {
                free_frame = i;
                break;
            }
        }

        int frame_to_replace = free_frame;
        if (free_frame == -1) {
            frame_to_replace = lru_algorithm();
            fprintf(log_file, "oss: Clearing frame %d and swapping in P%d page %d\n",
                    frame_to_replace, msg.pid, page_number);

            if (frame_table[frame_to_replace].dirty_bit == 1) {
                fprintf(log_file, "oss: Dirty bit of frame %d set, adding additional time to the clock\n", frame_to_replace);
                increment_clock();
            }
        }

        frame_table[frame_to_replace].pid = msg.pid;
        frame_table[frame_to_replace].page_number = page_number;
        frame_table[frame_to_replace].dirty_bit = 0;
        frame_table[frame_to_replace].last_access_time = shared_clock[0];
        frame_table[frame_to_replace].last_access_nano_time = shared_clock[1];

        page_table[msg.pid][page_number] = frame_to_replace;

        fprintf(log_file, "oss: Indicating to P%d that %s has happened to address %d\n",
                msg.pid, msg.is_write ? "write" : "read", msg.address);
    }
}

void check_terminated_children() {
    int status;
    pid_t pid;

    // Loop to reap all terminated child processes
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        fprintf(log_file, "oss: Child process %d terminated.\n", pid);

        // Find and clear the child PID from the child_pids array
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (child_pids[i] == pid) {
                child_pids[i] = 0; // Mark the slot as free
                break;
            }
        }

        // Log termination details
        if (WIFEXITED(status)) {
            fprintf(log_file, "oss: Child %d exited with status %d.\n", pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(log_file, "oss: Child %d terminated by signal %d.\n", pid, WTERMSIG(status));
        }
    }
}

void log_memory_layout() {
    fprintf(log_file, "Current memory layout at time %d:%d is:\n", shared_clock[0], shared_clock[1]);
    for (int i = 0; i < FRAME_COUNT; i++) {
        fprintf(log_file, "Frame %d: %s DirtyBit: %d LastRefS: %d LastRefNano: %d\n",
                i, frame_table[i].pid == -1 ? "No" : "Yes",
                frame_table[i].dirty_bit,
                frame_table[i].last_access_time,
                frame_table[i].last_access_nano_time);
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        fprintf(log_file, "P%d page table: [", i);
        for (int j = 0; j < PAGE_COUNT; j++) {
            fprintf(log_file, " %d", page_table[i][j]);
        }
        fprintf(log_file, " ]\n");
    }
}

void handle_sigint(int sig) {
    fprintf(stderr, "Master: Caught signal %d, terminating children.\n", sig);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
        }
    }
    while (wait(NULL) > 0);
    shmdt(shared_clock);
    shmctl(shmid_clock, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
    if (log_file) fclose(log_file);
    exit(1);
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case 'n':
                max_children = atoi(optarg);
                break;
            case 's':
                simul_limit = atoi(optarg);
                if (simul_limit > MAX_PROCESSES) simul_limit = MAX_PROCESSES;
                break;
            case 'i':
                launch_interval_ms = atoi(optarg);
                break;
            case 'f':
                strncpy(log_filename, optarg, sizeof(log_filename) - 1);
                break;
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    signal(SIGINT, handle_sigint);
    srand(time(NULL));

    shmid_clock = shmget(IPC_PRIVATE, sizeof(int) * 2, IPC_CREAT | 0666);
    if (shmid_clock == -1) {
        perror("shmget clock");
        exit(1);
    }
    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    shared_clock[0] = 0;
    shared_clock[1] = 0;

    msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        exit(1);
    }

    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("fopen log_file");
        exit(1);
    }

    initialize_frame_table();

    int children_launched = 0;
    int loop_counter = 0;

    while (1) {
        increment_clock();
        check_terminated_children();

        Message msg;
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            handle_memory_request(msg);
        }

        if (loop_counter % 50 == 0) {
            log_memory_layout();
        }

        usleep(launch_interval_ms * 1000);
        if (++loop_counter > 200) break;
    }

    handle_sigint(SIGINT);
    return 0;
}