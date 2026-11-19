# Проверка на телефоне

План одного дня с телефоном: какие прогоны в каком порядке, что каждый должен сказать и что прислать обратно, если
что-то не так. Каждый прогон идёт из RAM: в память телефона ничего не пишется, перезагрузка возвращает Android.

После каждого прогона `./bench run` сам печатает сводку по частям (`tools/logcheck.py`): `ok` — часть заработала,
`STOPPED` — остановилась (и строка, которая говорит почему), `disabled` — её не просили или плата её не описывает.
Полный журнал лежит в `logs/`, последний — `logs/last.log`.

## 0. Подготовка

```sh
scripts/setup-ubuntu.sh --check                 # всё ли стоит на машине
make kernel && make loader                      # ядро и загрузчик
```

Прошивки телефона кладутся в `userland/fw-*/`, по одной папке на часть (их нет и не будет в репозитории,
`.gitignore` их не пропустит): см. `userland/README.md`. Для Bluetooth, из корня рабочей копии:

```sh
adb pull /vendor/bt_firmware/image/ userland/fw-bt/    # msbtfw*.mbn или .tlv и msnv*.bin
```

В `board.dts` телефона в разделе `bluetooth { }` укажите `rampatch` и `nvm` — имена этих файлов. Потом соберите малый
образ, как обычно (`userland/build.sh` напишет, какие прошивки вошли), и:

Перед тем как подключать телефон, проверьте, что всё на месте:

```sh
make ready                                       # или scripts/phone-ready.py phone_look phone_bt
```

Она смотрит, есть ли adb и fastboot, сборка UEFI телефона, ядро и загрузчик, не старше ли они своих исходников (`OLD`:
пересоберите, иначе на телефоне пойдёт прежняя сборка), знает ли загрузчик ваш телефон, есть ли «личность» iPhone,
малый образ и прошивка Bluetooth для прогона с `bt=2`. `MISSING` — `./bench` остановится, `NOTE` — прогон пойдёт, но
сделает меньше. `scripts/phone-day.sh` сам начинает с этой проверки и не начнёт день, пока чего-то нет.

```sh
./bench enter                                    # резидентный загрузчик
./bench ramdisk                                  # образ системы, один раз за сессию
```

## 1. Прогоны по порядку

Каждый следующий имеет смысл, только если предыдущий прошёл. `scripts/phone-day.sh` делает это сам: прогоняет их
по порядку, после каждого смотрит журнал, останавливается на первом, где что-то не так, и складывает все журналы и
сводку (`summary.md`) в `logs/day-ВРЕМЯ/` — эту папку и присылайте. Можно назвать прогоны
(`scripts/phone-day.sh phone_look phone_bt`) или не останавливаться (`--keep-going`).

Одна сессия загрузчика — одно ядро: второе ядро в той же сессии до сих пор каждый раз сразу падало. Поэтому перед
каждым прогоном, кроме первого, `phone-day.sh` перезапускает загрузчик и заново отправляет образ системы
(`./bench fresh`): это несколько минут на прогон, так и задумано. Если запускаете прогоны руками, делайте то же:
`./bench fresh`, потом `./bench run`. Если ядро зависло и перестало отвечать, всё, что оно успело сказать, лежит в
папке дня как `ПРОГОН.live.log`.

| № | Команда | Что должно быть | Если нет |
| --- | --- | --- | --- |
| 1 | `BENCH_DEFCONFIG=phone_first ./bench run` | `kernel: started; no panic; back in the loader` | Пришлите `logs/last.log`: ядро не дошло до драйверов |
| 2 | `BENCH_DEFCONFIG=phone_look ./bench run` | Всё, что только читает: `said` или `ok` у яркости (`backlight=1` ничего не включает), температуры, памяти, батареи, кнопок, тача (`touchscreen=-1`: только состояние пинов и тактирования) и Bluetooth (`bt=-1`), без `STOPPED` | Строка `STOPPED` и `tools/logcheck.py --all logs/last.log` |
| 3 | `BENCH_DEFCONFIG=phone_ufs_check ./bench run` | `Storage (own driver) ok` с `0 NOT THE SAME, 0 failed reads` | `DIFFERENT` или `STOP`: phone_perf для этого телефона пока не годится, пришлите журнал |
| 4 | `BENCH_DEFCONFIG=phone_ufs_bench ./bench run` | Цифры скорости чтения с 1–31 запросами | — (только измерение) |
| 5 | `BENCH_DEFCONFIG=phone_perf ./bench run` | Частоты (`CPU frequency ok`), свой драйвер UFS, GPU. Строки `cpufreq: group N: the rows' core counts ...` присылайте в любом случае: по ним видно, не «турбо» ли верхние строки таблицы частот | Строка `STOPPED` (например, `the hardware is not in charge of its clock`) |
| 6 | `BENCH_DEFCONFIG=phone_metal ./bench run` | `GPU ok` (`A PICTURE DRAWN BY THE GPU IS ON THE PANEL`), Wi-Fi | `GPU STOPPED`: шаг, на котором остановился |
| 6a | `BENCH_DEFCONFIG=phone_metal BENCH_CMDLINE=gpu_private=1 ./bench run` (если 6 прошёл) | То же, что в 6: кольцо команд и fence доступны только привилегированным обращениям GPU, как в Linux | Если 6 прошёл, а 6a нет — пришлите оба журнала: по умолчанию это остаётся выключенным |
| 7 | `BENCH_DEFCONFIG=phone_bt ./bench run` | `Bluetooth controller ok`, `Bluetooth service ok`, `Bluetooth for Darwin ok`; переключатель Bluetooth в Настройках включается и видит устройства | Ниже, раздел 2 |

