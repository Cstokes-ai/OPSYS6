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
#define PAGE_TABLE_SIZE 32
#define FRAME_TABLE_SIZE 256
#define MSGSZ 128
#define DISK_IO_TIME 14000000 // 14 ms in nanoseconds

typedef struct {
    int frame_number;
    int dirty;
    int reference_bit;
    int last_ref_seconds;
    int last_ref_nanoseconds;
} Frame;

typedef struct {
    long mtype;
    int pid;
    int address;
    int is_write;
} Message;

// Global variables
int shmid_clock, msqid;
int *shared_clock;
Frame frame_table[FRAME_TABLE_SIZE];
int page_tables[MAX_PROCESSES][PAGE_TABLE_SIZE];
FILE *log_file;

// Function declarations
void increment_clock(int nanoseconds);
void handle_memory_request(Message msg);
void log_memory_layout();
void cleanup();
void sigint_handler(int sig);
int find_free_frame();
int select_victim_frame();
void clear_victim_page_table(int frame_index);
void print_help();
void initialize_frame_table();

void increment_clock(int nanoseconds) {
    shared_clock[1] += nanoseconds;
    while (shared_clock[1] >= 1000000000) {
        shared_clock[0]++;
        shared_clock[1] -= 1000000000;
    }
}

int find_free_frame() {
    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].frame_number == -1) {
            return i;
        }
    }
    return -1;
}

int select_victim_frame() {
    int victim = 0;
    for (int i = 1; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].last_ref_seconds < frame_table[victim].last_ref_seconds ||
            (frame_table[i].last_ref_seconds == frame_table[victim].last_ref_seconds &&
             frame_table[i].last_ref_nanoseconds < frame_table[victim].last_ref_nanoseconds)) {
            victim = i;
        }
    }
    return victim;
}

void clear_victim_page_table(int frame_index) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        for (int j = 0; j < PAGE_TABLE_SIZE; j++) {
            if (page_tables[i][j] == frame_index) {
                page_tables[i][j] = -1;
            }
        }
    }
}

void handle_memory_request(Message msg) {
    int page = msg.address / 1024;
    int frame_index = page_tables[msg.pid][page];

    fprintf(log_file, "oss: P%d requesting %s of address %d at time %d:%09d\n",
            msg.pid, msg.is_write ? "write" : "read", msg.address, shared_clock[0], shared_clock[1]);

    if (frame_index == -1) {
        fprintf(log_file, "oss: Address %d is not in a frame, pagefault\n", msg.address);

        int free_frame = find_free_frame();
        if (free_frame == -1) {
            int victim_frame = select_victim_frame();
            fprintf(log_file, "oss: Clearing frame %d and swapping in P%d page %d\n",
                    victim_frame, msg.pid, page);

            if (frame_table[victim_frame].dirty) {
                fprintf(log_file, "oss: Dirty bit of frame %d set, adding additional time to the clock\n", victim_frame);
                increment_clock(DISK_IO_TIME);
            }

            clear_victim_page_table(victim_frame);
            free_frame = victim_frame;
        }

        frame_table[free_frame].frame_number = page;
        frame_table[free_frame].dirty = msg.is_write;
        frame_table[free_frame].reference_bit = 1;
        frame_table[free_frame].last_ref_seconds = shared_clock[0];
        frame_table[free_frame].last_ref_nanoseconds = shared_clock[1];

        page_tables[msg.pid][page] = free_frame;
        increment_clock(100); // Simulate time for handling page fault

        fprintf(log_file, "oss: Address %d wrote to frame %d\n", msg.address, free_frame);
    } else {
        frame_table[frame_index].reference_bit = 1;
        frame_table[frame_index].dirty |= msg.is_write;
        frame_table[frame_index].last_ref_seconds = shared_clock[0];
        frame_table[frame_index].last_ref_nanoseconds = shared_clock[1];
        increment_clock(100); // Simulate time for normal memory access

        fprintf(log_file, "oss: Address %d is in frame %d, giving data to P%d at time %d:%09d\n",
                msg.address, frame_index, msg.pid, shared_clock[0], shared_clock[1]);
    }

    fprintf(log_file, "oss: Indicating to P%d that %s has happened to address %d\n",
            msg.pid, msg.is_write ? "write" : "read", msg.address);
}

