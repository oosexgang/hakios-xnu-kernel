# Metal

Разбор: что в iOS 16.5 просит Metal, где проходит граница, которую пришлось бы реализовать, и
стоит ли её переходить. Всё, что ниже, проверено по извлечённой системе
(`ios165/rootfs`, кэш `ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64`), по исходникам порта и
по журналам прогонов; адреса — невыровненные (unslid) адреса этого кэша.

Коротко, в четырёх пунктах:

- **Граница узкая.** Служба класса `IOAcceleratorES` в ядре порта уже есть (нарочно пустая), так что
  не хватает двух строковых свойств на ней и бандла с подклассом приватного `_MTLDevice`.
- **За границей не драйвер, а компилятор** — но стена оказалась ниже, чем я написал сначала.
  Шейдеры приходят в AIR, готового читателя которого в мире нет; **однако как формат AIR почти не
  сопротивляется** (обычный битовый код LLVM, 456 модулей системы из 456 читает готовый `llvm-dis`).
  Измерено в разделе 9, и вывод там: «игры» — твёрдое нет, «один системный шейдер на нашем GPU» —
  достижимо.
- **Половина потерь — «пока», а не «нельзя».** У CoreImage в iOS 16.5 есть полный задний конец на
  OpenGL ES, у движка CoreML — 127 процессорных ядер и ни одного металлического. Разбор — раздел 7.
- **Первый шаг оказался уже сделанным** (раздел 2), поэтому сделан другой: `metal_test`
  (`userland/src/metal_test.m`), который наконец отвечает, что система про Metal думает, — и первым
  же запуском исправил два моих утверждения.

## 1. Как iOS находит Metal

Вся процедура — пять шагов, и ни один из них не спрашивает ни про возможности устройства, ни про
что-либо ещё, кроме реестра IOKit.

1. `MTLRegisterDevices` (`Metal`, 306.6.4, `0x181b115ec`) берёт `IOMainPort`, затем
   `IOServiceMatching("IOAcceleratorES")` — строка `0x181b74481` — и
   `IOServiceGetMatchingServices`. **Это имя класса и есть всё, чем GPU находится**; других путей
   нет.
2. Для каждой найденной службы вызывается `getMetalPluginClassForService` (`0x181a39ba4`). Она
   читает `IORegistryEntryCreateCFProperty(служба, "MetalPluginName")` и проверяет, что это
   `CFString`.
3. Из ответа складывается путь: `"/System/Library/Extensions"` (`0x181b7441b`) + имя +
   `".bundle"` (`0x181b74436`), и бандл открывается `[NSBundle bundleWithPath:]` (`0x181bc1820`).
4. Читается второе свойство, `"MetalPluginClassName"`; класс берётся
   `[bundle classNamed:]` (`0x181bc1920`), а без этого свойства — `[bundle principalClass]`
   (`0x181bca6c0`). Затем `[класс isSubclassOfClass:_MTLDevice]` (`0x181bc79e0`, ссылка на класс —
   `0x1dcf08f68`): **не подкласс приватного `_MTLDevice` — выброшен**.
5. `[[класс alloc] initWithAcceleratorPort:служба]` (`0x181bc6320`) — и этот объект есть устройство.

Код шагов 2–4 Apple заодно выписала словами: в `AGXGPURawCounter` лежит текст сорвавшегося
`assert` (`0x207d22fff`):

```
((cfMmetalPluginName = (CFStringRef)IORegistryEntryCreateCFProperty(acceleratorPort,
    CFSTR("MetalPluginName"), NULL, 0)) != NULL) && CFStringGetCString(...)
```

На этом телефоне (iPhone10,3, A11) плагин — `/System/Library/Extensions/AGXMetalA11.bundle`:
`CFBundleExecutable` `AGXMetalA11`, `NSPrincipalClass` `AGXPrincipalDevice`, версия 227.6.3; вход —
`-[AGXA11Device initWithAcceleratorPort:]`. Сам исполняемый файл лежит в кэше dyld, а не на диске:
в бандле только `Info.plist`, таблицы счётчиков и девять `.metallib`.

`MTLCreateSystemDefaultDevice` (`0x181a34b3c`) выдаёт одно устройство из этого списка под
`dispatch_once` (сторож `0x1e1efe9b8`, блок `0x181a3442c`, массив `0x1e1efe9a0`). **Пустой список —
ответ, данный один раз и сохранённый до конца процесса, молча**: ни строки в журнал, ни кода
ошибки, ни причины. В iOS 16.5 второго способа спросить нет: `MTLCopyAllDevices` объявлен
`API_UNAVAILABLE(ios)` и в двоичном файле отсутствует.

## 2. Что здесь есть вместо этого

Здесь легко ошибиться, и я ошибся, пока не запустил `metal_test`: можно подумать, что служб класса
`IOAcceleratorES` в порте нет вовсе и итератор из шага 1 приходит пустым. **Это не так.** Порт
сам такую службу публикует, и шаг 1 проходит:

```
metal_test:   IOAcceleratorES "sgx": MetalPluginName (none), MetalPluginClassName (none)
```

