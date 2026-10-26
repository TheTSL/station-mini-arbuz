/*
 * i2cdump_ptrace.c — снимает I2C_RDWR-обмены процесса через ptrace.
 *
 *   i2cdump_ptrace <pid> [секунды]
 *
 * Пишет в /data/local/tmp/i2cframes.log:
 *   T=123 addr=0x3c w len=4: 00 12 34 56
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/uio.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MAXT 512
#define I2C_RDWR_CMD 0x0707
#define NR_IOCTL 54          /* ARM EABI */
#define NT_PRSTATUS 1
struct arm_regs { unsigned long uregs[18]; };

static volatile sig_atomic_t timed_out = 0;
static void on_alarm(int s) { (void)s; timed_out = 1; }

struct tstate { int tid; int entry; int active; };
static struct tstate ts[MAXT];
static int nts = 0;

static struct tstate *find(int tid)
{
    for (int i = 0; i < nts; i++)
        if (ts[i].tid == tid) return &ts[i];
    if (nts < MAXT) {
        ts[nts].tid = tid; ts[nts].entry = 1; ts[nts].active = 1;
        return &ts[nts++];
    }
    return NULL;
}

static int mem_read(int pid, unsigned long addr, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, addr + done, NULL);
        if (w == -1 && errno) return -1;
        size_t n = sizeof(long);
        if (n > len - done) n = len - done;
        memcpy((char *)buf + done, &w, n);
        done += n;
    }
    return 0;
}

static void log_line(const char *s)
{
    FILE *f = fopen("/data/local/tmp/i2cframes.log", "a");
    if (!f) return;
    fputs(s, f);
    fclose(f);
}

static void dump_rdwr(int pid, unsigned long arg)
{
    struct i2c_rdwr_ioctl_data d;
    if (mem_read(pid, arg, &d, sizeof(d)) < 0) return;

    for (unsigned int i = 0; i < d.nmsgs && i < 8; i++) {
        struct i2c_msg m;
        if (mem_read(pid, (unsigned long)d.msgs + i * sizeof(struct i2c_msg),
                     &m, sizeof(m)) < 0)
            return;
        char line[512];
        int n = snprintf(line, sizeof(line), "T=%d addr=0x%02x %c len=%u:",
                         pid, m.addr, (m.flags & I2C_M_RD) ? 'r' : 'w', m.len);
        if (!(m.flags & I2C_M_RD) && m.buf) {
            unsigned int ln = m.len < 64 ? m.len : 64;
            unsigned char data[64];
            if (mem_read(pid, (unsigned long)m.buf, data, ln) == 0) {
                for (unsigned int j = 0; j < ln && n < (int)sizeof(line) - 4; j++)
                    n += snprintf(line + n, sizeof(line) - n, " %02x", data[j]);
            }
        }
        snprintf(line + n, sizeof(line) - n, "\n");
        log_line(line);
    }
}

static void attach_tid(int tid)
{
    if (ptrace(PTRACE_ATTACH, tid, 0, 0) < 0) { fprintf(stderr, "attach %d fail: %s\n", tid, strerror(errno)); return; }
    fprintf(stderr, "attached %d\n", tid);
    waitpid(tid, NULL, __WALL);
    ptrace(PTRACE_SETOPTIONS, tid, 0, PTRACE_O_TRACESYSGOOD);
    struct tstate *t = find(tid);
    if (t) { t->entry = 1; t->active = 1; }
    ptrace(PTRACE_SYSCALL, tid, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <pid> [secs]\n", argv[0]); return 1; }
    int pid = atoi(argv[1]);
    int secs = (argc > 2) ? atoi(argv[2]) : 30;

    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/task", pid);
    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        attach_tid(atoi(e->d_name));
    }
    closedir(d);

    signal(SIGALRM, on_alarm);
    alarm((unsigned)secs);
    while (!timed_out) {
        int status;
        int tid = waitpid(-1, &status, __WALL);
        if (tid <= 0) { if (errno == EINTR) continue; break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            struct tstate *t = find(tid);
            if (t) t->active = 0;
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        fprintf(stderr, "stop tid=%d status=0x%x\n", tid, status);
        if ((status >> 8) != (SIGTRAP | 0x80)) {   /* не syscall-стоп */
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
            continue;
        }
        struct tstate *t = find(tid);
        if (!t) { ptrace(PTRACE_SYSCALL, tid, 0, 0); continue; }

        if (t->entry) {
            struct arm_regs regs;
            struct iovec iov = { &regs, sizeof(regs) };
            if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &iov) == 0) {
                unsigned long nr = regs.uregs[7];
                if (nr == NR_IOCTL) {
                    fprintf(stderr, "ioctl fd=%lu req=0x%lx\n", regs.uregs[0], regs.uregs[1]);
                    unsigned long fd = regs.uregs[0];
                    unsigned long req = regs.uregs[1];
                    unsigned long arg = regs.uregs[2];
                    (void)fd;
                    if (req == I2C_RDWR_CMD && arg)
                        dump_rdwr(tid, arg);
                }
            }
        }
        t->entry = !t->entry;
        ptrace(PTRACE_SYSCALL, tid, 0, 0);
    }

    for (int i = 0; i < nts; i++)
        if (ts[i].active) ptrace(PTRACE_DETACH, ts[i].tid, 0, 0);
    return 0;
}
