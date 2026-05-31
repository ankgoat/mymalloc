#include <linux/module.h>   /* module_init, module_exit, MODULE_LICENSE etc. */
#include <linux/init.h>     /* __init and __exit markers */
#include <linux/kernel.h>   /* pr_info, pr_err */
#include <linux/gfp.h>      /* __get_free_pages, GFP_KERNEL */
#include <linux/mm.h>       /* PAGE_SHIFT, PAGE_SIZE, virt_to_phys */
#include <linux/slab.h>     /* kcalloc, kfree — kernel heap allocator */
#include <linux/list.h>     /* struct list_head, list_add, list_del, list_for_each */
#include <linux/fs.h>       /* register_chrdev, file_operations */
#include <linux/uaccess.h>  /* copy_to_user, copy_from_user */
#include <linux/proc_fs.h>  /* proc_create, remove_proc_entry */
#include <linux/seq_file.h> /* seq_file, seq_printf */
#include "mymalloc_ioctl.h" /* shared ioctl command definitions */

/* POOL_ORDER: we ask the kernel for 2^10 = 1024 contiguous pages = 4 MB.
 * NR_ORDERS: we need free lists for orders 0,1,2,...,10 — that's 11 lists. */
#define POOL_ORDER   10
#define NR_ORDERS    (POOL_ORDER + 1)
#define DEVICE_NAME  "mymalloc"

/* Every page-aligned block in our pool gets one of these structs.
 * We keep them in a separate array (block_map) — NOT inside the pool memory —
 * so the pool stays clean and we can find any block's metadata in O(1). */
struct block {
    struct list_head node;   /* the hook that lets this block sit on a free_list */
    unsigned int     order;  /* what size is this block right now: 2^order pages */
    bool             free;   /* true = available to allocate, false = in use */
};

/* Virtual address of the first byte of our pool. */
static unsigned long    pool_addr;

/* Total number of pages in the pool (= 2^POOL_ORDER = 1024). */
static unsigned long    pool_pages;

/* free_lists[o] is the head of a linked list of all free blocks of order o.
 * At startup only free_lists[10] has anything — the whole pool as one block. */
static struct list_head free_lists[NR_ORDERS];

/* block_map[i] holds the metadata for the block starting at page offset i. */
static struct block    *block_map;

/* major number assigned by the kernel when we register the character device */
static int major;

/* handle to our /proc/mymalloc entry so we can remove it on unload */
static struct proc_dir_entry *proc_entry;

/* ---------- helpers ---------- */

/* Convert a virtual address inside the pool to a page offset from pool_addr.
 * >> PAGE_SHIFT divides by PAGE_SIZE (4096) using a fast bit-shift. */
static inline unsigned long addr_to_offset(unsigned long addr)
{
    return (addr - pool_addr) >> PAGE_SHIFT;
}

/* Convert a page offset back to the virtual address of that page. */
static inline unsigned long offset_to_addr(unsigned long offset)
{
    return pool_addr + (offset << PAGE_SHIFT);
}

/* Print the current free list state to dmesg. */
static void buddy_dump(void)
{
    int o;
    pr_info("mymalloc: free list state:\n");
    for (o = 0; o < NR_ORDERS; o++) {
        int count = 0;
        struct list_head *p;
        list_for_each(p, &free_lists[o])
            count++;
        if (count)
            pr_info("mymalloc:   order %2d (%5lu KB each): %d block(s)\n",
                    o, (PAGE_SIZE << o) / 1024, count);
    }
}

/* ---------- /proc/mymalloc ---------- */

/* Called every time someone reads /proc/mymalloc.
 * seq_printf works just like printf but writes into the proc file. */
