#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MAX_PROCESSES 18
#define PAGE_TABLE_SIZE 32
#define FRAME_TABLE_SIZE 256
#define MSGSZ 128

typedef struct {
    long mtype;
    int pid;
    int address;
    int is_write;
} Message;

typedef struct {
    int frame_number;
    int dirty;
    int reference_bit;
    time_t last_used;
} Frame;

int shmid_clock;
int *shared_clock;
int msqid;
FILE *log_file;

int max_children = 40;
int simul_limit = 18;
int launch_interval_ms = 100;
char log_filename[256] = "oss.log";

Frame frame_table[FRAME_TABLE_SIZE];
pid_t child_pids[MAX_PROCESSES];

void print_usage(char *progname) {
    printf("Usage: %s [-h] [-n proc] [-s simul] [-i interval_ms] [-f logfile]\n", progname);
}

void initialize_frame_table() {
    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        frame_table[i].frame_number = -1;
        frame_table[i].dirty = 0;
        frame_table[i].reference_bit = 0;
        frame_table[i].last_used = 0;
    }
}

void increment_clock() {
    shared_clock[1] += 1000; // nanoseconds
    if (shared_clock[1] >= 1000000000) {
        shared_clock[0]++;
        shared_clock[1] -= 1000000000;
    }
}

int find_frame(int page_number) {
    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].frame_number == page_number) {
            return i;
        }
    }
    return -1;
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
    time_t oldest = time(NULL);
    int victim = -1;
    for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
        if (frame_table[i].last_used < oldest) {
            oldest = frame_table[i].last_used;
            victim = i;
        }
    }
    return victim;
}

void handle_memory_request(Message msg) {
    int page = msg.address / 1024;
    int frame_index = find_frame(page);

    if (frame_index == -1) {
        int free_index = find_free_frame();
        if (free_index == -1) {
            int victim_index = select_victim_frame();
            fprintf(log_file, "oss: Replacing page in frame %d (page %d)\n",
                    victim_index, frame_table[victim_index].frame_number);
            free_index = victim_index;
        }

        frame_table[free_index].frame_number = page;
        frame_table[free_index].dirty = msg.is_write;
        frame_table[free_index].last_used = time(NULL);
        fprintf(log_file, "oss: P%d requested page %d -> placed in frame %d\n",
                msg.pid, page, free_index);
    } else {
        frame_table[frame_index].reference_bit = 1;
        frame_table[frame_index].dirty |= msg.is_write;
        frame_table[frame_index].last_used = time(NULL);
        fprintf(log_file, "oss: P%d accessed page %d -> already in frame %d\n",
                msg.pid, page, frame_index);
    }
}

void check_terminated_children() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (child_pids[i] == pid) {
                child_pids[i] = 0;
                break;
            }
        }
    }
}

void cleanup() {
    fprintf(log_file, "oss: Cleaning up resources and terminating.\n");

    // Terminate all remaining children
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
            waitpid(child_pids[i], NULL, 0);
        }
    }

    // Detach and remove shared memory
    shmdt(shared_clock);
    shmctl(shmid_clock, IPC_RMID, NULL);

    // Remove message queue
    msgctl(msqid, IPC_RMID, NULL);

    // Close log file
    if (log_file) fclose(log_file);
}

void handle_signal(int sig) {
    cleanup();
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

    srand(time(NULL));

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("fopen");
        exit(1);
    }

    // Shared memory for simulated clock
    key_t shmkey = ftok("oss.c", 1);
    shmid_clock = shmget(shmkey, sizeof(int) * 2, IPC_CREAT | 0666);
    if (shmid_clock == -1) {
        perror("shmget");
        exit(1);
    }
    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    if (shared_clock == (void *)-1) {
        perror("shmat");
        exit(1);
    }
    shared_clock[0] = 0;
    shared_clock[1] = 0;

    // Message queue
    key_t msgkey = ftok("oss.c", 2);
    msqid = msgget(msgkey, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        cleanup();
        exit(1);
    }

    initialize_frame_table();

    int active_children = 0;
    int total_spawned = 0;
    int next_launch_time = 0;

    while (1) {
        increment_clock();

        int current_time = shared_clock[0] * 1000 + shared_clock[1] / 1000000;
        if (active_children < simul_limit && total_spawned < max_children && current_time >= next_launch_time) {
            pid_t pid = fork();
            if (pid == 0) {
                char pid_arg[10];
                sprintf(pid_arg, "%d", total_spawned);
                execl("./user", "user", pid_arg, (char *)NULL);
                perror("execl");
                exit(1);
            } else if (pid > 0) {
                child_pids[total_spawned] = pid;
                active_children++;
                total_spawned++;
                next_launch_time = current_time + launch_interval_ms;
                fprintf(log_file, "oss: Launched child P%d (PID %d)\n", total_spawned - 1, pid);
            } else {
                perror("fork");
            }
        }

        Message msg;
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            if (msg.mtype == 1) {
                handle_memory_request(msg);

                msg.mtype = msg.pid + 1;
                if (msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                    perror("msgsnd ack");
                }
            } else if (msg.mtype == 2) {
                fprintf(log_file, "oss: P%d is terminating after finishing requests\n", msg.pid);
                active_children--;
            }
        }

        check_terminated_children();

        if (total_spawned >= max_children && active_children == 0) {
            break;
        }

        usleep(1000);
    }

    cleanup();
    return 0;
}
