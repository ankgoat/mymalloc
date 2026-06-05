mymalloc
A Linux kernel module implementing a buddy system memory allocator, modeled after the Linux kernel's own page allocator.
Built from scratch on an ARM64 QEMU sandbox — no pre-existing framework, no shortcuts. Every layer was written, broken, debugged, and verified by hand.

What this is
The Linux kernel manages physical memory using a buddy allocator: a system that tracks free pages in power-of-two sized blocks (called orders), splits large blocks to satisfy small requests, and merges adjacent free blocks back together when memory is freed. This project reimplements that system as a loadable kernel module, building it up layer by layer from raw page allocation to a full userspace API.
This is not a toy. It uses the same data structures, the same XOR-based buddy address calculation, and the same architectural pattern as the real kernel allocator. The difference is that every decision is visible, instrumented, and yours to control.

Environment
DetailValueHost OSUbuntu 24.04 under Parallels on Apple SiliconArchitectureARM64 (aarch64)KernelLinux 6.6.30 (built from source)UserspaceBusyBox 1.36.1 (static binary)VMQEMU qemu-system-aarch64, -machine virt -cpu cortex-a72EditorVS Code

Why ARM64? The host machine is Apple Silicon. Every build command requires ARCH=arm64 explicitly — most online guides assume x86_64 and will silently produce the wrong output without it.


Project Structure
mymalloc/
├── module/
│   ├── mymalloc.c          # Kernel module (all phases)
│   ├── mymalloc_ioctl.h    # Shared ioctl definitions
│   ├── Makefile            # Module build rules
│   └── test_mymalloc.c     # Userspace test program
└── .gitignore

Building and Running
Prerequisites
bash# Cross-compiler for ARM64 userspace
sudo apt install gcc-aarch64-linux-gnu

# QEMU
sudo apt install qemu-system-aarch64

# Kernel build dependencies
sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev
Build the module
bashcd ~/mymalloc/module
make

The Makefile points at a locally built Linux 6.6.30 source tree with ARCH=arm64. Adjust KDIR if your kernel source is elsewhere.

Build the userspace test
bashaarch64-linux-gnu-gcc -static \
  -o ~/mymalloc/initramfs/test_mymalloc \
  ~/mymalloc/module/test_mymalloc.c \
  -I~/mymalloc/module
Pack the initramfs and boot
bash# Copy module into initramfs tree
cp ~/mymalloc/module/mymalloc.ko ~/mymalloc/initramfs/

# Repack
cd ~/mymalloc/initramfs
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > ../initramfs.cpio.gz

# Boot the VM
cd ~/mymalloc
qemu-system-aarch64 \
  -machine virt -cpu cortex-a72 -smp 2 -m 512M \
  -kernel linux-6.6.30/arch/arm64/boot/Image \
  -initrd initramfs.cpio.gz \
  -append "console=ttyAMA0 nokaslr" \
  -nographic
Inside the VM
sh# VM prompt looks like: ~ #
insmod /mymalloc.ko
# Check dmesg for the assigned major number
mknod /dev/mymalloc c <major> 0
/test_mymalloc
cat /proc/mymalloc
rmmod mymalloc
Exit the VM: Ctrl-a then x.

Phases
Phase 0 — Sandbox ✅
Goal: Get a custom kernel running in QEMU with a minimal userspace, and load a Hello World module.
What was built:

Linux 6.6.30 compiled from source for ARM64
BusyBox 1.36.1 compiled as a fully static binary
Minimal initramfs with a working /init shell script
hello.ko — a trivial module that prints on load and unload

Key lesson: BusyBox 1.36.1 on Ubuntu 24.04 requires disabling CONFIG_TC before building (networking/tc.c incompatibility) and explicitly setting CONFIG_STATIC=y. The QEMU console is ttyAMA0 on ARM64, not ttyS0.

Phase 1 — Page Pool Allocation ✅
Goal: Grab a raw block of physical memory from the kernel and print its addresses.
What was built:

__get_free_pages(GFP_KERNEL, 10) to allocate 2¹⁰ = 1024 pages (4 MB)
page_to_phys(virt_to_page(addr)) to find the physical address
Clean deallocation with free_pages() on module unload

Verified output:
mymalloc: pool virtual addr:  0xffff000003800000
mymalloc: pool physical addr: 0x0000000043800000
mymalloc: pool size:          4194304 bytes (1024 pages)
Key lesson: The kernel gives you a virtual address. The physical address is different and requires the virt_to_page → page_to_phys conversion chain.

