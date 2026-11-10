# Bluetooth на Infinix Hot 20 (X6826B, MT6768, MT6631): от контроллера до `bluetoothd` iOS 16.5

Прогоны 601–711 (8–9 ноября 2026). Контроллер MT6631 (в CONNSYS, как Wi-Fi: `wifi-x6826b.md`) включается ядром;
настоящий `bluetoothd` из iOS 16.5 говорит с ним HCI по своему родному транспорту и **доходит до `PoweredOn`**:
приложение через CoreBluetooth видит `state 5`, сканирование доходит до радио, настоящие LE-объявления соседних устройств
приходят в `bluetoothd` и **доходят до приложения**: `bt_test` (скан без фильтра) показывает устройства с именем, RSSI,
данными производителя и сервисов (прогон 711: шесть устройств за десять секунд, одно из них с именем).

## Путь от радио до приложения

```
CoreBluetooth в приложении ──XPC── bluetoothd (iOS 16.5, настоящий)
   bluetoothd ──ioctl/getsockopt── kern_control "com.apple.uart.sk.bluetooth"        redwood_connsys.c (bt_ctl_*)
   bluetoothd ──os_channel_create── Skywalk kernel pipe "redwood_bt"                 redwood_btsk.c
   ядро: H4-пакеты ⇄ STP, канал 0 (BT_TASK_INDX) ⇄ BTIF ⇄ CONNSYS ⇄ MT6631          redwood_connsys.c (bt_bridge)
```

- **Включение** (`connsys_bt=1`, шаг 20 в `redwood_connsys.c`): после WMT (шаги 1–11) `WMT_FUNC_CTRL` `01 06 02 00 00 01`
  (тип 0 — BT, 1 — вкл), ответ `02 06 01 00 00`. Перед ответом прошивка шлёт свои события опкода 0x16 — они не ответ.
  Затем сырой HCI в кадрах STP канала 0 (`connsys_stp_task`). Контроллер: HCI 5.2, производитель 0x0046 (MediaTek), адрес
  по умолчанию `00:00:46:76:63:01`, ACL 1021×696, LE 251×8 (прогон 602).
- **Узлы и транспорт** (`connsys_bt=2`): `/dev/cu.bluetooth`, `/dev/uart.bluetooth`, `/dev/btwake`, `/dev/btreset`,
  `/dev/btpoweroff` (читаются/пишутся, терминальные ioctl принимаются — этого не хватило: транспорт `bluetoothd` другой),
  управляющий сокет ядра `com.apple.uart.sk.bluetooth` (`CTL_FLAG_REG_SOCK_STREAM`: он подключается SOCK_STREAM) и
  Skywalk kernel pipe по образцу `bsd/skywalk/nexus/kpipe/nx_kpipe_loopback.c`: `getsockopt 0x40105412` отдаёт UUID
  канала, `os_channel_create` открывает его, TX-кольцо пользователя — пакеты H4 для контроллера, RX-кольцо заполняет
  `sync_rx` из очереди от контроллера, один пакет в слоте.
- **Транспорт `H4BC` — единственный рабочий у `bluetoothd`** (прогон 612): драйвер `H4` в его таблице не находится.

## Что нужно было от окружения

| Что | Как найдено | Что сделано |
| --- | --- | --- |
| `No bluetooth on this device.` (603) | дизассемблер `FindLocalMacAddress`: адрес из `BT_DEVICE_ADDRESS` (обнуляется), из IORegistry-узла `bluetooth`/`marconi-bt` (узлы дерева устройств службами не становятся, 606) или из boot-arg | boot-arg `wlan.debug.generate-mac=1`: `bluetoothd` делает случайный адрес сам (607) |
| `bluetoothd` падает на identity root | связка ключей без `keybagd`/`securityd`/`aks_keys=1` | образ `rc.launchd.metalaks`: `aks_restore`, `keybagd --init`, потом Metal; задания `keybagd`, `securityd` (608) |
| `BlueTool timed out running boot script` | BlueTool выбирает прошивку Broadcom по `kMGQWifiVendor`, файлов `.hcd` в образе нет | служба `com.apple.BlueTool` внутри настоящего процесса BlueTool (`libredwood`, `bluetool_serves`): `result 0`, `manufacturer`, `firmware` (611). Отдельный бинарник нельзя: `launchd` паникует на не-платформенном exec (610) |
| `Local address` и маска команд | контроллер не ставит LE-биты в `Read_Local_Supported_Commands` (октеты 24–28) | ядро добавляет биты Core 5.2 (616, 617) |
| `Write_Default_Erroneous_Data_Reporting` (0x0c5b) | контроллер: Unknown command, `bluetoothd` считает это неисправностью | отвечаем как на выполненную (619) |
| вендорные команды Apple/Broadcom (OGF 0x3f) | `Unknown command` на каждую | `Command Complete`, статус 0 (628) |
| рестарты `bluetoothd` каждые ~17 с | `_exit(1)` в `bluetoothd+0x90238`, причина `reason=3602` (`VSC stall detected`) — зависла вендорная команда, ей нужны данные в ответе | см. ниже |

