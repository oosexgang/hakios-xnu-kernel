# Ядро HakiOS

Ядро Apple XNU (`xnu-12377.121.6`, ядро Darwin 26 и 27) и всё, что нужно, чтобы запустить Darwin на телефонах на
Snapdragon и в QEMU: наш загрузчик, наши драйверы и замены закрытых частей Apple.

Ничего от Apple в репозитории нет: ни прошивок, ни IPSW, ни образов системы, ни скриншотов. Всё это собирается на
вашей машине из ваших же файлов. English: [README.md](README.md).

## Где проект сейчас

| Что | Состояние |
| --- | --- |
| Darwin 27.2 (24B119, файлы iPhone12,1) | Домашний экран на Renoir (Snapdragon 780G) из RAM, отрисовка программная (`gpu=0`); в QEMU — экран блокировки |
| Darwin 26.6.2 (23G90) | Домашний экран на Renoir, впервые 31 октября 2026 |
| Каждый драйвер | Что он делает и насколько привязан к одному телефону: [drivers.md](Documentation/drivers.md); что телефон может включить дальше: `tools/boards.py status ИМЯ` |
| Новое, на телефоне ещё не проверено | Bluetooth для Darwin (`bt=2`, [bluetooth.md](Documentation/bluetooth.md)), свой драйвер UFS в `phone_perf`, один образ на телефон ([images.md](Documentation/images.md)) |
| Пока нет | Всего, что Darwin рисует через GPU (иконки приложений, Liquid Glass), пока GPU выключен ([metal-glass.md](Documentation/metal-glass.md)) |

«Собирается», «проходит тесты на хосте», «работает в QEMU» и «видели на телефоне» — четыре разных утверждения; у
каждого пункта выше указано, какое из них.

## Свой телефон

Ядро не привязано ни к одному телефону: всё, что относится к устройству, лежит в его пакете (`board.dts`,
`soc.dtsi`), а драйверы берут данные оттуда. Пакет может жить в вашем собственном репозитории:

```sh
tools/device.py init ../мой-телефон --name ИМЯ --soc sm7325    # репозиторий телефона
make phone FROM=https://github.com/вы/мой-телефон.git           # пакет, ядро, загрузчик и один образ
fastboot boot build/images/ИМЯ/boot.img                         # из RAM, в память телефона ничего не пишется
```

По шагам: [Documentation/quickstart.ru.md](Documentation/quickstart.ru.md). Подробно о переносе на новый телефон:
[Documentation/porting.md](Documentation/porting.md).

## Проверка на телефоне

`scripts/phone-day.sh` прогоняет все прогоны по порядку, после каждого смотрит журнал (`tools/logcheck.py`) и
складывает журналы со сводкой в одну папку — её и присылайте. План по шагам:
[Documentation/phone-test.ru.md](Documentation/phone-test.ru.md).

## Где что лежит

| Папка | Что |
| --- | --- |
| `arch/arm64/` | Машина: загрузчик (`boot/`), платы (`boot/dts/`), defconfig'и прогонов (`configs/`) |
| `drivers/` | Драйверы (`board/`) и замены расширений ядра Apple (`iokit/`) |
| `kernel/` | Выпуск Apple, патчи к нему и инструменты сборки ядра |
| `darwin/`, `userland/` | Пользовательское пространство Darwin: образы и вставляемые библиотеки |
| `tools/`, `workbench/` | Инструменты и тесты, которым не нужен телефон |
| `scripts/` | То, что запускает человек: настройка машины, день с телефоном, новый драйвер, выпуск исходников |
| `samples/` | Заготовки: репозиторий телефона, свой defconfig |
| `MAINTAINERS` | Какие файлы, тесты и документы у каждой части проекта |

Карта целиком: [Documentation/layout.md](Documentation/layout.md). Что нового: [Documentation/whats-new.ru.md](Documentation/whats-new.ru.md).

## Как предложить изменение

`make check` должен проходить. `scripts/tests-for.py --changed --run` прогоняет тесты того, что вы изменили. Один
коммит — одна тема; в теме сказано, что затронуто, а в тексте — зачем. Ничего от Apple, производителя телефона или
поставщика не коммитится (`tools/check-publishable.sh` проверяет). Подробно: [CONTRIBUTING.md](CONTRIBUTING.md);
об уязвимостях: [SECURITY.md](SECURITY.md).

## Лицензия

Наша работа под MIT (`LICENSES/preferred/MIT`); то, что расширяет ядро Apple и пришло из HakiOS, — под APSL 2.0
(`LICENSES/other/APSL-2.0`). Список в [NOTICE.md](NOTICE.md).
