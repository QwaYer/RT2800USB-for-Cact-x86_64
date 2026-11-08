# План работ: RT2800USB-for-Cact x86_64 (CactOS 3.0.0)

Форк от `2.0.0` (i686). Модуль пересобирается под ядро 3.0.0
(`CactKernel-x86_64`) — заголовки и ABI ядра уже 64-битные.

Это `.cctk` — **ET_REL**, который загрузчик ядра отображает и переразмещает;
компилируется одним объектом, а не архивируется.

## Инвентарь 32-битных поверхностей

- `cross/i686-cact-clang.ini` — `-m32`, `cpu=i686`.
- `meson.build` — компиляция `clang -m32 ... -c`; комментарий ссылается на
  i686-файл и `CactKernel-x86_32`/`LocalRepoCactOS-x86_32`.
- `meson_options.txt` — дефолты `kern_root=../CactKernel-x86_32`,
  `local_repo=../LocalRepoCactOS-x86_32`.
- `README.md` — бейджи/ссылки `i686`, `*-x86_32`.
- Код модуля: локальные декларации ядерных функций и обработчиков с 32-битными
  типами (CR3, devfs `off`, `vmm_map`, LBA) — правятся по факту компиляции.

## Решения

1. **Тулчейн**: `cross/x86_64-cact-clang.ini` (`-m64`), сборка `-m64`,
   дефолты → `../CactKernel-x86_64` и `../LocalRepoCactOS-x86_64`.
2. `-fno-pie`, `-mno-mmx/-mno-sse/-mno-sse2` — без изменений (ring 0).
3. **Ширина — по заголовкам ядра** (Фаза 1).

## Этапы

### Фаза 0 — тулчейн и пути

### Фаза 1 — ширина типов

**Milestone:** `ninja -C build-meson` собирает `rt2800usb.cctk` (ELF64/REL).

## Проверки

- **Сборка** — `meson setup build-meson --cross-file cross/x86_64-cact-clang.ini
  -Dkern_root=../CactKernel-x86_64 -Dlocal_repo=../LocalRepoCactOS-x86_64 &&
  ninja -C build-meson`.
- **`readelf -h`** — `ELF64`/`EM_X86_64`, тип `REL`.

## Открытые вопросы

- **Модуль линкуется ядром, а не ld.so** — `clibc.so` тут не участвует.
