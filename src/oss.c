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
#define MAX_RESOURCES 5
#define MAX_INSTANCES 10
#define MAX_REQUESTS 100

typedef struct {
    long mtype;
    int pid;
    int address;
    int is_write; // 1 = write 0 = read
} Message;

typedef struct {
    int total[MAX_RESOURCES];
    int available[MAX_RESOURCES];
    int allocation[MAX_PROCESSES][MAX_RESOURCES];
} Resource;

// Blocked request info
typedef struct {
    int valid;
    int pid;
    int resource;
    int quantity;
} BlockedRequest;

int shmid_clock;
int msqid;
int *shared_clock;
Resource *resources;
FILE *log_file;
pid_t child_pids[MAX_PROCESSES] = {0};

int max_children = 5;
int simul_limit = 5;
int launch_interval_ms = 1000;
char log_filename[256] = "oss.log";

BlockedRequest blocked[MAX_PROCESSES] = {0}; // Array for blocked processes

void print_usage(const char *progname) {
    printf("Usage: %s [-h] [-n proc] [-s simul] [-i intervalMs] [-f logfile]\n", progname);
}

void increment_clock() {
    shared_clock[1] += rand() % 1000;
    if (shared_clock[1] >= 1000000000) {
        shared_clock[0]++;
        shared_clock[1] -= 1000000000;
    }
}

void print_resource_table() {
    fprintf(log_file, "\nResource Allocation Table at time %d:%d:\n", shared_clock[0], shared_clock[1]);
    fprintf(log_file, "    Total    Available\n");
    for (int i = 0; i < MAX_RESOURCES; i++) {
        fprintf(log_file, "R%d:   %3d        %3d\n", i, resources->total[i], resources->available[i]);
    }
    fprintf(log_file, "\nAllocation per Process:\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        int allocated = 0;
        for (int j = 0; j < MAX_RESOURCES; j++) {
            if (resources->allocation[i][j] > 0) {
                allocated = 1;
                break;
            }
        }
        if (allocated) {
            fprintf(log_file, "P%d: ", i);
            for (int j = 0; j < MAX_RESOURCES; j++) {
                fprintf(log_file, "R%d=%d ", j, resources->allocation[i][j]);
            }
            fprintf(log_file, "\n");
        }
    }
    fprintf(log_file, "\n");

    // Also print blocked processes
    fprintf(log_file, "Blocked Processes:\n");
    int any_blocked = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (blocked[i].valid) {
            fprintf(log_file, "P%d is blocked requesting %d of R%d\n", blocked[i].pid, blocked[i].quantity, blocked[i].resource);
            any_blocked = 1;
        }
    }
    if (!any_blocked) {
        fprintf(log_file, "None\n");
    }
    fflush(log_file);
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

void check_terminated_children() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int local_index = -1;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (child_pids[i] == pid) {
                local_index = i;
                break;
            }
        }
        if (local_index == -1) continue;

        fprintf(log_file, "Master detected Process P%d terminated at time %d:%d\n",
                local_index, shared_clock[0], shared_clock[1]);

        // Release its resources
        for (int j = 0; j < MAX_RESOURCES; j++) {
            resources->available[j] += resources->allocation[local_index][j];
            resources->allocation[local_index][j] = 0;
        }

        fflush(log_file);
        child_pids[local_index] = 0;
    }
}

void launch_child_processes(int *children_launched) {
    if (*children_launched >= max_children) return;

    int active = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] > 0) active++;
    }
    if (active >= simul_limit) return;

    int idx = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (child_pids[i] == 0) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;

    pid_t pid = fork();
    if (pid == 0) {
        char msqid_str[12], clockid_str[12], index_str[12];
        sprintf(msqid_str, "%d", msqid);
        sprintf(clockid_str, "%d", shmid_clock);
        sprintf(index_str, "%d", idx);
        execl("./user", "./user", msqid_str, clockid_str, index_str, NULL);
        perror("execl failed");
        exit(1);
    } else if (pid > 0) {
        (*children_launched)++;
        child_pids[idx] = pid;
        fprintf(log_file, "Master launched Process P%d (PID %d) at time %d:%d\n",
                idx, pid, shared_clock[0], shared_clock[1]);
        fflush(log_file);
    } else {
        perror("fork failed");
    }
}

void initialize_resources() {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        resources->total[i] = (rand() % MAX_INSTANCES) + 1;
        resources->available[i] = resources->total[i];
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        for (int j = 0; j < MAX_RESOURCES; j++) {
            resources->allocation[i][j] = 0;
        }
    }
}

// Attempt to unblock waiting processes
void attempt_to_unblock() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (blocked[i].valid) {
            int res = blocked[i].resource;
            int qty = blocked[i].quantity;
            if (resources->available[res] >= qty) {
                // Grant the request
                resources->available[res] -= qty;
                resources->allocation[blocked[i].pid][res] += qty;
                fprintf(log_file, "Master unblocked P%d and granted %d of R%d at time %d:%d\n",
                        blocked[i].pid, qty, res, shared_clock[0], shared_clock[1]);
                blocked[i].valid = 0; // Remove from blocked list
            }
        }
    }
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

    int children_launched = 0;
    int loop_counter = 0;

    while (1) {
        increment_clock();
        check_terminated_children();
        launch_child_processes(&children_launched);

        Message msg;
        if (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            fprintf(log_file, "Master received msg from P%d: %s address %d at time %d:%d\n",
                    msg.pid, msg.is_write ? "Writing to" : "Reading from",
                    msg.address, shared_clock[0], shared_clock[1]);

            //  Implement memory management logic
            // Handle memory requests, page faults, and LRU page replacement
        }

        usleep(launch_interval_ms * 1000);
        if (++loop_counter > 200) break;
    }

    handle_sigint(SIGINT);
    return 0;
}