### Какая вендорная команда что просит (по именам из `Sending: BD_VSC_…` против проводов, прогон 659 и далее)

Ядро (`bt_quirks`) переписывает ответы контроллера «Unknown command» на `Command Complete`:

- по статусу — всё, кроме перечисленного: `0xfce9` (BD_VSC_LE_META_ENA_FEATURES), `0xfce2` с подкомандами `0x0b`
  (CHANGE_LE_SETTINGS), `0x1c` (SET_MIN_AFH_LE_CHANNELS), `0x26` (SET_SAR_LOCATION), `0xfd47`
  (ENABLE_EXTENDED_DUPLICATE_FILTER), `0xfd4e`. **Данные на 0xfce9 — ломают очередь** (650–658: следующая команда не уходит);
- байт данных после статуса — `0xfd48` (`bcm_vsc.c:642 NumReadBytesAvail >= 1`);
- 16 нулей — `0xfce2` с подкомандой `0x15` (**BD_VSC_OLYMPIC_ANTENNA_PREF_B**: шесть байт `BT_VSCAntPrefParams_t`
  после заголовка `15 00 00 00`; её ответ — поток байт) и `0x36` (BT_VSC_OLYMPIC_SET_RETAIN_LE_SCAN_DUPLICATES_ON_START);
- 64 байта (первый — `0x51`) — `0xfd1b 0x51` (**BD_VSC_FACTORY_CAL_READ_TABLE**, чтение таблицы).

После этого очередь доходит до конца: `Power state change`, `managerState 5`, `Update Dynamic Store with state On`,
`CBMsgIdStateUpdated` во все сессии, и приложение получает `state 5` (прогон 670).

### Как это искали (инструменты остались в `libredwood`)

`bluetoothd` исчезает с консоли вместе с процессом, поэтому внутри него (`userland/src/inject.c`, имя программы
`bluetoothd`, и только в нём):
- перехват `_os_log_impl`/`_os_log_error_impl`/`_os_log_fault_impl` пишет его сообщения о состоянии
  (до запуска стека — не пишет, после включения — только скан) в буфер `kern.redwood.host_keep`, который ядро добавляет в
  конец лога: «`bt[pid] Sending: …`», «`completed with status …`», «`Bluetooth error - restarting {reason, context}`»;
- перехват `_exit` (там, откуда `bluetoothd` уходит: `+0x90238` и `+0x902a8`) возвращается из его функций перезапуска, как их
  эпилог, если причина — зависание VSC (`reason=3602`); `bluetoothd` остаётся жить, а не перезапускается;
- `bt_test` (`userland/src/bt_test.m`, права `kTCCServiceBluetoothAlways`): состояние центрального менеджера и скан.

Расшифровка: `3602` — «VSC stall detected»; 621 и 628 — OI_STATUS «команда не поддерживается/по таймауту»; формат
ответа VSC определяет `_BCM_ByteStreamEventParser` (`BYTESTREAM_CB` — нужны данные, `COMPLETE_CB` — только статус).

## Образ и сборка

- Раздел загрузки (32 МБ) был полон: `libOSMesa.8.dylib` (11 МБ) переехал в системный образ
  (`/usr/lib/redwood/libOSMesa.8.dylib`), в образе загрузки — ссылка (`OSMESA_EXT=1` в `runkit/mk.sh`, `userland/build.sh`
  кладёт копию в системный образ; образ без `OSMESA_EXT` остаётся как был). Системный раздел `xnusys` перепрошит один раз
  (`tools/system-flash.sh`, 8 ноября); образ 30,4 МБ из 32.
