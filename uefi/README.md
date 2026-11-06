# UEFI для redwood

Порт [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) на POCO X5 Pro 5G (redwood).
В апстриме этого устройства нет; порт сделан по образцу `lisa` (тот же SoC-пакет `KodiakPkg`).

Для запуска пересобирать UEFI не нужно — готовый образ лежит в `prebuilt/uefi`, `mkxnuimg.py` берёт его сам.

Что здесь:

- `BASE` — коммит Mu-Silicium, на котором сделан порт;
- `mu-silicium.patch` — правки существующих файлов:
  `BootShim/BootShim.S` (копирует за образом UEFI наш контейнер «XNUP» по адресу 0xF0000000),
  `BootManagerLib` (вместо меню запускает загрузчик XNU из контейнера);
- `files/` — новые файлы: `Platforms/Xiaomi/redwoodPkg` (карта памяти, список драйверов),
  `Silicon/…/BootManagerLib/XnuLoader.[ch]`, `Resources/Configs/redwood.toml`, `Resources/DTBs/redwood.dtb`
  (дерево устройств из vendor_boot телефона), `Binaries/redwood` (драйверы UEFI из прошивки
  `OS1.0.3.0.UMSMIXM`, извлечены [UEFIReader](https://github.com/WOA-Project/UEFIReader) из `xbl.img`,
  с пятью штатными заплатками Silicium — см. `files/Binaries/redwood/README.md`).

## Сборка

Пакеты — как в `setup_env.sh` из Mu-Silicium (на Arch: `git base-devel lld nasm clang llvm aarch64-linux-gnu-gcc
python python-pip python-virtualenv`).

```sh
uefi/apply.sh                                   # клонирует Mu-Silicium (с подмодулями, ~3 ГБ) и накладывает порт
python3 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt
cd Mu-Silicium
source ../venv/bin/activate
python3 build_uefi.py -d redwood -r RELEASE     # → Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

Грабля: зависимость `mu_nasm` ставится через NuGet и требует Mono. Обход без Mono — подложить системный nasm:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -s /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/
```

После сборки `mkxnuimg.py` (его вызывает `./xl enter`) берёт UEFI уже из `Mu-Silicium/`, а
`tools/update-prebuilt.sh` обновляет `prebuilt/uefi`.

`uefi/export.sh` — обратное действие: сохранить правки из `Mu-Silicium/` сюда.

Сборка «с нуля» по этой инструкции заново не проверялась (дерево собиралось по ходу работы над портом).
