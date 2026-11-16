# С Ubuntu до Darwin 27 на телефоне

Что сделано в проекте и как с машины на Ubuntu собрать ядро и образы и загрузить Darwin 27.2 на
телефон из оперативной памяти. Ничего не прошивается и не стирается: телефон стартует в стоковом
fastboot, после перезагрузки снова Android.

Подробности по каждому шагу: [building.md](building.md) (сборка), [darwin26-boot.md](darwin26-boot.md)
(журнал запуска Darwin 26 и 27), [darwin/README.md](../darwin/README.md) (скрипты пользовательской
части), [troubleshooting.md](troubleshooting.md) (ошибки).

## Сначала честно: что можно с чистого клона, а что нет

| Что | С чистого клона на Ubuntu | Что ещё нужно |
| --- | --- | --- |
| Проверки (`make check`), загрузчик UEFI, ядро, собранное LLVM, в QEMU (`make qemu`) | Да | Пакеты из шага 1 |
| Ядро, которое загружается на телефоне | Да | theos с его `ld64` (шаг 3) |
| Образы Darwin 27 (системный и корневой) | Нет | IPSW от Apple; рабочее дерево команды `darwin-renoir`, оно пока не опубликовано |
| Загрузка на телефон | Нет | Тот же `darwin-renoir` (резидентный загрузчик и инструмент передачи по USB) и сам телефон |

Darwin 27 до домашнего экрана доходил на **renoir** (Snapdragon 780G), последний раз 10.11.2026.
На redwood Darwin 26/27 ещё не загружался; macan загружался, но его драйверов пока нет в основной ветке
(их README на ветке `devices` говорят, что работает). В QEMU Darwin 27 тоже доходит до домашнего экрана
(14.11.2026: экран блокировки, домашний экран, Пункт управления, «Настройки» → «Об этом устройстве»:
iOS 27.2); что для этого нужно, описано в [darwin26-boot.md](darwin26-boot.md), раздел «Darwin 27 in
QEMU». Само ядро в QEMU загружается и до своей оболочки и проверяется там целиком: сеть, диски, файлы,
звук, ввод, процессы, потоки, память, IOKit и экран, на двух и на четырёх процессорах (шаг 3, `make qemu`).

## Что сделано

