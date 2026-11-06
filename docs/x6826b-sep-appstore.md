# SEP и App Store на X6826B

Задача — в `docs/x6826b-sep-appstore-agent-prompt.md`. Здесь — что найдено и проверено, разделами с датой.

## 1 ноября 2026: исходная точка

### Селекторы AppleKeyStore в iOS 16.5 (из самой библиотеки)

В iOS 16.5 libaks — это `/System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore` в dyld shared
cache образа (`ios165/20F66__iPhone10,3_6`), 284 экспортируемые функции. Номера взяты дизассемблированием: для каждого
вызова `IOConnectCallMethod` — непосредственное значение в `w1` (селектор) и `w3` (число входных скаляров) в той же
функции. Всего 126 мест вызова. Каждая экспортируемая функция сначала берёт соединение (`0x1f22c7cf4`, открывает
`IOService:/IOResources/AppleKeyStore`) и при неудаче возвращает `0xe00002bc`.

Главные для keybag и связки (адреса — в кеше, без сдвига):

| функция libaks | селектор | вход | выход |
|---|---|---|---|
| `aks_create_bag` (→ `0x1f22d38f8`) | 0x02 | 6 скаляров: тип, −1, **указатель** на код-пароль в памяти процесса, его длина, 0, 0 | 1 скаляр: описатель bag |
| `aks_save_bag` | 0x03 | 1 скаляр: описатель | структура до 0x8000 байт: сам bag |
| `aks_unload_bag` | 0x04 | 1 скаляр | — |
| `aks_set_system_with_passcode` (`aks_set_system` — с 0 в 4-м) | 0x05 | 4 скаляра | — |
| `aks_load_bag` | 0x06 | структура: байты bag | 1 скаляр: описатель |
| `aks_get_lock_state` | 0x07 | 1 скаляр | 1 скаляр |
| `aks_lock_device` | 0x08 или 0x20 (`csel`) | 1 скаляр | — |
| `aks_unlock_device` | 0x09 | 3 скаляра | — |
| `aks_wrap_key` | 0x0a | скаляры: описатель, класс; структура: ключ | 1 скаляр (класс), структура (обёрнутый) |
| `aks_unwrap_key` | 0x0b | 2 скаляра; структура | структура |
| `aks_unlock_bag` / `aks_lock_bag` | 0x0c / 0x0d | 3 / 1 скаляр | — |
| `aks_get_system` | 0x0e | — | 1 скаляр |
| `aks_change_secret_se` | 0x0f | 11 скаляров | |
| `aks_get_device_state` / `..._extended_` | 0x11 / 0x23 (через `0x1f22d7370`, селектор в `w1` от вызывающего) | | |
| `aks_get_bag_uuid` | 0x17 | 1 скаляр | 16 байт |
| `aks_get_configuration` / `aks_set_configuration` | 0x1f / 0x1e | | |
| `aks_generation` | 0x28 | 2 скаляра | |
| `aks_fairplay_wrap`, `aks_fairplay_generate_csk` | 0x68 | 1 скаляр | |

Полный список — `docs/x6826b-aks-selectors.txt`: `tools/dsc/aks-selectors.py /System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore` (нужен capstone). Inferno (`sep-sim.c`)
нумерует сообщения SEP (`sks`, EP 18), а не селекторы пользовательского клиента: это два разных пространства номеров,
и 0x01 «Create Keybag» SEP — не селектор 0x01 AppleKeyStore.

### Почему в образе нет `fairplayd`