- Локальный (`runkit/` вне git) запуск: `runkit/bt-run.sh` (Metal + Wi-Fi + хранилище ключей + камера-драйвер + Bluetooth).
  Переменные: `RUN`, `SECS`, `BTLOG="com.apple.bluetoothd"` (подробный os_log `bluetoothd`; без — тихо), `OSMESA_EXT=1`,
  `NOMEDIA=0`. Задание — `runkit/rc.jobs.bt`.
- Boot-args: `connsys_step=17 connsys_wake=0 connsys_bt=2 aks_keys=1` (`wlan.debug.generate-mac=1` больше не нужен, см. адрес ниже).

## Почему приложение не получало устройства (прогоны 678–711) и что сделано

Объявления доходили до `bluetoothd` и до его разбора (`LE_HCIEventHandler_AdvertisingReport`, `bluetoothd+0x199348`), счётчик
«advertisements delivered» оставался 0. Дальше по цепочке (адреса `bluetoothd` iOS 16.5, без смещения загрузки):

1. `+0x56aa38` собирает устройство, `+0x57646c` обновляет его запись и обходит **записи скана** (`std::map` в объекте стека, `+0x1c8`):
   у каждой записи («агента» сессии) своё условие, `+0x58edc4` отвечает, нужен ли ей этот отчёт; только тогда по наблюдателям идёт блок
   `+0x579418`, и в `+0x243734` растёт счётчик доставки сессии.
2. Записей две: собственная записи `bluetoothd` (тип `0x20`, правила под устройства Apple, интервал 300/30 мс) и запись приложения (тип `1`). Для
   записи типа 1 `+0x58f2a0` → `+0x58eb7c` отвечает «да» только если **сессия на переднем плане** (байт `+0x18` сессии CBDaemon, третий
   аргумент конструктора записи `+0x58e798`) **и есть список сервисов** (`scanForPeripherals(withServices: nil)` его не даёт).
3. Передний план `bluetoothd` узнаёт у `BKSApplicationStateMonitor` по пакету приложения. У программы без пакета (`bt_test`, всё, что запущено
   из оболочки) состояния нет; в логе `sessionState:"unknown"`.

Исправление — в `libredwood` внутри `bluetoothd` (`userland/src/inject.c`, `bt_hooks`, `bt_entry_fix`): конструктору записи (`+0x58e798`)
ставится переход на заглушку, которая **делает передний план равным 1** и записи типа 1 **без списка сервисов превращает в запись типа
`0x20`** с байтом конфигурации `+1` (то есть «смотреть всё»; так устроены собственные сканы демона). Приложению с пакетом и сервисами это ничего не меняет.
Подмена одна, всё остальное в `bluetoothd` настоящее.

### Диагностика (загрузочный аргумент `bt_diag=1`)

Без аргумента стоит только исправление. С `bt_diag=1` (`EXTRA_ARGS="bt_diag=1"` у `runkit/bt-run.sh`) в `bluetoothd` ставятся:
- переходы на двадцать с лишним функций стека с записью аргументов и результатов в `kern.redwood.host_keep` (`bt-hook N`, `bt-ret N`, `bt-rx`);
- трасса вызовов: **13 тысяч функций** `bluetoothd` между `0x100100000` и `0x1005e0000` получают тот же шестнадцатибайтный переход
  на копию шаблона, который пишет номер функции в кольцо; кольцо читается на седьмом отчёте (порядок первых вызовов за три секунды).
  Найти по ней, какие функции вызывались, а какие нет, оказалось самым быстрым способом разобраться в закрытом коде;
- `host_keep` хранит первые 64 КБ и молча отбрасывает остальное; пока в нём строки старта, строки отчётов теряются. Из-за этого
  в прогонах 678–699 казалось, будто `bluetoothd` перестаёт разбирать отчёты после двух секунд скана. Это неправда (прогон 704, `bt-rx`).


## Settings → Bluetooth, поиск, соединение, пара (прогоны 735–751)

- **Settings не присоединялся**: `BluetoothManager` -> `BTSessionAttachWithQueue` отвечал ошибкой 9, `-[BluetoothManager init]` возвращал nil, переключатель был серым.
  Причина (MobileBluetooth `+0x89d88c`): после `MGGetBoolAnswer("bluetooth")` и `IsSimulator` функция ищет в реестре IOKit службу `marconi-bt`, а потом `bluetooth`. В дереве
  устройств этого телефона их нет. Ядро теперь создаёт службу «bluetooth» (`iokit/Kernel/RedwoodBluetooth.cpp`, вызывается из `connsys_bt_start`, когда контроллер ответил
  на шесть команд HCI). После этого `bluetoothd` получает `kCBMsgIdSessionAttach` (`com.apple.Preferences-MBF-…`), переключатель зелёный, панель показывает «Other Devices».
