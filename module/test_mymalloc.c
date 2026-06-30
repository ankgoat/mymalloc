#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "mymalloc_ioctl.h"

int main(void) {
    int fd = open("/dev/mymalloc", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("opened /dev/mymalloc\n\n");

    /* ---------- buddy path (unchanged from Phase 4) ---------- */
    struct mymalloc_arg args;

    args.order = 0;
    ioctl(fd, MYMALLOC_IOC_ALLOC, &args);
    unsigned long addr0 = args.addr;
    printf("buddy alloc order 0 -> addr 0x%lx\n", addr0);

    args.order = 2;
    ioctl(fd, MYMALLOC_IOC_ALLOC, &args);
    unsigned long addr2 = args.addr;
    printf("buddy alloc order 2 -> addr 0x%lx\n", addr2);

    args.addr = addr0; args.order = 0;
    ioctl(fd, MYMALLOC_IOC_FREE, &args);
    printf("buddy freed order 0 at 0x%lx\n", addr0);

    args.addr = addr2; args.order = 2;
    ioctl(fd, MYMALLOC_IOC_FREE, &args);
    printf("buddy freed order 2 at 0x%lx\n\n", addr2);

    /* ---------- NEW Phase 6: slab path ---------- */
    struct mymalloc_slab_arg s;

    /* Allocate three 64-byte slots from the slab */
    unsigned long slots[3];
    for (int i = 0; i < 3; i++) {
        s.slot_size = 64;
        ioctl(fd, MYMALLOC_IOC_SLAB_ALLOC, &s);
        slots[i] = s.addr;
        printf("slab alloc 64B slot %d -> addr 0x%lx\n", i, slots[i]);
    }

    /* Free them all back — the last free should return the block to buddy */
    for (int i = 0; i < 3; i++) {
        s.addr = slots[i];
        ioctl(fd, MYMALLOC_IOC_SLAB_FREE, &s);
        printf("slab freed slot %d at 0x%lx\n", i, slots[i]);
    }

    close(fd);
    printf("\nall done\n");
    return 0;
}