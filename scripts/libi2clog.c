/*
 * libi2clog.c — LD_PRELOAD-шим: логирует обмен с I2C-устройствами.
 *
 * Помечает fd, для которого вызывали ioctl(I2C_SLAVE), и логирует:
 *   - write() в такой fd (последовательность байт);
 *   - ioctl(I2C_RDWR).
 *
 * Лог: /data/local/tmp/i2c.log
 *   W fd=5 addr=0x3c len=4: 00 12 34 56
 *   R addr=0x3c len=1
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MAXFD 1024
static int i2c_fd[MAXFD];
static int i2c_addr[MAXFD];

static void log_line(const char *fmt, ...)
{
    va_list ap;
    FILE *f = fopen("/data/local/tmp/i2c.log", "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (fd >= 0 && fd < MAXFD && i2c_fd[fd]) {
        char hex[3 * 64 + 4];
        size_t n = count < 64 ? count : 64;
        size_t p = 0;
        for (size_t i = 0; i < n; i++)
            p += snprintf(hex + p, sizeof(hex) - p, "%02x", ((const unsigned char *)buf)[i]);
        log_line("W fd=%d addr=0x%02x len=%u:%s", fd, i2c_addr[fd], (unsigned)count, hex);
    }
    return (ssize_t)syscall(SYS_write, fd, buf, count);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg = NULL;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (fd >= 0 && fd < MAXFD) {
        if (req == I2C_SLAVE || req == I2C_SLAVE_FORCE) {
            i2c_fd[fd] = 1;
            i2c_addr[fd] = (int)(long)arg;
            log_line("S fd=%d addr=0x%02x", fd, i2c_addr[fd]);
        } else if (req == I2C_RDWR && arg) {
            struct i2c_rdwr_ioctl_data *d = arg;
            for (unsigned int i = 0; i < d->nmsgs; i++) {
                struct i2c_msg *m = &d->msgs[i];
                char hex[3 * 64 + 4];
                size_t p = 0;
                if (!(m->flags & I2C_M_RD)) {
                    size_t n = m->len < 64 ? m->len : 64;
                    for (size_t j = 0; j < n; j++)
                        p += snprintf(hex + p, sizeof(hex) - p, "%02x", m->buf[j]);
                    log_line("RW addr=0x%02x w len=%u:%s", m->addr, m->len, hex);
                } else {
                    log_line("RW addr=0x%02x r len=%u", m->addr, m->len);
                }
            }
        }
    }

    return (int)syscall(SYS_ioctl, fd, req, arg);
}
