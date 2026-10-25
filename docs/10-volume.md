# 10. Громкость: как она устроена

## Шкала

Внутри `maind` громкость хранится как **0…10000** (5000 = 50%) плюс отдельный флаг mute.
Файлы `/data/quasar/last_user_volume` (`3.100000`) и `/data/quasar/last_vendor_volume` (`5000`) при старте **игнорируются** — проверено перезагрузками с разными значениями, всегда возвращается 50%.

## ALSA `Master` — зеркало

- `amixer -c 0 sset Master 30%` в режиме Алисы → через пару секунд снова 50%: `maind` возвращает своё значение.
- Поставить 100% удаётся (это его «unity»).
- Значит громкость Алисы **не** управляется через ALSA; `maind` применяет её в своём движке (glagol, лог `glagol.cpp: Changed volume to 0.5`).

## protobuf-схема (вытащена из `protodesc_cold`)

```proto
message VolumeManager {
  oneof {
    State state = 1;
    SetVolumeSignal set_volume_signal = 2;
  }
  message SetVolumeSignal {
    int32  platform_volume = 1;  // 0..10000
    bool   is_muted = 3;
    string source = 4;
  }
}
```

Секция `protodesc_cold` устроена как таблица + подряд идущие `FileDescriptorProto`; границы пришлось искать сканированием (тег `0x0a` + длина + имя `*.proto`).

## Транспорт

- Локальные порты `maind`: 9850 (yiod), 9875, 9877 (yio_audio), 9878 (authd), 9881 (pushd), 9884 (firstrund), 9898/9899 — только `127.0.0.1`.
- В бинаре: `yandex_io/protos/yandex_io.proto`, `remoting.proto`, `ciphertext`, `aes128crt_key`, `session_id`, `EncryptedSetupCredentialsMessage`.
- Модуль `yandex_io/modules/volume_manager/implementations/alsa/alsa_volume_manager.cpp`, JSON-поле `"volumeState"`, `Remoting.DeviceStateCapabilityMethod.VolumeState`.

Вывод: YandexIO — шифрованный protobuf с сессией (AES-128-CTR). Для внешнего управления громкостью в режиме Алисы нужен клиент YandexIO с привязкой и ключом сессии, который выдаёт облако. Это отдельный большой проект.

## Практический обход

- **Свой режим** (`maind` не запущен): громкость полностью наша — ALSA `Master` + жесты + бот.
- **Режим Алисы**: громкость родная (голос/жест), бот об этом сообщает и не пытается её менять.
- При переключении в свой режим громкость переносится (запоминается перед ребутом и восстанавливается после).
