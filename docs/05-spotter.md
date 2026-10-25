# 05. Wake-word и споттер

## Что лежит в прошивке

```
/system/vendor/quasar/spotter_models/
  activation/alisa/   acoustic_model.nnet, flags.txt, words.txt, lda.mat, tts-timings-info/
  activation/yandex/  acoustic_model.nnet, flags.txt, words.txt, tts-timings-info/
  command/            слова-команды (включи, выключи, свет, ...)
  vad/                детектор речи
  counteractivation/
```

## Формат модели

- Заголовок: `\0B<Nnet3>` — текстовый Kaldi nnet3 с бинарными матрицами.
- Кастомные слои Yandex: `Q16FixedAffineComponent`, `Q16GroupConvCorouComponent` (квантование Q16), а также `svdf`, `hardswish`.
- `flags.txt` активации: `spotter-type chen14`, `sample-rate 16000`, `frame-size 25`, `frame-shift 10`, `mel-num-bins 40`, `string-representations алиса`, пороги `thresholds 0.44`, `subthresholds 0.22`.
- `words.txt` — **буквы**, а не слово: `filler 0, а 1, л 2, и 3, с 4, А 5`.

## Следствие

Сеть выдаёт вероятности только для букв ключевого слова. Заменить «Алиса» на «Арбуз» переименованием нельзя: классов `б, р, у, з` в модели нет. Нужна собственная модель в формате Yandex (Kaldi nnet3 + их квантованные слои), обученная на новом слове.

Второй вариант — модель `activation/yandex`: в `quasar.cfg` путь `spotterModelPath` переключается на `.../activation/yandex`, и колонка реагирует на «Яндекс».

## Секретные фразы

В `quasar.cfg` есть `aliced.secretMsgList` и `setupd.secretMsgList` — сопоставление SHA-1 распознанной строки с командой. Годится для фиксированной фразы, но не для произвольного текста.
