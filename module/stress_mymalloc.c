#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include "mymalloc_ioctl.h"

#define NUM_WORKERS   4      /* how many processes run at once   */
#define OPS_PER_WORKER 2000  /* random operations each one does  */
#define MAX_HELD      64     /* max outstanding allocs per worker */
#define MAX_ORDER     5      /* allocate orders 0..5 (up to 128KB)*/

/* One worker process: does OPS_PER_WORKER random alloc/free ops. */
static void worker(int id)
{
    int fd = open("/dev/mymalloc", O_RDWR);
    if (fd < 0) { perror("worker open"); exit(1); }

    /* Seed each worker differently so they don't all do the same thing. */
    srand(time(NULL) ^ (id * 2654435761u));

    /* Track this worker's outstanding allocations. */
    struct mymalloc_arg held[MAX_HELD];
    int held_count = 0;

    for (int i = 0; i < OPS_PER_WORKER; i++) {
        int do_alloc = rand() & 1;

        if (do_alloc && held_count < MAX_HELD) {
            /* Allocate a random-order block. */
            struct mymalloc_arg a;
            a.order = rand() % (MAX_ORDER + 1);
            if (ioctl(fd, MYMALLOC_IOC_ALLOC, &a) == 0 && a.addr) {
                held[held_count++] = a;
            }
        } else if (held_count > 0) {
            /* Free one we already hold (pick a random one). */
            int idx = rand() % held_count;
            ioctl(fd, MYMALLOC_IOC_FREE, &held[idx]);
            /* Remove it from our list by swapping in the last entry. */
            held[idx] = held[held_count - 1];
            held_count--;
        }
    }

    /* Clean up: free everything still held so the pool can fully coalesce. */
    for (int i = 0; i < held_count; i++) {
        ioctl(fd, MYMALLOC_IOC_FREE, &held[i]);
    }

    close(fd);
    printf("worker %d done (%d ops)\n", id, OPS_PER_WORKER);
    exit(0);
}

int main(void)
{
    printf("stress test: %d workers x %d ops = %d total operations\n",
           NUM_WORKERS, OPS_PER_WORKER, NUM_WORKERS * OPS_PER_WORKER);

    /* Fork the worker processes. */
    for (int w = 0; w < NUM_WORKERS; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            worker(w);          /* child never returns */
        } else if (pid < 0) {
            perror("fork");
            return 1;
        }
    }

    /* Parent waits for all children to finish. */
    for (int w = 0; w < NUM_WORKERS; w++) {
        wait(NULL);
    }

    printf("all workers finished -- check /proc/mymalloc for consistency\n");
    return 0;
}