Не потому, что её нет в IPSW. В листинге rootfs IPSW (`ios165/rootfs.slt`) она есть: `usr/sbin/fairplayd.H2`,
19 316 144 байта, задание `com.apple.fairplayd.H2` (Mach-службы `com.apple.fairplayd`, `com.apple.fairplayd.versioned`,
`com.apple.unfreed` — особый порт хоста 17). Её вырезает наш сборщик: `ios165/select-system.py`, `wanted_file`,
правило `usr/libexec/`, `usr/sbin/` → `size <= BIG_PROGRAM`, а `BIG_PROGRAM = 10 << 20` («daemons bigger than this
… are not on the way to the screen»). `rootfs.dmg` после распаковки удалён (`fetch.sh` без `KEEP`), так что достать
файл сейчас можно только новым скачиванием образа rootfs (4,7 ГБ с CDN Apple) — спрошено у пользователя. FairPlay
не реализуется и не обходится: можно только описать, что ей нужно от ядра.

### Сохранение `/private/var` на X6826B: его нет

- `runkit/mkimg.sh` (рецепт образа boot_a для X6826B) вырезает `root/bin/var_store` и задание
  `com.redwood.varstore` — место в разделе boot.
- И сам `var_store` (`userland/src/var_store.c`) пишет только в `/dev/rufs_darwin` — UFS-раздел `darwin` у POCO
  (Qualcomm). На X6826B накопитель — eMMC, драйвер `bsd/dev/redwood_emmc.c` «Read only, by construction»
  (`d_write = eno_rdwrt`, открытие на запись — `EROFS`).

То есть на X6826B keybag и связка не могут пережить перезагрузку, что бы ни делало хранилище ключей: нужен путь записи
на eMMC и место на нём (раздел, который не принадлежит Android и не нужен для загрузки). Это решение пользователя —
запись во флеш телефона.

### Прогон 423 (1 ноября): только наблюдение

Ядро — сборка 398: прежняя заглушка, плюс журнал каждого вызова по boot-arg `aks_log=1` (процесс, селектор, скаляры,
размеры, ответ; первые 24 вызова каждого селектора; первый — ещё и в строки для хоста). Образ — `runkit/mkimg-aks.sh`
(keybagd среди заданий, `keychain_test` оставлен и подписан через `tools/ldid-codesign.sh`, сценарий
`userland/rc.jobs.aks`; `runkit/` вне git, как и `runkit/mkimg.sh`). Аргументы — обычные для X6826B и `aks_log=1`.

Что спрашивали у AppleKeyStore за 300 с:

```
aks: first 0x0 by keybagd [] in 0 -> 0x0
aks: first 0x11 by backboardd [0] in 0 -> 0x0
aks: first 0x7 by powerd [0] in 0 -> 0x0
aks: first 0x23 by nanoregistryd [0] in 0 -> 0x0
```

и ещё SpringBoard (0x23), виджеты и Spotlight (0x00, 0x07). Только состояние; ни одного отказа; ни создания, ни
загрузки bag, ни обёртывания ключа.

- **keybagd стартует и не падает** (`13 (parent 1, state 2, flags 0x4004) /usr/libexec/keybagd` в начале и через
  две минуты), но только открывает соединение (0x00) и ждёт запросов.
- **Системный bag при загрузке грузит не задание keybagd, а boot-task launchd «keybag».** Список boot-task'ов
  вшит в `/sbin/launchd` (plist внутри файла): `keybag` — `/usr/libexec/keybagd --init`, `AllowCrash`,
  `PerformAfterUserspaceReboot`. При загрузке с RAM-диска launchd пропускает их все:
  `[Event: keybag] Skipping boot-task` (и `data-protection` — `/usr/libexec/init_data_protection`, `fips`,
  `mount-phase-*`).
- **Связка ключей на X6826B сейчас не работает вовсе, и не из-за ядра:** `keychain_test` получает −25291 на каждом
  шаге, а launchd пишет `failed lookup: name = com.apple.securityd, requestor = keychain_test[101], error = 3: No such
  process`. Образ X6826B собирается без `REST=1`, задания `com.apple.securityd` в нём нет (в README связка проверена
  на POCO и в QEMU, где оно есть).