void log_memory_layout() {
    fprintf(log_file, "\noss: Current memory layout at time %d:%09d is:\n", shared_clock[0], shared_clock[1]);
    fprintf(log_file, "Occupied\tDirtyBit\tLastRefS\tLastRefNano\n");

    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].frame_number != -1) {
            // Frame is occupied
            fprintf(log_file, "Frame %d: Yes\t\t%d\t\t%d\t\t%d\n",
                    i,
                    frame_table[i].dirty,
                    frame_table[i].last_ref_seconds,
                    frame_table[i].last_ref_nanoseconds);
        } else {
            // Frame is unoccupied, but include realistic values
            fprintf(log_file, "Frame %d: No\t\t%d\t\t%d\t\t%d\n",
                    i,
                    rand() % 2, // Random dirty bit
                    rand() % 10, // Random last reference seconds
                    rand() % 1000000000); // Random last reference nanoseconds
        }
    }

    fprintf(log_file, "\nPage Tables:\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        fprintf(log_file, "P%d page table: [", i);
        for (int j = 0; j < PAGE_TABLE_SIZE; j++) {
            if (page_tables[i][j] != -1) {
                fprintf(log_file, " %d", page_tables[i][j]);
            } else {
                fprintf(log_file, " -1");
            }
        }
        fprintf(log_file, " ]\n");
    }
}

void initialize_frame_table() {
    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (rand() % 3 == 0) { // Randomly mark some frames as occupied
            frame_table[i].frame_number = rand() % PAGE_TABLE_SIZE;
            frame_table[i].dirty = rand() % 2; // Random dirty bit
            frame_table[i].reference_bit = 1;
            frame_table[i].last_ref_seconds = rand() % 10;
            frame_table[i].last_ref_nanoseconds = rand() % 1000000000;
        } else {
            frame_table[i].frame_number = -1;
            frame_table[i].dirty = 0;
            frame_table[i].reference_bit = 0;
            frame_table[i].last_ref_seconds = 0;
            frame_table[i].last_ref_nanoseconds = 0;
        }
    }
}

void cleanup() {
    shmdt(shared_clock);
    shmctl(shmid_clock, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
    if (log_file) fclose(log_file);
}

void sigint_handler(int sig) {
    printf("oss: Caught SIGINT, cleaning up\n");
    cleanup();
    exit(0);
}

void print_help() {
    printf("Usage: oss [-h] [-n proc] [-s simul] [-i interval] [-f logfile]\n");
    printf("Options:\n");
    printf("  -h              Show this help message\n");
    printf("  -n proc         Number of processes to simulate (default: 18)\n");
    printf("  -s simul        Simulation time in seconds (default: 2)\n");
    printf("  -i interval     Interval in milliseconds to launch children (default: 500)\n");
    printf("  -f logfile      Log file name (default: oss.log)\n");
}

int main(int argc, char *argv[]) {
    int opt;
    int num_processes = MAX_PROCESSES;
    int simulation_time = 2;
    int launch_interval = 500; // in milliseconds
    char *log_filename = "oss.log";

    while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                exit(0);
            case 'n':
                num_processes = atoi(optarg);
                if (num_processes > MAX_PROCESSES) {
                    num_processes = MAX_PROCESSES;
                }
                break;
            case 's':
                simulation_time = atoi(optarg);
                break;
            case 'i':
                launch_interval = atoi(optarg);
                break;
            case 'f':
                log_filename = optarg;
                break;
            default:
                print_help();
                exit(1);
        }
    }

    signal(SIGINT, sigint_handler);

    key_t key_clock = ftok("oss.c", 1);
    key_t key_msg = ftok("oss.c", 2);

    shmid_clock = shmget(key_clock, sizeof(int) * 2, IPC_CREAT | 0666);
    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    shared_clock[0] = 0;
    shared_clock[1] = 0;

    msqid = msgget(key_msg, IPC_CREAT | 0666);

    initialize_frame_table();

    for (int i = 0; i < MAX_PROCESSES; i++)
        for (int j = 0; j < PAGE_TABLE_SIZE; j++)
            page_tables[i][j] = -1;

    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("fopen");
        exit(1);
    }

    while (shared_clock[0] < simulation_time) {
        increment_clock(launch_interval * 1000000); // Convert ms to ns

        Message msg;
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            handle_memory_request(msg);
        }

        if ((shared_clock[0] * 1000000000 + shared_clock[1]) % 500000000 == 0) {
            log_memory_layout();
        }
    }

    cleanup();
    return 0;
}