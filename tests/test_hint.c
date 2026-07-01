/*
 * test_hint.c -- manual functional test for MYMALLOC_IOC_HINT
 *
 * Sequence:
 *   1. Alloc an order-4 block (leaves buddies at various orders)
 *   2. Free it back (pool returns to a single order-10 block)
 *   3. Alloc an order-3 block (splits down, leaves free blocks at 3..9)
 *   4. Read /proc/mymalloc/status (before hints)
 *   5. Send PRESPLIT hint at order 2 -- expect a free block to appear at order 2
 *   6. Read /proc/mymalloc/status (after PRESPLIT)
 *   7. Send PRECOALESCE hint at order 3 -- if two order-3 buddies are free, merge
 *   8. Read /proc/mymalloc/status (after PRECOALESCE)
 *
 * Build (on HOST, targeting the VM's userspace... actually this must run
 * INSIDE the VM since /dev/mymalloc only exists there):
 *   aarch64-linux-gnu-gcc -static -o test_hint test_hint.c
 * Then copy test_hint into the initramfs alongside stress_mymalloc, OR
 * run it from a 9p share once that's set up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include "mymalloc_ioctl.h"

static void dump_status(void)
{
    char buf[4096];
    int fd = open("/proc/mymalloc/status", O_RDONLY);
    ssize_t n;
    if (fd < 0) { perror("open status"); return; }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; printf("%s\n", buf); }
    close(fd);
}

int main(void)
{
    int fd = open("/dev/mymalloc", O_RDWR);
    struct mymalloc_arg a;
    struct mymalloc_hint_arg h;

    if (fd < 0) { perror("open /dev/mymalloc"); return 1; }

    printf("=== step 1: alloc order 4 ===\n");
    a.order = 4;
    if (ioctl(fd, MYMALLOC_IOC_ALLOC, &a) < 0) { perror("alloc"); return 1; }
    printf("allocated at addr 0x%lx\n", a.addr);

    printf("\n=== step 2: free it ===\n");
    if (ioctl(fd, MYMALLOC_IOC_FREE, &a) < 0) { perror("free"); return 1; }

    printf("\n=== step 3: alloc order 3 (splits pool down) ===\n");
    a.order = 3;
    if (ioctl(fd, MYMALLOC_IOC_ALLOC, &a) < 0) { perror("alloc"); return 1; }
    printf("allocated at addr 0x%lx\n", a.addr);

    printf("\n=== step 4: status BEFORE hints ===\n");
    dump_status();

    printf("\n=== step 5: PRESPLIT hint at order 2 ===\n");
    h.action = MYMALLOC_HINT_PRESPLIT;
    h.order  = 2;
    if (ioctl(fd, MYMALLOC_IOC_HINT, &h) < 0) { perror("hint presplit"); return 1; }

    printf("\n=== step 6: status AFTER PRESPLIT (expect order 2 count > 0) ===\n");
    dump_status();

    printf("\n=== step 7: PRECOALESCE hint at order 3 ===\n");
    h.action = MYMALLOC_HINT_PRECOALESCE;
    h.order  = 3;
    if (ioctl(fd, MYMALLOC_IOC_HINT, &h) < 0) { perror("hint precoalesce"); return 1; }

    printf("\n=== step 8: status AFTER PRECOALESCE ===\n");
    dump_status();

    close(fd);
    printf("\n=== test complete ===\n");
    return 0;
}