Её делает `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (строки 1471–1493) — пустой `IOAcceleratorES`
с именем «sgx», и в самом ядре порта написано, зачем:

> The graphics accelerator. There is none, but CoreAnimation waits half a minute for a service of
> this class to appear before it looks for a GPU at all (once per process). An accelerator without
> the properties that name its Metal and OpenGL ES drivers is passed over by both, which leaves
> CoreAnimation with its software renderer, as before.

То есть **шаг, который напрашивается как «первый» — сказать iOS правду, чтобы она пошла работающей
дорогой, — в порте уже сделан, и сделан именно так**: служба есть, чтобы CoreAnimation не ждал
полминуты, и нарочно без двух свойств, чтобы и Metal, и OpenGL ES прошли мимо. Цепочка из раздела 1
обрывается не на шаге 1, а на шаге 2.

Adreno же отдаётся userland'у отдельно и под своим именем: `iokit/Kernel/RedwoodGPU.cpp`
(строки 289–307) регистрирует `RedwoodGPU : IOService`, `setName("RedwoodGPU")`,
`kIOUserClientClassKey = "RedwoodGPUUserClient"` — это то, с чем говорит freedreno из Mesa, и к
`IOAcceleratorES` оно отношения не имеет. Причём `RedwoodGPUStart()` зовётся из
`bsd/dev/redwood_adreno.c` (строка 3858) только когда GPU действительно поднялся, так что в QEMU
`metal_test` и не нашёл ничего (`what the registry has that is a GPU: none`).

**Что из этого следует для раздела 4.** Границу стерегут не IOKit и не ядро: класс нужной службы в
ядре уже есть, имя уже правильное. Не хватает ровно двух строковых свойств на этой службе и бандла,
на который они указывают. Это делает вариант (А) ближе, чем кажется, — и ровно настолько же не
отменяет стены, о которую он разбивается (компилятор AIR).

Ответ же про само Metal порт даёт правдивый: `MTLCreateSystemDefaultDevice() = nil`, молча. И
никакого бита возможностей, которым можно было бы соврать в ту или другую сторону, не существует —
`metal_test` спросил и `DeviceSupportsMetal` у MobileGestalt: такого ключа нет (`(none)`).

## 3. Кто просит Metal и что с ними происходит

Metal загружают **153 образа кэша** (`ipsw dyld imports … Metal`). Из тех, до которых порт
доходит:

| Кто | Что просит | Что сейчас |
| --- | --- | --- |
| `QuartzCore` — оконный сервер | `CA::WindowServer::EAGLServer::renderer()` (`0x1882e2548`) **сначала** пробует `CAMetalContextCreate()` (`0x18837b718`) и `CA::OGL::new_metal_context()` (`0x18847efcc`), и только если рендерера так и нет — путь EAGLContext/OpenGL ES | Работает без Metal: за `EAGLContext` стоит `userland/src/eagl.c`. При отказе обоих путей CoreAnimation пишет `CoreAnimation: Failed to initialize Metal or OpenGLES contexts!` — в журналах порта ровно **один раз за прогон**, потому что `inject.c` (`my_renderer`) запоминает отказ; без этого попытка повторялась бы каждый кадр |
| `CoreImage` | `MTLCreateSystemDefaultDevice`, затем `.metallib` ядер фильтров | **Есть полный задний конец на OpenGL ES** — см. раздел 7; есть и строка `No Metal renderer available.` (`0x194c477a5`) |
| `CameraUI`, `CameraEffectsKit`, `CMPhoto`, `CMCapture`, `CameraColorProcessing`, `HDRProcessing`, `RawCamera`, `Portrait`, `AppleDepth`, `DepthProcessorV1` | устройство, очередь, текстурный кэш, свои `.metallib` | `Failed to create default Metal device` (`CameraUI`, `0x1cde2bd26`), `"Error: Metal device must not be NULL"` (`CameraEffectsKit`, `0x1ff99168c` — это `assert`), `Cannot create default metal device` (`AppleDepth`, `0x1e747cd48`) |
| `AppleCVA`, `AppleCVAPhoto`, `CoreAppleCVA`, `Vision`, `MediaAnalysis`, `Espresso`, `MLCompute`, `MetalPerformanceShaders*` | то же | `AppleCVA` (`0x1beeb40cd`): `Assertion failed: device = (id<MTLDeviceSPI>)MTLCreateSystemDefaultDevice()`; `AppleCVAPhoto` (`0x1af45f0c7`): `MTLCreateSystemDefaultDevice() is nil`; `Espresso`: `Metal device not found.` |
| `WebCore`, `WebKit`, `WebGPU`, `libANGLE-shared.dylib` | WebGL и WebGPU идут через Metal | Сам Safari работает: страницы рисует CoreGraphics |
| `SpringBoardFoundation`, `SpringBoardUIServices`, `WallpaperKit`, `PosterBoard`, `PaperBoardUI` | эффекты экрана блокировки | Экран блокировки рисуется; часть эффектов — нет |
| `SceneKit`, `SpriteKit`, `RealityKit`/`CoreRE`, `ARKitCore`, `AssetViewer` | всё | Ничего не получают |

**Главное наблюдение: отказ Metal нигде не слышен.** Во всех 112 журналах прогонов
(`darwin-redwood-sileo2/logs`) единственные строки про Metal — наши собственные две
(`no GPU to draw with…`, `SpringBoard: no GPU draws…`), а в журналах с включённым GL —
одна строка CoreAnimation за прогон. Тринадцать тысяч упоминаний `MTLCompilerService` в журналах к
Metal отношения не имеют: это launchd заводит заглушку XPC-службы в домене каждого процесса,
который слинкован с Metal (`created the implicit endpoint upfront` / `draining messages`), а не
запуски компилятора.

Что порт теряет на отрисовке окон, измеримо иначе: у рендерера OpenGL ES в QuartzCore нет шейдеров
примерно для половины из девяноста «функций» CoreAnimation, тогда как у Metal есть все. На деле
CoreAnimation спросил и получил отказ только по двум из них — 40 `sb3` и 60 `p3c`; остальное, чего
не хватает, он рисует вовсе без программы, а три такие функции плюс уменьшение в восемь раз
`eagl.c` рисует своими программами (см. «GPU» в README). Полную перепись даёт `glcensus=1`; в
журналах, которые есть, он не включался.

## 4. Чем пришлось бы быть реализации

Граница узкая, а то, что за ней, — нет. Размеры (из кэша этого же телефона):

| Что | Байт | Символов | Методов ObjC | Классов |
| --- | --- | --- | --- | --- |
| `AGXMetalA11` — драйвер, который пришлось бы заменить | 2 578 880 | 5 798 | 1 158 | 53 |
| `_MTLDevice` — базовый класс, от которого надо наследоваться | — | — | **549** | 36 ivar'ов |
| `AGXA11FamilyDevice` — сколько из них переопределено | — | — | 236 | — |
| `IOGPU` — обёртка над клиентом IOKit | 461 664 | 2 266 | 711 | 36 |
| `MTLCompiler` — передняя часть компилятора шейдеров | 983 544 | 2 971 | — | — |
| `AGXCompilerCore` — его задняя часть под AGX | 2 680 736 | 7 509 | — | — |
| `AppleMetalGLRenderer` — OpenGL ES **поверх** Metal, как это делает сама iOS | 258 256 | 1 092 | 2 | 1 |

Для сравнения — чем порт заменяет графическую библиотеку сейчас: `userland/src/eagl.c` — 3 685
строк, `src/eagl_slots.txt` — 409 записей таблицы, из которых помечено используемыми QuartzCore
**62**, написано руками 30, и заменяется **шесть** методов ObjC у `EAGLContext`.

### Что ломается первым

Не объём классов, а **язык шейдеров**. Metal получает программы не текстом, а в виде AIR —
битового кода LLVM в контейнере `.metallib`; разбирает его `MTLCompiler`, а в машинный код
превращает задняя часть под конкретный GPU (`AGXCompilerCore`), и делается это в отдельном
процессе `MTLCompilerService`. Сам контейнер версионирован (`This version of metallib is too old
and not supported any longer.`, `0x181b6cccb`), а задняя часть компилятора — тоже плагин со своей
проверкой пути и суффикса (`MTLCompilerPluginInterface::init`). В извлечённом корне девять
`.metallib` на 3,75 МБ, и Metal знает к ним прямые пути — например
`/System/Library/Frameworks/CoreImage.framework/CoreImage.metallib` (`0x181b6c367`).

У Mesa компилятор под Adreno (`ir3`) принимает NIR. **Готового читателя AIR в мире нет** (обзор —
9.3), а обойти его нельзя: всё, что просит Metal — CoreAnimation, CoreImage, MPS, камера — несёт
с собой уже скомпилированный AIR. Значит, первым сорвётся не отрисовка, а самый первый
`newLibraryWithData:` на любом системном `.metallib`, то есть раньше, чем что-нибудь будет
нарисовано.

Оговорка, которую я обязан сделать: «нет читателя» — это про сегодняшний мир, а не про
невозможность. **Написать его можно, и дешевле, чем я думал** — раздел 9 мерит, насколько.

Второе, что ломается, — и ломается тише: наследование от `_MTLDevice` означает, что мы получаем
его 36 приватных ivar'ов, разметка которых принадлежит сборке Metal 306.6.4 и нигде не описана, а
549 его собственных методов эти ivar'ы читают. Ошибка в разметке — порча памяти внутри чужого
кода, без диагностики.

### Где на самом деле проходит шов: `IOGPU`

Наследоваться от `_MTLDevice` напрямую драйвер Apple не стал, и это важно для оценки. `AGXMetalA11`
не импортирует `_MTLDevice` вовсе: он импортирует `IOGPUMetalDevice`, `IOGPUMetalCommandQueue`,
`IOGPUMetalCommandBuffer`, `IOGPUMetalBuffer`, `IOGPUMetalHeap` и ещё с десяток классов семейства
`IOGPUMetal*` из `IOGPU.framework`, а уже `IOGPU` импортирует `OBJC_CLASS_$__MTLDevice` вместе с его
метаклассом (признак прямого наследования). Цепочка такая:

```
AGXA11FamilyDevice  ->  IOGPUMetalDevice (IOGPU.framework)  ->  _MTLDevice (Metal.framework)
```

`IOGPU` — это и есть готовый средний слой: 36 классов и 711 методов ObjC, которые переводят
приватные `_MTL*` в обращения к клиенту IOKit (`IOGPUDeviceCreate`, `IOGPUCommandQueueCreate`,
`IOGPUCommandQueueSubmitCommandBuffers`, разделяемая память, …). Для варианта (А) это и подарок, и
счёт: подарок — что эти 711 методов писать не надо; счёт — что тогда **ядру порта придётся
реализовать ABI IOGPU** на клиенте `IOAcceleratorES`, то есть ещё один протокол поверх того, что
`RedwoodGPU` уже говорит на языке msm DRM для Mesa.

### Три пути и их цена

**(А) Поверхность Metal над Mesa.** Дописать на уже существующую службу `IOAcceleratorES` (раздел 2)
два свойства, положить в образ `/System/Library/Extensions/RedwoodMetal.bundle`
(место открыто: `userland/build.sh` собирает корень из файлов iOS по их настоящим путям и уже
кладёт туда свои — например `/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver`) и реализовать
граф объектов Metal на Mesa. Цена: ~53 класса и ~1 200 методов драйвера **плюс компилятор
AIR→NIR**, которого не существует. В единицах этого проекта — не файл и не два, а ещё одна Mesa, и
худшая её половина — без апстрима.

**(Б) Перевод Metal в тот OpenGL ES, который уже работает.** Строго хуже (А): ту же проблему
`_MTLDevice` приходится решать целиком, AIR остаётся на входе (то есть нужен AIR→GLSL), а
единственное, что экономится, — задняя часть под Adreno, то есть именно та часть, которую Mesa и
так даёт бесплатно. Заодно GLES2/3 не выражает вычислительные конвейеры, argument buffers и
значительную часть того, за чем Metal и зовут. Показательно, что сама iOS переводит в обратную
сторону: `AppleMetalGLRenderer.bundle` — это GLES поверх Metal, один класс и 1 092 символа.
Дешёвое направление — GL→Metal, а у порта уже есть то, что делает его ненужным: настоящий GLES.

**(В) Ни то, ни другое.** Признать, что Metal нет, и добиваться, чтобы выбирались пути без него.
Цена — отдельные файлы и десятки строк, в привычном для проекта виде. У всего, что порту
действительно нужно, путь без Metal есть: у CoreImage — задний конец на OpenGL ES, у
Espresso и CoreML — процессор и ANE, у декодирования видео — свой блок, а не Metal; у
CoreAnimation — рендерер OpenGL ES, который порт уже и использует. Раздел 7 разбирает это по
каждому из шести мест, которые без Metal не деградируют, а падают.

**Вывод — (В).** Metal здесь не «недоделан», а упирается в чужой промежуточный язык, и это не та
стена, которую обходят упорством: её обходят только компилятором AIR, а он сам по себе больше
всего, что в проекте написано про графику. Частичного Metal при этом не бывает: любая дорога
внутрь проходит через `newLibraryWith…` и объект состояния конвейера, так что «только вычисления»
или «только blit» не спасают.

Ограничение честности: драйвер iPhone X (`AGXMetalA11`) переиспользовать нельзя ни в каком виде —
он порождает поток команд AGX и говорит с ядром по ABI AGX; к Adreno это не имеет отношения.

## 5. Ложный след, который стоит знать

Выглядит соблазнительно: в дереве устройств, которое выкладывает загрузчик, есть
`graphics-featureset-class` = `APPLE4` и
`graphics-featureset-fallbacks` = `APPLE3:APPLE3v1:APPLE2:APPLE1:GLES2,0`
(`xnuloader/identity.h`, скопировано с настоящего iPhone X). Последняя ступень — `GLES2,0`, то
есть у самой Apple под всеми наборами Metal предусмотрена ступень OpenGL ES. Напрашивается
действовать как с цветовым охватом: `loader.c`, `dt_identity()` уже переписывает
`artwork-display-gamut` с `P3` на `sRGB`, чтобы WebKit перестал заводить поверхности по десять бит
на цвет и страницы перестали выходить чёрными (вернуть прежнее — `gamut=p3`).

**Так делать не нужно.** Эти два свойства MobileGestalt публикует как `GraphicsFeatureSetClass` и
`GraphicsFeatureSetFallbacks`, а читают их `libMobileGestalt`, `CoreUI` (`CUICatalog`, ключ
`kMGArtworkGraphicsFeatureSetClass`), `CoreThemeDefinition` и `TextureIO` — то есть **каталоги
скомпилированного оформления**: какой вариант текстуры достать из `.car`. Ни одно решение о том,
чем рисовать, на них не завязано. Понизить класс до `GLES2,0` — значит заставить CoreUI брать
другие варианты картинок, а Metal от этого не появится и не исчезнет.

И прогон добавил к этому ещё довод: **в порте MobileGestalt под этими именами не отвечает вовсе**
(`GraphicsFeatureSetClass = (none)`, `GraphicsFeatureSetFallbacks = (none)`), хотя в дереве
устройств значения лежат. То есть менять дерево тем более незачем: читатель и так ничего не
получает. А ключа `DeviceSupportsMetal` в системе нет — **бита «есть ли Metal» не существует**, и
решение принимается ровно одно и в одном месте: нашёлся ли на службе `IOAcceleratorES` плагин
(шаг 2 раздела 1).

## 6. Первый шаг: `metal_test` (сделан)

Поскольку отказ Metal нигде не слышен, а весь план выше держится на утверждениях о том, кто и как
падает, первое, что нужно, — это чтобы система сама говорила, что она про Metal думает.
`userland/src/metal_test.m` (собирается `build.sh`, попадает в `/redwood/bin`, запускается
`RC_JOBS=rc.jobs.metal`) печатает всю цепочку раздела 1 по шагам:

1. службы класса `IOAcceleratorES` и у каждой — `MetalPluginName` и `MetalPluginClassName`
   (сейчас: ни одной, и значит Metal не найдёт устройства вовсе);
2. что в реестре **есть** похожего на GPU — `RedwoodGPU`, `IOGPU`, `AGXAccelerator`,
   `IOAccelerator` — и с каким `IOUserClientClass`: видно, что GPU поднят, но под другим именем;
3. бандлы в `/System/Library/Extensions`: грузится ли принципиальный класс каждого и является ли он
   подклассом `_MTLDevice`, — то есть мог ли бы вообще загрузиться плагин в этом образе, и на каком
   шаге всё обрывается;
4. что получает программа: `MTLCreateSystemDefaultDevice()`, а при удаче — имя, `registryID` и
   поддерживаемые семейства;
5. что iOS всё равно рассказывает про графику (раздел 5): `GraphicsFeatureSetClass`,
   `GraphicsFeatureSetFallbacks` и рядом — чтобы было видно, что ответ `APPLE4` не зависит ни от
   чего;
6. умеет ли работать фреймворк, у которого путь без Metal есть: CoreImage прогоняет `CIColorInvert` по четырём
   пикселям и результат читается обратно. Оранжевый `1; 0,5; 0` должен стать примерно
   `0; 128; 255`. Это единственная здесь проверка, отвечающая на вопрос «может ли эта система
   вообще обработать изображение», а он нужен и камере, и Фото.

**Каждый опасный вопрос задаёт отдельный процесс**, и это не мелочь, а исправленная ошибка. Сперва
`metal_test` разветвлялся (`fork`) и спрашивал в потомке — и все проверки до единой стали умирать с
SIGABRT, но не в CoreImage, а в самой среде Objective-C:

```
objc[98]: +[__NSCFNumber initialize] may have been in progress in another thread when fork() was
called. We cannot safely call it or ignore it in the fork() child process. Crashing instead.
```

Это `objc_initializeAfterForkError`: потомку `fork` нельзя пользоваться Objective-C, если у родителя
было больше одного потока, а у `metal_test` их больше (довольно слинковаться с CoreImage и
OpenGLES). Прогон, сделанный так, не говорит о CoreImage ничего. Теперь `metal_test` запускает себя
заново (`posix_spawn` с именем вопроса в аргументе: `ci-eagl`, `ci-default`, …), потому что `exec`
начинает среду с нуля. Урок общий: **в этом проекте нельзя проверять что-либо из iOS в потомке
`fork` — только в новом процессе.**

Проверка бандлов идёт **последней** нарочно: взять принципиальный класс — значит загрузить код
бандла и дать выполниться его инициализаторам, а `/System/Library/Extensions/AGXMetalA11.bundle`
в образе порта есть (`apfs.py ls userland/system.apfs /System/Library/Extensions`), и это настоящий
драйвер Apple для GPU A11, чей исполняемый файл лежит в кэше dyld, который порт тоже везёт. Если на
этом программа кончится, всё остальное уже сказано. Кстати, `AGXPrincipalDevice` — это фабрика: у
него ровно один метод, `-initWithAcceleratorPort:`, который выбирает настоящий класс устройства.

Почему именно это и почему отдельно от всего: программа полезна, даже если дальше ничего не
случится (вопрос «не из-за Metal ли это?» будет возникать и у камеры, и у Фото, и у WebGL), она
ничего не меняет в поведении системы, и она же — предпосылка для любой следующей работы: сперва
измерить, потом трогать.

Устроено как остальные диагностики порта (`mg_test`, `cg_test`, `font_test`, …):

```sh
userland/build.sh                       # obj/metal_test -> root/bin/metal_test
RC_JOBS=rc.jobs.metal qemu/run-ios.sh 240                    # в QEMU
XL_CMDLINE="... gpu=1" RC_JOBS=rc.jobs.metal ./xl run        # на телефоне
```

**Разница между QEMU и телефоном ровно одна**, и её стоит знать, читая вывод: пункт 2 показывает
`RedwoodGPU` только на телефоне. `RedwoodGPUStart()` зовётся из `bsd/dev/redwood_adreno.c` (строка
3858) лишь тогда, когда GPU действительно поднялся (`done >= 21 && core_runs`, то есть `gpu=1` и
прошивки из `fw-gpu/`); в QEMU GPU нет вовсе, и служб не будет никаких. Про само Metal ответ
одинаков: устройства нет ни там, ни там.

### Что он напечатал (QEMU, 19 октября 2026)

```
metal_test: the services Metal looks for (IOAcceleratorES)
metal_test:   IOAcceleratorES "sgx": MetalPluginName (none), MetalPluginClassName (none)
metal_test: what the registry has that is a GPU
metal_test:   none
metal_test: the device a program gets
metal_test:   MTLCreateSystemDefaultDevice() = nil (and says nothing about why)
metal_test: what iOS publishes about graphics (artwork, not rendering)
metal_test:   GraphicsFeatureSetClass = (none)
metal_test:   GraphicsFeatureSetFallbacks = (none)
metal_test:   ArtworkDeviceSubType = (none)
metal_test:   HasExtendedColorDisplay = <CFBoolean>{value = false}
metal_test:   main-screen-class = <CFNumber>{value = +15}
metal_test:   DeviceSupportsMetal = (none)
metal_test: CoreImage, which has a path without Metal
metal_test:   asking for a CIContext (the default one)...
metal_test:   the default CIContext: the process died of signal 11 right there
metal_test:   asking for a CIContext (software renderer)...
metal_test:   a software-renderer CIContext: the process died of signal 11 right there
metal_test: the plug-in bundles of /System/Library/Extensions (loads their code: kept last)
metal_test:   Metal's own base class _MTLDevice: there
metal_test:   AppleMetalGLRenderer.bundle: executable (none), principal class (none) -- loaded, but not a _MTLDevice: Metal would drop it
metal_test:   AGXGPURawCounterBundle.bundle: executable AGXGPURawCounterBundle, principal class (none) -- loaded, but not a _MTLDevice: Metal would drop it
metal_test:   AGXMetalA11.bundle: executable AGXMetalA11, principal class AGXPrincipalDevice -- loaded, but not a _MTLDevice: Metal would drop it
```

Что из этого следует, по порядку:

1. **Служба есть** (раздел 2) — я ошибался, думая, что её нет. Цепочка рвётся на шаге 2.
2. GPU в QEMU нет, как и ожидалось; этот пункт нужно смотреть на телефоне с `gpu=1`.
3. `nil`, молча, — как и говорит разбор кода.
4. **MobileGestalt не отвечает про графику вовсе.** `GraphicsFeatureSetClass` и
   `GraphicsFeatureSetFallbacks` под этими именами дают `(none)`, хотя в дереве устройств лежат
   `APPLE4` и `APPLE3:…:GLES2,0`. Оговорка: часть ключей MobileGestalt спрашивают по
   «обфусцированным» именам (см. `mg_test`), так что это может быть и свойством имени, а не
   отсутствием ответа; проверять — через `mg_test` с обфусцированным ключом. Зато
   `HasExtendedColorDisplay = false` — видно, что правка охвата из `loader.c` работает, — а
   `DeviceSupportsMetal` не существует как ключ: **бита «поддерживается ли Metal» в системе нет**,
   и это ровно то, на чём стоит вывод раздела 5.
5. **CoreImage здесь не деградирует, а убивает процесс.** `+[CIContext contextWithOptions:]`
   падает с SIGSEGV — и с `nil`, и с `kCIContextUseSoftwareRenderer`. Это главный факт, которого ни
   у кого не было: дело не в том, что фильтр посчитается медленно или вернёт `nil`, — **контекст
   CoreImage на этом порте нельзя даже создать**. Поэтому и проверки идут каждая в своём потомке:
   первый прогон молча обрывался на этом месте, и в журнале оставалось только
   `metal_test[20] Corpse allowed 1 of 5`. Задний конец на OpenGL ES (раздел 7) от этого не
   перестаёт существовать — но дорога к нему начинается не с `contextWithOptions:`, а с
   `contextWithEAGLContext:`.
6. **Бандл Apple в этом образе грузится.** `AGXMetalA11.bundle` нашёлся, его код загрузился, класс
   `AGXPrincipalDevice` из `Info.plist` разрешился — то есть драйвер A11 в порте можно поднять до
   точки входа. Важная оговорка: `AGXPrincipalDevice` действительно не подкласс `_MTLDevice` (у него
   вообще один метод, `-initWithAcceleratorPort:`, он фабрика), но Metal спросил бы не его, а класс
   из свойства `MetalPluginClassName`, которого на службе нет. Так что «Metal would drop it» здесь —
   про `NSPrincipalClass` из `Info.plist`, а не приговор настоящему пути.

## 7. Шесть мест, которые без Metal падают: у кого есть путь без него

Раздел 3 перечислил тех, кто без устройства не деградирует, а останавливается. Поскольку Metal
здесь не появится, полезен обратный вопрос: что из этого **нельзя** сделать без него, а что просто
ещё не сделано. Разница между «камера не умеет эффектов» и «камера не умеет эффектов пока».

Главное, что видно из зависимостей: **четыре из шести ездят на CoreImage или на Espresso**. Значит,
починок нужно не шесть, а две.

### CoreImage — не Metal-only; у него **два** полных задних конца

Самая важная находка всего разбора. В iOS 16.5 CoreImage несёт две параллельных реализации:

| OpenGL ES | Metal |
| --- | --- |
| `CI::GLContext`, `CI::GLMainProgram`, `CI::GLTextureManager`, `CI::GLTextureImage`, `CI::GLTextureNode` | `CI::MetalContext`, `CI::MetalMainProgram`, `CI::MetalDAG`, `CI::MetalKernel` |

Доказательства, что путь на OpenGL ES живой, а не остаток прошлого:

- CoreImage **слинкован с `OpenGLES`** (и с `SoftLinking`);
- открытое API никуда не делось: `+[CIContext contextWithEAGLContext:]`,
  `contextWithEAGLContext:options:`, `-initWithEAGLContext:options:`, плюс внутреннее
  `+[CIContext(Internal) internalContextWithEAGLContext:options:]`;
- у него своя обёртка над EAGL: `CIEAGLContextCreate`, `CIEAGLContextSetParameter`,
  `CIEAGLContextGetAPI`, `CIEAGLContextSetBackgroundEnabled` и
  **`CIEAGLContextTexImageIOSurface`**;
- шейдеры компилируются на ходу: `__CreateShader`, `__CompileShader`, `__AttachShader`,
  `__GetShaderiv`, `__GetShaderInfoLog`, `__DeleteShader`, `construct_shader`, `compile_shader`
  — то есть GLSL, а не AIR, и значит **стена из раздела 4 здесь не стоит**;
- есть диагностика, осмысленная только для GLES: `CI::format_is_not_supported_as_destination_on_gles`;
- `-[CIContext initWithOptions:]` ветвится по `CIGLIsUsable()` (`0x19497ef8c`), а тот через
  `_wrapGLIsUsable` (`0x194b0dee0`) — это `mov w0, #1; ret`, **всегда «да»**;
