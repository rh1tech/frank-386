# Справочник Native DOS API

Этот файл описывает публичный интерфейс нативных приложений murm386/FDOS. Здесь
намеренно нет документации по прикладным compatibility-слоям, инструкций по
сборке, `elf2ez` и сторонним библиотекам.

Текущая версия ABI: **21** (`DOS_API_VERSION`).

## 1. ABI и системная таблица

Нативные приложения вызывают сервисы firmware через read-only таблицу по
`DOS_OS_API_SYS_TABLE_BASE` (`0x10100000`). Типизированные обёртки находятся в
публичных заголовках `apps/api`; напрямую индексировать таблицу приложению не
следует.

Таблица append-only: новые публичные сервисы добавляются только в конец, после
чего увеличивается `DOS_API_VERSION`. Значение существующего слота менять
нельзя.

Основные публичные слоты:

| Слот | Интерфейс | Заголовок |
|---:|---|---|
| 0 | `get_PC()` | `dos-api.h` |
| 2 | `bios_intcall()` | `dos-api.h` |
| 3..8 | чтение/запись физической памяти гостя | `dos_phys.h` |
| 9 | `psram_size()` | `psram.h` |
| 10 | backend форматирования | runtime |
| 11 | завершение native-процесса | `dos_process.h` |
| 12 | `dos_yield()` | `dos_yield.h` |
| 101 | диагностический latch | `dos_diag.h` |
| 102 | backend сканирования | runtime |
| 103..105,109 | SRAM memory primitives | runtime |
| 106 | состояние DOS termination | `dos_process.h` |
| 107 | информация текущего EZ-процесса | `ez.h` |
| 108,110 | информация о DOS-памяти | runtime |
| 111..115 | allocator native-процесса | runtime `stdlib.h` |
| 116 | `dos_video_get_buffer()` | `dos_video.h` |
| 117 | `set_tsr0_callback()` | `tsr_callback.h` |
| 118 | `set_tsr1_callback()` | `tsr_callback.h` |
| 119 | `dos_keyboard_get_event()` | `dos_keyboard.h` |

Слоты 13..100 — backend compiler-runtime/math. Обычно приложение получает к
ним доступ через операторы C и `math.h`, а не напрямую.

## 2. CPU, BIOS и физическая память гостя

`dos-api.h` содержит низкоуровневые интерфейсы:

```c
PC *get_PC(void);
void bios_intcall(CPU *cpu, int intnum, const char *owner);
```

При наличии более высокоуровневой DOS/runtime-обёртки следует использовать её.

`dos_phys.h` работает с физическими адресами гостя через активный backend
памяти murm386:

```c
uint8_t  dos_phys_read8(uint32_t addr);
uint16_t dos_phys_read16(uint32_t addr);
uint32_t dos_phys_read32(uint32_t addr);
void dos_phys_write8(uint32_t addr, uint8_t value);
void dos_phys_write16(uint32_t addr, uint16_t value);
void dos_phys_write32(uint32_t addr, uint32_t value);
```

Нельзя самостоятельно превращать guest address в native pointer, если такой
pointer явно не выдан соответствующим API.

## 3. Кооперативная точка обслуживания платформы

`dos_yield.h`:

```c
uint32_t dos_yield(void);
```

Во время выполнения native-приложение владеет core0. `dos_yield()` даёт
эмулятору кооперативную точку обслуживания и возвращает текущее время эмулятора
в микросекундах.

`dos_yield()` — системный primitive, **не scheduler таймеров**. Планировщики
вроде DMX TSM относятся к соответствующим compatibility-библиотекам, а не к
DOS API.

## 4. Native timer/service callbacks

`tsr_callback.h`:

```c
typedef void (*tsr_callback_t)(void);

tsr_callback_t set_tsr0_callback(tsr_callback_t callback);
tsr_callback_t set_tsr1_callback(tsr_callback_t callback);
```

Setter атомарно вытесняет текущий callback и возвращает предыдущий. Цепочка
строится явно:

```c
static tsr_callback_t previous;

static void handler(void)
{
    /* короткое realtime-обслуживание */
    if (previous)
        previous();
}

previous = set_tsr0_callback(handler);
```

Вызов предыдущего callback продолжает цепочку; отсутствие такого вызова
подавляет вытесненную работу.

### TSR0

TSR0 выполняется на core0 из существующего высокочастотного timer path перед
штатной обработкой гостевого системного таймера. Default callback обслуживает
эмулируемый PIT/IRQ0. Обычно replacement должен вызвать предыдущий callback,
если он специально не забирает эту работу себе.

Callback асинхронен относительно foreground native-кода. В нём нельзя делать
долгие DOS/file/stdio операции.

### TSR1

TSR1 выполняется на core1 из IRQ обработки каждой видеостроки (video scanline
DMA). Это realtime hook для короткой работы, независимой от core0.

Долгий TSR1 callback нарушает video deadlines и может привести к срыву или
пропаданию картинки. В нём нельзя блокироваться, вызывать DOS, allocator,
форматированный I/O и выполнять другую тяжёлую работу.

Код callback и всё состояние, к которому он обращается, должны оставаться
резидентными всё время, пока hook установлен.

## 5. Native keyboard event queue

`dos_keyboard.h`:

