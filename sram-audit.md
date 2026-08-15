# murm386 — ревизия SRAM: символы, живущие только на старте

Сборка: `m1p2-286-504MHz-1.6V-P66-I2S-v1.06-emm.elf`. Цель RP2350, `RAM = 0x20000000..0x2003e000` (248 KiB) — основной дефицитный ресурс.

## Итог (что можно вернуть)

| Категория | Размер | Символов | Действие |
|---|---:|---:|---|
| **DEAD: раскрутка стека/EH (никогда не выполняется)** | **4.4 KiB** | 36 | Отключить unwind-таблицы или увести в FLASH |
| **init-only: FDOS `DoConfig()/DoInstall()` (`config.c`)** | **3.5 KiB** | 39 | Оверлей `.bss/.data` на буфер, живущий только после старта |
| init/one-shot?: `LH`/UMB в `fcom.c` | 1.8 KiB | 10 | Проверить против интерактивного `LH`, затем оверлей |
| lib-in-RAM, не горячее (libgcc/libm) | 4.6 KiB | 28 | Увести в FLASH |
| runtime-ABI: комплексная плавучка (в таблице DOS API) | 4.9 KiB | 4 | Можно увести в FLASH (низкий риск) |
| init-only, но **обязано** быть в RAM | 0.6 KiB | 4 | Не трогать (перенастраивают flash/clock/psram) |

Быстрый безопасный выигрыш = **EH/unwind + config.c ≈ 7.9 KiB**. С переносом не-горячего libgcc/libm и комплексной плавучки — суммарно до **~15 KiB** RAM (плюс их `.data`-таблицы уедут следом).

Полный поимённый список с классификацией — в `sram-symbols.csv` (681 символ).

## Куда уходит SRAM

`.data` = 86.6 KiB, `.bss` = 36.4 KiB, heap 2 KiB. Но `.data` — это **на 86% код**, а не данные:

- `.time_critical*` (явно помеченные горячие функции) — 62.8 KiB. Из них `cpu.c` = 38.8 KiB (ядро эмулятора 286 — трогать нельзя).
- `.text*`, «утёкший» в RAM — 14.0 KiB.
- реальные инициализируемые данные/таблицы — ~9.7 KiB.

## Первопричина переноса кода в RAM — линкер-скрипт

`memmap.ld.in`, секция `.lca` отправляет весь `.text` во FLASH **кроме** трёх масок:

```
.lca : {
    *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
} > FLASH
```

Всё, что исключено (**весь libgcc.a, весь libm.a**, mem-функции libc), проваливается ниже в `.data`-«ловушку» `*(.text*)` и оседает в RAM. Так в SRAM попадают и раскрутка стека, и редкие libm-функции, и комплексная плавучка — независимо от того, горячие они или нет. Это и есть главный структурный источник «мусора» в SRAM.

---

## A. Блок, который вы просили: символы только для старта

### A1. FDOS-инициализация — `config.c` (`DoConfig()/DoInstall()`) — 3.5 KiB, boot-only ✔

`DoConfig(0/1/2)` и `DoInstall()` вызываются только из `kernel.c` при инициализации FDOS (комментарий там: «long before the first guest program starts»). После разбора CONFIG.SYS/меню/страны эти данные больше не используются. Крупнейшие:

```
1320  .bss   InstallCommands              (INSTALL= из CONFIG.SYS)
 890  .bss   MenuStruct                   (загрузочное меню CONFIG.SYS)
 351  .data  specificCountriesSupported   (таблица COUNTRY=)
 342  .data  commands                     (таблица директив конфига)
 144  .data  table.2
 128  .bss   ErrorAlreadyPrinted
 … ещё ~30 мелких (numInstallCmds, MenuLine, Menus, UmbState, cfgFile, …)
```
Полностью init-only; код `config.c` и так во FLASH, стоит вернуть только его `.bss/.data`.

### A2. FCOM-инициализация — `LH`/UMB в `fcom.c` — 1.8 KiB, проверить ⚠

```
640  .bss  lh_umbRegion
512  .bss  lh_block
512  .bss  availBlock.4
128  .bss  lh_fnam
 …  lh_optL/optS/… флаги
```
Это снимок карты UMB для загрузки high. Если `LH` используется только в AUTOEXEC (на старте) — блок init-only и переносим. Если интерактивный `LH` доступен в работающей системе — данные нужны в рантайме. **Нужно подтвердить** прежде чем трогать.

### A3. Boot-функции в RAM, которые НЕЛЬЗЯ переносить — 0.6 KiB

```
284  psram_init_with_freq   (libpsram)
168  reconfigure_clocks     (main.c, __no_inline_not_in_flash_func)
140  set_flash_timings      (main.c, __no_inline_not_in_flash_func)
  8  psram_init
```
Все выполняются один раз на старте, но обязаны быть в RAM: они перенастраивают тайминги flash/QMI и тактирование — исполнять их из flash в этот момент нельзя. Оставить как есть.

