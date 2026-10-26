/*
 * volgest.c — громкость жестом над VL53L0.
 *
 * Относительное управление (по калибровочным замерам датчика):
 *   - зона обнаружения руки: до 42 см по индивидуальной калибровке устройства
 *   - изменение на 15 см перемещения руки соответствует полному ходу громкости
 *   - гашение с задержкой 0.5 с, чтобы не мигало
 *
 * Громкость задаётся штатным amixer CLI. Кольцо пробуется через I2C,
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

#define MAX_MM       420    /* уверенно обнаруживаемая ближняя зона */
#define MAX_HAND_CM  60.0   /* заявленная дальность ToF для жестов */
#define FAR_HAND_ENTER_MM 50
#define FAR_HAND_RELEASE_MM 30
#define AMBIENT_ALPHA 0.01
#define LEVELS       40
#define FULL_TRAVEL_CM 15.0
#define MIN_LIMIT_RELEASE_CM 3.0
#define MAX_LIMIT_RELEASE_CM 1.5
#define STEP_DEADBAND 2
#define HOVER_STABLE_MS 250
#define HOVER_SPAN_MM 12
#define MOVE_START_CM 0.8
#define SETTLE_MS    25
#define ZONE_MS      150
#define HAND_LOST_MS 1000
#define CLEAR_MS     500
#define VOL_MS       40
#define VOL_SLEW_PCT 8
#define FEEDBACK_STEP_PCT 10
#define FEEDBACK_COOLDOWN_MS 250
#define LED_SEGMENTS 10
#define LED_REFRESH_MS 200
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

static void write_le16(FILE *fp, unsigned int value)
{
    fputc((int)(value & 0xff), fp);
    fputc((int)((value >> 8) & 0xff), fp);
}

static void write_le32(FILE *fp, unsigned int value)
{
    write_le16(fp, value & 0xffff);
    write_le16(fp, value >> 16);
}

static void prepare_volume_feedback(void)
{
    const unsigned int rate = 48000, frames = 1536;
    FILE *fp = fopen("/tmp/volgest-step.wav", "wb");
    if (!fp) return;

    fwrite("RIFF", 1, 4, fp); write_le32(fp, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, fp); write_le32(fp, 16);
    write_le16(fp, 1); write_le16(fp, 2); write_le32(fp, rate);
    write_le32(fp, rate * 4); write_le16(fp, 4); write_le16(fp, 16);
    fwrite("data", 1, 4, fp); write_le32(fp, frames * 4);

    for (unsigned int i = 0; i < frames; i++) {
        double envelope = 1.0;
        if (i < 96) envelope = (double)i / 96.0;
        else if (i >= frames - 96) envelope = (double)(frames - i) / 96.0;
        unsigned int phase = i % 48;
        int wave;
        if (phase < 12) wave = (int)phase * 117;
        else if (phase < 24) wave = 1400 - (int)(phase - 12) * 117;
        else if (phase < 36) wave = -(int)(phase - 24) * 117;
        else wave = -1400 + (int)(phase - 36) * 117;
        int sample = (int)(envelope * wave);
        write_le16(fp, (unsigned int)(unsigned short)(short)sample);
        write_le16(fp, (unsigned int)(unsigned short)(short)sample);
    }
    fclose(fp);
}

static void play_volume_feedback(void)
{
    int status = system("/tmp/pcm_play hw:0,0 /tmp/volgest-step.wav "
                        ">/dev/null 2>&1 &");
    (void)status;
}