- **Классический поиск** работает: Inquiry (`01 01 04`, GIAC, 5 × 1,28 с), расширенные результаты (`04 2f`), Inquiry_Complete; устройства попадают в список Settings.
- **Соединение**: нажатие на устройство даёт `kCBMsgIdDeviceConnectServicesMsg` → Create_Connection (`01 05 04`) → ACL-соединение (прогон 751, handle 0x33), L2CAP,
  SDP-обход настоящих записей сервисов устройства. Дальше идёт пара: Authentication_Requested → Link_Key_Request (отрицательный ответ) → IO_Capability_Request/Response →
  `User_Confirmation_Request` с числом на сравнение (`04 33`). Оно остаётся без подтверждения, и Settings через некоторое время пишет «Pairing Unsuccessful».
  Подтверждение нужно с обеих сторон (окно «Pair» в Settings и устройство); пока не пробовалось: устройство «W2» чужое, парить его без ведома владельца не нужно.
- Ядро теперь пишет в лог весь обмен HCI на 30 с после Create_Connection (`bt_dump`, до 500 строк), ACL в обе стороны.
- Остаётся: подтверждение пары с настоящим устройством, ключ связи (хранилище ключей есть), профили (A2DP/HFP/HID) и звук.

## Адрес (прогон 752)

Адрес постоянный: служба «bluetooth» в реестре несёт `local-mac-address` (адрес Wi-Fi + 1 в последнем байте, `RedwoodBluetoothAddress` из `redwood_net.c`),
`bluetoothd` берёт его сам (`FindLocalMacAddress`). `wlan.debug.generate-mac=1` больше не нужен.

## Имя устройства (прогоны 760–762)

Панель Settings писала «This iPhone is discoverable as “(null)”», а `bluetoothd` называл контроллер «iOS» (`Write_Local_Name`, `01 13 0c`: `69 4f 53`).
`BluetoothSettings` (`+0x207a14c70` в кэше 16.5) берёт имя у `SCDynamicStoreCopyComputerName(NULL, NULL)`, то есть из настроек системной конфигурации
(`System/System/ComputerName`); у восстановленного iPhone оно записано при настройке, здесь его не было.
Теперь его пишет `net_test` (`device_name` в `userland/src/net_test.m`, запускается заданием `com.redwood.wifi` рядом с ожиданием радио, или `net_test name`):
`SCPreferencesSetComputerName` + commit + apply, и только после того, как `configd` сам создал конфигурацию (`CurrentSet`), иначе файл из одного имени
был бы принят за готовую конфигурацию без сетевых служб. Имя по умолчанию «iPhone» (так отвечает MobileGestalt), загрузочный аргумент `devname=ИМЯ` задаёт другое.
Прогон 762: `bluetoothd` сам увидел изменение и послал второй `Write_Local_Name` с «iPhone», панель пишет «discoverable as “iPhone”», Wi-Fi остался подключён.
`net_test` должен быть в образе (в локальных прогонах `KEEP="net_test"`; в прогоне 761 его не было, и имя не появилось).

## Что не сделано

1. **Подтверждение пары.** Обмен доходит до `User_Confirmation_Request`; подтвердить число нужно на обеих сторонах, это не пробовалось (рядом было только чужое устройство).
   Поэтому не проверены: ключ связи и его хранение, повторное соединение.
2. **Профили и звук.** A2DP/HFP/HID не проверены; для A2DP нужен звук, которого у телефона под XNU пока нет.
3. **LE GATT.** Скан приложению доставляется (прогон 711), соединение LE и чтение характеристик не пробовались.
4. Настоящее приложение с пакетом (из `SpringBoard`) в CoreBluetooth не проверено, только `bt_test` и Settings.
5. Режим сна и coex с Wi-Fi не тронуты. Wi-Fi и Bluetooth вместе работают (прогон 753), измерений нет.
6. Остальные вендорные команды Apple: ответы подобраны только для пройденных путей (старт, скан, inquiry, соединение).