- `kCIContextUseSoftwareRenderer` и `kCIContextOpenGLShareContext` на месте.

И отдельно: `CIEAGLContextTexImageIOSurface` — **та же точка входа, которую `userland/src/eagl.c`
уже реализует** для CoreAnimation (`texImageIOSurface:target:internalFormat:width:height:format:…`).
То есть порт уже умеет ровно то, чего CoreImage хочет от библиотеки OpenGL ES.

Но прогон (раздел 6) добавил к этому важное: **дорога по умолчанию не просто закрыта, она
смертельна**, — и, что важнее, видно, **где именно** она обрывается. `metal_test` ловит сигнал сам
и печатает адреса без сдвига кэша; `ipsw dyld a2s` по ним даёт:

```
+[CIContext contextWithOptions:]                        + 36
  -[CIContext initWithOptions:]                         + 504
    +[CIContext(Internal) internalContextWithEAGLContext:options:]  + 580
      CI::GLContext::GLContext(CI::GLContext::ShareContextInfo, CGColorSpace*, …)  + 500
        CI::GLContext::init()                           + 204   <- здесь
```

Сигнал 11, `si_addr` 0, а программный счётчик после вычитания сдвига — `0xfffffffff9af0000`, то
есть вызов по мусорному указателю.

Из этого следует самое неожиданное в разборе: **контекст по умолчанию и так выбирает задний конец на
OpenGL ES, а не на Metal.** Он идёт прямо в `internalContextWithEAGLContext:options:` и строит
`CI::GLContext` — что в точности согласуется с `CIGLIsUsable()`, скомпилированным в «return 1»
(выше). И падает он не оттого, что нет Metal, а оттого, что **`CI::GLContext::init()` начинает
работу, когда никакого контекста OpenGL ES ему не дали**: по этой дороге `EAGLContext` — `nil`.

