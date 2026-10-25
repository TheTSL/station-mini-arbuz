#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

#define NEAR_MM       100
#define MUTE_STABLE_MS 900
#define STABLE_D       12
#define STABLE_MS      400
#define MIN_SEG_MM     150
#define COOLDOWN_MS    1000

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
static void run(const char *cmd) { if (system(cmd)) {} }
#define AMIX "LD_LIBRARY_PATH=/system/vendor/quasar/libs /system/workdir/bin/amixer -c 0 "

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    fprintf(stderr, "volgest: reading %s\n", dev);

    struct input_event ev;
    int last_d = -1, turn_d = -1, slope = 0;
    long long last_t = 0, stable_since = 0, last_cmd = 0, near_since = 0;
    int mute_latched = 0;

    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_ABS || ev.code != 8) continue;
        int d = ev.value / 1000;
        if (d <= 0) continue;
        long long t = now_ms();

        if (last_d < 0) { last_d = d; turn_d = d; last_t = t; stable_since = t; continue; }

        int diff = d - last_d;
        if (abs(diff) >= 6) {
            int s = diff > 0 ? 1 : -1;
            if (slope != 0 && s != slope) turn_d = last_d;  /* разворот */
            slope = s;
            stable_since = 0;
        } else {
            if (!stable_since) stable_since = t;
        }

        /* mute: стабильно вплотную */
        if (d < NEAR_MM) {
            if (!near_since) near_since = t;
            if (!mute_latched && t - near_since > MUTE_STABLE_MS && t - last_cmd > COOLDOWN_MS) {
                run(AMIX "sset 'SPK mute' toggle >/dev/null 2>&1");
                fprintf(stderr, "MUTE toggle (d=%d)\n", d);
                last_cmd = t; mute_latched = 1; turn_d = d;
            }
        } else { near_since = 0; mute_latched = 0; }

        /* конец движения: оцениваем последний участок */
        if (stable_since && t - stable_since > STABLE_MS && t - last_cmd > COOLDOWN_MS) {
            int seg = d - turn_d;
            if (abs(seg) > MIN_SEG_MM) {
                if (seg > 0) {
                    run(AMIX "sset 'Master' 8%+ >/dev/null 2>&1");
                    fprintf(stderr, "VOL + (%d -> %d)\n", turn_d, d);
                } else {
                    run(AMIX "sset 'Master' 8%- >/dev/null 2>&1");
                    fprintf(stderr, "VOL - (%d -> %d)\n", turn_d, d);
                }
                last_cmd = t;
            }
            turn_d = d;
            stable_since = 0;
        }

        last_d = d;
        last_t = t;
    }
    return 0;
}