static int mymalloc_proc_show(struct seq_file *m, void *v)
{
    int o;
    seq_printf(m, "mymalloc free lists:\n");
    for (o = 0; o < NR_ORDERS; o++) {
        int count = 0;
        struct list_head *p;
        list_for_each(p, &free_lists[o])
            count++;
        seq_printf(m, "  order %2d: %3d free block(s)  (%5lu KB each)\n",
                   o, count, (PAGE_SIZE << o) / 1024);
    }
    return 0;
}

/* Called when userspace opens /proc/mymalloc.
 * single_open wires up the show function above to handle the read. */
static int mymalloc_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, mymalloc_proc_show, NULL);
}

/* The file operations for our /proc entry.
 * single_open requires seq_read, seq_lseek, and single_release. */
static const struct proc_ops mymalloc_proc_ops = {
    .proc_open    = mymalloc_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ---------- buddy allocator ---------- */

/* Allocate a block of 2^order pages from our pool.
 * Returns the virtual address of the block, or 0 on failure. */
static unsigned long buddy_alloc(unsigned int order)
{
    unsigned int o;

    /* Scan upward from the requested order for the first non-empty list. */
    for (o = order; o < NR_ORDERS; o++) {
        struct block *blk;
        unsigned long offset;

        if (list_empty(&free_lists[o]))
            continue;

        /* Pop the first block off this free list. */
        blk = list_first_entry(&free_lists[o], struct block, node);
        list_del_init(&blk->node);
        blk->free = false;

        offset = (unsigned long)(blk - block_map);

        /* Split down until we reach the requested order.
         * Each right half becomes a free buddy on the list one order lower. */
        while (o > order) {
            o--;
            unsigned long buddy_offset = offset + (1UL << o);
            struct block *buddy = &block_map[buddy_offset];
            buddy->order = o;
            buddy->free  = true;
            list_add(&buddy->node, &free_lists[o]);
        }

        blk->order = order;
        pr_info("mymalloc: alloc order %u -> offset %lu (addr 0x%lx)\n",
                order, offset, offset_to_addr(offset));
        return offset_to_addr(offset);
    }

    pr_err("mymalloc: buddy_alloc(%u) failed\n", order);
    return 0;
}

/* Free a block and coalesce with its buddy if possible.
 * addr  = virtual address returned by buddy_alloc
 * order = the same order that was passed to buddy_alloc */
static void buddy_free(unsigned long addr, unsigned int order)
{
    unsigned long offset = addr_to_offset(addr);

    pr_info("mymalloc: free order %u -> offset %lu (addr 0x%lx)\n",
            order, offset, addr);

    /* Coalescing loop: keep merging with our buddy, one order at a time.
     * The XOR trick finds the buddy: flip the one bit that separates us. */
    while (order < POOL_ORDER) {
        unsigned long buddy_offset = offset ^ (1UL << order);
        struct block *buddy = &block_map[buddy_offset];

        /* Stop if the buddy is not free or is a different order. */
        if (!buddy->free || buddy->order != order)
            break;

        list_del_init(&buddy->node);
        buddy->free = false;
        offset = offset & ~(1UL << order);

        pr_info("mymalloc: merge order %u -> offset %lu order %u\n",
                order, offset, order + 1);
        order++;
    }

    struct block *blk = &block_map[offset];
    blk->order = order;
    blk->free  = true;
    list_add(&blk->node, &free_lists[order]);
}

/* ---------- character device file operations ---------- */

/* Called when userspace opens /dev/mymalloc. */
static int mymalloc_open(struct inode *inode, struct file *file)
{
    pr_info("mymalloc: device opened\n");
    return 0;
}

/* Called when userspace closes /dev/mymalloc. */
static int mymalloc_release(struct inode *inode, struct file *file)
{
    pr_info("mymalloc: device closed\n");
    return 0;
}

/* Called when userspace calls ioctl() on /dev/mymalloc. */
static long mymalloc_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg)
{
    struct mymalloc_arg kargs;

    /* Copy the argument struct from userspace into kernel memory. */
    if (copy_from_user(&kargs, (void __user *)arg, sizeof(kargs)))
        return -EFAULT;

    switch (cmd) {
    case MYMALLOC_IOC_ALLOC:
        kargs.addr = buddy_alloc(kargs.order);
        if (!kargs.addr)
            return -ENOMEM;
        if (copy_to_user((void __user *)arg, &kargs, sizeof(kargs)))
            return -EFAULT;
        break;
    case MYMALLOC_IOC_FREE:
        buddy_free(kargs.addr, kargs.order);
        break;
    default:
        return -ENOTTY;
    }
    return 0;
}

