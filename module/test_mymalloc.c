#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "mymalloc_ioctl.h"

int main(void)
{
    int fd;
    struct mymalloc_arg args;

    /* Open the device file we created with mknod. */
    fd = open("/dev/mymalloc", O_RDWR);
    if (fd < 0) {
        perror("open /dev/mymalloc");
        return 1;
    }
    printf("opened /dev/mymalloc\n");

    /* Allocate order 0 (1 page = 4 KB). */
    args.order = 0;
    if (ioctl(fd, MYMALLOC_IOC_ALLOC, &args) < 0) {
        perror("ioctl ALLOC order 0");
        return 1;
    }
    printf("alloc order 0 -> addr 0x%lx\n", args.addr);
    unsigned long addr0 = args.addr;

    /* Allocate order 2 (4 pages = 16 KB). */
    args.order = 2;
    if (ioctl(fd, MYMALLOC_IOC_ALLOC, &args) < 0) {
        perror("ioctl ALLOC order 2");
        return 1;
    }
    printf("alloc order 2 -> addr 0x%lx\n", args.addr);
    unsigned long addr2 = args.addr;

    /* Free order 0 block. */
    args.addr  = addr0;
    args.order = 0;
    if (ioctl(fd, MYMALLOC_IOC_FREE, &args) < 0) {
        perror("ioctl FREE order 0");
        return 1;
    }
    printf("freed order 0 at addr 0x%lx\n", addr0);

    /* Free order 2 block. */
    args.addr  = addr2;
    args.order = 2;
    if (ioctl(fd, MYMALLOC_IOC_FREE, &args) < 0) {
        perror("ioctl FREE order 2");
        return 1;
    }
    printf("freed order 2 at addr 0x%lx\n", addr2);

    close(fd);
    printf("all done\n");
    return 0;
}