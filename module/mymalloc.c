#include <linux/module.h>   /* module_init, module_exit, MODULE_LICENSE etc. */
#include <linux/init.h>     /* __init and __exit markers */
#include <linux/kernel.h>   /* pr_info, pr_err */
#include <linux/gfp.h>      /* __get_free_pages, GFP_KERNEL */
#include <linux/mm.h>       /* PAGE_SHIFT, PAGE_SIZE, virt_to_phys */
#include <linux/slab.h>     /* kcalloc, kfree — kernel heap allocator */
#include <linux/list.h>     /* struct list_head, list_add, list_del, list_for_each */
#include <linux/fs.h>       /* register_chrdev, file_operations */
#include <linux/uaccess.h>  /* copy_to_user, copy_from_user */
#include <linux/proc_fs.h>  /* proc_create, proc_mkdir, remove_proc_entry */
#include <linux/seq_file.h> /* seq_file, seq_printf */
#include <linux/spinlock.h> /* spinlock_t, spin_lock_irqsave */
#include <linux/ktime.h>    /* ktime_get for nanosecond timestamps */
#include <linux/bitops.h>   /* find_first_bit, set_bit, clear_bit, test_bit */
#include "mymalloc_ioctl.h" /* shared ioctl command definitions */

/* POOL_ORDER: we ask the kernel for 2^10 = 1024 contiguous pages = 4 MB.
 * NR_ORDERS: we need free lists for orders 0,1,2,...,10 — that's 11 lists. */
#define POOL_ORDER   10
#define NR_ORDERS    (POOL_ORDER + 1)
#define DEVICE_NAME  "mymalloc"

/* Trace ring buffer holds the most recent TRACE_CAP allocator events. */
#define TRACE_CAP    256

/* Every page-aligned block in our pool gets one of these structs.
 * We keep them in a separate array (block_map) — NOT inside the pool memory —
 * so the pool stays clean and we can find any block's metadata in O(1). */
struct block {
    struct list_head node;   /* the hook that lets this block sit on a free_list */
    unsigned int     order;  /* what size is this block right now: 2^order pages */
    bool             free;   /* true = available to allocate, false = in use */
};

/* ---------- slab cache (Phase 6) ----------
 * The slab layer carves a single buddy block into fixed-size slots so that
 * small, uniform allocations don't each consume a whole page. One slab is
 * active at a time; a free_bitmap tracks which slots are in use. */
struct slab_cache {
    bool          active;        /* is a slab currently carved out?            */
    unsigned int  slot_size;     /* size of each slot in bytes                 */
    unsigned int  nr_slots;      /* how many slots the slab block holds        */
    unsigned int  nr_free;       /* how many slots are currently free          */
    unsigned long base_addr;     /* virtual address of the slab's buddy block  */
    unsigned int  block_order;   /* buddy order of the backing block           */
    unsigned long free_bitmap[BITS_TO_LONGS(PAGE_SIZE / 8)]; /* 1 = free slot  */
};

/* ---------- trace ring buffer (Phase 8) ----------
 * Each event records a nanosecond timestamp, the operation name, the order and
 * address involved, a snapshot of all 11 free-list counts, and the post-op
 * fragmentation score. Exposed as a CSV stream via /proc/mymalloc/trace. */
struct trace_event {
    u64           timestamp_ns;
    const char   *op;
    unsigned int  order;
    unsigned long addr;
    int           free_counts[NR_ORDERS];
    unsigned int  frag_score_pct;
};

/* ---------- global allocator state ---------- */

static unsigned long    pool_addr;    /* virtual address of first pool byte    */
static unsigned long    pool_pages;   /* total pages in the pool (= 1024)      */
static struct list_head free_lists[NR_ORDERS];  /* one free list per order     */
static int              free_counts[NR_ORDERS]; /* count of free blocks/order  */
static struct block    *block_map;    /* metadata array, one entry per page    */
static int              major;        /* major number for the char device      */
static struct proc_dir_entry *proc_dir; /* /proc/mymalloc directory handle     */

static struct slab_cache slab;        /* the single active slab cache          */

/* Trace ring buffer + write head + total-events counter. */
static struct trace_event trace_buf[TRACE_CAP];
static unsigned int       trace_head;   /* next write index (wraps at CAP)     */
static unsigned int       trace_count;  /* total events ever recorded          */