Phase 2 — Buddy Allocator Core ✅
Goal: Implement buddy_alloc(order) with per-order free lists and block splitting.
What was built:

struct block with list_head, order, and free fields
free_lists[NR_ORDERS] — one list_head per order (0 through 10)
block_map[] — one struct block per page in the pool
buddy_alloc(order): walks free lists upward to find a block, splits it down to the requested order, places the unused halves back onto the appropriate free lists

Verified output (split staircase):
mymalloc: alloc order 0 -> offset 0
mymalloc: free list state:
  order  1 (    8 KB each): 1 block(s)
  order  2 (   16 KB each): 1 block(s)
  ...
  order 10 ( 4096 KB each): 0 block(s)
Key lesson: Splitting is recursive. Allocating a single page from a 4 MB pool creates 10 "leftover" blocks, one at each order from 1 to 9. This is exactly where external fragmentation comes from.

Phase 3 — Buddy Coalescing ✅
Goal: Implement buddy_free() that merges adjacent free blocks back up to the maximum order.
What was built:

The XOR buddy address calculation: buddy_offset = offset ^ (1UL << order)
Iterative merge loop: check if the buddy is free and at the same order; if yes, merge and try again one order up
All three merge scenarios verified: no merge possible, partial merge, and full reassembly

Verified output (full merge):
mymalloc: free order 0 -> offset 0
mymalloc: merge order 0 -> offset 0 order 1
mymalloc: merge order 1 -> offset 0 order 2
...
mymalloc: merge order 9 -> offset 0 order 10
mymalloc: free list state:
  order 10 ( 4096 KB each): 1 block(s)
Key lesson: The XOR trick works because buddy pairs are always aligned. offset ^ (1 << order) gives the buddy address with a single instruction — the same trick used in the real Linux kernel.

Phase 4 — Character Device + ioctl ✅
Goal: Expose the allocator to userspace through a character device and ioctl interface.
What was built:

/dev/mymalloc character device registered with register_chrdev()
MYMALLOC_IOC_ALLOC and MYMALLOC_IOC_FREE ioctl commands defined in mymalloc_ioctl.h
copy_from_user / copy_to_user for safe kernel↔userspace data transfer
Static ARM64 test program (test_mymalloc.c) that opens the device, allocates two blocks at different orders, and frees them

Verified output:
opened /dev/mymalloc
alloc order 0 -> addr 0xffff000003800000
alloc order 2 -> addr 0xffff000003802000
freed order 0 at addr 0xffff000003800000
freed order 2 at addr 0xffff000003802000
all done
Key lesson: The major number assigned by register_chrdev(0, ...) changes between boots. Always check dmesg after insmod and run mknod with the current major before testing.

Phase 5 — /proc Interface ✅
Goal: Expose live allocator state through the proc filesystem.
What was built:

/proc/mymalloc created with proc_create() and proc_ops
seq_file interface with single_open / seq_printf for safe, buffered output
Per-order free count displayed for all 11 orders
Entry removed cleanly on rmmod with remove_proc_entry()

Verified output:
mymalloc free lists:
  order  0:   0 free block(s)  (    4 KB each)
  order  1:   0 free block(s)  (    8 KB each)
  ...
  order 10:   1 free block(s)  ( 4096 KB each)
Key lesson: seq_file handles the case where proc output is larger than a single page — it calls your show function multiple times if needed. Always use it instead of raw read for proc entries.

Key Gotchas

Makefile tabs — Recipe lines must start with a real TAB character, not spaces. Use printf '\t$(MAKE)...\n' to generate Makefile content from the terminal if needed.
VM vs host prompt — parallels@...$ is the host. ~ # is inside the VM. Running a host command inside the VM (or vice versa) will waste time and produce confusing errors.
Stale .ko — After editing mymalloc.c, you must: rebuild → copy .ko into the initramfs tree → repack the cpio → reboot the VM. Skipping any step means the VM runs the old module.
ARM64 everywhere — ARCH=arm64 must appear in every kernel build command. It is not the default on any x86 host.
Dynamic major number — The character device major number is assigned at runtime and can change between boots. Always read it from dmesg before running mknod.


What's Next
Phases 6–12 extend this into ML-driven research: a telemetry layer, a fragmentation prediction model, and a closed-loop hint system that proactively reshapes the allocator's free lists based on learned allocation patterns.
See the research plan document for the full roadmap.

Author
Ankith Goswami
