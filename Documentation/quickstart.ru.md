# Быстрый старт

Коротко и по шагам: от чистой Ubuntu до образа для своего телефона, который загружается одной командой. Подробности
каждого шага в английских документах, ссылки рядом. Полный путь до Darwin 27 на телефоне описан в
[darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md).

## 1. Машина

```sh
scripts/setup-ubuntu.sh --check     # чего не хватает (группы: check, kernel, loader, qemu, phone)
scripts/setup-ubuntu.sh             # поставить всё
make check                          # всё, что проверяется без телефона: должно пройти
```

Заголовки `iPhoneOS16.5.sdk` и, по желанию, theos ставятся отдельно: [building.md](building.md).

## 2. Свой телефон в своём репозитории

Ядро не привязано ни к одному телефону: всё, что относится к устройству (адреса, пины, питание, какие части
включать), лежит в пакете устройства, `board.dts` и `soc.dtsi`. Пакет можно держать в отдельном репозитории:

```sh
tools/device.py init ../mido --name mido --soc sm7325 --description "Мой телефон"
```

Получится репозиторий с пакетом, `BUILDING.md`, `.gitignore` (IPSW, прошивки и дерево устройства Android туда не
попадут) и проверкой на каждый push. Заполните `board.dts` по [porting.md](porting.md): каждый ключ объяснён в
`arch/arm64/boot/dts/_template/board.dts`. Что телефон может включить дальше, скажет `tools/boards.py status mido`.
Потом закоммитьте и запушьте репозиторий туда, откуда его смогут склонировать другие.

## 3. Ядро и образ одной командой

```sh
make phone FROM=https://github.com/вы/mido.git
```

Это подтянет пакет (`tools/device.py add --from`), соберёт ядро и загрузчик и сделает **один образ** в
`build/images/mido/`:

| Файл | Что это |
| --- | --- |
| `mido.payload` | Загрузчик, ядро и аргументы загрузки в одном контейнере |
| `boot.img` | То же вместе с UEFI телефона (если сборка UEFI лежит в `firmware/mido/uefi/`) |
| `manifest.json` | Что вошло в образ: коммит, пакет, аргументы, SHA-256 каждого файла |
| `README.md` | Как его загрузить |

Другой набор аргументов: `make image DEVICE=mido DEFCONFIG=phone_look` (`tools/defconfig.py --list` перечисляет все).

## 4. Загрузка

С загрузчика телефона (`adb reboot bootloader`):

```sh
fastboot boot build/images/mido/boot.img
```

Ничего не записывается в память телефона: всё работает из RAM, перезагрузка возвращает Android. В QEMU тот же образ
загружается так же, как на телефоне: `tools/qemu-kernel.sh --image build/images/qemu-virt/qemu-virt.payload`
([images.md](images.md)).

Для прогонов с журналом и сводкой по частям есть `./bench`: он запускает наш загрузчик по USB и тоже ничего не
пишет в память телефона.

```sh
BENCH_BOARD=mido make ready     # всё ли на месте: ядро и загрузчик знают телефон и не старше исходников, «личность», малый образ
BENCH_BOARD=mido make day       # прогоны по порядку; журналы и summary.md в logs/day-ВРЕМЯ/
```

План дня и что должен сказать каждый прогон: [phone-test.ru.md](phone-test.ru.md).

## 5. Какой это iPhone для Darwin

Darwin определяет, на каком устройстве работает, по свойствам из дерева устройства iPhone: модель, экран,
вибромотор. Это данные Apple: в репозитории и в опубликованных образах их нет. Добавьте свои, из своего IPSW:

```sh
tools/image.py personalize build/images/mido/boot.img --identity DeviceTree.bin   # получится boot-mine.img
```

Вместо `DeviceTree.bin` можно передать сам IPSW: `--identity iPhone.ipsw --model n104ap` (n104ap — это iPhone12,1).
`DeviceTree.bin` оставляет рядом с IPSW `darwin/tools/unpack.sh`. Загрузчик возьмёт эти свойства из образа и напишет
в журнал `identity: from the payload (...)`.

## 6. Образ в репозиторий телефона

```sh
make publish DEVICE=mido REPO=../mido
cd ../mido && git add images && git commit -m "images: первый образ" && git push
```

Образ ляжет в `images/ВЕРСИЯ/`, а `images/README.md` перечислит все версии. Публикация откажет, если в образе есть
данные Apple, образ системы или файл не совпадает с манифестом. `boot.img` с UEFI телефона (там драйверы
производителя) кладётся только с `--with-uefi`, если вам можно его публиковать.

## 7. Работа над кодом

| Нужно | Команда |
| --- | --- |
| Какие тесты запускать после правки файла | `scripts/tests-for.py --changed` (по [MAINTAINERS](../MAINTAINERS)) |
| Один тест драйвера отдельно | `workbench/mutate.sh ИМЯ_test board/файл.c x x` (`scripts/tests-for.py` подскажет точную команду) |
| Заготовка нового драйвера | `scripts/new-driver.py ИМЯ` (драйвер, его тест, строка в `make check` и MAINTAINERS) |
| Проверить, что тест ловит поломку | `workbench/mutate.sh ИМЯ_test board/файл.c 'было' 'стало'` |
| Всё перед коммитом | `make check` |
| Исходники релиза | `scripts/release.sh 1.0` (архив, SHA256SUMS и заметки из CHANGELOG в `build/release/`) |

Куда какое изменение относится, расписано в [CONTRIBUTING.md](../CONTRIBUTING.md). План проверки на телефоне по
шагам: [phone-test.ru.md](phone-test.ru.md).
