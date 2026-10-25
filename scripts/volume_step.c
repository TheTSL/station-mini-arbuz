/*
 * volume_step.c — единая функция изменения громкости.
 *
 * Скопирована логика штатного изменения громкости колонки:
 *   - внутренняя шкала 0..10000 (как у maind: 5000 = 50%);
 *   - шаг по умолчанию 1000 (10%);
 *   - ограничение 0..10000;
 *   - mute отдельным флагом;
 *   - применение через ALSA Master (в своём режиме);
 *   - сохранение значения в /data/quasar/last_vendor_volume.
 *
 * Сборка:
 *   aarch64-linux-gnu-gcc -O2 -static -o volume_step volume_step.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOL_MIN      0
#define VOL_MAX      10000
#define VOL_STEP     1000

static int clamp(int v)
{
    if (v < VOL_MIN) return VOL_MIN;
    if (v > VOL_MAX) return VOL_MAX;
    return v;
}

/* Читает текущую громкость из файла maind (0..10000) */
int volume_read(void)
{
    FILE *f = fopen("/data/quasar/last_vendor_volume", "r");
    int v = 5000;
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = 5000;
        fclose(f);
    }
    return clamp(v);
}

/* Сохраняет громкость в файл maind */
void volume_save(int v)
{
    FILE *f = fopen("/data/quasar/last_vendor_volume", "w");
    if (!f) return;
    fprintf(f, "%d", clamp(v));
    fclose(f);
}

/* Применяет громкость к ALSA Master (0..100%) */
void volume_apply(int v)
{
    char cmd[256];
    int pct = (clamp(v) * 100 + VOL_MAX / 2) / VOL_MAX;
    snprintf(cmd, sizeof(cmd),
             "LD_LIBRARY_PATH=/system/vendor/quasar/libs "
             "/system/workdir/bin/amixer -c 0 sset Master %d%% >/dev/null 2>&1", pct);
    if (system(cmd)) { /* ignore */ }
}

/* Основная функция: шаг громкости.
 * cur   — текущее значение 0..10000 (если <0 — прочитать из файла)
 * delta — приращение в единицах шкалы (например +1000 / -1000)
 * mute  — указатель на флаг mute (может быть NULL)
 * return — новое значение 0..10000
 */
int volume_step(int cur, int delta, int *mute)
{
    int v;

    if (cur < 0)
        cur = volume_read();

    v = clamp(cur + delta);

    if (mute && *mute && delta > 0)
        *mute = 0;              /* прибавление громкости снимает mute */

    volume_save(v);
    volume_apply(v);
    return v;
}

#ifdef VOLUME_STEP_DEMO
int main(int argc, char **argv)
{
    int delta = (argc > 1) ? atoi(argv[1]) : VOL_STEP;
    int mute = 0;
    int cur = volume_read();
    int next = volume_step(cur, delta, &mute);
    printf("volume: %d -> %d (%d%%)\n", cur, next, next / 100);
    return 0;
}
#endif