/* Table of file operations our character device supports. */
static const struct file_operations mymalloc_fops = {
    .owner          = THIS_MODULE,
    .open           = mymalloc_open,
    .release        = mymalloc_release,
    .unlocked_ioctl = mymalloc_ioctl,
};

/* ---------- init / exit ---------- */

static int __init mymalloc_init(void)
{
    unsigned long i;

    /* Allocate the pool pages from the kernel. */
    pool_pages = 1UL << POOL_ORDER;
    pool_addr  = __get_free_pages(GFP_KERNEL, POOL_ORDER);
    if (!pool_addr) {
        pr_err("mymalloc: failed to allocate pool\n");
        return -ENOMEM;
    }
    memset((void *)pool_addr, 0, pool_pages * PAGE_SIZE);

    /* Initialise all free list heads to empty. */
    for (i = 0; i < NR_ORDERS; i++)
        INIT_LIST_HEAD(&free_lists[i]);

    /* Allocate the block_map metadata array on the kernel heap. */
    block_map = kcalloc(pool_pages, sizeof(struct block), GFP_KERNEL);
    if (!block_map) {
        free_pages(pool_addr, POOL_ORDER);
        return -ENOMEM;
    }
    for (i = 0; i < pool_pages; i++)
        INIT_LIST_HEAD(&block_map[i].node);

    /* Seed the allocator: whole pool = one free block at order POOL_ORDER. */
    block_map[0].order = POOL_ORDER;
    block_map[0].free  = true;
    list_add(&block_map[0].node, &free_lists[POOL_ORDER]);

    pr_info("mymalloc: pool ready -- %lu pages at 0x%lx\n",
            pool_pages, pool_addr);

    /* Register the character device. 0 = ask kernel for a free major number. */
    major = register_chrdev(0, DEVICE_NAME, &mymalloc_fops);
    if (major < 0) {
        pr_err("mymalloc: register_chrdev failed: %d\n", major);
        kfree(block_map);
        free_pages(pool_addr, POOL_ORDER);
        return major;
    }
    pr_info("mymalloc: registered with major number %d\n", major);
    pr_info("mymalloc: run this in the VM: mknod /dev/mymalloc c %d 0\n", major);

    /* Create /proc/mymalloc. 0444 = readable by everyone, not writable. */
    proc_entry = proc_create("mymalloc", 0444, NULL, &mymalloc_proc_ops);
    if (!proc_entry) {
        pr_err("mymalloc: failed to create /proc/mymalloc\n");
        unregister_chrdev(major, DEVICE_NAME);
        kfree(block_map);
        free_pages(pool_addr, POOL_ORDER);
        return -ENOMEM;
    }
    pr_info("mymalloc: /proc/mymalloc created\n");

    buddy_dump();
    return 0;
}

static void __exit mymalloc_exit(void)
{
    remove_proc_entry("mymalloc", NULL);   /* remove /proc/mymalloc */
    unregister_chrdev(major, DEVICE_NAME); /* remove the character device */
    kfree(block_map);                      /* free the metadata array */
    free_pages(pool_addr, POOL_ORDER);     /* give pool pages back to kernel */
    pr_info("mymalloc: unloaded\n");
}

module_init(mymalloc_init);
module_exit(mymalloc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ankith Goswami");
MODULE_DESCRIPTION("Buddy allocator -- phase 5: proc filesystem");