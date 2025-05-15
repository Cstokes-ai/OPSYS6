#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define MAX_USERS 18
#define NUM_FRAMES 5   // For easier observation
#define NUM_PAGES 32
#define CLOCK_STEP 100000
#define MAX_SIM_TIME 20000000

typedef struct {
    int in_use;
    int is_dirty;
    int owner_pid;
    int page_idx;
    unsigned long last_accessed;
} FrameEntry;

typedef struct {
    long mtype;
    int pid;
    int page_number;
    int action; // 1 = Read, 2 = Write, 3 = Terminate
} Message;

FrameEntry mem_Frame[NUM_FRAMES];
int userPageTable[MAX_USERS][NUM_PAGES];
pid_t child_pids[MAX_USERS];
int sim_clock = 0;
int runningUsers = 0;
FILE *log_fp;

void setupFrames();
void setupPageTables();
void displayMemory();
void processPageRequest(int user_id, int page_num, int action);
void cleanupResources();
int pickVictimFrame();

void sigHandler(int sig) {
    cleanupResources();
    exit(0);
}

int main() {
    int msgid;
    key_t key = ftok("oss.c", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);

    signal(SIGINT, sigHandler);

    log_fp = fopen("oss.log", "w");
    if (log_fp == NULL) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    setupFrames();
    setupPageTables();

    printf("{DEBUG} oss: Simulation initiated\n");
    fprintf(log_fp, "oss: Simulation initiated\n");

    for (int i = 0; i < MAX_USERS; i++) {
        char user_index_str[8];
        snprintf(user_index_str, sizeof(user_index_str), "%d", i);
        if ((child_pids[i] = fork()) == 0) {
            execl("./user", "./user", user_index_str, NULL);
            perror("{DEBUG} Failed to launch user process");
            exit(1);
        }
        runningUsers++;
    }

    while (sim_clock < MAX_SIM_TIME && runningUsers > 0) {
        sim_clock += CLOCK_STEP;

        Message msg;
        if (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            if (msg.action == 1 || msg.action == 2) {
                printf("{DEBUG} oss: User %d requests to %s page %d at %d ns\n", 
                        msg.pid % MAX_USERS, 
                        msg.action == 1 ? "read" : "write", 
                        msg.page_number, sim_clock);
                fprintf(log_fp, "oss: User %d requests to %s page %d at %d ns\n", 
                        msg.pid % MAX_USERS, 
                        msg.action == 1 ? "read" : "write", 
                        msg.page_number, sim_clock);
                processPageRequest(msg.pid % MAX_USERS, msg.page_number, msg.action);
            } else if (msg.action == 3) {
                printf("{DEBUG} oss: User %d has exited\n", msg.pid % MAX_USERS);
                fprintf(log_fp, "oss: User %d has exited\n", msg.pid % MAX_USERS);
                for (int i = 0; i < NUM_FRAMES; i++) {
                    if (mem_Frame[i].in_use && mem_Frame[i].owner_pid == msg.pid % MAX_USERS) {
                        mem_Frame[i].in_use = 0;
                        mem_Frame[i].is_dirty = 0;
                        mem_Frame[i].owner_pid = -1;
                        mem_Frame[i].page_idx = -1;
                    }
                }
                runningUsers--;
            }
        }

        displayMemory();
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    
    for (int i = 0; i < MAX_USERS; i++) {
        fprintf(log_fp, "User %d: ", i);
        for (int j = 0; j < NUM_PAGES; j++) {
            fprintf(log_fp, "%2d ", userPageTable[i][j]);
        }
        fprintf(log_fp, "\n");
    }

    fprintf(log_fp, "\n==== Final User Page Tables ====\n");
    for (int i = 0; i < MAX_USERS; i++) {
        fprintf(log_fp, "Page Table %d: ", i);
        for (int j = 0; j < NUM_PAGES; j++) {
            fprintf(log_fp, "%2d ", userPageTable[i][j]);
        }
        fprintf(log_fp, "\n");
    }

    cleanupResources();
    return 0;
}

void setupFrames() {
    for (int i = 0; i < NUM_FRAMES; i++) {
        mem_Frame[i].in_use = 0;
        mem_Frame[i].is_dirty = 0;
        mem_Frame[i].owner_pid = -1;
        mem_Frame[i].page_idx = -1;
        mem_Frame[i].last_accessed = 0;
    }
}

void setupPageTables() {
    for (int i = 0; i < MAX_USERS; i++) {
        for (int j = 0; j < NUM_PAGES; j++) {
            userPageTable[i][j] = -1;
        }
    }
}

void processPageRequest(int user_id, int page_num, int action) {
    int frame_idx = -1;

    for (int i = 0; i < NUM_FRAMES; i++) {
        if (mem_Frame[i].in_use && mem_Frame[i].owner_pid == user_id &&
            mem_Frame[i].page_idx == page_num) {
            frame_idx = i;
            mem_Frame[i].last_accessed = sim_clock;
            printf("{DEBUG} oss: Page %d for User %d found in frame %d (%s)\n", 
                    page_num, user_id, i, action == 1 ? "read" : "write");
            fprintf(log_fp, "oss: Page %d for User %d found in frame %d (%s)\n", 
                    page_num, user_id, i, action == 1 ? "read" : "write");
            if (action == 2) mem_Frame[i].is_dirty = 1;
            return;
        }
    }

    for (int i = 0; i < NUM_FRAMES; i++) {
        if (!mem_Frame[i].in_use) {
            frame_idx = i;
            break;
        }
    }

    if (frame_idx == -1) frame_idx = pickVictimFrame();

    if (mem_Frame[frame_idx].is_dirty) {
        printf("{DEBUG} oss: Flushing dirty frame %d to disk\n", frame_idx);
        fprintf(log_fp, "oss: Flushing dirty frame %d to disk\n", frame_idx);
    }

    userPageTable[mem_Frame[frame_idx].owner_pid][mem_Frame[frame_idx].page_idx] = -1;

    mem_Frame[frame_idx].in_use = 1;
    mem_Frame[frame_idx].is_dirty = (action == 2);
    mem_Frame[frame_idx].owner_pid = user_id;
    mem_Frame[frame_idx].page_idx = page_num;
    mem_Frame[frame_idx].last_accessed = sim_clock;

    userPageTable[user_id][page_num] = frame_idx;
    printf("{DEBUG} oss: Loaded page %d into frame %d for User %d (%s)\n", page_num, frame_idx, user_id, action == 1 ? "read" : "write");
    fprintf(log_fp, "oss: Loaded page %d into frame %d for User %d (%s)\n", page_num, frame_idx, user_id, action == 1 ? "read" : "write");
}

int pickVictimFrame() {
    unsigned long oldest = sim_clock;
    int victim_idx = 0;
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (mem_Frame[i].last_accessed < oldest) {
            oldest = mem_Frame[i].last_accessed;
            victim_idx = i;
        }
    }
    return victim_idx;
}