- `keybagd --init` печатает в `/var/logs/keybagd_init.log` («IN MKB_INIT» … «DONE MKB_INIT») и пропускает работу, если
  устройство в режиме диагностики или «DEVICE HAS EPHEMERAL DATA VOLUME» (`os_variant_uses_ephemeral_storage`). Ядро
  берёт `hw.ephemeral_storage` из `/chosen/ephemeral-storage` дерева устройств (`bsd/kern/kern_mib.c:714`), а наши
  загрузчики этого свойства не ставят — гипотеза, что пропуска не будет; не проверено.

### Ключи (сборка 399–400, не запускалось)

`iokit/Kernel/MiniAppleKeyStore.cpp`, boot-arg `aks_keys=1` (без него — прежнее поведение):

- bag: случайный ключ устройства и 16 ключей классов (`read_random`), UUID; до 8 bag'ов сразу, описатель — номер,
  описатель 0 — системный bag (так его зовут libaks и securityd);
- 0x02 создать (без код-пароля; с код-паролем — отказ и строка в журнале), 0x03 сохранить, 0x04 выгрузить,
  0x05 сделать системным, 0x06 загрузить, 0x0e, 0x17;
- 0x0a/0x0b: AES key wrap (RFC 3394) ключом класса; при развороте проверяется целостность (чужой ключ —
  `kIOReturnNotPermitted`); ключ, обёрнутый прежней заглушкой (метка `HKwr`), читается по-прежнему;
- до того как есть системный bag, обёртывание отвечает `kIOReturnNotReady`.
- кто что может (по правам из подписи вызывающего, `IOUserClient::copyClientEntitlement` → miniamfi): bag'и — создать,
  отдать с ключами (0x03), загрузить, сделать системным, выгрузить — только с `com.apple.keystore.device` (он у
  keybagd); обернуть и развернуть ключ — ещё и с `com.apple.keystore.access-keychain-keys` (у securityd). Права взяты
  из подписей самих программ этого образа (`codesign -d --entitlements`). Остальным — `kIOReturnNotPrivileged` и
  строка `aks: NAME[PID] may not use selector 0x..`. Без этого любая программа могла бы унести ключи классов, развернуть
  чужие ключи связки или подменить системный bag (нашла автоматическая проверка безопасности коммита cba1a2a).
  Подмену самого файла `/private/var/keybags/*.kb` это не закрывает: bag не привязан к секрету устройства, которого
  на этом железе нет;
- Сохранённый bag (то, что keybagd пишет в `/private/var/keybags`) несёт ключ устройства как есть: аппаратного
  ключа (UID SEP) на MT6768 нет. Связка защищена от других программ, но не от того, кто прочтёт раздел данных.

Проверено: код обёртывания сходится с тестовым вектором RFC 3394 4.6 (на хосте, CommonCrypto как AES); ядро
собирается (сборка 401 — с проверкой прав). Не проверено: ни один прогон с `aks_keys=1` (424 остановлен до загрузки XNU по просьбе пользователя:
телефон занят под Wi-Fi).

### Что дальше (когда телефон освободится)

1. Прогон с `aks_keys=1 aks_log=1`, `userland/rc.launchd.aks` (`keybagd --init` до launchd, как boot-task) и
   `com.apple.securityd` среди заданий (уже в `runkit/mkimg-aks.sh`): создаёт ли `--init` системный bag, что пишет,
   проходит ли `keychain_test`.
2. Сохранение на X6826B: нужен путь записи на eMMC и место — решение пользователя.
3. App Store: нужен QEMU на этом Mac (не установлен; `qemu/run-ios.sh` под Linux) — решение пользователя.

## 3 ноября 2026: keybagd --init, системный bag и файл, который оставляет восстановление

Wi-Fi закончен, телефон снова доступен. adb и fastboot — из `~/Library/Android/sdk/platform-tools` (Homebrew на
Sonoma нет). Образы — `runkit/mkimg-aks.sh` (теперь обёртка над текущим `runkit/mkimg.sh`, а не его копия), аргументы
без `metal=1 camtl=1` и с `aks_log=1 aks_keys=1`.