```c
typedef struct dos_keyboard_event {
    int is_down;
    int keycode;
} dos_keyboard_event_t;

#define DOS_KEYBOARD_EVENT_CONSUME 0x01u
#define DOS_KEYBOARD_EVENT_NEWEST  0x02u

int dos_keyboard_get_event(dos_keyboard_event_t *event, uint32_t flags);
```

Метод читает host-очередь клавиатурных событий до i8042/guest IRQ1. `keycode` —
Linux input keycode, `is_down` равен 1 для нажатия и 0 для отпускания.
Возвращается `1`, если событие получено, `0`, если очередь пуста, и `-1` для
неверных аргументов/флагов.

Два бита флагов независимы:

| Флаги | Результат |
|---|---|
| `0` | посмотреть самое старое событие, не удаляя его |
| `DOS_KEYBOARD_EVENT_CONSUME` | забрать и удалить самое старое событие |
| `DOS_KEYBOARD_EVENT_NEWEST` | посмотреть самое новое событие, не удаляя его |
| `DOS_KEYBOARD_EVENT_CONSUME | DOS_KEYBOARD_EVENT_NEWEST` | забрать и удалить самое новое событие |

При удалении самого нового события более старые элементы очереди сохраняются.
Операция выполняется атомарно относительно timer-обработчика PS/2.

## 6. Raw video backing store

`dos_video.h`:

```c
uint8_t *dos_video_get_buffer(uint32_t *size);
```

Возвращает raw backing buffer VGA renderer и его физический размер. Такой
доступ обходит VGA aperture translation, latch, write mode, маски и plane
logic. Использовать его можно только когда приложение сознательно знает layout
текущего backing store.

Для обычной VGA-семантики следует обращаться через guest physical VGA aperture.

## 7. Conventional memory DOS

`dos_mem.h`:

```c
void *dos_alloc_low(size_t size);
void dos_free_low(void *ptr);
uint16_t dos_ptr_segment(const void *ptr);
uint32_t dos_ptr_linear(const void *ptr);
```

`dos_alloc_low()` выделяет conventional memory, пригодную для real-mode API.
`dos_ptr_segment()` возвращает paragraph segment для выровненного DOS-блока.
`dos_ptr_linear()` возвращает точный linear address либо `UINT32_MAX`, если
pointer не относится к первому MiB гостевой RAM.

Обычные `malloc/calloc/realloc/free` — отдельный интерфейс. Начиная с API v17
памятью управляет allocator native-процесса через слоты 111..115.

## 8. Состояние процесса и EZ

`dos_process.h`:

```c
void dos_process_exit(int status) __attribute__((noreturn));
bool dos_termination_requested(void);
```

`dos_termination_requested()` нужен CRT/fini-коду, чтобы не разрушать состояние,
которое DOS termination намеренно оставил резидентным.

`ez.h` предоставляет информацию о текущем EZ-процессе через слот 107. Формат
файла EZ и утилита `elf2ez` не относятся к DOS API и документируются отдельно.

## 8. Владение векторами прерываний

`dos_vect.h` предоставляет native-аналог DOS getvect/setvect:

```c
typedef bool (*dos_native_vector_handler_t)(void *cpu);

bool dos_native_setvect(dos_native_vector_t *state,
                        unsigned intno,
                        dos_native_vector_handler_t handler);
void dos_native_restorevect(dos_native_vector_t *state);
```

ARM-адрес нельзя напрямую записать в x86 IVT, поэтому wrapper сохраняет и
вытесненный x86 vector, и вытесненный native BIOS handler, а при teardown
восстанавливает оба уровня.

## 9. Файлы, каталоги, консоль и libc subset

`io.h`, `fcntl.h`, `direct.h`, `stdio.h`, `conio.h`, `string.h`, `stdlib.h`,
`ctype.h`, `math.h` описывают только реально реализованный native runtime
subset. Это не aliases к host/Pico SDK libc.

Доступны DOS-style `open/read/write/lseek/close`, directory helpers и native
`FILE` wrapper для `fopen/fread/fwrite/fseek`, formatted I/O и scanning.

Длинные DOS I/O операции используют `dos_yield()` только как общий service
point эмулятора и ничего не знают о прикладных timer schedulers.

## 10. Hardware availability и port I/O

`sound_hw.h`:

```c
uint32_t sound_hw_mask(void);
```

возвращает реально включённые/созданные звуковые устройства. `conio.h`
предоставляет port-I/O wrappers (`inp`, `inpw`, `outp`, ...).

Приложению не следует повторно читать конфигурационные файлы murm386 для
определения доступного hardware.

## 11. Диагностика

При `DIAG` заголовок `dos_diag.h` предоставляет дешёвый diagnostic latch через
слот 101. Запись — обычный native store без входа в DOS, stdio, FatFS или
scheduler.

## 12. Граница API

`apps/api` содержит только platform/DOS/runtime интерфейсы, пригодные для
несвязанных между собой native-приложений.

Compatibility-реализации сторонних библиотек сюда не входят. В частности,
**DMX TSM не является DOS API**. Native-замена DMX/TSM для Doom находится в
`apps/doom` и строится поверх общих primitives `set_tsr0_callback()` и
`dos_yield()`.
