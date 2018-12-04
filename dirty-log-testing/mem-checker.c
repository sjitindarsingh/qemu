#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>

#define DEF_ALIGN (1UL << 16)
#define DEF_STRIDE (1UL << 12)
#define MAX_WORKERS 256

static bool stopping = false;

void usage(char *progname)
{
    printf("%s <working set size in bytes> [<workers> [<alignment>]]\n", progname);
}

int worker_loop(int id, size_t wss_bytes, unsigned long stride, unsigned long align)
{
    uint8_t *mem = malloc(wss_bytes + align);
    unsigned long i;
    bool failed = false;
    unsigned long pass = 0;

    if (!mem) {
        printf("failed to allocate %lu bytes\n", wss_bytes);
        return 1;
    }

    for (i = 0; i < wss_bytes; i += stride) {
        uint64_t *mem64 = (uint64_t *)&mem[i];
        mem64[0] = id;
        mem64[1] = i;
        mem64[2] = -1UL;
    }

    while (!failed && !stopping) {
        for (i = 0; i < wss_bytes; i += stride) {
            uint64_t *mem64 = (uint64_t *)&mem[i];
            if (mem64[0] != id) {
                printf("worker %d, addr %lx, id: expected: 0x%lx, got: 0x%lx\n",
                       id, i, (uint64_t)id, mem64[0]);
                failed = true;
            }
            if (mem64[1] != i) {
                printf("worker %d, addr %lx, offset marker: expected: 0x%lx, got: 0x%lx\n",
                       id, i, i, mem64[1]);
                failed = true;
            }
            if (mem64[2] != pass - 1) {
                printf("worker %d, addr %lx, pass marker: expected: 0x%lx, got: 0x%lx\n",
                       id, i, pass - 1, mem64[2]);
                failed = true;
            }
            mem64[2] = pass;
            if (failed)
                break;
        }
        pass++;
    }
    if (failed)
        return 1;

    return 0;
}

void sa_handler_worker(int sig)
{
    stopping = true;
}

void sa_handler_parent(int sig)
{
    static bool signalled = false;

    if (!signalled && sig == SIGCHLD) {
        kill(0, SIGINT);
        signalled = true;
    }
    stopping = true;
}

int main(int argc, char **argv)
{
    int workers = 1;
    unsigned long alignment = DEF_ALIGN, stride = DEF_STRIDE;
    size_t wss_bytes = 0;
    int i;
    pid_t worker_pids[MAX_WORKERS] = {0};
    struct sigaction sigact_worker = {0}, sigact_parent = {0};
    int parent_ret = 0;
    int workers_remaining;

    sigact_parent.sa_handler = sa_handler_parent;
    sigaction(SIGINT, &sigact_parent, NULL);
    sigaction(SIGCHLD, &sigact_parent, NULL);

    if (argc < 2) {
        usage(argv[0]);
        exit(1);
    }

    wss_bytes = atol(argv[1]);

    if (argc > 2) {
        workers = atoi(argv[2]);
    }
    if (argc > 3) {
        stride = atol(argv[3]);
    }
    if (argc > 4) {
        alignment = atol(argv[4]);
    }

    for (i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            sigact_worker.sa_handler = sa_handler_worker;
            sigaction(SIGINT, &sigact_worker, NULL);
            printf("starting worker %d with %lu bytes\n", i, wss_bytes / workers);
            return worker_loop(i, wss_bytes / workers, stride, alignment);
        }
        if (pid == -1) {
            printf("error launching worker %d: %d", i, errno);
            return -1;
        }
        worker_pids[i] = pid;
    }

    workers_remaining = workers;
    while (!stopping) {
        usleep(1000*250);
    }

    for (i = 0; i < workers; i++) {
        int ret, status = 0;
        pid_t pid = worker_pids[i];
        do {
            ret = waitpid(pid, &status, 0);
        } while (ret == -1 && errno == 4);
        if (ret == -1) {
            printf("error collecting worker %d: %d\n", i, errno);
            parent_ret = 1;
        }
        if (WIFEXITED(status)) {
            int child_ret = WEXITSTATUS(status);
            if (child_ret != 0) {
                printf("worker %d (%d) exited with failure: %d\n", i, pid, child_ret);
                parent_ret = 1;
            }
        } else {
            printf("worker %d exited abnormally\n", i);
            parent_ret = 1;
        }
        printf("collected worker: %d (%d)\n", i, pid);
    }

    return parent_ret;
}
