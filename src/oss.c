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
void log_process_termination(int pid, int memory_access_time);

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

    // Log the memory request
    fprintf(log_file, "oss: P%d requesting %s of address %d at time %d:%09d\n",
            msg.pid, msg.is_write ? "write" : "read", msg.address, shared_clock[0], shared_clock[1]);

    if (frame_index == -1) {
        // Page fault
        fprintf(log_file, "oss: Address %d is not in a frame, pagefault\n", msg.address);

        int free_frame = find_free_frame();
        if (free_frame == -1) {
            // No free frame, select a victim frame
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

        // Assign the page to the free frame
        frame_table[free_frame].frame_number = page;
        frame_table[free_frame].dirty = msg.is_write;
        frame_table[free_frame].reference_bit = 1;
        frame_table[free_frame].last_ref_seconds = shared_clock[0];
        frame_table[free_frame].last_ref_nanoseconds = shared_clock[1];

        page_tables[msg.pid][page] = free_frame;
        increment_clock(100); // Simulate time for handling page fault

        fprintf(log_file, "oss: Indicating to P%d that %s has happened to address %05d\n",
                msg.pid, msg.is_write ? "write" : "read", msg.address);
    } else {
        // Page is in a frame
        frame_table[frame_index].reference_bit = 1;
        frame_table[frame_index].dirty |= msg.is_write;
        frame_table[frame_index].last_ref_seconds = shared_clock[0];
        frame_table[frame_index].last_ref_nanoseconds = shared_clock[1];
        increment_clock(100); // Simulate time for normal memory access

        fprintf(log_file, "oss: Address %d in frame %d, %s data to P%d at time %d:%09d\n",
                msg.address, frame_index, msg.is_write ? "writing" : "giving", msg.pid, shared_clock[0], shared_clock[1]);
        fprintf(log_file, "oss: Indicating to P%d that %s has happened to address %05d\n",
                msg.pid, msg.is_write ? "write" : "read", msg.address);
    }
}

void log_memory_layout() {
    fprintf(log_file, "\noss: Current memory layout at time %d:%09d is:\n", shared_clock[0], shared_clock[1]);
    fprintf(log_file, "Frame\tOccupied\tDirtyBit\tLastRefS\tLastRefNano\n");

    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].frame_number != -1) {
            // Frame is occupied
            fprintf(log_file, "Frame %d: Yes\t\t%d\t\t%d\t\t%d\n",
                    i,
                    frame_table[i].dirty,
                    frame_table[i].last_ref_seconds,
                    frame_table[i].last_ref_nanoseconds);
        } else {
            // Frame is unoccupied
            fprintf(log_file, "Frame %d: No\t\t0\t\t0\t\t0\n", i);
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

void log_process_termination(int pid, int memory_access_time) {
    fprintf(log_file, "oss: P%d terminated at time %d:%09d\n", pid, shared_clock[0], shared_clock[1]);
    fprintf(log_file, "oss: P%d effective memory access time: %d nanoseconds\n", pid, memory_access_time);
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
    printf("  -i interval     Interval in milliseconds to launch new processes (default: 100)\n");
    printf("  -f logfile      Log file for output (default: oss_log.txt)\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    // Argument parsing
    int opt;
    int num_processes = MAX_PROCESSES;
    int sim_time = 2; // seconds
    int interval = 100; // ms
    char *log_filename = "oss_log.txt";

    while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                exit(0);
            case 'n':
                num_processes = atoi(optarg);
                break;
            case 's':
                sim_time = atoi(optarg);
                break;
            case 'i':
                interval = atoi(optarg);
                break;
            case 'f':
                log_filename = optarg;
                break;
            default:
                print_help();
                exit(1);
        }
    }

    // Initialize log file
    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("oss: Failed to open log file");
        exit(1);
    }

    // Shared memory for clock
    shmid_clock = shmget(IPC_PRIVATE, 2 * sizeof(int), IPC_CREAT | 0666);
    if (shmid_clock == -1) {
        perror("oss: Failed to create shared memory for clock");
        exit(1);
    }
    shared_clock = shmat(shmid_clock, NULL, 0);
    if (shared_clock == (int *)-1) {
        perror("oss: Failed to attach shared memory for clock");
        exit(1);
    }

    // Initialize frame table and page tables
    initialize_frame_table();

    // Simulate memory management for processes
    for (int time = 0; time < sim_time; time++) {
        // Increment the clock to simulate time passing
        increment_clock(1000000); // Increment by 1 millisecond

        // Simulate receiving a memory request
        Message msg = {0}; // Initialize all fields to zero
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            // Process the memory request
            handle_memory_request(msg);
        }

        // Periodically log the memory layout
        if (time % 1 == 0) { // Log every second
            log_memory_layout();
        }

        // Simulate launching new processes or handling termination
        if (interval >= 1000 && time % (interval / 1000) == 0 && time < num_processes) {
            // Simulate launching a new process
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                char sim_pid[10];
                sprintf(sim_pid, "%d", time);
                execl("./user", "./user", sim_pid, NULL);
                perror("execl");
                exit(1);
            } else if (pid < 0) {
                perror("oss: Failed to fork process");
            }
        }
    }

    // Simulate process termination
    for (int i = 0; i < num_processes; i++) {
        log_process_termination(i, rand() % 1000000); // Random memory access time for demonstration
    }

    cleanup();
    return 0;
}
