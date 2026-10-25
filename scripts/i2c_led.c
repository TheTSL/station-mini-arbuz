/*
 * i2c_led.c — прямая работа с LED-кольцом (TI SN3236, i2c-0, 0x3c).
 *
 * Использование:
 *   i2c_led <bus> <addr> <cmd> [значение]
 *
 * cmd:
 *   is31      — попытка инициализации по карте IS31FL3236:
 *               PWM 0x00..0x23 = value (по умолчанию 0xFF),
 *               0x24 (global current) = 0x7F, 0x25 (global control) = 0x01,
 *               0x27 (update) = 0x00
 *   pwm <v>   — установить все PWM в v и обновить
 *   off       — все PWM в 0 и обновить
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

    if (!strcmp(cmd, "is31")) {
        unsigned char pwm[36];
        int v = (argc > 4) ? atoi(argv[4]) : 0xFF;
        memset(pwm, v, sizeof(pwm));
        write_regs(fd, 0x00, pwm, 36);
        write_reg(fd, 0x24, 0x7F);
        write_reg(fd, 0x25, 0x01);
        write_reg(fd, 0x26, 0x00);
        write_reg(fd, 0x27, 0x00);
        printf("is31 init done (pwm=%d)\n", v);
    } else if (!strcmp(cmd, "pwm")) {
        unsigned char pwm[36];
        int v = (argc > 4) ? atoi(argv[4]) : 0xFF;
        memset(pwm, v, sizeof(pwm));
        write_regs(fd, 0x00, pwm, 36);
        write_reg(fd, 0x27, 0x00);
        printf("pwm=%d\n", v);
    } else if (!strcmp(cmd, "color")) {
        /* color <r> <g> <b> [current] — одинаковый цвет на всех светодиодах */
        if (argc < 7) { fprintf(stderr, "color <r> <g> <b> [cur]\n"); close(fd); return 1; }
        int r = atoi(argv[4]), g = atoi(argv[5]), b = atoi(argv[6]);
        int cur = (argc > 7) ? atoi(argv[7]) : 0x08;
        unsigned char pwm[36];
        for (int i = 0; i < 12; i++) {
            pwm[i*3+0] = (unsigned char)r;
            pwm[i*3+1] = (unsigned char)g;
            pwm[i*3+2] = (unsigned char)b;
        }
        write_reg(fd, 0x24, (unsigned char)cur);
        write_regs(fd, 0x00, pwm, 36);
        write_reg(fd, 0x27, 0x00);
        printf("color r=%d g=%d b=%d cur=%d\n", r, g, b, cur);
    } else if (!strcmp(cmd, "off")) {
        unsigned char pwm[36];
        memset(pwm, 0, sizeof(pwm));
        write_regs(fd, 0x00, pwm, 36);
        write_reg(fd, 0x27, 0x00);
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