/* One spinlock guards ALL allocator state: free lists, counts, slab, trace. */
static DEFINE_SPINLOCK(allocator_lock);

/* ---------- address helpers ---------- */

static inline unsigned long addr_to_offset(unsigned long addr)
{
    return (addr - pool_addr) >> PAGE_SHIFT;
}

static inline unsigned long offset_to_addr(unsigned long offset)
{
    return pool_addr + (offset << PAGE_SHIFT);
}

/* ---------- fragmentation score (Phase 9) ----------
 * A 0–100 score measuring how far the largest available free block is from
 * the pool maximum. 0 = a full order-10 block is free (no fragmentation);
 * 100 = nothing larger than order-0 remains. Computed after every operation. */
static unsigned int compute_frag_score(void)
{
    int o;
    /* Find the highest order that still has a free block. */
    for (o = POOL_ORDER; o >= 0; o--) {
        if (free_counts[o] > 0) {
            /* Largest free block is 2^o pages; full pool is 2^POOL_ORDER.
             * Score scales linearly with how many orders below max we are. */
            return (unsigned int)((POOL_ORDER - o) * 100 / POOL_ORDER);
        }
    }
    /* No free blocks at all -> fully fragmented / exhausted. */
    return 100;
}

/* ---------- trace recording (Phase 8) ----------
 * Snapshot the current allocator state into the ring buffer. Caller must hold
 * allocator_lock. op is a static string literal ("buddy_alloc" etc.). */
static void trace_record(const char *op, unsigned int order, unsigned long addr)
{
    struct trace_event *e = &trace_buf[trace_head];
    int o;

    e->timestamp_ns = ktime_get();
    e->op           = op;
    e->order        = order;
    e->addr         = addr;
    for (o = 0; o < NR_ORDERS; o++)
        e->free_counts[o] = free_counts[o];
    e->frag_score_pct = compute_frag_score();

    trace_head = (trace_head + 1) % TRACE_CAP;
    trace_count++;
}

/* ---------- buddy allocator ---------- */

static void buddy_dump(void)
{
    int o;
    pr_info("mymalloc: free list state:\n");
    for (o = 0; o < NR_ORDERS; o++) {
        if (free_counts[o])
            pr_info("mymalloc:   order %2d (%5lu KB each): %d block(s)\n",
                    o, (PAGE_SIZE << o) / 1024, free_counts[o]);
    }
}

/* Allocate 2^order pages. Returns virtual address, or 0 on failure.
 * Caller must hold allocator_lock. */
static unsigned long buddy_alloc(unsigned int order)
{
    unsigned int o;

    for (o = order; o < NR_ORDERS; o++) {
        struct block *blk;
        unsigned long offset;

        if (list_empty(&free_lists[o]))
            continue;

        blk = list_first_entry(&free_lists[o], struct block, node);
        list_del_init(&blk->node);
        free_counts[o]--;
        blk->free = false;

        offset = (unsigned long)(blk - block_map);

        /* Split down to the requested order, freeing each upper-half buddy. */
        while (o > order) {
            unsigned long buddy_offset;
            struct block *buddy;
            o--;
            buddy_offset = offset + (1UL << o);
            buddy = &block_map[buddy_offset];
            buddy->order = o;
            buddy->free  = true;
            list_add(&buddy->node, &free_lists[o]);
            free_counts[o]++;
        }

        blk->order = order;
        pr_info("mymalloc: buddy_alloc order %u -> offset %lu (addr 0x%lx)\n",
                order, offset, offset_to_addr(offset));
        return offset_to_addr(offset);
    }

    pr_err("mymalloc: buddy_alloc(%u) failed\n", order);
    return 0;
}

/* Free a block and coalesce with its buddy where possible.
 * Caller must hold allocator_lock. */