### Прогон 468: «нет системного bag» — и keybagd перезагружает в recovery

`userland/rc.launchd.aks` запускает до launchd то, что на устройстве делает boot-task «keybag». Ядро — сборка 436.

```
aks: keybagd[6] sel 0x0 in [] struct 0/0 scalars out 0 -> 0x0
aks: keybagd[6] sel 0xe in [] struct 0/0 scalars out 1 -> 0xe00002d8
[Subsystem: nvram] [Event: write] keybagd with data size 0x32
[Subsystem: nvram] [Event: write] auto-boot with data size 0x5
FATAL KEYBAG ERROR: failed to load system bag
REBOOTING INTO RECOVERY MODE.
panic(cpu 2 caller 0xfffffff007a5ee60):  initproc failed to start -- exit reason namespace 2 subcode 0xf
```

Так и делает iPhone без системного bag. Телефон вернулся в Android сам: NVRAM у ядра порта в памяти, `auto-boot=false`
до флеша не доходит.

### Прогон 469: системный bag есть, а файла нет

Ядро (сборка 437) делало системный bag при первом вопросе:

```
aks: a system bag made, handle 1 (none had been loaded)
aks: keybagd[6] sel 0xe in [] struct 0/0 scalars out 1 -> 0x0
[Subsystem: nvram] [Event: delete] keybagd
FATAL KEYBAG ERROR: kb_load
REBOOTING INTO RECOVERY MODE.
```

### Что keybagd ждёт на самом деле (дизассемблирование keybagd этого образа, `otool -tV`)

libaks вшит в keybagd статически (`otool -L`: ни AppleKeyStore.framework, ни MobileKeyBag). Адреса — в keybagd.

- `load_devicebag` (`0x100012c40`): `aks_get_system`. Ответ **`0xe00002f0` (kIOReturnNotFound)** — «загрузить файл»:
  прочитать bag (`0x100001dfc`), `aks_load_bag` (`kb_deserialize`), `aks_set_system` (`kb_set`). Любой другой отказ
  — второй `aks_get_system` и, не получив, «failed to load system bag» (468). Если bag уже есть — файл читается всё
  равно (`0x100012d3c`), и без файла выходит «kb_load» (469).
- Файл — `/private/var/keybags/systembag.kb` (`"%s%s"` из `"/private/var/"` и `"keybags"`, затем `"%s/%s.kb"`);
  из словаря берётся ключ `KeyBagKeys` (`0x100001e98`, строки CFString — разобраны вручную по указателям в
  `__cfstring`).