static void set_hand_state(int state)
{
    static int previous = -1;
    static const char *const labels[] = { "absent", "detecting", "present", "lost" };
    if (state < 0 || state > 3 || state == previous) return;

    FILE *fp = fopen("/tmp/volgest.hand", "w");
    if (fp) {
        fprintf(fp, "%s\n", labels[state]);
        fclose(fp);
    }
    fprintf(stderr, "hand=%s\n", labels[state]);
    previous = state;
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
    if (mm < sensor_mm[0]) {
        /* Экстраполяция сохраняет чувствительность у ближнего предела сенсора. */
        return physical_cm[0] + (mm - sensor_mm[0]) *
               (double)(physical_cm[1] - physical_cm[0]) /
               (sensor_mm[1] - sensor_mm[0]);
    }
    if (mm > sensor_mm[5]) {
        /* Продлеваем масштаб за последнюю калибровочную точку (40 см). */
        return physical_cm[5] + (mm - sensor_mm[5]) *
               (double)(physical_cm[5] - physical_cm[4]) /
               (sensor_mm[5] - sensor_mm[4]);
    }

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

static void led_bar(int n, int r, int g, int b, int hand_marker)
{
    if (i2c_fd < 0) return;
    if (n < 0) n = 0;
    if (n > LED_SEGMENTS) n = LED_SEGMENTS;

    unsigned char buf[1 + LED_SEGMENTS * 3];
    buf[0] = 0x01;
    for (int i = 0; i < LED_SEGMENTS; i++) {
        int on = (i < n);
        int pixel_r = r, pixel_g = g, pixel_b = b;
        if (hand_marker && i == LED_SEGMENTS / 2) {
            on = 1;
            pixel_r = 153;
            pixel_g = pixel_b = 0;
        }
        buf[1 + i*3 + 0] = on ? (unsigned char)pixel_b : 0;
        buf[1 + i*3 + 1] = on ? (unsigned char)pixel_g : 0;
        buf[1 + i*3 + 2] = on ? (unsigned char)pixel_r : 0;
    }
    if (write(i2c_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) return;
    unsigned char commit[2] = { 0x25, 0xAA };
    if (write(i2c_fd, commit, 2) != 2) return;
}

static void led_volume(int pct, int hand_marker)
{
    static const int rainbow[11][3] = {
        {  90,   0, 153 }, /* violet */
        {  45,   0, 153 }, /* indigo */
        {   0,   0, 153 }, /* blue */
        {   0,  77, 153 }, /* azure */
        {   0, 153, 153 }, /* cyan */
        {   0, 153,   0 }, /* green */
        {  77, 153,   0 }, /* yellow-green */
        { 153, 153,   0 }, /* yellow */
        { 153,  76,   0 }, /* orange */
        { 153,  38,   0 }, /* vermilion */
        { 153,   0,   0 }  /* red */
    };
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int decile = pct / 10;
    int n = (pct + 9) / 10;
    if (n < 1) n = 1;  /* keep a visible violet marker at 0% while a hand is present */
    led_bar(n, rainbow[decile][0], rainbow[decile][1], rainbow[decile][2],
            hand_marker);
}

int main(int argc, char **argv)
{
    const int calibration_mode = argc > 1 && !strcmp(argv[1], "--calibrate");
    const char *dev = (!calibration_mode && argc > 1) ? argv[1] : "/dev/input/event0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    if (!calibration_mode)
        i2c_fd = i2c_open_dev();
    if (!calibration_mode)
        prepare_volume_feedback();
    struct input_event ev;
    double ema = -1;
    int last_written = -1;
    int current_pct = calibration_mode ? 0 : read_volume();
    int cur_step = (current_pct * LEVELS + 50) / 100;
    int cand_step = -1, anchor_step = cur_step;
    double anchor_cm = 0;
    int in_zone = 0, armed = 0, min_latched = 0, max_latched = 0;
    int pending_vol = -1, applied_vol = calibration_mode ? -1 : current_pct;
    int shown = 0, led_bucket = -1;
    long long cand_t = 0, last_apply = 0;
    long long zone_since = 0, hover_since = 0, hand_lost_since = 0;
    long long last_feedback = 0, last_control_report = 0;
    long long last_led_refresh = 0;
    int hover_min = 0, hover_max = 0;
    double ambient_mm = -1;
    long long last_near = 0, last_report = 0;

    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_ABS || ev.code != 8) continue;

        int raw = ev.value / 1000;
        if (raw <= 0) continue;
        long long t = now_ms();

        if (ambient_mm < 0) ambient_mm = raw;
        int raw_background = raw >= ambient_mm - FAR_HAND_RELEASE_MM;

        /* Do not blend a background return into a live hand track: near the
         * sensor, the hand can briefly disappear and the raw range jumps to
         * the room. Blending that jump made the ghost distance look like a
         * fast outward hand movement and drove volume to 100%. */
        if (!(in_zone && raw_background))
            ema = (ema < 0) ? raw : (ema * 0.8 + raw * 0.2);
        int d = (int)(ema + 0.5);

        /* A hand must be closer than the slowly tracked room background at
         * every distance. The old unconditional near-zone shortcut accepted
         * brief ToF reflections as a real hand and could arm on empty space. */
        int within_hand_limit = distance_mm_to_cm(d) <= MAX_HAND_CM;
        int far_hand_enter = d < ambient_mm - FAR_HAND_ENTER_MM;
        int far_hand_keep = raw < ambient_mm - FAR_HAND_RELEASE_MM;
        if (!in_zone && d > MAX_MM && !far_hand_enter)
            ambient_mm += AMBIENT_ALPHA * (d - ambient_mm);
        int hand_enter = within_hand_limit && far_hand_enter;
        int hand_keep = within_hand_limit && far_hand_keep;
        int hand_seen = hand_enter && hand_keep;

        if (last_written < 0 || abs(d - last_written) >= 2) {
            FILE *fp = fopen("/tmp/volgest.dist", "w");
            if (fp) {
                fprintf(fp, "%d", d);
                fclose(fp);
            }
            last_written = d;
        }

        if (calibration_mode) {
            set_hand_state(hand_enter ? 2 : 0);
            if (t - last_report >= 100) {
                fprintf(stderr, "%lld raw=%d filtered=%d\n", t, raw, d);
                last_report = t;
            }
            continue;
        }

        if (!in_zone) {
            if (hand_enter) {
                set_hand_state(1);
                if (!zone_since) zone_since = t;
                if (t - zone_since >= ZONE_MS) {
                    in_zone = 1;
                    armed = 0;
                    hand_lost_since = 0;
                    hover_since = 0;
                    hover_min = hover_max = d;
                    cand_step = cur_step;
                    cand_t = t;
                    fprintf(stderr, "hand_confirmed: waiting for steady hover\n");
                }
            } else {
                zone_since = 0;
                hover_since = 0;
                cand_step = -1;
                set_hand_state(0);
            }
        } else if (!hand_keep) {
            if (!hand_lost_since) {
                hand_lost_since = t;
                set_hand_state(3);
                /* Freeze the level during a brief sensor dropout. */
                pending_vol = applied_vol;
                cur_step = (applied_vol * LEVELS + 50) / 100;
                armed = 0;
                hover_since = 0;
                fprintf(stderr, "hand_lost: holding control for %d ms\n", HAND_LOST_MS);
            } else if (t - hand_lost_since >= HAND_LOST_MS) {
                in_zone = 0;
                armed = 0;
                zone_since = 0;
                hover_since = 0;
                cand_step = -1;
                hand_lost_since = 0;
                set_hand_state(0);
                pending_vol = applied_vol;
                cur_step = (applied_vol * LEVELS + 50) / 100;
                fprintf(stderr, "hand_timeout: control ended\n");
            }
        } else if (hand_lost_since) {
            /* Require a fresh steady hover after reacquisition. */
            hand_lost_since = 0;
            set_hand_state(1);
            hover_since = 0;
            hover_min = hover_max = d;
            cur_step = (applied_vol * LEVELS + 50) / 100;
            pending_vol = applied_vol;
            cand_step = cur_step;
            cand_t = t;
            fprintf(stderr, "hand_returned: waiting for steady hover\n");
        }

        if (in_zone && !hand_lost_since && hand_keep) {
            if (!armed) {
                set_hand_state(1);
                if (!hover_since) {
                    hover_since = t;
                    hover_min = hover_max = d;
                } else {
                    if (d < hover_min) hover_min = d;
                    if (d > hover_max) hover_max = d;
                    if (hover_max - hover_min > HOVER_SPAN_MM) {
                        /* Moving or noisy: restart the pause-before-motion detector. */
                        hover_since = t;
                        hover_min = hover_max = d;
                    } else if (t - hover_since >= HOVER_STABLE_MS) {
                        armed = 1;
                        anchor_cm = distance_mm_to_cm(d);
                        anchor_step = cur_step;
                        min_latched = cur_step == 0;
                        max_latched = cur_step == LEVELS;
                        cand_step = cur_step;
                        cand_t = t;
                        set_hand_state(2);
                        fprintf(stderr, "hand_armed: move hand to adjust volume\n");
                    }
                }
            } else {
                double hand_cm = distance_mm_to_cm(d);
                double travel_cm = hand_cm - anchor_cm;
                double target_step;
                int step;

                if (min_latched) {
                    /* Track further inward motion. Once the hand has deliberately
                     * retreated by 3 cm, make that position a fresh 0% anchor. Do
                     * not map the retreat itself to volume: only subsequent motion
                     * outward raises the level. */
                    if (hand_cm < anchor_cm) anchor_cm = hand_cm;
                    travel_cm = hand_cm - anchor_cm;
                    if (travel_cm < MIN_LIMIT_RELEASE_CM) {
                        step = 0;
                    } else {
                        min_latched = 0;
                        anchor_cm = hand_cm;
                        anchor_step = 0;
                        step = 0;
                        cand_step = 0;
                        cand_t = t;
                        fprintf(stderr, "min_rearmed: new 0%% anchor; move outward to raise volume\n");
                    }
                } else if (max_latched) {
                    /* Symmetric hold at 100%; the hand must move inward
                     * deliberately before the level starts falling. */
                    if (hand_cm > anchor_cm) anchor_cm = hand_cm;
                    travel_cm = hand_cm - anchor_cm;
                    if (travel_cm > -MAX_LIMIT_RELEASE_CM) {
                        step = LEVELS;
                    } else {
                        max_latched = 0;
                        anchor_step = LEVELS;
                        target_step = LEVELS + travel_cm * LEVELS / FULL_TRAVEL_CM;
                        step = (int)(target_step + 0.5);
                    }
                } else {
                    target_step = anchor_step + travel_cm * LEVELS / FULL_TRAVEL_CM;
                    step = target_step >= 0 ? (int)(target_step + 0.5) :
                                              (int)(target_step - 0.5);
                    if (target_step >= LEVELS) {
                        step = LEVELS;
                        max_latched = 1;
                        anchor_cm = hand_cm;
                        anchor_step = LEVELS;
                    } else if (target_step <= 0) {
                        step = 0;
                        min_latched = 1;
                        anchor_cm = hand_cm;
                        anchor_step = 0;
                    }
                }

                /* No volume changes until movement is clearly outside the hover band. */
                if (!min_latched && !max_latched &&
                    (travel_cm < 0 ? -travel_cm : travel_cm) < MOVE_START_CM)
                    step = anchor_step;
                if (t - last_control_report >= 150) {
                    fprintf(stderr,
                            "control raw=%d filtered=%d ambient=%.0f hand=%.1f anchor=%.1f travel=%.1f step=%d min=%d max=%d\n",
                            raw, d, ambient_mm, hand_cm, anchor_cm, travel_cm,
                            step, min_latched, max_latched);
                    last_control_report = t;
                }
                if (abs(step - cur_step) < STEP_DEADBAND)
                    step = cur_step;

                if (step != cand_step) { cand_step = step; cand_t = t; }

                if (cand_step != cur_step && t - cand_t >= SETTLE_MS) {
                    cur_step = cand_step;
                    pending_vol = (cur_step * 100 + LEVELS / 2) / LEVELS;
                }
            }
            last_near = t;
        }

        /* гасим с задержкой, чтобы не мигало */
        if (hand_seen) last_near = t;
        if (shown && !in_zone && !hand_seen && t - last_near > CLEAR_MS) {
            led_bar(0, 0, 0, 0, 0);
            shown = 0;
        }

        if (pending_vol >= 0 && pending_vol != applied_vol && t - last_apply >= VOL_MS) {
            int next_vol = pending_vol;
            if (next_vol > applied_vol + VOL_SLEW_PCT)
                next_vol = applied_vol + VOL_SLEW_PCT;
            else if (next_vol < applied_vol - VOL_SLEW_PCT)
                next_vol = applied_vol - VOL_SLEW_PCT;

            int previous_vol = applied_vol;
            if (set_volume(next_vol) == 0) {
                applied_vol = next_vol;
                fprintf(stderr, "volume=%d%% target=%d%%\n", applied_vol, pending_vol);
                if (applied_vol / FEEDBACK_STEP_PCT !=
                        previous_vol / FEEDBACK_STEP_PCT &&
                    (!last_feedback || t - last_feedback >= FEEDBACK_COOLDOWN_MS)) {
                    play_volume_feedback();
                    last_feedback = t;
                }
            } else {
                fprintf(stderr, "volgest: amixer failed for %d%%\n", pending_vol);
            }
            last_apply = t;
        }

        /* Reassert the display while the hand is captured so the ring stays lit. */
        if (in_zone || hand_seen) {
            int bucket = applied_vol / 10;
            if (t - last_led_refresh >= LED_REFRESH_MS || bucket != led_bucket) {
                led_volume(applied_vol, hand_seen);
                led_bucket = bucket;
                shown = 1;
                last_led_refresh = t;
                last_near = t;
            }
        }
    }
    return 0;
}