И `kCIContextUseSoftwareRenderer: @YES` идёт **тем же путём, с тем же стеком и той же смертью**:
на iOS 16.5 этот ключ ничего не значит, программного растеризатора у CoreImage нет.

Значит вопрос «достижимо ли это» получает хороший ответ: дорога, которой CoreImage и так хочет
идти, — ровно та, что у порта есть. Не хватает ей того единственного, чего она просит: контекста
OpenGL ES.

### Опыт на телефоне: CoreImage поверх нашего OpenGL ES

Это и было проверено — на телефоне, с `gpu=1` и образом `GL=1`, то есть когда за `EAGLContext`
стоит настоящий Adreno через freedreno:

```
redwood-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa):
            OpenGL ES 3.2 Mesa 25.0.7; Adreno 7c+ Gen 3; shaders OpenGL ES GLSL ES 3.20
metal_test:   EAGLContext: EAGLContext, API 3
metal_test:     signal 11 at address 0x8
```

`EAGLContext` создался — наш, по-настоящему на GPU телефона. А `CIContext` поверх него — нет, и
стек (адреса без сдвига, имена по `llvm-nm` извлечённого CoreImage) показывает, где именно:

```
+[CIContext contextWithEAGLContext:]                              + 36
  -[CIContext initWithEAGLContext:options:]                       + 116
    +[CIContext(Internal) internalContextWithEAGLContext:options:] + 580
      CI::GLContext::GLContext(CI::GLContext::ShareContextInfo, CGColorSpace*, …) + 380
        CI::GLContext::_wrapEAGLContextCreate                     + 128
          -[EAGLContext sharegroup]                               + 4    <- здесь, si_addr 0x8
```

