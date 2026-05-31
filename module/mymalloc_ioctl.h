#ifndef MYMALLOC_IOCTL_H
#define MYMALLOC_IOCTL_H

#include <linux/ioctl.h>

/* The argument struct passed between userspace and the kernel.
 * For alloc: userspace fills in 'order', kernel fills in 'addr'.
 * For free:  userspace fills in both 'addr' and 'order'. */
struct mymalloc_arg {
    unsigned long addr;  /* virtual address of the block (offset from pool_addr) */
    unsigned int  order; /* order of the block (0,1,2,...,POOL_ORDER) */
};

/* Magic number — uniquely identifies our device's ioctl commands.
 * Chosen to avoid clashing with other drivers. */
#define MYMALLOC_IOC_MAGIC 0xBB

/* ALLOC command: userspace passes order in, kernel writes addr back. */
#define MYMALLOC_IOC_ALLOC _IOWR(MYMALLOC_IOC_MAGIC, 1, struct mymalloc_arg)

/* FREE command: userspace passes both addr and order in. */
#define MYMALLOC_IOC_FREE  _IOW(MYMALLOC_IOC_MAGIC, 2, struct mymalloc_arg)

#endif /* MYALLOC_IOCTL_H */