> Прикладной init-код (native BIOS `bios_*`, `fdos_*`, kernel) в SRAM **не** лежит — он уже во FLASH через `.lca`. Поэтому по BIOS/FDOS/FCOM «init-only» экономия — это их **данные** (A1/A2), а не код.

---

## B. Мёртвый груз в RAM: раскрутка стека / EH — 4.4 KiB ✔ (лучший кандидат)

36 функций из `libgcc.a(unwind-arm.o / pr-support.o / libunwind.o)`: `_Unwind_*`, `__gnu_unwind_*`, `unwind_phase2_forced`, personality-роутины `__aeabi_unwind_cpp_pr0/1/2` и т.д. В прошивке нет ни C++-исключений, ни backtrace (проверено grep’ом) — этот код **не исполняется вообще**, но занимает быструю SRAM. Втягивается только потому, что GCC по умолчанию генерит `.ARM.exidx`/`.extab` с ссылками на personality-функции.

## C. Не-горячий lib-код в RAM — 4.6 KiB + комплексная плавучка 4.9 KiB

- Редкие libm: `rint`, `logb`, `frexp`, `ilogb` и пр.
- Комплексная плавучка `__muldc3/__divdc3/__mulsc3/__divsc3` (4.9 KiB) — **не мертва**: экспортируется в таблице DOS API (`dos_api.c`, элементы 95–98) для гостевых приложений. Вызывается по указателю, поэтому спокойно живёт во FLASH; в RAM нужна только если гость реально считает комплексную арифметику в горячем цикле (маловероятно).

---

## Рекомендации (по убыванию «выгода/риск»)

**1. Убрать unwind-таблицы (снимает 4.4 KiB RAM + `.ARM.exidx/.extab` из FLASH).**
Добавить прошивочной цели флаги компиляции:
```
-fno-unwind-tables -fno-asynchronous-unwind-tables
# и для любого C++: -fno-exceptions -fno-rtti
```
Проверить, что линковка проходит без ссылок на `_Unwind_*` (в этом коде их нет). Это удаляет мёртвый код целиком, а не просто переносит.

**2. Не тащить весь libgcc/libm в RAM.** Перед секцией `.data` в `memmap.ld.in` явно поймать не-горячее в FLASH (ld кладёт вход в первую подходящую выходную секцию, поэтому из RAM-ловушки они исчезнут):
```
.libcode_in_flash : {
    *libgcc.a:unwind-arm.o(.text* .rodata*)
    *libgcc.a:pr-support.o(.text* .rodata*)
    *libgcc.a:libunwind.o(.text* .rodata*)
    *libm.a:*(.text* .rodata*)
    /* по желанию — экспортируемая, но не горячая комплексная плавучка: */
    *libgcc.a:_divdc3.o(.text*) *libgcc.a:_muldc3.o(.text*)
    *libgcc.a:_divsc3.o(.text*) *libgcc.a:_mulsc3.o(.text*)
} > FLASH
```
Оставить в RAM только горячие soft-float, реально дергаемые ядром/FPU/аудио (double add/sub/mul, int64 divmod) — их лучше добавлять в RAM точечно, а не «всем архивом».

**3. Оверлей init-данных FDOS (`config.c`, ~3.5 KiB).** Пометить boot-only объекты секцией и разместить её (NOLOAD) поверх области, которая наполняется только после старта (часть `GFX_BUFFER`, кэш JIT или гостевой DOS-RAM):
```c
#define BOOT_DATA __attribute__((section(".boot_reclaim")))
static struct instCmds InstallCommands[MAX_INSTALL_CMDS] BOOT_DATA;
static struct MenuSelector MenuStruct[MENULINESMAX] BOOT_DATA;
```
В линкере — `.boot_reclaim (NOLOAD)` с `ORIGIN`, совпадающим с началом переиспользуемого буфера. Требует гарантии, что buffer не трогается до конца `DoConfig()/DoInstall()`. То же применимо к A2 после подтверждения.

## Метод и оговорки

- В map нет таблицы перекрёстных ссылок (`--cref` не включён), поэтому «init-only» доказывалось сопоставлением размещения (map) и мест вызова/использования (grep по `src/`). Где однозначно подтвердить не удалось (A2, `fcom LH`) — помечено «проверить».
- Классификации/размеры — в `sram-symbols.csv` (колонка `classification`). Значения `cpu.c`/видео/аудио/USB `.time_critical` — рантайм, не трогать.
- Совет: пересобрать с `-Wl,--cref` и `--print-memory-usage` — тогда init-only можно доказывать формально по числу ссылок, а не эвристикой.