- `KBLoadLoadKeyBagFile` (`0x100001ef4`): если у `IODeviceTree:/defaults` есть свойство `no-effaceable-storage`
  (`0x10000497c`: `IORegistryEntryFromPath`, `IORegistryEntryCreateCFProperty`), файл читается как обычный
  property list (`load_binary_dict`, `0x10000caa8`; ещё пробуется `%s.writing`), «with No Crypto». Иначе — «with
  Crypto»: ключом из Effaceable Storage (`AppleEffaceableStorage`, «Can't find EffaceableStorage kext!»), которого
  на MT6768 нет.

### Что сделано под это (сборка 438)

- Ядро при `aks_keys=1`: без системного bag `0x0e` отвечает `kIOReturnNotFound`, как ждёт keybagd; само bag не
  делает. В `/defaults` дерева устройств ставится `no-effaceable-storage` (узел уже есть: `mtkloader.c:882`).
- `userland/src/aks_restore.c` → `/redwood/bin/aks_restore` (подписан `aks_restore.entitlements`:
  `com.apple.keystore.device`, иначе ядро не даст сделать bag): если `systembag.kb` нет — bag (0x02), его байты
  (0x03), выгрузить (0x04), записать `{KeyBagKeys: <bag>}` двоичным plist через `.writing` и `rename`. Это то, что
  оставляет восстановление iOS; на X6826B, без сохранения `/private/var`, — каждую загрузку заново.
- `userland/rc.launchd.aks`: `aks_restore`, потом `keybagd --init`, потом launchd.

### Прогон 470: bag из файла загружен — и тут же выгружен

```
aks_restore: the system bag made (handle 1, 584 bytes), /private/var/keybags/systembag.kb written (648 bytes)
aks: keybagd[8] sel 0xe in [] struct 0/0 scalars out 1 -> 0xe00002f0
aks: keybagd[8] sel 0x6 in [] struct 584/0 scalars out 1 -> 0x0
aks: keybagd[8] sel 0x5 in [1,0,0,0] struct 0/0 scalars out 0 -> 0x0
aks: keybagd[8] sel 0x4 in [1] struct 0/0 scalars out 0 -> 0x0
FATAL KEYBAG ERROR: failed to load system bag
```

Формат файла подошёл: keybagd прочёл его без шифрования и отдал байты `aks_load_bag`. Но после `aks_set_system`
он сразу выгружает описатель, по которому загрузил, — а у ядра системным был этот же описатель. Сборка 439:
системный bag — копия в своей ячейке; выгрузка исходного описателя её не трогает, выгрузить сам системный нельзя
(`kIOReturnNotPermitted`), повторная загрузка того же bag не отдаёт описатель системного.

### Прогон 471: keybagd --init проходит, связка работает на настоящих ключах

```
aks: the system bag set from handle 1 (now handle 2)
****** DONE MKB_INIT ****
tolog: /usr/libexec/keybagd ended: exit 0
15 (parent 1, state 2, flags 0x4004) /usr/libexec/keybagd             (в начале и через две минуты)
aks: first 0xa by securityd [0,a] in 32 -> 0x0
aks: first 0xb by securityd [0,7] in 40 -> 0x0
keychain_test: delete before: -25300
keychain_test: add: 0
keychain_test: read back: 0, THE SAME BYTES
keychain_test: delete: 0
keychain_test: add "written by run of 1793738889": 0
keychain_test: read: 0, written by run of 1793738889
```

Что спрашивали у хранилища за прогон: SpringBoard 0x11 (12 раз) и 0x23 (7), securityd 0x0a (7), 0x0b (5), 0x07 (5),
0x0e, keybagd 0x0e, 0x06, 0x05, 0x04, `aks_restore` 0x02, 0x03, 0x04, powerd и расширения 0x07. Ненулевой ответ один —
ожидаемый `0xe00002f0` в `keybagd --init` («bag ещё нет, загрузи файл»). Строк `may not use selector` нет: права
keybagd и securityd совпали с ожидаемыми.

securityd обёртывает 32-байтовые ключи элементов ключами классов (0x0a, класс 10 — `cku` по нумерации классов открытого Security, `keyclass_t`; «после первой
разблокировки, только это устройство») и разворачивает 40-байтовые (0x0b, класс 7 — `ck`). Это RFC 3394 ключами из
системного bag, а не прежнее обратимое преобразование.

### Где это относительно «готово»

| условие | состояние |
|---|---|
| `keybagd` запускается и не падает | да: `--init` exit 0, демон жив весь прогон (471) |
| системный keybag создаётся | да: `aks_restore`, как восстановление (470, 471) |
| системный keybag загружается после перезагрузки | **на X6826B — нет**: `/private/var` в памяти, сохранения нет (см. 1 ноября). В каждой загрузке `aks_restore` делает новый bag, а связка начинается пустой |
| связка читает свои записи после перезагрузки | **на X6826B — нет**, по той же причине; в пределах загрузки — да (471) |
| нет отказов на селекторы, которые система вызывает | да (471) |

`aks_keys=1` — пока не по умолчанию: без `aks_restore` и `keybagd --init` (образы других работ их не запускают)
системного bag нет, обёртывание отвечает `kIOReturnNotReady`, и связка — а с ней HTTPS через securityd и trustd —
перестала бы работать.
