/*
 * volgest.c — громкость жестом над VL53L0.
 *
 * Относительное управление (по калибровочным замерам датчика):
 *   - красный/mute: датчик сообщает не более 96 мм (нижний предел около 88 мм)
 *   - зона громкости: примерно 10..42 см
 *   - изменение на 15 см перемещения руки соответствует полному ходу громкости
 *   - гашение с задержкой 0.5 с, чтобы не мигало
 *
 * Громкость и mute задаются штатным amixer CLI. Кольцо пробуется через I2C,
 * но на этой плате устойчивое управление кольцом пока не подтверждено.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/wait.h>

#define RED_MM       96     /* ближе — красный / начало mute */
#define RED_EXIT_MM  105    /* гистерезис выхода */
#define MAX_MM       420    /* вход в рабочую зону */
#define ZONE_EXIT_MM 430    /* выход из зоны; фон в комнате начинается выше 440 мм */
#define LEVELS       40
#define FULL_TRAVEL_CM 15.0
#define STEP_DEADBAND 2
#define MUTE_HOLD_MS 700
#define SETTLE_MS    120
#define ZONE_MS      250
#define HAND_LOST_MS 1000
#define CLEAR_MS     500
#define VOL_MS       100
#define I2C_BUS      0
#define I2C_ADDR     0x3c

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int i2c_fd = -1;

static int i2c_open_dev(void)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", I2C_BUS);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE, I2C_ADDR) < 0) { close(fd); return -1; }
    return fd;
}

static int amixer_command(const char *control, const char *value)
{
    char cmd[320];
    int status;
    snprintf(cmd, sizeof(cmd),
             "LD_LIBRARY_PATH=/system/vendor/quasar/libs "
             "/system/workdir/bin/amixer -c 0 sset '%s' %s >/dev/null 2>&1",
             control, value);
    status = system(cmd);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

static int set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char value[16];
    snprintf(value, sizeof(value), "%d%%", pct);
    return amixer_command("Master", value);
}

static int read_volume(void)
{
    const char *query =
        "LD_LIBRARY_PATH=/system/vendor/quasar/libs "
        "/system/workdir/bin/amixer -c 0 sget Master 2>/dev/null";
    FILE *fp = popen(query, "r");
    if (!fp) return 0;
    char line[256];
    int pct = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Front Left:")) {
            char *start = strchr(line, '[');
            if (start) {
                char *end = NULL;
                long value = strtol(start + 1, &end, 10);
                if (end != start + 1 && *end == '%') {
                    if (value < 0) value = 0;
                    if (value > 100) value = 100;
                    pct = (int)value;
                    break;
                }
            }
        }
    }
    pclose(fp);
    return pct;
}

static double distance_mm_to_cm(int mm)
{
    /* Получено при ручной калибровке 15, 20, 25, 30 и 40 см. */
    static const int sensor_mm[] = { 93, 110, 145, 175, 320, 400 };
    static const int physical_cm[] = { 10, 15, 20, 25, 30, 40 };
    if (mm <= sensor_mm[0]) return physical_cm[0];
    if (mm >= sensor_mm[5]) return physical_cm[5];

    for (size_t i = 1; i < sizeof(sensor_mm) / sizeof(sensor_mm[0]); i++) {
        if (mm <= sensor_mm[i]) {
            double fraction = (double)(mm - sensor_mm[i - 1]) /
                              (sensor_mm[i] - sensor_mm[i - 1]);
            return physical_cm[i - 1] +
                   fraction * (physical_cm[i] - physical_cm[i - 1]);
        }
    }
    return physical_cm[5];
}

static void toggle_mute(void)
{
    const char *query =
        "LD_LIBRARY_PATH=/system/vendor/quasar/libs "
        "/system/workdir/bin/amixer -c 0 sget 'SPK mute' 2>/dev/null";
    FILE *fp = popen(query, "r");
    if (!fp) return;
    char line[256], value[16] = "";
    int is_muted = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "[on]")) is_muted = 1;
        else if (strstr(line, "[off]")) is_muted = 0;
    }
    int status = pclose(fp);
    if (is_muted < 0 || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return;
    snprintf(value, sizeof(value), "%s", is_muted ? "off" : "on");
    (void)amixer_command("SPK mute", value);
}

static void led_bar(int n, int r, int g, int b)
{
    if (i2c_fd < 0) return;
    if (n < 0) n = 0;
    if (n > 6) n = 6;

    unsigned char buf[19];
    buf[0] = 0x01;
    for (int i = 0; i < 6; i++) {
        int on = (i < n);
        buf[1 + i*3 + 0] = on ? (unsigned char)b : 0;
        buf[1 + i*3 + 1] = on ? (unsigned char)g : 0;
        buf[1 + i*3 + 2] = on ? (unsigned char)r : 0;
    }
    if (write(i2c_fd, buf, 19) != 19) return;
    unsigned char commit[2] = { 0x25, 0xAA };
    if (write(i2c_fd, commit, 2) != 2) return;
}