**CoreImage прошёл ровно тем путём, который у порта есть, и остановился на одном нереализованном
методе.** `src/eagl.c` подменяет у `EAGLContext` семь методов — те, о которых спрашивает
CoreAnimation, — и `-sharegroup` среди них не было; а `my_initWithAPI()` нарочно не вызывает
`initWithAPI:` самой Apple (своё состояние он держит в `struct eagl` рядом с объектом), так что
настоящий `-[EAGLContext sharegroup]` читает ivar'ы, которых никто не заполнял, и падает. С
программным драйвером Mesa адрес падения тот же — дело не в драйвере.

**Чего CoreImage на самом деле хочет**, видно из `_wrapEAGLContextCreate` (`0x194b0e10c`): он
собирает словарь свойств тремя `setObject:forKey:`, добавляет четвёртый элемент **только если
`[наш_контекст sharegroup]` не nil** (`cbz x0`, переход через), и затем зовёт
`objc_msgSend$initWithAPI:properties:`. То есть он **не делит наш контекст, а создаёт свой** — и
создаёт его через тот самый метод, который `eagl.c` подменяет первым в своём списке. А группа
совместного доступа **необязательна**: nil означает «без разделения», и это предусмотренный случай.

### И этого хватило: CoreImage работает

`-sharegroup` добавлен в `src/eagl.c` (возвращает nil — здесь никто контекстами не делится), и
CoreImage пошёл дальше:

