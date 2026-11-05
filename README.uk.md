# HakiOS

[English](README.md) · [中文](README.zh.md) · Українська

**Відкрите ядро Darwin від Apple (XNU, xnu-8796.121.2) на Android-телефонах**, так само як Android — це Linux на телефонах: власний
завантажувач, власні драйвери, власна система конфігурацій і власна версія. Те саме ядро працює в QEMU без нічого від Apple і без нічого
з будь-якого телефона (`make qemu-open`). Це дослідницький стенд, а не прошивка: за замовчуванням усе працює з ОЗП, а перезавантаження
повертає телефон в Android.

**Документація:** [docs/](docs/index.md) (англійською) і сайт [hackios-web.vercel.app](https://hackios-web.vercel.app).

**Новини, 4 листопада 2026:** на POCO X5 Pro 5G віконний сервер малює через Metal на Adreno 642L, двома шляхами. hakios-metal: гортання
сторінок 110–115 кадрів на секунду, прокручування в Settings 118, кнопка «додому» 106, рендер 828x1792, розтягнутий на панель; 101, близько 90 і близько 90 на повних 1125x2436. Metal поверх Vulkan від UniXNU: гортання 50–57, прокручування 41.
Панель працює на 120 Гц, і **120 кадрів на секунду не досягнуто**. [Metal на Adreno](docs/reference/metal.md),
[вимірювання](docs/reference/metal-measurements.md).

Почніть із [Getting started](docs/getting-started/index.md), [On the phone](docs/getting-started/phone.md) або
[Port a device](docs/porting/index.md). Що працює, по частинах: [головна сторінка документації](docs/index.md#what-works).

## Репозиторії

| Репозиторій | Вміст |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | **спільний**: порт ядра, усі спільні драйвери, завантажувач, користувацький простір, інструменти, ця документація і `devices/qemu` |
| [`hakios-metal/`](hakios-metal) | модель команд Metal на Adreno через драйвер GPU ядра |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: defconfig, стан, платформа UEFI, скрипт прошивок і шар його SoC (сімейство Qualcomm SC7280) |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20 (MediaTek Helio G85): експериментальний порт із власним завантажувачем. Його немає в маніфесті цього репозиторію |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | прошивка, що запускає завантажувач: Mu-Silicium зі шляхом завантаження, банером і назвою HakiOS, а також U-Boot від HakiOS |
| [hackios-web](https://github.com/oosexgang/hackios-web) | вебсайт |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | старий: вказівник, збережений заради посилань. Користувацький простір тепер у `userland/` тут |

Як вони пов'язані: [репозиторії](docs/porting/repositories.md).

## Що приносите ви, і чого тут ніколи немає

У цих репозиторіях немає нічого від Apple і жодних прошивок вендора. Користувацький простір Darwin, якщо він вам потрібен, ви збираєте самі
з IPSW, який берете самі, а прошивки знімаються з вашого власного телефона; за вас нічого не завантажується, не розшифровується й не
обходиться. Сервіси Apple (активація, Apple ID, iCloud, App Store) не підтримуються і не є метою.
[Що є в репозиторії, а чого ніколи немає](docs/index.md#what-is-in-the-repository-and-what-never-is).

## Внесок

Див. [CONTRIBUTING.md](CONTRIBUTING.md) і [COMMUNITY.md](COMMUNITY.md).

## Ліцензія та юридична сторона

Патч до XNU і похідні від нього файли — під Apple Public Source License 2.0 ([LICENSE.APSL](LICENSE.APSL)), решта — під MIT
([LICENSE](LICENSE)), як зазначено в заголовках файлів. Сторонні компоненти: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md). Що поширюється, а що ні: [LEGAL.md](LEGAL.md). Усе це не
є юридичною порадою.

HakiOS — незалежний дослідницький проєкт. Він **не пов'язаний з Apple і не схвалений нею**; «Darwin», «iPhone», «XNU» та «Apple» названо лише
для опису того, з чим працює код. Якщо ви вважаєте, що файл тут порушує чиїсь права, відкрийте issue, і його буде видалено.

## Команда

hakios — експериментальний проєкт, мета якого — перенести darwin на android-залізо. Наша мета — зробити hakios вільним і доступним для всіх.

@oodze — засновник, @netusd — реклама / ceo-dev, @wannqn — розробник, @hxfuxyyXD — розробник, Rhaenja Hun（杭润嘉） — розробник / спонсор

Дякуємо команді silicium за вихідний код!
