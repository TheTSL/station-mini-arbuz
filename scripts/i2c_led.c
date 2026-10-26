/*
 * i2c_led.c — прямая работа с LED-кольцом (IS31FL3236A, i2c-0, 0x3c).
 *
 * Использование:
 *   i2c_led <bus> <addr> <cmd> [значение]
 *
 * cmd:
 *   is31      — включить драйвер и заполнить PWM 0x01..0x24
 *   pwm <v>   — установить все PWM в v и обновить
 *   off       — все PWM в 0 и обновить, не выключая драйвер через 0x00
 *   reg <r> <v> — записать один регистр
 *
 * Сборка:
 *   aarch64-linux-gnu-gcc -O2 -static -o i2c_led i2c_led.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define CHANNELS 36
#define MAX_PWM 153

static int i2c_open(int bus, int addr)
{
    char path[32];
    int fd;
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    fd = open(path, O_RDWR);
    if (fd < 0) { perror("open i2c"); return -1; }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { perror("I2C_SLAVE"); close(fd); return -1; }
    return fd;
}

static int write_regs(int fd, unsigned char start, const unsigned char *vals, int n)
{
    unsigned char buf[64];
    if (n + 1 > (int)sizeof(buf)) return -1;
    buf[0] = start;
    memcpy(buf + 1, vals, n);
    if (write(fd, buf, n + 1) != n + 1) { perror("write"); return -1; }
    return 0;
}

static int write_reg(int fd, unsigned char reg, unsigned char val)
{
    unsigned char b[2] = { reg, val };
    if (write(fd, b, 2) != 2) { perror("write reg"); return -1; }
    return 0;
}

static unsigned char safe_pwm(int value)
{
    if (value < 0) value = 0;
    if (value > MAX_PWM) value = MAX_PWM;
    return (unsigned char)value;
}

static int write_frame(int fd, const unsigned char pwm[CHANNELS])
{
    unsigned char control[CHANNELS];
    memset(control, 0x07, sizeof(control)); /* outputs on, quarter current */
    if (write_reg(fd, 0x00, 0x01) < 0 ||
        write_regs(fd, 0x01, pwm, CHANNELS) < 0 ||
        write_regs(fd, 0x26, control, CHANNELS) < 0 ||
        write_reg(fd, 0x4a, 0x00) < 0 ||
        write_reg(fd, 0x25, 0x00) < 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bus> <addr> <is31|pwm|off|reg> [value]\n", argv[0]);
        return 1;
    }
    int bus = atoi(argv[1]);
    int addr = (int)strtol(argv[2], NULL, 0);
    const char *cmd = argv[3];
    int fd = i2c_open(bus, addr);
    if (fd < 0) return 2;

    if (!strcmp(cmd, "is31") || !strcmp(cmd, "pwm")) {
        unsigned char pwm[CHANNELS];
        unsigned char value = safe_pwm(argc > 4 ? atoi(argv[4]) : 64);
        memset(pwm, value, sizeof(pwm));
        if (write_frame(fd, pwm) != 0) { close(fd); return 3; }
        printf("pwm=%u\n", value);
    } else if (!strcmp(cmd, "mask")) {
        /* mask <start> <end> <val> — только каналы start..end */
        if (argc < 7) { fprintf(stderr, "mask <start> <end> <val>\n"); close(fd); return 1; }
        int s0 = atoi(argv[4]), e0 = atoi(argv[5]), val = atoi(argv[6]);
        if (s0 < 0 || s0 > e0 || e0 >= CHANNELS) { close(fd); return 1; }
        unsigned char pwm[CHANNELS];
        memset(pwm, 0, sizeof(pwm));
        for (int i = s0; i <= e0; i++) pwm[i] = safe_pwm(val);
        if (write_frame(fd, pwm) != 0) { close(fd); return 3; }
        printf("mask %d..%d = %u\n", s0, e0, safe_pwm(val));
    } else if (!strcmp(cmd, "color")) {
        /* color <r> <g> <b> — одинаковый цвет на всех светодиодах */
        if (argc < 7) { fprintf(stderr, "color <r> <g> <b>\n"); close(fd); return 1; }
        int r = atoi(argv[4]), g = atoi(argv[5]), b = atoi(argv[6]);
        unsigned char pwm[CHANNELS];
        for (int i = 0; i < CHANNELS / 3; i++) {
            pwm[i*3+0] = safe_pwm(r);
            pwm[i*3+1] = safe_pwm(g);
            pwm[i*3+2] = safe_pwm(b);
        }
        if (write_frame(fd, pwm) != 0) { close(fd); return 3; }
        printf("color r=%u g=%u b=%u\n", safe_pwm(r), safe_pwm(g), safe_pwm(b));
    } else if (!strcmp(cmd, "off")) {
        unsigned char pwm[CHANNELS];
        memset(pwm, 0, sizeof(pwm));
        if (write_frame(fd, pwm) != 0) { close(fd); return 3; }
        printf("off\n");
    } else if (!strcmp(cmd, "reg")) {
        if (argc < 6) { fprintf(stderr, "reg <r> <v>\n"); close(fd); return 1; }
        int r = (int)strtol(argv[4], NULL, 0);
        int v = (int)strtol(argv[5], NULL, 0);
        if (write_reg(fd, (unsigned char)r, (unsigned char)v) == 0)
            printf("reg 0x%02x = 0x%02x\n", r, v);
    } else {
        fprintf(stderr, "unknown cmd\n");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
