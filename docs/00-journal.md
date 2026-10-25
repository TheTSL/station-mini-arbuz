# 00. Журнал исследований

Хронология проверенных фактов и находок. Без личных данных.

## 2026-10-24 / 25 — первичный разбор

- Устройство: YNDX-0004, `yandexmini`, Amlogic A113X, Buildroot, ядро 4.9.68.
- Корень: UBIFS rw, отдельный `/bkupgrade`; `quasar` в `/system/vendor/quasar`.
- Найдены сервисы: `maind` (platformd/mediad/telemetry/fluent-bitd/updatesd), `quasar_launcher2`, `bsa_server`, `unbound`, `dropbear`.

## Прошивка

- Заводская 1.3.4.12 (2019) не поддерживает удалённую активацию ADB.
- Целевая рабочая: **1.305.4.16.3237824040.20250721** (2025-07-21).
- Механика даунгрейда: локальный HTTPS-прокси с сертами eMudhra + манифест `/check_updates` при **любом отличии** версии.
- Образ 56 МБ раздан локально по HTTP; установка прошла.

## Активация ADB

- `maind` запускает `activate_adb.sh --quasmodrom --network activate`.
- Маркеры: `/data/quasar_activate_adb_quasmodrom`, `/data/quasar_activate_network_adb`.
- `adbd` поднимается на TCP 5555.
- Проверено: `uid=0(root)`.

## Закрепление

- `/etc/init.d/S98adbfix` — `check` при старте + сторож каждые 2 минуты.
- Броня: `activate_adb.sh` → обёртка, `deactivate` блокируется.
- OTA: `device_ota_update.sh` = `exit 0`; `updatesd` убран из `launch.json`/`servicesList`; `update-info.json` удалён.
- После перезагрузки ADB и SSH вернулись сами.

## Сеть

- `maind --platformd` держит `networkd`/`wifid`.
- `wpa_supplicant` и `dhcpcd` — отдельные процессы, запускаются init-скриптами.
- Пустой `launch.json` + ребут → сеть, SSH и ADB работают **без `maind`**.
- AP+STA: `iw phy` разрешает, `wl apsta=1`, но создание `wlan1` переносит STA и `wlan0` остаётся без IP — репитер не поднялся.

## Звук

- PCM: `hw:0,0` TDM-B (playback, держит `mediad`), `hw:0,1` TDM-C, `hw:0,2` PDM (микрофонный массив).
- `default` в `/etc/asound.conf` → `hw:0,3`, которого нет.
- `/dev/audiodsp0` — DSP для AC3/DTS, к PCM-выводу напрямую не относится.
- Микрофон: `arecord -D plughw:0,2 -f S16_LE -r 16000 -c 1` даёт реальный сигнал (RMS ~6000).
- Причина отказа `aplay`: пропатченная Linkplay `libasound` («cloudy») проводит все ioctl, но возвращает `ENOMEM` из userspace.
- Решение: статическая stock alsa-lib 1.2.11 + свой плеер → `open/hw_params/prepare` = 0, воспроизведение в реальном времени.
- Параметры: S16_LE, 2 канала, 48000 Гц, period 2048, buffer 8192 (ограничен предвыделением 32 КБ).
- `dmix` поверх `hw:0,0` ломает звук `maind` — откатили.

## Wake-word

- Модель: Kaldi nnet3 (`\0B<Nnet3>`) с кастомными слоями Yandex (`Q16FixedAffineComponent`, `Q16GroupConvCorouComponent`), Q16.
- `words.txt` активации — буквы: `а, л, и, с, А` + filler; пороги 0.44/0.22.
- Переименование слова невозможно без модели под новые буквы; переключение на `activation/yandex` — одной строкой в `quasar.cfg`.