```
$ /redwood/bin/metal_test ci-eagl-software
metal_test:   /redwood/lib/libredwood-gl.dylib loaded; EAGLContext is ours now
metal_test:   EAGLContext: EAGLContext, API 3
redwood-gl: -sharegroup asked for: none (nothing here shares a context)
redwood-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa): ...
metal_test:   CIContext: CIContext
metal_test:   backend: OpenGL ES (CI::GLContext)
metal_test:   the context's Metal device: nil
metal_test:   running the filter kernel...
metal_test:   CoreImage rendered: first pixel 0 229 255 255
```

**CoreImage на этом порте считает, и считает без всякого Metal.** `-isOpenGLBased` отвечает «да»,
`-device` — nil, ядро фильтра скомпилировалось и посчиталось, пиксели прочитаны обратно. Видно и то,
что предсказывал разбор `_wrapEAGLContextCreate`: CoreImage сам создал несколько своих контекстов —
каждый через наш `my_initWithAPI()`, отсюда повторяющиеся строки `an OpenGL ES 3 context`.

Про цвет: ожидалось «примерно 0 128 255», вышло `0 229 255 255`, и **это верно**, а ошибка была в
ожидании. CoreImage считает в линейном пространстве: sRGB 0,5 → линейное 0,214 → инверсия 0,786 →
обратно в sRGB ≈ 0,906, то есть 231; измерено 229. Красный 0 и синий 255 точны. Инвертировать в sRGB
напрямую, как я прикидывал, CoreImage и не должен.

Одной строки в таблице методов `eagl.c` хватило, чтобы «Фото» и камера получили обратно обработку
изображений, — ровно той, которой не хватало.

**Вывод: «пока», а не «нельзя», и вход только один.** Нужно дать CoreImage `EAGLContext` через
`+[CIContext contextWithEAGLContext:]` — путь по умолчанию (`contextWithOptions:`) к этому не ведёт,
он падает раньше в `CI::GLContext::init()`. Дальше — работа в `inject.c`: подставлять такой контекст
там, где framework просит контекст по умолчанию.

### Espresso (движок CoreML) — не Metal-only; процессорный движок полноправный

`engine_cpu`, **`force_engine_cpu`**, `force_engine_gpu`, `espresso_create_context_auto`,
`espresso_context_create_for_cpu_test_vectors`. Есть и движки Нейронного процессора
(`ANERuntimeEngine`, `ANECompilerEngine`) — но драйвера ANE в порте нет, так что речь о процессорном.

И он не символический, а полный. В самом `Espresso` **127 разных ядер `…_kernel_cpu`** — свёртки,
обратные свёртки, скалярные произведения, пулинг, конкатенация, срезы, `conv3d`, `topk` и прочее —
и **ни одного** `…_kernel_metal`, `…_kernel_gpu` или `…_kernel_mps`: сторона Metal живёт в отдельных
движках, которые зовут `MetalPerformanceShaders`, а процессорная посчитана внутри, поверх BNNS из
`Accelerate` (23 импортированных символа `BNNS*`). Показательно и то, что для MPS предусмотрены
заглушки на случай его отсутствия: **`DummyMPSEngine`, `DummyMPSBuffersEngine`,
`DummyMPSGraphEngine`**. **Вывод: «пока», и ближе всего к готовому из шести.**

### AppleCVA — частично

Четыре службы: `CVAFaceTracking*`, `CVANetPipe*`, `CVASegmentation*`, `CVAViewpointCorrection*`.
У первой есть явный ключ **`kCVAFaceTracking_ForceCPU`**; библиотека слинкована с `Accelerate`,
`Espresso` и `Vision` и пользуется `vImageBuffer_InitWithCGImage`, `vImageRotate`. Но
безусловный `assert` на `MTLCreateSystemDefaultDevice()` (`0x1beeb40cd`) никуда не делся, и рядом
`CVMetalTextureCacheCreate`, `MPSCNNSoftMax`. **Вывод: слежение за лицами — «пока»; сегментация и
коррекция точки зрения выглядят привязанными к устройству.**

### CameraEffectsKit — частично, и в основном наследует судьбу CoreImage

Пользуется `CIContext`, `CIImage`, `CIFilter` — то есть что сможет CoreImage, смогут и эти эффекты.
Есть явный **`initForCPU`** и composition через `CGContext*`. Но часть эффектов сама заводит
устройство, очередь и текстурный кэш, и там жёсткий `assert` `"Error: Metal device must not be NULL"`.
**Вывод: «пока» в той части, что идёт через CoreImage.**

### AppleDepth — сеть не привязана, обработка привязана

Слинкован с `AppleNeuralEngine`, `Espresso` и `Accelerate`, так что сама сеть глубины может
считаться на процессоре. Но вся до- и постобработка только на Metal: `ADMetalUtils`,
`preprocessorInputMetalHelper`, `postprocessorOutputMetalHelper`, `rebuildMetalPreprocessingKernels`,
`metalDevice`, и процессорных двойников у них нет. Зацепка есть: строка
`Metal device not provided, using default device` (`0x1e747cd18`) значит, что устройство можно
**передать извне**. **Вывод: ближе всего к «нельзя» из шести.**

### CameraUI — видоискатель на Metal, остальное приложение нет

Живой предпросмотр — `CAMMetalView`, `CAMMetalContext`, `CAMetalLayer`. Всё прочее (кнопки, бейджи,
индикаторы, затвор `CAMLiquidShutterRenderer`) рисуется CoreGraphics и UIKit — десятки вызовов
`CGContext*`; фильтры идут через `applyingCIFiltersWithHandler`, то есть опять CoreImage. Есть и
`CAMDisabledPreviewController` — режим без предпросмотра. **Вывод: устройство нужно поверхности
живого предпросмотра; съёмка и интерфейс сами по себе к Metal не привязаны.**

## 8. Чего ещё стоит проверить дальше (по убыванию пользы)

- **CoreImage через `contextWithEAGLContext:` — на телефоне.** Самое большое из того, что
  осталось, и самое дешёвое: раздел 7 показывает, что задний конец на GLES у CoreImage полный,
  слинкованный и ветвящийся по `CIGLIsUsable()`, который всегда «да», а нужная ему точка входа
  (`CIEAGLContextTexImageIOSurface`) у `eagl.c` уже написана. Проверка на один прогон: добавить в
  `metal_test` ветку `contextWithEAGLContext:` и запустить с `gpu=1` на образе `GL=1`, где за
  `EAGLContext` стоит Mesa. Если ядро посчитается — дальше работа в `inject.c`: подставлять такой
  контекст там, где framework просит контекст по умолчанию (и тем самым перестать падать). Это
  закрывает заодно часть `CameraEffectsKit` и фильтры `CameraUI`.
