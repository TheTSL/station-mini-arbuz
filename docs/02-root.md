# 02. Root: ADB и SSH

## Механизм активации ADB

`maind` при получении конфига с `adbd.remoteActivation=true` запускает:

```
/system/vendor/quasar/activate_adb.sh --quasmodrom --network activate
```

Скрипт (`adb_options.inc`) создаёт маркеры в `/data/`:

- `/data/quasar_activate_adb_quasmodrom` — источник активации;
- `/data/quasar_activate_network_adb` — сетевой режим (TCP 5555).

`onboot`/init при старте вызывают `activate_adb.sh check`: если маркер есть — adbd поднимается снова.

## Постоянный ADB

Установлен `/etc/init.d/S98adbfix`:

- при `start` вызывает `activate_adb.sh check`;
- раз в 2 минуты проверяет наличие процесса `adbd` и перезапускает при необходимости.

## Броня от деактивации

Облачный конфиг может прислать `deactivate`. Оригинальный скрипт переименован в `activate_adb.sh.real`, а на его место установлена обёртка:

```sh
#!/bin/sh
for a in "$@"; do
  case "$a" in
    deactivate)
      echo "$(date): fortress blocked deactivate" >> /data/quasar/quasar_adb.log
      exit 0
      ;;
  esac
done
exec /system/vendor/quasar/activate_adb.sh.real "$@"
```

## SSH

Сторонний dropbear в `/data/opt/dropbear`, автозапуск `/etc/init.d/S99dropbear`, порт 2222, ключ `dropbear_ed25519_host_key`. Доступ по ключу, root-логин.

## Полезные пути

| Что | Где |
|---|---|
| Маркеры ADB | `/data/quasar_activate_*` |
| Лог ADB | `/data/quasar/quasar_adb.log` |
| launch.json | `/system/vendor/quasar/launch.json` |
| quasar.cfg | `/system/vendor/quasar/quasar.cfg` |
| Бэкапы оригиналов | `/data/local/tmp/backup_q/` |
