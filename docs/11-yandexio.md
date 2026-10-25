# 11. YandexIO: схема и что мешает

## Файлы протокола

В секции `protodesc_cold` лежат, в частности:

- `yandex_io/protos/yandex_io.proto`
- `yandex_io/protos/remoting.proto`
- `yandex_io/protos/connection_type.proto`
- `yandex_io/protos/account_storage.proto`
- `yandex_io/protos/alice_state.proto`
- `yandex_io/protos/model_objects.proto`
- `yandex_io/sdk/directive_sequencer/protos/directive.proto`
- `yandex_io/sdk/private/endpoint_storage/protos/endpoint.proto`

## `yandex_io.proto` — сообщения (21)

```
IOEvent, SupportedFeatures, IOControlCommand, VqeInfo, IOAudioChannelData,
IOAudioChannelsData, ProducedChannelInfo, ProducedChannelsInfo, IOChannelRequest,
IOHubMessage, TvPolicyInfo, DirectiveEntity, DirectiveEntityList,
EntityDescriptionList, DirectiveList, ExternalDirectives, UserAccount,
UserAccountList, VoiceButtonsInfo, UserOptionsList, FilePlayerCapabilityEvent
```

## `remoting.proto` — `Remoting` (методы)

| Поле | № | Назначение |
|---|---|---|
| endpoint_storage_method | 1 | endpoints |
| directive_handler_method | 2 | директивы |
| capability_state_changed_method | 3 | **состояния capability (в т.ч. VolumeState)** |
| endpoint_method | 6 | endpoint |
| alice_capability_method | 8 | Alice |
| alice_request_events_method | 9 | события Alice |
| capability_events_method | 10 | события |
| file_player_capability_method | 12 | плеер |
| playback_control_capability_method | 13 | управление воспроизведением |
| spotter_capability_method | 14 | споттер |
| capability_add_listener_method | 20 | подписка |

Громкость фигурирует как `Remoting.DeviceStateCapabilityMethod.VolumeState`
(JSON-поле `"volumeState"`), реализация — `yandex_io/modules/volume_manager/implementations/alsa/alsa_volume_manager.cpp`.

## Криптография

В бинаре:

- `aes128crt_key` — ключ AES-128-CTR;
- `encrypted_session_key` — зашифрованный ключ сессии;
- `EncryptedSetupCredentialsMessage` (`ciphertext`, `session_id`);
- `DevicePairingId`, `device_pairing_id`;
- `initial_pairing_request` / `initial_pairing_answer` (сценарий стерео-пары, но показывает формат привязки).

## Вывод

Локальные порты YandexIO (`9850`, `9877`) обмениваются **шифрованными protobuf-сообщениями**; ключ сессии получается в процессе привязки (шифруется ключом устройства). Без привязки команду не отправить.

Чтобы продолжить, нужно:

1. разобрать `volume_manager`-модуль: какие поля `Remoting` он слушает (`capability_state_changed_method` + capability id);
2. понять привязку: где хранится `DevicePairingId`/ключ (возможно, `/data/quasar/...`), как проходит `initial_pairing`;
3. реализовать клиента: framing → расшифровка → protobuf → вызов метода.

Это отдельный проект; в текущей конфигурации проще пользоваться своим режимом, где громкость полностью наша.