- **Espresso на процессорный движок.** `force_engine_cpu` есть; проверить, выбирается ли он сам,
  когда устройства нет, и если нет — подтолкнуть. Это CoreML, Vision и `mediaanalysisd`.
- **Переписать перепись.** Прогнать `glcensus=1` на сценариях `rc.jobs.gfx` и `rc.jobs.fps` и
  получить наконец полное число: по каким функциям CoreAnimation отказано и сколько отрисовок идёт
  без программы. Сейчас известно только, что спрошено и отказано по двум (40 `sb3`, 60 `p3c`).
- **Конвейер снимка.** Какие его шаги (`CMPhoto`, `HDRProcessing`, `Portrait`, `AppleDepth`) всё же
  требуют устройства, а какие идут через CoreImage. Прямо касается ветки `camera`.
- **Не начинать (А) и (Б)** без фронтенда AIR. Если он когда-нибудь появится (чужой проект, не
  этот), раздел 4 перечисляет, что останется сделать; до тех пор объём работы не уменьшится.

## 9. Насколько AIR поддаётся: разбор

Раздел 4 назвал AIR стеной. Пользователь хочет **настоящий Metal** — игры и всё прочее, — поэтому
стену надо было измерить, а не объявить. Измерил. **Я был неправ: как формат AIR почти не
сопротивляется.** Ниже — чем именно, по файлам и байтам.

### 9.1 Что лежит в `.metallib`

В извлечённой системе десять файлов `.metallib`. Пять из них — контейнеры с магией `MTLB`
(`4d544c42`); у пяти `*_rt.metallib` внутри `AGXMetalA11.bundle` магия другая — это внутренние
библиотеки драйвера AGX, не AIR.

Разбор контейнера (разбирает его `tools/metallib.py`; проверено на
`Extensions/AppleMetalGLRenderer.bundle/default.metallib`, 1 111 748 байт):

| смещение | что |
| --- | --- |
| `0x00` | `"MTLB"` |
| `0x04` | версия контейнера, здесь 1.2 |
| `0x10` | полный размер файла — `0x10f6c4` = 1 111 748, **совпал побайтово** |
| `0x18`, `0x20` | начало и конец списка функций |
| `0x28`, `0x30` | public metadata: смещение и длина |
| `0x38`, `0x40` | private metadata |
| `0x48`, `0x50` | **битовый код**: смещение и длина |

Список функций — счётчик `u32`, затем записи; у записи сначала `u32` её длины (считая само это
поле), затем поля с тегами по образцу `[4 байта тега][u16 длины][значение]` до тега `ENDT`:
`NAME` (имя), `TYPE` (0 vertex, 1 fragment, 2 kernel, 3 visible, 4 intersection), `HASH` (32 байта),
`OFFT` (три `u64`: смещения в public, private и битовый код), `VERS`, `MDSZ` (размер модуля),
`RFLT`. В конце файла ещё секции `RBUF` и `AIRR` (отражение) и тег `ENDT`.

Главное: **у каждой функции свой отдельный модуль**, и он лежит в стандартной обёртке битового кода
LLVM — `0x0B17C0DE`, а внутри обычная магия `BC\xC0\xDE`:

```
0000afc0: dec0 170b 0000 0000 1400 0000 900b 0000  (обёртка: версия 0, смещение 0x14, размер 0xb90)
0000afd0: ffff ffff 4243 c0de 3514 0000 0300 0000  (BC\xC0\xDE — дальше обычный битовый код)
```

Из пяти контейнеров извлечено **456 модулей**, и все 456 прочитаны обычным `llvm-dis` (LLVM 22.1.8)
**без единой ошибки**. То есть никакого «своего формата» нет: читать AIR умеет готовый LLVM,
безо всяких правок.

### 9.2 Насколько AIR отличается от обычного LLVM IR

Отличий ровно три, и все узкие.

**Триплет.** `target triple = "air64-apple-ios16.5.0"` у всех 456 модулей; `datalayout` —
совершенно обычный. Версия AIR 2.5.3.0, язык «Metal 3.0», компилятор
`Apple metal version 31001.722 (metalfe-31001.722.3)`.

**Адресные пространства — всего три:** 2 (constant, 3427 обращений), 1 (device, 984), 3
(threadgroup, 46). Это прямо ложится на модель памяти NIR и SPIR-V. Текстуры и семплеры — **не
особые типы LLVM, а просто указатели** (`ptr addrspace(1)` и `ptr addrspace(2)`), так что экзотики
в типах нет вовсе.

**Именованные метаданные — девять видов,** и в них вся «метальность»:

| метаданные | в скольких модулях |
| --- | --- |
| `!air.version`, `!air.language_version`, `!air.compile_options` | 456 |
| `!air.fragment` | 271 |
| `!air.visible` | 135 |
| `!air.kernel` | 47 |
| `!air.function_constants` | 11 |
| `!air.sampler_states` | 10 |
| `!air.vertex` | 3 |

Узел точки входа описывает её интерфейс — именно то, что NIR называет переменными ввода-вывода.
Например у `clearFragment01`:

```
!16 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color0"}
!18 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 48, !"air.location_index", i32 14, i32 1,
        !"air.read", !"air.address_space", i32 2, ... !"air.arg_name", !"color"}
```

**Свои функции («интринсики»).** Во всех 456 модулях встречается 125 разных объявлений `air.*`, но
имена устроены как `air.<операция>.<суффикс типа>`, и после снятия суффиксов остаётся **около 51
операции** четырёх семейств:

- **арифметика (≈28):** `fast_clamp`, `fast_fmin/fmax`, `fast_sqrt`, `fast_exp`, `fast_pow`,
  `fast_sin/cos`, `fast_atan2`, `fast_floor/ceil/round/fract`, `fast_fabs`, `fast_ldexp`,
  `saturate`, `mix`, `dot`, `sign`, `min/max`, `floor`, `convert`, `all`;
- **текстуры (≈12):** `sample_texture_2d/3d`, `sample_depth_2d`, `read_texture_2d`,
  `read_texture_2d_ms`, `read_depth_2d_ms`, `write_texture_2d/3d`, `gather_texture_2d`,
  `get_width/height_texture_2d`, `get_num_samples_texture_2d_ms`;
- **атомарные (≈9):** `atomic.global.{load,store,add,min,max}`, `atomic.local.{load,store,add}`;
- **синхронизация (1):** `wg.barrier`.

У каждой есть прямое соответствие в NIR: арифметика — это `nir_op_*`, текстуры — `nir_tex_instr`,
атомарные — `nir_intrinsic_*_atomic`, барьер — `nir_intrinsic_barrier`. **Это короткий список, а не
длинный хвост редкостей.**

И ещё одно число, самое говорящее: **283 модуля из 456 (62 %) не зовут ни одного `air.*` вовсе** —
это чистый LLVM IR.

