# mymalloc

A Linux kernel module that implements memory allocation from scratch, then learns to manage its own fragmentation using a neural network.

## What this project is

mymalloc started as a hand built memory allocator that runs inside the Linux kernel. The kernel manages physical memory with something called a buddy allocator: it keeps free memory in power of two sized blocks, splits big blocks when it needs small ones, and merges neighbors back together when memory is freed. I rebuilt that whole system myself as a loadable kernel module so I could see and control every decision it makes.

That was the foundation. Everything after it is about making the allocator faster, more observable, and eventually able to tune itself.

## The foundation (Phases 0 to 5)

The first half of the project was the core allocator. I built it up one layer at a time, starting from raw page management and ending with a full interface that user programs can talk to.

By the end of Phase 5 the module could hand out and free memory in proper buddy orders, split large blocks and merge free neighbors using the same XOR based buddy math the real kernel uses, expose its internal state through /proc so I could watch the free lists live, and take requests from user space through a device file.

This part taught me how the kernel actually thinks about memory, and how careful you have to be when your code runs with no safety net underneath it.

## Building on it (Phases 6 to 11)

Once the allocator worked, I spent the second half of the project turning it into something that can measure itself, predict trouble, and act on those predictions in real time. Each phase built directly on the one before it.

**Phase 6, a faster allocator with a slab layer.** Real allocators do not go back to the buddy system for every tiny request, so I added a slab cache for small fixed size objects, plus a lock so the whole thing stays correct when more than one thing touches it at once.

**Phase 7, proving it holds up under pressure.** I wrote a stress test that hammers the allocator from several threads at the same time, to make sure the locking was right and nothing broke under load.

**Phase 8, giving it eyes.** I added telemetry. Every allocation and free now records what happened and what memory looked like at that moment. That trace streams out as simple data that tools on the outside can read and follow along with.

**Phase 9, one number for health.** I turned all that raw state into a single fragmentation score from 0 to 100, where 0 means memory is in great shape and 100 means it is badly broken up. Now the allocator could describe its own condition at a glance.

**Phase 10, teaching it to predict.** I trained a small neural network on the allocator's own history so it could look at recent activity and predict where fragmentation was heading. It lands within a few percent of the real value.

**Phase 11, closing the loop.** This is the part that ties everything together. The model's predictions now travel from the outside world into the running kernel and change how it manages memory before fragmentation gets bad. When the model sees trouble coming, it tells the allocator to split or merge blocks ahead of time, and the kernel does it on the spot. I proved the whole path end to end: a hint sent from the host reaches the kernel, triggers exactly one correct action, and is consumed so it can never fire twice.

## How it all fits together

The finished system is a full feedback loop. The allocator reports what it is doing, an outside collector gathers that, the model predicts what comes next, and its advice flows back into the kernel to keep memory healthy. The allocator watches itself, thinks ahead, and adjusts, all while running live.

## Environment

- Host: Ubuntu 24.04 on Apple Silicon, through Parallels
- Architecture: ARM64
- Kernel: Linux 6.6.30, built from source
- Userspace: BusyBox
- Runs inside a QEMU virtual machine

Because the host is Apple Silicon, everything is built for ARM64, which meant being careful with cross compilation the whole way through.

## What I learned

How the kernel really manages memory underneath everything, not just in theory but by building it.

How to write code that runs in kernel space, where mistakes are unforgiving and every lock matters.

How to make a system observable, so it can tell you what it is doing instead of leaving you to guess.

How to connect machine learning to a live system in a way that actually changes its behavior, not just reports on it.

And most of all, how to take something that already works and keep building on it, layer after layer, without breaking what came before.
