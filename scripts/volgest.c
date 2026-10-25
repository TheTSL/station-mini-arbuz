#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

/* Ступенчатая громкость жестом:
 *   d < MUTE_MM            -> mute по удержанию
 *   d в [MIN_MM..MAX_MM]   -> 0..20 шагов по 5%
 *   выше MAX_MM            -> максимум
 * Каждое изменение шага сопровождается звуком: вверх/вниз.
 * Mute — отдельные звуки включения/выключения.
 */

#define MUTE_MM      100
#define MUTE_HOLD_MS 900
#define MIN_MM       150
#define MAX_MM       400
#define STEP_MM      15
#define MAX_STEP     19
#define OUT_MM       400
#define SETTLE_MS    200

#define PLAYER "/tmp/pcm_play hw:0,0 /bkupgrade/snd/"

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
static void run(const char *cmd) { if (system(cmd)) {} }
#define AMIX "LD_LIBRARY_PATH=/system/vendor/quasar/libs /system/workdir/bin/amixer -c 0 "

static void play(const char *name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), PLAYER "%s >/dev/null 2>&1", name);
    run(cmd);
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    fprintf(stderr, "volgest(step): %s\n", dev);

    struct input_event ev;
    double ema = -1;
    int cur_step = -1, cand_step = -1, muted = 0;
    int last_written = -1;
    long long cand_t = 0, near_since = 0, last_mute = 0;

    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_ABS || ev.code != 8) continue;
        int raw = ev.value / 1000;
        if (raw <= 0) continue;
        long long t = now_ms();

        ema = (ema < 0) ? raw : (ema * 0.6 + raw * 0.4);
        int d = (int)(ema + 0.5);

        if (abs(d - last_written) >= 2) {
            FILE *fp = fopen("/tmp/volgest.dist", "w");
            if (fp) { fprintf(fp, "%d", d); fclose(fp); }
            last_written = d;
        }

        if (d > OUT_MM) { cand_step = -1; continue; }

        int step;
        if (d <= MIN_MM) step = 0;
        else if (d >= MAX_MM) step = MAX_STEP;
        else step = (d - MIN_MM) / STEP_MM;

        if (cand_step != step) { cand_step = step; cand_t = t; }

        if (cand_step != cur_step && t - cand_t >= SETTLE_MS) {
            int prev = cur_step;
            cur_step = cand_step;
            int vol = 5 + cur_step * 5;
            char cmd[256];
            snprintf(cmd, sizeof(cmd), AMIX "sset 'Master' %d%% >/dev/null 2>&1", vol);
            run(cmd);
            fprintf(stderr, "step=%d vol=%d%% (d=%d)\n", cur_step, vol, d);
            if (prev >= 0) play(cur_step > prev ? "vol_up.wav" : "vol_down.wav");
        }
    }
    return 0;
}