**Сколько всего в языке.** Выборка из пяти библиотек — не весь Metal, поэтому границу я взял из
самого заднего конца Apple: в `AGXCompilerCore` 339 разных строк `air.*` (в `MTLCompiler` 276). Из
них около 117 — варианты `get_*_texture_*` (запросы к текстуре по размерностям и типам, одна
реализация с параметрами), а значительная часть остального — не интринсики, а **имена атрибутов
ввода-вывода** (`vertex_id`, `thread_position_in_grid`, `threads_per_simdgroup`,
`viewport_array_index`, `tile_index`, …), которым в NIR отвечают системные значения. Отдельными
группами стоят то, чего системным шейдерам не нужно, а игре может понадобиться: `simd_*`/`quad_*`
(операции в группе потоков), трассировка лучей (`world_to_object_transform`, `world_space_origin`,
`triangle_data`, `intersection`), mesh-шейдеры (`set_indices_mesh`), таблицы видимых функций
(`visible_function_table`).

### 9.3 Что уже существует

Разделять надо строго: **в Metal** конвертеров много, **из Metal** — почти ничего.

- **[worthdoingbadly.com/metalbitcode](https://worthdoingbadly.com/metalbitcode/)** — единственный
  содержательный разбор. Устанавливает то же, что и я: `MTLB` держит обычный битовый код LLVM,
  `llvm-dis` его читает, `llc` даже собирает шейдер в код x86-64 и ARM64. Но: проверено на одном
  файле, теги `HASH`/`VERS`/`OFFT` оставлены необъяснёнными, вызов `air.convert...` так и остался
  неразрешённым, а цель была пустить шейдер на процессоре — автор сам называет результат
  «любопытством», потому что LLVM не превращает SPMD-шейдер в хороший SIMD-код. **Перевода в чужое
  GPU-представление там нет.**
- **[mtl-info](https://lib.rs/crates/mtl-info)** — утилита на Rust: перечисляет функции в
  `.metallib` и дизассемблирует их. Версия 0.2.0, март 2019, помечена нестабильной; новых
  контейнеров почти наверняка не знает.
- **MetalLibraryArchive** — извлекает функции из `.metallib`; снова только распаковка.
- **[naga](https://docs.rs/crate/naga)** (wgpu) — в таблице форматов AIR упомянут, но клетка пуста;
  Metal у него только на выходе (`msl-out`).
- **ANGLE**, Apple Shader Converter, HLSL→Metal — всё это дорога **в** Metal.
- **Asahi Linux** — те, кто ближе всех к задаче, сознательно пошли мимо: их компилятор — задний
  конец NIR, а LLVM Apple они сочли необязательным. Читателя AIR у них нет.
- **SPIRV-LLVM-Translator** — казался бы готовым мостом LLVM IR → SPIR-V, но он поддерживает
  только OpenCL/Compute-разновидность SPIR-V. Для вершинных и фрагментных шейдеров это не путь, так
  что дорога «AIR → SPIR-V → `spirv_to_nir`» в лоб не годится; остаётся **AIR → NIR** напрямую.

Итог обзора: **читателя AIR в мире нет, но и запретов на него нет.** Моя прежняя формулировка
(«фронтенда AIR нет ни в Mesa, ни где-либо ещё») верна как утверждение о сегодняшнем дне и неверна
как вывод о невозможности.

### 9.4 Честный ответ

**Насколько это большая работа.** Мерка есть: фронтенд SPIR-V → NIR в Mesa
(`src/compiler/spirv`) — это `spirv_to_nir.c` на 294 871 байт, `vtn_variables.c` 122 904,
`vtn_structured_cfg.c` 60 724, `vtn_alu.c` 53 648, `vtn_opencl.c` 45 555, `vtn_cfg.c` 27 197,
`vtn_glsl450.c` 23 284 и ещё десяток файлов — **около 750 КБ написанного руками C**, то есть
примерно 25 тысяч строк, делавшихся годами множеством людей, причём для представления, которое
**само задумано для такого перевода и везёт с собой машиночитаемую грамматику**. У AIR грамматики
нет: смысл каждого `air.*` и каждого ключа метаданных надо выяснять по наблюдению.

С другой стороны, половина работы уже не нужна: **разбор формата бесплатен** (456 из 456 модулей
читает готовый LLVM), типы обычные, адресных пространств три, а не двадцать.

Оценка по шагам, честная:

| что | объём |
| --- | --- |
| один простой шейдер из настоящей `.metallib` посчитан на Adreno | дни, ~1–2 тысячи строк |
| то, что нужно системным шейдерам: vertex/fragment/kernel, 51 операция, 2D-текстуры | месяцы, ~5–10 тысяч строк |
| то, что нужно игре: argument buffers, simdgroup, тесселяция, mesh, трассировка лучей, таблицы функций, imageblocks | годы, и это уже масштаб Mesa |

**И главное, чего нельзя забывать: шейдеры — только одна из трёх частей.** Раздел 4 мерил остальные
две: драйвер (граф объектов `_MTLDevice`/`IOGPUMetalDevice` — 53 класса и 1158 методов у
`AGXMetalA11`, 711 методов у `IOGPU`) и ABI ядра под клиентом `IOAcceleratorES`. Компилятор AIR я
называл тем, что «сорвётся первым», — и это по-прежнему так, но теперь видно, что он не самый
большой из трёх, а самый **измеримый**. Убрать его — значит остаться с двумя другими.

Поэтому ответ пользователю: **настоящий Metal — игры и всё прочее — здесь не выходит**, но причина
другая, чем я говорил. Не «непреодолимый чужой язык шейдеров», а «три больших подсистемы, каждая
размером с то, что в проекте уже написано про графику, и ни одной нельзя обойтись». Это **нет**, и
оно твёрдое для цели «игры». Оно **не** твёрдое для цели «один шейдер из системной библиотеки
считается нашим GPU» — вот это выходит, и довольно скоро.

### 9.5 Первый шаг, если его захотят

Он уже выбран и полностью описан данными выше. `clearFragment01` из
`AppleMetalGLRenderer.bundle/default.metallib` — настоящий фрагментный шейдер самой Apple, четыре
команды LLVM, **ни одного `air.*`**:

```llvm
define <{ <4 x float> }> @clearFragment01(ptr addrspace(2) noalias readonly captures(none)
                                          dereferenceable(48) %0) {
  %2 = getelementptr inbounds %struct.ClearFragmentInput, ptr addrspace(2) %0, i64 0, i32 0
  %3 = load <4 x float>, ptr addrspace(2) %2, align 16
  %4 = insertvalue <{ <4 x float> }> undef, <4 x float> %3, 0
  ret <{ <4 x float> }> %4
}
```

Метаданные говорят остальное: вход — константный буфер по `air.location_index` 14 в адресном
пространстве 2, 48 байт; выход — `air.render_target` 0. В NIR это **два узла**: `nir_load_ubo` и
`nir_store_output`. Между «здесь» и «работает» стоит только обвязка: пройти по модулю LLVM, собрать
`nir_shader`, отдать его `ir3` и нарисовать треугольник — то есть примерно то, что уже делает
`bin/gl_test`, только шейдер приходит не текстом GLSL, а из `.metallib`.

62 % модулей системы не зовут `air.*` вовсе, так что сразу за первым шагом лежит заметная их доля —
и только потом начинаются текстуры, за ними всё остальное.