Снимок экрана во время прогона: `./bench screenshot` (`logs/screen.png`). Что изменилось по сравнению с прошлым
прогоном: `tools/logcheck.py --against logs/СТАРЫЙ.log logs/last.log`.

## 2. Bluetooth подробнее

Bluetooth на Snapdragon здесь новый и на телефоне ещё не проверялся, поэтому шаги мельче:

1. `BENCH_DEFCONFIG=phone_look ./bench run`: строки `bt-uart:` показывают, что видно до любой записи (пины, питание,
   UART).
2. `BENCH_CMDLINE=bt=1 BENCH_DEFCONFIG=phone_metal ./bench run`: контроллер включён и опознан, строка
   `product id ..., ROM version ..., SoC id ...`.
3. `BENCH_DEFCONFIG=phone_bt ./bench run`: прошивка (`sending /fw/...`, `... taken`), сброс, передача Darwin
   (`the controller is Darwin's from here on`), служба в реестре (`service "bluetooth" registered`) и подключение
   bluetoothd (`connected by bluetoothd`, `the channel opened`).
4. Если шаг 3 остановился на прошивке, попробуйте ROM без неё: `BENCH_CMDLINE=bt_fw=0 BENCH_DEFCONFIG=phone_bt ./bench run`.

Что прислать: `tools/logcheck.py --all logs/last.log` и строки `bt-uart:` и `bt:` целиком — в них версия чипа, каждый
отказ и каждое событие контроллера.

## 2a. Что посмотреть после исправлений 18–19 ноября

Эти места исправлены по коду и тестам на хосте, на телефоне их ещё никто не видел. `tools/logcheck.py` помечает
`STOPPED` каждую из строк ниже, которая говорит о сбое.

- **Тач.** Теперь `/dev/hakios-touch` читает одна программа (оконный сервер), а пишет только root. Если тач не
  реагирует, ищите `hakios-touch: pid N may not read the touches: pid M does` (устройство занято другим процессом) и
  `hakios: input: ... waiting for the other reader` от backboardd. `rwtouch` запускайте от root.
- **Wi-Fi.** После подключения в строке станции (`lx: the network is joined ...`) есть счётчик
  `N dropped for coming unciphered`. Если интернета нет, а это число растёт, значит, драйвер не помечает
  расшифрованные кадры: пришлите эту строку. При отказе подключения ищите `THE HANDSHAKE DID NOT COME THROUGH` и
  причину после двоеточия (например, `its RSN element is not the beacon's`).
- **Звук.** `sound=4`: музыка не должна пропадать насовсем после долгой игры. Если пропала, пришлите строки `sound:` и
  `adsp:`; при крахе DSP будет `THE AUDIO DSP SAYS IT HAS GIVEN UP`.
- **Батарея.** Процент должен совпадать с Android ± пара процентов. Ток больше 20 А и температура вне −40…100 °C
  теперь считаются неизвестными; если процент скачет к 0, пришлите `sysctl kern.hakios | grep battery`.
- **Перегрев.** `thermal: 1 sensor that answered before is silent` означает, что датчик замолчал и разгон не
  вернётся до его ответа. Пришлите этот журнал: значит, какой-то датчик TSENS перестаёт отвечать.
- **Кнопки.** `buttons: ... let go: ... has not been read for N looks` означает, что контроллер кнопок перестал
  отвечать; кнопку отпустили, чтобы она не залипла.
- **Камера.** Запрос длинной выдержки в темноте больше не должен отключать камеру. При отказе будет
  `camera: ...: nothing more is written in this run`.

## 3. Один образ, без ./bench

`make images DEVICE=ИМЯ` собирает сразу по образу на каждый прогон (`build/images/ИМЯ/phone_bt/` и т.д.). То же ядро
одним файлом, как его загрузит любой ([images.md](images.md)):

```sh
make image DEVICE=ИМЯ DEFCONFIG=phone_bt
tools/image.py personalize build/images/ИМЯ/boot.img --identity DeviceTree.bin   # данные своего IPSW
fastboot boot build/images/ИМЯ/boot-mine.img
```

Образ запускает ядро один раз и после его конца перезагружает телефон; журнал так не забрать, поэтому для отладки
лучше `./bench`.

## 4. Что прислать, если что-то не так

- `logs/last.log` — весь журнал прогона;
- вывод `tools/logcheck.py --all logs/last.log`;
- какой прогон (`BENCH_DEFCONFIG`, `BENCH_CMDLINE`) и коммит (`git log -1 --oneline`);
- снимок экрана, если дело в картинке.

Что значит конкретная строка и что с ней делать — [troubleshooting.md](troubleshooting.md).