static void buddy_free(unsigned long addr, unsigned int order)
{
    unsigned long offset = addr_to_offset(addr);
    struct block *blk;

    pr_info("mymalloc: buddy_free order %u -> offset %lu (addr 0x%lx)\n",
            order, offset, addr);

    while (order < POOL_ORDER) {
        unsigned long buddy_offset = offset ^ (1UL << order);
        struct block *buddy = &block_map[buddy_offset];

        if (!buddy->free || buddy->order != order)
            break;

        list_del_init(&buddy->node);
        free_counts[order]--;
        buddy->free = false;
        offset = offset & ~(1UL << order);

        pr_info("mymalloc: merge order %u -> offset %lu order %u\n",
                order, offset, order + 1);
        order++;
    }

    blk = &block_map[offset];
    blk->order = order;
    blk->free  = true;
    list_add(&blk->node, &free_lists[order]);
    free_counts[order]++;
}

/* ---------- slab allocator (Phase 6) ---------- */

/* Carve a fresh slab of nr_slots x slot_size from a buddy block big enough
 * to hold them. Caller must hold allocator_lock. Returns 0 on success. */
static int slab_init(unsigned int slot_size)
{
    unsigned int needed, order, o;
    unsigned long base;

    if (slot_size == 0 || slot_size > PAGE_SIZE) {
        pr_err("mymalloc: slab_init: invalid slot_size %u\n", slot_size);
        return -EINVAL;
    }

    /* One page worth of slots keeps the slab simple and bounded. */
    slab.slot_size = slot_size;
    slab.nr_slots  = PAGE_SIZE / slot_size;

    /* Find the buddy order whose block fits one page (order 0 = one page). */
    needed = 1; /* one page */
    order  = 0;
    while ((1U << order) < needed)
        order++;

    base = buddy_alloc(order);
    if (!base)
        return -ENOMEM;

    slab.active     = true;
    slab.base_addr  = base;
    slab.block_order = order;
    slab.nr_free    = slab.nr_slots;

    /* Mark every slot free in the bitmap. */
    for (o = 0; o < slab.nr_slots; o++)
        set_bit(o, slab.free_bitmap);

    pr_info("mymalloc: slab_init slot_size=%u nr_slots=%u base=0x%lx\n",
            slab.slot_size, slab.nr_slots, slab.base_addr);
    return 0;
}

/* Allocate one slot from the active slab. Caller holds allocator_lock. */
static unsigned long slab_alloc(unsigned int slot_size)
{
    unsigned long slot;
    unsigned long addr;

    if (!slab.active) {
        if (slab_init(slot_size) != 0)
            return 0;
    } else if (slab.slot_size != slot_size) {
        pr_err("mymalloc: slab slot_size mismatch (have %u, want %u)\n",
               slab.slot_size, slot_size);
        return 0;
    }

    if (slab.nr_free == 0) {
        pr_err("mymalloc: slab full\n");
        return 0;
    }

    slot = find_first_bit(slab.free_bitmap, slab.nr_slots);
    if (slot >= slab.nr_slots) {
        pr_err("mymalloc: slab full\n");
        return 0;
    }

    clear_bit(slot, slab.free_bitmap);
    slab.nr_free--;

    addr = slab.base_addr + slot * slab.slot_size;
    pr_info("mymalloc: slab_alloc slot %d -> addr 0x%lx\n", (int)slot, addr);
    return addr;
}

/* Free one slot back to the active slab. Caller holds allocator_lock. */
static void slab_free(unsigned long addr)
{
    unsigned long slot;

    if (!slab.active) {
        pr_err("mymalloc: slab_free called but slab not active\n");
        return;
    }

    if (addr < slab.base_addr ||
        addr >= slab.base_addr + (unsigned long)slab.nr_slots * slab.slot_size) {
        pr_err("mymalloc: slab_free addr 0x%lx out of range\n", addr);
        return;
    }

    slot = (addr - slab.base_addr) / slab.slot_size;

    if (test_bit(slot, slab.free_bitmap)) {
        pr_err("mymalloc: slab_free double-free on slot %d\n", (int)slot);
        return;
    }

    set_bit(slot, slab.free_bitmap);
    slab.nr_free++;
    pr_info("mymalloc: slab_free slot %d (addr 0x%lx) nr_free=%u\n",
            (int)slot, addr, slab.nr_free);

    /* When the slab empties out, hand its backing block back to the buddy. */
    if (slab.nr_free == slab.nr_slots) {
        pr_info("mymalloc: slab fully free -- returning block to buddy\n");
        buddy_free(slab.base_addr, slab.block_order);
        slab.active = false;
    }
}