void displayMemory() {
    static int log_lines = 0;
    const int MAX_LOG_LINES = 500;
    if (log_lines >= MAX_LOG_LINES) return;

    fprintf(log_fp, "\nMemory snapshot at %d ns:\n", sim_clock);
    log_lines++;
    fprintf(log_fp, "Frame Table:\n");
    log_lines++;
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (mem_Frame[i].in_use) {
            fprintf(log_fp, "Frame %d: User %d, Page %d, Dirty: %d, LastUsed: %lu\n", 
                   i, mem_Frame[i].owner_pid, mem_Frame[i].page_idx, mem_Frame[i].is_dirty, mem_Frame[i].last_accessed);
        } else {
            fprintf(log_fp, "Frame %d: Free\n", i);
        }
        log_lines++;
        if (log_lines >= MAX_LOG_LINES) return;
    }
}

void cleanupResources() {
    printf("{DEBUG} oss: Releasing all resources...\n");
    fprintf(log_fp, "oss: Releasing all resources...\n");
    for (int i = 0; i < MAX_USERS; i++) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGKILL);
    }

    fclose(log_fp);

    key_t key = ftok("oss.c", 65);
    int msgid = msgget(key, 0666 | IPC_CREAT);
    msgctl(msgid, IPC_RMID, NULL);
}