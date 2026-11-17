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

```sh
./bench enter                                    # резидентный загрузчик
./bench ramdisk                                  # образ системы, один раз за сессию
```

## 1. Прогоны по порядку

Каждый следующий имеет смысл, только если предыдущий прошёл. `scripts/phone-day.sh` делает это сам: прогоняет их
по порядку, после каждого смотрит журнал, останавливается на первом, где что-то не так, и складывает все журналы и
сводку (`summary.md`) в `logs/day-ВРЕМЯ/` — эту папку и присылайте. Можно назвать прогоны
(`scripts/phone-day.sh phone_look phone_bt`) или не останавливаться (`--keep-going`).

| № | Команда | Что должно быть | Если нет |
| --- | --- | --- | --- |
| 1 | `BENCH_DEFCONFIG=phone_first ./bench run` | `kernel: started; no panic; back in the loader` | Пришлите `logs/last.log`: ядро не дошло до драйверов |
| 2 | `BENCH_DEFCONFIG=phone_look ./bench run` | Всё, что только читает: `said` или `ok` у яркости, температуры, памяти, батареи, кнопок, тача и Bluetooth (`bt=-1`), без `STOPPED` | Строка `STOPPED` и `tools/logcheck.py --all logs/last.log` |
| 3 | `BENCH_DEFCONFIG=phone_ufs_check ./bench run` | `Storage (own driver) ok` с `0 NOT THE SAME, 0 failed reads` | `DIFFERENT` или `STOP`: phone_perf для этого телефона пока не годится, пришлите журнал |
| 4 | `BENCH_DEFCONFIG=phone_ufs_bench ./bench run` | Цифры скорости чтения с 1–31 запросами | — (только измерение) |
| 5 | `BENCH_DEFCONFIG=phone_perf ./bench run` | Частоты (`CPU frequency ok`), свой драйвер UFS, GPU | Строка `STOPPED` |
| 6 | `BENCH_DEFCONFIG=phone_metal ./bench run` | `GPU ok` (`A PICTURE DRAWN BY THE GPU IS ON THE PANEL`), Wi-Fi | `GPU STOPPED`: шаг, на котором остановился |
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