/* ---------- /proc/mymalloc/status (Phase 6/9) ---------- */

static int status_show(struct seq_file *m, void *v)
{
    int o;

    seq_printf(m, "mymalloc free lists:\n");
    for (o = 0; o < NR_ORDERS; o++)
        seq_printf(m, "  order %2d: %3d free block(s)  (%5lu KB each)\n",
                   o, free_counts[o], (PAGE_SIZE << o) / 1024);

    seq_printf(m, "mymalloc slab cache:\n");
    if (slab.active) {
        seq_printf(m, "  slot_size : %u bytes\n", slab.slot_size);
        seq_printf(m, "  nr_slots  : %u\n",       slab.nr_slots);
        seq_printf(m, "  nr_free   : %u\n",       slab.nr_free);
        seq_printf(m, "  base_addr : 0x%lx\n",    slab.base_addr);
    } else {
        seq_printf(m, "  (inactive)\n");
    }

    seq_printf(m, "fragmentation score : %u / 100\n", compute_frag_score());
    seq_printf(m, "trace events recorded: %u\n", trace_count);
    return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
    return single_open(file, status_show, NULL);
}

static const struct proc_ops status_ops = {
    .proc_open    = status_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ---------- /proc/mymalloc/trace (Phase 8) ---------- */

static int trace_show(struct seq_file *m, void *v)
{
    unsigned int n, i, idx;
    int o;

    /* CSV header. */
    seq_printf(m, "timestamp_ns,op,order,addr");
    for (o = 0; o < NR_ORDERS; o++)
        seq_printf(m, ",free%d", o);
    seq_printf(m, ",frag_score_pct\n");

    /* How many valid events are in the ring, and where the oldest one is. */
    n = (trace_count < TRACE_CAP) ? trace_count : TRACE_CAP;
    idx = (trace_count < TRACE_CAP) ? 0 : trace_head;

    for (i = 0; i < n; i++) {
        struct trace_event *e = &trace_buf[(idx + i) % TRACE_CAP];
        seq_printf(m, "%llu,%s,%u,0x%lx",
                   e->timestamp_ns, e->op, e->order, e->addr);
        for (o = 0; o < NR_ORDERS; o++)
            seq_printf(m, ",%d", e->free_counts[o]);
        seq_printf(m, ",%u\n", e->frag_score_pct);
    }
    return 0;
}

static int trace_open(struct inode *inode, struct file *file)
{
    return single_open(file, trace_show, NULL);
}

static const struct proc_ops trace_ops = {
    .proc_open    = trace_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ---------- character device file operations ---------- */

static int mymalloc_open(struct inode *inode, struct file *file)
{
    pr_info("mymalloc: device opened\n");
    return 0;
}

static int mymalloc_release(struct inode *inode, struct file *file)
{
    pr_info("mymalloc: device closed\n");
    return 0;
}

static long mymalloc_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg)
{
    unsigned long flags;
    long ret = 0;

    switch (cmd) {
    case MYMALLOC_IOC_ALLOC: {
        struct mymalloc_arg kargs;
        if (copy_from_user(&kargs, (void __user *)arg, sizeof(kargs)))
            return -EFAULT;
        spin_lock_irqsave(&allocator_lock, flags);
        kargs.addr = buddy_alloc(kargs.order);
        if (kargs.addr)
            trace_record("buddy_alloc", kargs.order, kargs.addr);
        spin_unlock_irqrestore(&allocator_lock, flags);
        if (!kargs.addr)
            return -ENOMEM;
        if (copy_to_user((void __user *)arg, &kargs, sizeof(kargs)))
            return -EFAULT;
        break;
    }
    case MYMALLOC_IOC_FREE: {
        struct mymalloc_arg kargs;
        if (copy_from_user(&kargs, (void __user *)arg, sizeof(kargs)))
            return -EFAULT;
        spin_lock_irqsave(&allocator_lock, flags);
        buddy_free(kargs.addr, kargs.order);
        trace_record("buddy_free", kargs.order, kargs.addr);
        spin_unlock_irqrestore(&allocator_lock, flags);
        break;
    }
    case MYMALLOC_IOC_SLAB_ALLOC: {
        struct mymalloc_slab_arg kslab;
        if (copy_from_user(&kslab, (void __user *)arg, sizeof(kslab)))
            return -EFAULT;
        spin_lock_irqsave(&allocator_lock, flags);
        kslab.addr = slab_alloc(kslab.slot_size);
        if (kslab.addr)
            trace_record("slab_alloc", 0, kslab.addr);
        spin_unlock_irqrestore(&allocator_lock, flags);
        if (!kslab.addr)
            return -ENOMEM;
        if (copy_to_user((void __user *)arg, &kslab, sizeof(kslab)))
            return -EFAULT;
        break;
    }
    case MYMALLOC_IOC_SLAB_FREE: {
        struct mymalloc_slab_arg kslab;
        if (copy_from_user(&kslab, (void __user *)arg, sizeof(kslab)))
            return -EFAULT;
        spin_lock_irqsave(&allocator_lock, flags);
        slab_free(kslab.addr);
        trace_record("slab_free", 0, kslab.addr);
        spin_unlock_irqrestore(&allocator_lock, flags);
        break;
    }
    default:
        return -ENOTTY;
    }
    return ret;
}

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
    struct proc_dir_entry *err_status;

    pool_pages = 1UL << POOL_ORDER;
    pool_addr  = __get_free_pages(GFP_KERNEL, POOL_ORDER);
    if (!pool_addr) {
        pr_err("mymalloc: failed to allocate pool\n");
        return -ENOMEM;
    }
    memset((void *)pool_addr, 0, pool_pages * PAGE_SIZE);

    for (i = 0; i < NR_ORDERS; i++) {
        INIT_LIST_HEAD(&free_lists[i]);
        free_counts[i] = 0;
    }

    block_map = kcalloc(pool_pages, sizeof(struct block), GFP_KERNEL);
    if (!block_map) {
        free_pages(pool_addr, POOL_ORDER);
        return -ENOMEM;
    }
    for (i = 0; i < pool_pages; i++)
        INIT_LIST_HEAD(&block_map[i].node);

    block_map[0].order = POOL_ORDER;
    block_map[0].free  = true;
    list_add(&block_map[0].node, &free_lists[POOL_ORDER]);
    free_counts[POOL_ORDER] = 1;

    slab.active = false;

    pr_info("mymalloc: pool ready -- %lu pages at 0x%lx\n",
            pool_pages, pool_addr);

    major = register_chrdev(0, DEVICE_NAME, &mymalloc_fops);
    if (major < 0) {
        pr_err("mymalloc: register_chrdev failed: %d\n", major);
        kfree(block_map);
        free_pages(pool_addr, POOL_ORDER);
        return major;
    }
    pr_info("mymalloc: registered with major number %d\n", major);
    pr_info("mymalloc: run: mknod /dev/mymalloc c %d 0\n", major);

    /* /proc/mymalloc/ directory with two entries: status and trace. */
    proc_dir = proc_mkdir("mymalloc", NULL);
    if (!proc_dir) {
        pr_err("mymalloc: failed to create /proc/mymalloc\n");
        unregister_chrdev(major, DEVICE_NAME);
        kfree(block_map);
        free_pages(pool_addr, POOL_ORDER);
        return -ENOMEM;
    }

    err_status = proc_create("status", 0444, proc_dir, &status_ops);
    if (!err_status) {
        remove_proc_entry("mymalloc", NULL);
        unregister_chrdev(major, DEVICE_NAME);
        kfree(block_map);
        free_pages(pool_addr, POOL_ORDER);
        return -ENOMEM;
    }
    proc_create("trace", 0444, proc_dir, &trace_ops);

    pr_info("mymalloc: /proc/mymalloc/status and /proc/mymalloc/trace created\n");

    buddy_dump();
    return 0;
}

static void __exit mymalloc_exit(void)
{
    remove_proc_entry("status", proc_dir);
    remove_proc_entry("trace", proc_dir);
    remove_proc_entry("mymalloc", NULL);
    unregister_chrdev(major, DEVICE_NAME);
    kfree(block_map);
    free_pages(pool_addr, POOL_ORDER);
    pr_info("mymalloc: unloaded\n");
}

module_init(mymalloc_init);
module_exit(mymalloc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ankith Goswami");
MODULE_DESCRIPTION("Buddy + slab + trace + frag score -- phase 9");
