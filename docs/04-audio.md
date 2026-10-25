# 04. Звук: ALSA, TDM и DSP

## Устройства

```
card 0: AMLAUGESOUND
  device 0: TDM-B-dummy  — playback + capture
  device 1: TDM-C-dummy  — playback + capture
  device 2: PDM-dummy    — capture (микрофонный массив)
/dev/audiodsp0           — DSP для декодирования AC3/DTS (major 257, драйвер "dsp")
```

- `/etc/asound.conf`: `pcm.!default { type hw; card 0; device 3 }` — device 3 в системе отсутствует, поэтому `default` бесполезен.
- Реальный playback — `hw:0,0` (TDM-B), его и открывает `mediad`.
- Микрофоны — `hw:0,2` (PDM). Занят `maind`, освобождается только при остановленном `maind`.

## Драйвер (Amlogic auge/tdm)

Ключевые параметры `aml_tdm_hardware`:

| Параметр | Значение |
|---|---|
| Форматы | S16_LE, S24_LE, S32_LE |
| Каналы | 1–32 |
| Rate | 8000–48000 |
| Период | 64 B – 256 КБ |
| Буфер | до 512 КБ |
| Предвыделение | 32 КБ (min), 256 КБ (max) |

`hw_params` = `snd_pcm_lib_malloc_pages()`. При нехватке DMA-памяти возвращается `ENOMEM`.

## Главная находка

Штатная `aplay`/`libasound` из прошивки — **пропатченная Linkplay** (пишет в лог `cloudy snd_pcm_open`). Она проводит все ioctl успешно (`HW_REFINE`, `HW_PARAMS`, `SW_PARAMS`, `PREPARE` → 0), но затем возвращает `ENOMEM` из userspace.

Решение: собрать **stock alsa-lib 1.2.11** статически под aarch64 и написать свой плеер.

```
snd_pcm_open("hw:0,0") -> 0
snd_pcm_hw_params      -> 0
snd_pcm_prepare        -> 0
writei (5 c PCM)       -> OK, реальное время
```

Параметры, которые работают: `S16_LE`, 2 канала, 48000 Гц, period 2048, buffer 8192 (буфер ограничен предвыделенными 32 КБ).

## Практические заметки

- Громкость: при амплитуде ~14000 и Master 100 % слышен треск (перегруз); на 25 % амплитуды и Master 60 % — чисто.
- `SPK mute` (`amixer -c 0 sset 'SPK mute' off`) нужно включать перед воспроизведением.
- `mediad` держит PCM; чтобы забрать устройство, его нужно остановить (но супервизор перезапускает).
- `dmix` поверх `hw:0,0` ломает звук `maind` — не использовать.

## Сборка плеера

```sh
# cross
aarch64-linux-gnu-gcc -O2 -static -Ialsa-lib/include -o pcm_play pcm_play.c \
    -Lalsa-lib/src/.libs -lasound -lpthread -lm -ldl -lrt
```

Исходник — `scripts/pcm_play.c`.