- **Телефон — это данные, а не код.** Пакет устройства — каталог `NAME/` с `board.dts` (что есть у
  телефона) и `soc.dtsi` (где блоки его чипа) в синтаксисе device tree. Пакеты лежат на ветке
  [`devices`](https://github.com/oosexgang/xnu-kernel-qcom/tree/devices) (renoir, redwood, macan),
  в основной ветке только эмулятор и шаблоны. Отдельной папки `soc/` больше нет: каждый пакет
  самодостаточен. Таблицы для ядра, загрузчика и пользовательской части генерирует
  `tools/boards.py gen` при каждой сборке.
- **Ядро.** Релиз Apple `xnu-12377.121.6` + файлы порта (`kernel/overlay/`) + тематические патчи
  (`kernel/patches/`). Драйверы лежат в `bsd/dev/board/`, интерфейсы устройств в `bsd/sys/board/`,
  без префикса `hakios_` в именах файлов. Драйвер запускается, только если плата перечисляет его
  `feature`.
- **Darwin, а не iOS.** Везде, где речь о системе: `darwin/`, `DARWIN_FW`, `DARWIN_SDK`,
  `/System/Volumes/Darwin`.
- **Серийные номера.** Загрузчик выводит из одного 64-битного зерна правдоподобный и каждый раз
  одинаковый для этого зерна серийный номер, MLB, ECID, MAC-адреса Wi-Fi и Bluetooth.
  `tools/identity.py` хранит зерно для каждого телефона, `boot26.py` передаёт его как
  `identity=`. Без зерна загрузчик берёт случайное (`EFI_RNG_PROTOCOL` + счётчик).
- **Камера.** Где у чипа такты камеры и как настроены его PLL — в `camera { }` файла `soc.dtsi`.
  Драйвер сам считает делители; какой датчик — задняя, а какой — фронтальная камера Darwin, решает
  плата.
- **Проверки.** `make check` гоняет драйверы на хосте и под `qemu-aarch64` против моделей железа
  (`workbench/`), проверяет файлы плат, документацию и то, что в репозитории нет ничего, что нельзя
  публиковать. CI делает то же на каждый push.

## 1. Ubuntu: пакеты

Ubuntu 24.04:

```sh
sudo apt-get install build-essential git curl clang lld python3 perl xz-utils flex bison m4 \
    autoconf automake gawk clang-20 lld-20 llvm-20 zlib1g-dev libssl-dev \
    gcc-aarch64-linux-gnu qemu-user qemu-system-arm qemu-efi-aarch64 shellcheck fastboot
```

Ядру нужен clang 19 или новее (`XNU_CLANG_DIR=/usr/lib/llvm-20/bin`). `gawk` обязателен: `mawk` из
Ubuntu ломает генератор таблицы системных вызовов.

## 2. Клон, пакет телефона, проверки

Все каталоги лежат рядом, в одном родительском (дальше `$DK`): так их ищут скрипты.

```sh
mkdir -p ~/darwin_kernel && cd ~/darwin_kernel          # это $DK
git clone https://github.com/oosexgang/xnu-kernel-qcom.git hakios
cd hakios
git worktree add ../hakios-devices devices             # пакеты телефонов
export HAKIOS_DEVICES=$PWD/../hakios-devices           # (или: tools/device.py add renoir)

make check                                             # всё, что не требует телефона
tools/boards.py status renoir                          # что у телефона включено и чего не хватает
loader/build.sh                                        # loader/loader.efi
workbench/loader_boot_test.sh                          # загрузчик под настоящим UEFI в QEMU
```

## 3. Ядро

Для ядра, которое **загружается**, нужен theos: его компоновщик `ld64` и SDK `iPhoneOS16.5.sdk`
([установка на Linux](https://theos.dev/docs/installation-linux)), по умолчанию в `~/theos`.

```sh
export THEOS=~/theos XNU_CLANG_DIR=/usr/lib/llvm-20/bin
kernel/build.sh
```

Скрипт сам скачает релиз Apple в `$DK/xnu-12377-hakios`, наложит файлы порта и патчи, сгенерирует
таблицу плат из пакетов, которые нашёл, и соберёт `$DK/xnu-12377-hakios/BUILD/obj/mach.development.bcm2837`.

Без theos, только инструментами LLVM, ядро тоже собирается и компонуется, и это ядро загружается в QEMU.
На телефоне проверено ядро, собранное с theos.

```sh
make kernel-check                                      # ядро инструментами LLVM (THEOS не нужен)
make qemu                                              # загрузка в QEMU: проверки из оболочки, тесты XNU (kernPOST=1), четыре процессора
tools/qemu-root.sh root.img                            # корень из программ этого репозитория
tools/qemu-kernel.sh --cpus 2 --ramdisk root.img --boot-args rd=md0 --until 'a shell on the console' \
    --type 'help\nioreg 3\nthreads\nas 501:20 id\n' --screen screen.png ../xnu-12377-hakios/BUILD/obj/mach.development.bcm2837
```

Самому поработать в оболочке ядра: `tools/qemu-kernel.sh --interactive --cpus 2 --ramdisk root.img
--boot-args rd=md0 <ядро>` (консоль в этом терминале, выход Ctrl-C; с `--window` ещё и экран в окне).
Что умеет оболочка в QEMU и что проверяет `make qemu`: [qemu.md](qemu.md). Команда `as UID:GID КОМАНДА`
выполняет команду от имени другого пользователя: так видно, что ядро не позволяет тому, кто не root
(`as 501:20 kill 1`, `as 501:20 chown 0:0 ФАЙЛ`).

## 4. IPSW Darwin 27.2

Нужна сборка **24B119**, файлы **iPhone12,1**. Образы дисков в IPSW Darwin 26/27 зашифрованы (AEA).
`darwin/tools/aea.py` расшифровывает их на Linux прямо из IPSW на CDN Apple, по range-запросам, не
скачивая IPSW целиком (нужны `python3-cryptography` и `liblzfse1`):

```sh
IPSW=https://updates.cdn-apple.com/2026FallFCS/22a2de9b-5ef9-44b7-9ace-62f861167fd8/iPhone12,1_27.2_24B119_Restore.ipsw
python3 darwin/tools/aea.py list "$IPSW"                                   # что в IPSW
python3 darwin/tools/aea.py decrypt "$IPSW" 141-10128-227.dmg.aea cryptex-os.dmg   # cryptex системы, 6,2 ГБ
python3 darwin/tools/aea.py decrypt "$IPSW" 141-08279-215.dmg.aea rootfs.dmg       # корневая ФС, 9,3 ГБ
python3 darwin/tools/aea.py get "$IPSW" 141-08824-244.dmg cryptex-app.dmg         # cryptex приложений (не зашифрован)
```

Если загрузка оборвалась, `decrypt ... N` продолжает с кластера `N` (номер виден по размеру файла: кластер
равен 256 МБ). Дальше образы открывает `7z` (версии 23 или новее читают APFS); файлы, сжатые LZBITMAP, 7z
оставляет пустыми, их доделывает `unzbm`. Разложите всё в `$DK/darwin27` так:

```
darwin27/rootfs/           корневая файловая система
darwin27/cryptex/os/       cryptex системы
darwin27/cryptex/app/      cryptex приложений
darwin27/dyld/             dyld_shared_cache_arm64e и его подкэши (.01, .02, ...)
```

Файлы Apple в репозиторий не попадают никогда.

## 5. Кэш без PAC

У Snapdragon 780G нет аутентификации указателей: каждую PAC-инструкцию кэша надо заменить на
обычную, а контрольную сумму corecrypto пересчитать.

```sh
export DARWIN_FW=$DK/darwin27
cc -O2 -o darwin/tools/pacstrip darwin/tools/pacstrip.c -lcrypto
darwin/tools/pacstrip cache "$DARWIN_FW/dyld-strip" "$DARWIN_FW"/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py "$DARWIN_FW/dyld-strip" \
    AE2C707EB9149A03BA76C9B332CA5C2FBF758191271C221E28EACA40D8065C18 \
    F0BBB5196E13FC74FEF80014965B915B0882AD557535BCAA07E34BA4959771D1
```

Два значения FIPS относятся к 24B119. Для другой сборки они берутся из лога первой загрузки
(строки «In variable» и «MAC generated»).

## 6. Образы (нужен `darwin-renoir`)

`darwin-renoir` кладётся в `$DK/darwin-renoir`. Один раз до всего остального: его скрипт
mk-mesa.sh (в каталоге tools), потом `darwin/userland/mesa26e.sh` и `darwin/userland/gl26.sh`.

```sh
export OBJ=$PWD/darwin/userland/obj27
BUILD=24B119 bash darwin/userland/inj26.sh             # наши библиотеки, arm64e
bash darwin/userland/root26-darwin.sh                  # $DARWIN_FW/root26-darwin.apfs и $DARWIN_FW/jobs26
LAUNCHDAEMONS="$DARWIN_FW/jobs26" WORK="$DARWIN_FW/image-tree-prune" \
    python3 darwin/tools/mkimage26.py --prune "$DARWIN_FW/system27-prune.apfs"
```

`--prune` выкидывает то, что здесь всё равно не работает, чтобы образ влез в 4 ГиБ − 1 (больше
fastboot передать не может) и в 6 ГБ ОЗУ. Слишком большой образ сборщик откажется делать.

## 7. Загрузка

Телефон с разблокированным загрузчиком, в стоковом fastboot, по USB:

```sh
fastboot getvar product                                # должен ответить renoir
python3 darwin/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF
```

`--wifi-mac` — MAC-адрес Wi-Fi этого телефона. Пути по умолчанию: ядро из шага 3,
`$DK/darwin27/system27-prune.apfs` и `$DK/darwin27/root26-darwin.apfs`. Параметры загрузки берутся
из `boot { }` пакета (`--boot-arg NAME=VALUE` добавляет или заменяет один, `--return-after N`
возвращает телефон загрузчику с логом через N секунд). Что делает каждый параметр:
[boot-args.md](boot-args.md).

Что происходит: fastboot запускает резидентный загрузчик, тот принимает системный образ (около
двух минут по USB), потом ядро с корневым образом. Домашний экран появляется примерно через две
минуты после старта.

## Что будет и чего не будет

- Есть: домашний экран, строка состояния (часы сдвинуты под камеру в экране), виджеты Погоды и
  Календаря, касания и кнопки, частоты процессора (`cpufreq=1`). Выключены по умолчанию (`boot { }`
  пакета renoir): Wi-Fi, звук, камера, вибромотор, батарея.
- Графика программная (`gpu=0`): Liquid Glass, фигуры CASDF и большие часы экрана блокировки не
  рисуются, в диалогах артефакты. Аппаратный GPU и Metal пока выключены.
- Каждая загрузка — первая (`/var` в ОЗУ): первые минуты системные демоны грузят процессор.
- Нагрев не решён: ограничителя температуры для renoir в этой конфигурации нет.
- Зерно серийных номеров (`identity=`) резидентный загрузчик `darwin-renoir` пока не читает: это
  делает загрузчик этого репозитория (`loader/`).

## Если что-то не так

- Подробный журнал запуска с каждым препятствием и его решением: [darwin26-boot.md](darwin26-boot.md).
- Ошибки проверок и сборки: [troubleshooting.md](troubleshooting.md).
- Новый телефон вместо renoir: [porting.md](porting.md).
