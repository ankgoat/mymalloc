/*
 * hint_inject.c -- Phase 11 live bridge, in-VM half.
 *
 * Runs inside the VM. Polls /share/ for files named "hint_NNNNN", each
 * containing one line of the form "ACTION ORDER" (e.g. "PRESPLIT 2").
 *
 * For each hint file found, in ascending numeric order:
 *   1. read the single line
 *   2. call ioctl(MYMALLOC_IOC_HINT) with the parsed action + order
 *   3. unlink the file so it's never processed again
 *
 * The file-per-hint protocol avoids the "have I already read this line"
 * problem that plagued the append-log design, since a file's presence
 * IS the queue entry -- once unlinked, it can't be re-fired.
 *
 * Build (on HOST):
 *   aarch64-linux-gnu-gcc -static -I module -o hint_inject tests/hint_inject.c
 *
 * Run inside VM as background:
 *   ./hint_inject &
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "mymalloc_ioctl.h"

#define HINT_DIR      "/share"
#define HINT_PREFIX   "hint_"
#define POLL_USEC     100000   /* 100ms poll interval */
#define LINE_BUF_SZ   256
#define MAX_QUEUE     64       /* max hint files processed per poll */

static const char *action_name(unsigned int a)
{
    switch (a) {
    case MYMALLOC_HINT_PRESPLIT:    return "PRESPLIT";
    case MYMALLOC_HINT_PRECOALESCE: return "PRECOALESCE";
    default:                        return "NOOP";
    }
}

static int parse_action(const char *tok)
{
    if (strcmp(tok, "PRESPLIT") == 0)    return MYMALLOC_HINT_PRESPLIT;
    if (strcmp(tok, "PRECOALESCE") == 0) return MYMALLOC_HINT_PRECOALESCE;
    return MYMALLOC_HINT_NOOP;
}

/* qsort comparator: sort by numeric suffix of "hint_NNNNN" */
static int cmp_hint_name(const void *a, const void *b)
{
    const char *na = *(const char **)a;
    const char *nb = *(const char **)b;
    /* Skip past "hint_" prefix and compare as unsigned longs */
    unsigned long ia = strtoul(na + strlen(HINT_PREFIX), NULL, 10);
    unsigned long ib = strtoul(nb + strlen(HINT_PREFIX), NULL, 10);
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

int main(void)
{
    int dev_fd;
    unsigned long sent = 0, failed = 0;

    dev_fd = open("/dev/mymalloc", O_RDWR);
    if (dev_fd < 0) {
        perror("hint_inject: open /dev/mymalloc");
        return 1;
    }

    printf("hint_inject: watching %s/%s* (file-per-hint), device fd=%d\n",
           HINT_DIR, HINT_PREFIX, dev_fd);
    fflush(stdout);

    for (;;) {
        DIR *d;
        struct dirent *ent;
        char *names[MAX_QUEUE];
        int n_names = 0;
        int i;

        d = opendir(HINT_DIR);
        if (!d) {
            usleep(POLL_USEC);
            continue;
        }

        /* Collect matching filenames first, then close the dir before
         * we start unlinking (safer across filesystems). */
        while ((ent = readdir(d)) != NULL && n_names < MAX_QUEUE) {
            if (strncmp(ent->d_name, HINT_PREFIX, strlen(HINT_PREFIX)) != 0)
                continue;
            names[n_names] = strdup(ent->d_name);
            if (!names[n_names]) break;
            n_names++;
        }
        closedir(d);

        if (n_names == 0) {
            usleep(POLL_USEC);
            continue;
        }

        /* Process in numeric order so hints apply in the order the host
         * queued them. */
        qsort(names, n_names, sizeof(char *), cmp_hint_name);

        for (i = 0; i < n_names; i++) {
            char path[512];
            char line[LINE_BUF_SZ];
            char action_str[64];
            unsigned int order;
            struct mymalloc_hint_arg h;
            FILE *f;

            snprintf(path, sizeof(path), "%s/%s", HINT_DIR, names[i]);

            f = fopen(path, "r");
            if (!f) {
                /* Vanished between readdir and open -- someone else took it,
                 * or race with host. Skip silently. */
                free(names[i]);
                continue;
            }

            if (!fgets(line, sizeof(line), f)) {
                /* Empty file -- host still writing. Leave it for next poll. */
                fclose(f);
                free(names[i]);
                continue;
            }
            fclose(f);

            if (sscanf(line, "%63s %u", action_str, &order) != 2) {
                fprintf(stderr, "hint_inject: malformed %s: %s", path, line);
                unlink(path);
                free(names[i]);
                continue;
            }

            h.action = parse_action(action_str);
            h.order  = order;

            if (ioctl(dev_fd, MYMALLOC_IOC_HINT, &h) < 0) {
                perror("hint_inject: ioctl HINT failed");
                failed++;
            } else {
                sent++;
                printf("hint_inject: sent %s order=%u from %s "
                       "(total sent=%lu failed=%lu)\n",
                       action_name(h.action), order, names[i], sent, failed);
                fflush(stdout);
            }

            /* Delete the hint file so it can't fire twice. This is the
             * whole point of the file-per-hint protocol. */
            unlink(path);
            free(names[i]);
        }

        usleep(POLL_USEC);
    }

    close(dev_fd);
    return 0;
}