int main(int argc, char **argv)
{
    const int calibration_mode = argc > 1 && !strcmp(argv[1], "--calibrate");
    const char *dev = (!calibration_mode && argc > 1) ? argv[1] : "/dev/input/event0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    if (!calibration_mode)
        i2c_fd = i2c_open_dev();
    struct input_event ev;
    double ema = -1;
    int last_written = -1;
    int current_pct = calibration_mode ? 0 : read_volume();
    int cur_step = (current_pct * LEVELS + 50) / 100;
    int cand_step = -1, anchor_step = cur_step, anchor_valid = 0;
    double anchor_cm = 0;
    int in_zone = 0, red = 0, mute_latched = 0;
    int pending_vol = -1, applied_vol = calibration_mode ? -1 : current_pct;
    int bar_n = -1, shown = 0;
    long long cand_t = 0, near_since = 0, last_mute = 0, last_apply = 0;
    long long zone_since = 0, hand_lost_since = 0;
    long long last_near = 0, last_report = 0;

    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_ABS || ev.code != 8) continue;

        int raw = ev.value / 1000;
        if (raw <= 0) continue;
        long long t = now_ms();

        /* Stronger smoothing keeps sensor quantization from looking like motion. */
        ema = (ema < 0) ? raw : (ema * 0.8 + raw * 0.2);
        int d = (int)(ema + 0.5);

        if (last_written < 0 || abs(d - last_written) >= 2) {
            FILE *fp = fopen("/tmp/volgest.dist", "w");
            if (fp) {
                fprintf(fp, "%d", d);
                fclose(fp);
            }
            last_written = d;
        }

        if (calibration_mode) {
            if (t - last_report >= 100) {
                fprintf(stderr, "%lld raw=%d filtered=%d\n", t, raw, d);
                last_report = t;
            }
            continue;
        }

        /* красный: рука у колонки, с гистерезисом */
        if (!red) {
            if (d <= RED_MM) red = 1;
        } else if (d > RED_EXIT_MM) {
            red = 0;
        }

        if (red) {
            if (bar_n != 6 || !shown) { led_bar(6, 255, 0, 0); bar_n = 6; shown = 1; }
            last_near = t;
            if (!near_since) near_since = t;
            if (!mute_latched && t - near_since > MUTE_HOLD_MS && t - last_mute > 1500) {
                toggle_mute();
                last_mute = t;
                mute_latched = 1;
            }
        } else {
            near_since = 0;
            mute_latched = 0;
        }

        if (!in_zone) {
            if (d <= MAX_MM) {
                if (!zone_since) zone_since = t;
                if (t - zone_since >= ZONE_MS) {
                    in_zone = 1;
                    anchor_valid = 1;
                    hand_lost_since = 0;
                    anchor_cm = distance_mm_to_cm(d);
                    anchor_step = cur_step;
                    cand_step = cur_step;
                    cand_t = t;
                }
            } else {
                zone_since = 0;
                anchor_valid = 0;
                cand_step = -1;
            }
        } else if (d > ZONE_EXIT_MM) {
            if (!hand_lost_since) {
                hand_lost_since = t;
                fprintf(stderr, "hand_lost: holding control for %d ms\n", HAND_LOST_MS);
            } else if (t - hand_lost_since >= HAND_LOST_MS) {
                in_zone = 0;
                anchor_valid = 0;
                zone_since = 0;
                cand_step = -1;
                hand_lost_since = 0;
                fprintf(stderr, "hand_timeout: control ended\n");
            }
        } else if (hand_lost_since) {
            /* Resume without a jump: make the returning hand the new anchor. */
            hand_lost_since = 0;
            anchor_cm = distance_mm_to_cm(d);
            anchor_step = cur_step;
            cand_step = cur_step;
            cand_t = t;
            fprintf(stderr, "hand_returned: control resumed\n");
        }

        if (in_zone && anchor_valid && !hand_lost_since && d <= ZONE_EXIT_MM) {
            double travel_cm = distance_mm_to_cm(d) - anchor_cm;
            double target_step = anchor_step + travel_cm * LEVELS / FULL_TRAVEL_CM;
            int step = target_step >= 0 ? (int)(target_step + 0.5) :
                                          (int)(target_step - 0.5);
            if (step < 0) step = 0;
            if (step > LEVELS) step = LEVELS;

            /* Ignore sub-centimeter sensor jitter around the current level. */
            if (abs(step - cur_step) < STEP_DEADBAND)
                step = cur_step;

            if (step != cand_step) { cand_step = step; cand_t = t; }

            if (cand_step != cur_step && t - cand_t >= SETTLE_MS) {
                cur_step = cand_step;
                pending_vol = (cur_step * 100 + LEVELS / 2) / LEVELS;
                int leds = (cur_step * 6 + LEVELS / 2) / LEVELS;
                if (!red && (leds != bar_n || !shown)) {
                    led_bar(leds, 0, 160, 255);
                    bar_n = leds;
                    shown = 1;
                }
            }
            last_near = t;
        }

        /* гасим с задержкой, чтобы не мигало */
        if (shown && !red && !in_zone && t - last_near > CLEAR_MS) {
            led_bar(0, 0, 0, 0);
            bar_n = 0;
            shown = 0;
        }

        if (pending_vol >= 0 && pending_vol != applied_vol && t - last_apply >= VOL_MS) {
            if (set_volume(pending_vol) == 0) {
                applied_vol = pending_vol;
                fprintf(stderr, "volume=%d%%\n", applied_vol);
            } else {
                fprintf(stderr, "volgest: amixer failed for %d%%\n", pending_vol);
            }
            last_apply = t;
        }
    }
    return 0;
}
