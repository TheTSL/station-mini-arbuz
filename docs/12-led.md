# 12. LED-кольцо

## Что известно

- В device tree узел `aml,ledring` (i2c-1, 0x1f, `aml_pca9557`), но **драйвер не привязан**; в ядре `CONFIG_AMLOGIC_LEDRING is not set`.
- На `i2c-0` по адресу `0x3c` висит `ti,sn3236` (36-канальный LED-драйвер).
- `/sys/class/leds/` пуст, отдельного `/dev`-узла нет.
- Кольцом управляет **`mv_ioguard`**: в бинаре есть `i2c_dev`, `led_function`,
  `led_effect_start`, `led_polling_thread`, `g_led_controller`, а также уведомления
  `GNOTIFY=MCULEDANGLE/MCULEDIDLE/MCULEDLISTENING/MCULEDTHINKING/MCULEDPLAYTTS`.

## Попытка прямого управления

Собран инструмент `scripts/i2c_led.c` (запись в `/dev/i2c-0`, адрес `0x3c`) с командами
`is31`, `pwm`, `color`, `off`, `reg`. Записи проходят без ошибок, но **кольцо на них не
реагирует** — протокол чипа отличается от IS31FL3236, либо `mv_ioguard` постоянно
перезаписывает состояние.

Важно: попытка выставить все 36 каналов в 255 при токе `0x7F` совпала с перезагрузками
колонки (просадка питания). Работать только на малой яркости.

## Следующий шаг

`mv_ioguard` слушает Unix-сокеты:

```
/tmp/RequestIOGuard
/tmp/RequestGoheadCmd
/tmp/volumeprompt
/tmp/volumeprompt1
```

Через них, судя по строкам (`IO UnixSocketServer_recv`), принимаются команды.
Нужно снять формат (например, `GNOTIFY=...`) и отправлять свои — тогда подсветкой
можно управлять без разбора I2C-протокола чипа.
