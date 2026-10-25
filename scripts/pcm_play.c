#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "hw:0,0";
    const char *path = argc > 2 ? argv[2] : "/tmp/tone.raw";
    snd_pcm_t *pcm = NULL;
    int err;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    printf("open: %d (%s)\n", err, err < 0 ? snd_strerror(err) : "ok");
    if (err < 0) return 1;

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) { printf("any: %s\n", snd_strerror(err)); return 2; }
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) printf("access: %s\n", snd_strerror(err));
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0) printf("format: %s\n", snd_strerror(err));
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0) printf("channels: %s\n", snd_strerror(err));
    unsigned int rate = 48000;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) printf("rate: %s\n", snd_strerror(err));
    snd_pcm_uframes_t period = (argc > 3) ? (snd_pcm_uframes_t)atoi(argv[3]) : 1024;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0) printf("period: %s\n", snd_strerror(err));
    snd_pcm_uframes_t buffer = (argc > 4) ? (snd_pcm_uframes_t)atoi(argv[4]) : 8192;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0) printf("buffer: %s\n", snd_strerror(err));

    err = snd_pcm_hw_params(pcm, hw);
    printf("hw_params: %d (%s)\n", err, err < 0 ? snd_strerror(err) : "ok");
    if (err < 0) return 3;

    snd_pcm_uframes_t p2, b2;
    snd_pcm_hw_params_get_period_size(hw, &p2, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &b2);
    printf("period=%lu buffer=%lu rate=%u\n", (unsigned long)p2, (unsigned long)b2, rate);

    err = snd_pcm_prepare(pcm);
    printf("prepare: %d (%s)\n", err, err < 0 ? snd_strerror(err) : "ok");
    if (err < 0) return 4;

    FILE *f = fopen(path, "rb");
    if (!f) { printf("no file %s\n", path); return 5; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz > 44) { fseek(f, 44, SEEK_SET); sz -= 44; }
    char *buf = malloc(sz);
    if (!buf) { printf("malloc failed\n"); return 6; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { printf("read failed\n"); return 7; }
    fclose(f);
    printf("pcm bytes=%ld\n", sz);

    long off = 0;
    while (off < sz) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf + off, (sz - off) / 4);
        if (n < 0) {
            printf("writei: %s\n", snd_strerror((int)n));
            n = snd_pcm_recover(pcm, (int)n, 1);
            if (n < 0) break;
            continue;
        }
        off += n * 4;
    }
    printf("written=%ld\n", off);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    printf("done\n");
    return 0;
}

