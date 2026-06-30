#ifndef MYMALLOC_IOCTL_H
#define MYMALLOC_IOCTL_H
#include <linux/ioctl.h>

struct mymalloc_arg {
    unsigned long addr;
    unsigned int  order;
};

struct mymalloc_slab_arg {
    unsigned long addr;
    unsigned int  slot_size;
};

#define MYMALLOC_IOC_MAGIC      0xBB

#define MYMALLOC_IOC_ALLOC      _IOWR(MYMALLOC_IOC_MAGIC, 1, struct mymalloc_arg)
#define MYMALLOC_IOC_FREE       _IOW (MYMALLOC_IOC_MAGIC, 2, struct mymalloc_arg)
#define MYMALLOC_IOC_SLAB_ALLOC _IOWR(MYMALLOC_IOC_MAGIC, 3, struct mymalloc_slab_arg)
#define MYMALLOC_IOC_SLAB_FREE  _IOW (MYMALLOC_IOC_MAGIC, 4, struct mymalloc_slab_arg)

/*
 * Phase 11: ML hint argument.
 * action: 0 = NOOP, 1 = PRESPLIT, 2 = PRECOALESCE
 * order:  the allocation order the hint applies to
 */
struct mymalloc_hint_arg {
    unsigned int action;
    unsigned int order;
};

#define MYMALLOC_HINT_NOOP        0
#define MYMALLOC_HINT_PRESPLIT    1
#define MYMALLOC_HINT_PRECOALESCE 2

#define MYMALLOC_IOC_HINT _IOW(MYMALLOC_IOC_MAGIC, 5, struct mymalloc_hint_arg)

#endif /* MYMALLOC_IOCTL_H */