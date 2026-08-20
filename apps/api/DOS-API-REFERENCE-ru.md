# Справочник по Native DOS API

Это русская версия `DOS-API-REFERENCE.md`. Имена API, файлов, символов,
директив CMake, ABI-термины и фрагменты кода намеренно оставлены в исходном
виде, чтобы документ можно было использовать непосредственно при разработке.

Этот документ описывает слой нативных ARM-приложений в `apps/api`,
устаревший путь выполнения relocatable ELF, формат исполняемых файлов EZ и
обычный процесс сборки/конвертации через `elf2ez`.

Он рассчитан на два одинаково важных сценария использования:

1. **in-tree приложения** внутри `murm386/apps`, которые напрямую используют
   каталог `apps/api` из репозитория;
2. **standalone-порты/проекты**, которые копируют согласованную версию `apps/api`
   в собственное дерево исходников и собираются с этой копией. Для большого
   стороннего проекта это часто удобнее: порт хранит собственный проверенный
   снимок userspace SDK и его не требуется физически переносить под
   `murm386/apps`.

Native DOS API — это ABI между кодом прошивки/ядра и ARM-приложениями.
Это **не** ABI libc из Pico SDK. Приложения должны использовать заголовки и
runtime из одной согласованной версии `apps/api` и не должны случайно подтягивать
произвольные реализации Pico SDK/newlib в нативный DOS executable.

> **Правило версий:** никогда не зашивайте версию API в приложение или host-
> утилиту. Подключайте `dos_api_version.h` / заголовки EZ проекта и используйте
> `DOS_API_VERSION`. Таблица только расширяется в конец: номера существующих слотов нельзя
> перенумеровывать.

---

## 1. Модель выполнения

Нативная DOS-программа всё равно является DOS-процессом:

- у него есть PSP и обычные правила владения и времени жизни DOS;
- файловые дескрипторы DOS, текущий каталог, DTA, environment и семантика
  дочерних процессов остаются DOS-семантикой;
- low/conventional memory является гостевой x86-памятью;
- код приложения выполняется нативно как ARM/Thumb-код на core 0;
- родительский DOS/x86-контекст выполнения приостанавливается, пока работает
  нативный дочерний процесс.

Поэтому приложение одновременно существует в двух адресных пространствах:

1. **нативное ARM-адресное пространство** — обычные C-объекты, native stack,
   код;
2. **гостевое физическое x86/DOS-пространство** — PSP, IVT, BDA, VGA-память,
   conventional memory, DMA-буферы и структуры DOS.

Не разыменовывайте гостевой физический адрес, например `0xA0000` или `0xB8000`, как ARM-указатель. Используйте API гостевой памяти.

---

## 2. Структура публичного API

Публичные заголовки находятся в:

```text
apps/api/
```

Общая userspace-реализация подключается так:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../api/native-dos-runtime.cmake)
native_dos_runtime_attach(${PROJECT_NAME})
```

`native_dos_runtime_attach()` добавляет bridge для libc/compiler-runtime:

```text
dos-api-sdtfn.c
dos-api-math.c
dos-api-divmod.S
```

и компилирует C runtime bridge с `-fno-builtin`. Это сделано намеренно:
эти файлы определяют такие символы, как `memcpy`, `printf`, вспомогательные
функции compiler EABI и т. п.; GCC не должен незаметно заменять их тела
обратными вызовами libc из toolchain.

Приложение должно подключать только конкретный публичный заголовок, нужный
подсистеме. Не подключайте `dos-api.h` только ради одного небольшого сервиса:
он открывает структуры эмулятора (`PC`, `CPU`), которые могут конфликтовать с
именами приложения и без необходимости связывают приложение с внутренностями
прошивки.

### 2.1 Два поддерживаемых варианта структуры проекта

API можно использовать как непосредственно из дерева, так и как локально скопированный userspace SDK.

**Структура in-tree:**

```text
murm386/
    apps/
        api/
        myapp/
            CMakeLists.txt
            ...
```

Приложение может использовать `../api`.

**Структура standalone с локальной копией:**

```text
my-port/
    api/                    <- copied from murm386/apps/api
    src/
    CMakeLists.txt
    memmap.ld.in
    check_unresolved.cmake
    tools/
        elf2ez[.exe]        <- optional local copy of the host converter
```

Standalone-проект не требуется копировать внутрь дерева исходников murm386.
Вместо этого можно задать одну переменную корня API и использовать пути
относительно неё:

```cmake
set(NATIVE_DOS_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/api")

include("${NATIVE_DOS_API_DIR}/native-dos-runtime.cmake")
native_dos_runtime_attach(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${NATIVE_DOS_API_DIR}"
)

target_sources(${PROJECT_NAME} PRIVATE
    "${NATIVE_DOS_API_DIR}/crt0.c"
    "${NATIVE_DOS_API_DIR}/crt0.S"
)
```

`native-dos-runtime.cmake` ищет свои implementation-файлы относительно
собственного каталога, поэтому тот же модуль работает и после копирования
`apps/api` в другое место.

### 2.2 Что следует копировать

Для standalone-порта предпочтительно копировать **весь каталог `apps/api` как
единый версионируемый комплект**, а не вручную выбранный набор заголовков.

Каталог содержит несколько слоёв, которые развиваются совместно:

- публичные ABI-заголовки и определения слотов;
- адаптеры DOS/libc;
- ARM EABI/compiler-runtime glue;
- определения EZ;
- CRT-код запуска/завершения;
- общий CMake-модуль подключения runtime.

Например, `stdio.h`, скопированный из одной ревизии API, вместе с более старым
`dos-api-sdtfn.c` может успешно скомпилироваться, но ссылаться на сервис
системной таблицы, который эта версия runtime оборачивает неправильно.

Относитесь к локальной копии API как к снимку небольшого SDK:

```text
third_party/native-dos-api/
    ...exact copy of apps/api...
```

Зафиксируйте, из какой ревизии murm386/API взята копия. При обновлении
заменяйте или сливайте **весь снимок API согласованно**, затем пересобирайте
проект и повторно запускайте проверку неразрешённых символов.

Специфичный для приложения compatibility-код обычно должен оставаться вне
этого каталога. Если отсутствующая функция или сервис действительно имеет
общий характер, сначала добавьте его в канонический `apps/api`, а затем
обновите локальную копию. Так каждый большой порт не превращается в отдельный
fork runtime.

### 2.3 Что не входит в копируемый API

Standalone-приложению всё равно нужен небольшой проектный/сборочный каркас,
который сам по себе не является частью `apps/api`:

- `CMakeLists.txt` приложения;
- подходящий linker script/template для relocatable link (`memmap.ld.in`);
- проверка unresolved symbols;
- host-бинарник `elf2ez` либо путь к host-утилите из репозитория.

Для нового проекта `apps/test` лучше подходит как исходный каркас,
чем `apps/doom`: у него небольшой набор исходников, но он использует те же
механизмы runtime/CRT без платформенного слоя большой игры.

---

## 3. Системная таблица

Системная таблица прошивки начинается по адресу:

```c
#define DOS_OS_API_SYS_TABLE_BASE ((void *)0x10100000ul)
```

Обёртки в `apps/api` вызывают сервисы косвенно через эту таблицу.

Основные фиксированные слоты:

| Слот | Сервис |
|---:|---|
| 0 | `get_PC()` |
| 1 | таблица нативных обработчиков BIOS (`handlers[]`) |
| 2 | `bios_intcall()` |
| 3..5 | чтение гостевой физической памяти 8/16/32 |
| 6..8 | запись гостевой физической памяти 8/16/32 |
| 9 | размер/сервис PSRAM, используемый публичной PSRAM-обёрткой |
| 10 | backend `vsnprintf` в прошивке |
| 11 | завершение/unwind нативного процесса |
| 12 | cooperative `dos_yield()` |
| 13..18 | ранние compiler/division helpers, сохранённые ABI |
| 19..100 | математические и ARM EABI/compiler-runtime сервисы |
| 101 | диагностический latch |
| 102 | backend `vsscanf` в прошивке |
| 103 | SRAM-resident `memcpy` для нативных приложений |
| 104 | общий SRAM-resident `memset` (`nf_memset`) |
| 105 | SRAM-resident `memcmp` для нативных приложений |
| 106 | состояние завершения native DOS |
| 107 | информация о текущем EZ-процессе |
| 108 | размер DOS-блока по сегменту данных |
| 109 | SRAM-resident `memmove` для native app |
| 110 | размер крупнейшего свободного DOS-блока в байтах |
| 111..115 | принадлежащий ядру allocator native app: `malloc/calloc/realloc/free/largest` |
| 116 | прямой native pointer на backing buffer VGA renderer (`gfx_buffer`) и его размер |

Новые сервисы добавляйте только в конец. Программа, которой нужен более новый
слот, должна объявлять более новый `DOS_API_VERSION`; загрузчик старой прошивки
отклонит её до начала выполнения.

Слоты 19..100 символически определены в `dos_math_api.h`. Не дублируйте их
числовые значения в других местах. Текущая версия ABI — `DOS_API_VERSION 19`;
слот 116 добавляет опциональный прямой доступ к backing buffer VGA renderer.

---

## 4. Базовые низкоуровневые сервисы

### 4.1 `dos-api.h`

Используйте этот заголовок только тогда, когда действительно нужен прямой
доступ к объекту CPU/PC эмулятора.

```c
PC *pc = get_PC();
bios_intcall(pc->cpu, 0x21, "owner string");
```

Он также определяет вспомогательные функции отображения conventional guest RAM:

```c
void *dos_guest_linear_ptr(uint32_t linear);
void *dos_guest_far_ptr(uint16_t seg, uint16_t off);
```

Guest RAM отображается в адресное пространство нативного приложения начиная с
`DOS_GUEST_RAM_BASE` (`0x11000000` в текущем ABI). Эти функции предназначены
для стандартных структур DOS, действительно находящихся в conventional guest RAM.

### 4.2 `dos_phys.h`

Используйте для **доступа к физической/эмулируемой памяти**, особенно к памяти,
семантика которой реализуется устройствами:

```c
uint8_t  dos_phys_read8(uint32_t addr);
uint16_t dos_phys_read16(uint32_t addr);
uint32_t dos_phys_read32(uint32_t addr);

void dos_phys_write8(uint32_t addr, uint8_t value);
void dos_phys_write16(uint32_t addr, uint16_t value);
void dos_phys_write32(uint32_t addr, uint32_t value);
```

Примеры:

```c
dos_phys_write8(0xA0000, pixel);   /* VGA aperture */
dos_phys_write16(0xB8000, cell);   /* text VRAM */
```

Это не то же самое, что `dos_guest_far_ptr()`: прямой native pointer не
воспроизводит семантику VGA planes/write mode/устройства.

### 4.3 `dos_video.h`: быстрый прямой доступ к видеобуферу

Для native renderer, которым нужен максимально короткий путь записи, API также
открывает raw backing store VGA renderer:

```c
uint32_t size;
uint8_t *vram = dos_video_get_buffer(&size);
```

Возвращаемый pointer — native-адрес буфера, используемого VGA/HDMI renderer как
`gfx_buffer`; `size` — его размер в байтах (сейчас 256 КиБ). На этом пути нет
вызова системной таблицы на каждый пиксель/байт, поэтому обычные native stores,
`memcpy()` и `memset()` могут работать с полной скоростью нативной памяти.

**Это expert/fast-path interface, а не альтернативная эмуляция x86 VGA
aperture.** Прямая запись полностью обходит `vga_mem_write()`. Поэтому не
применяются VGA memory-map selection, chain-4/odd-even addressing, latches,
write modes, set/reset logic, bit masks и sequencer plane-write masks. Запись
байта в `vram[n]` означает буквально «записать байт `n` raw backing buffer». В
общем случае это не эквивалентно:

```c
dos_phys_write8(0xA0000u + n, value);
```

Используйте `dos_phys_write*()`, когда программе нужна обычная семантика VGA
register/aperture. Используйте `dos_video_get_buffer()` только тогда, когда
приложение сознательно знает организацию backing buffer для активного video
mode (например, native renderer сразу формирует именно это представление).

Video core может параллельно читать тот же буфер. API не предоставляет
неявных locking, dirty tracking или frame synchronization; если нужны обновления
без tearing, приложение должно само организовать соответствующий порядок
записи. Никогда не выходите за возвращённый размер.

### 4.4 `conio.h`

Нативный ввод-вывод через порты:

```c
uint8_t  inp(uint16_t port);
uint16_t inpw(uint16_t port);
void outp(uint16_t port, uint8_t value);
void outpw(uint16_t port, uint16_t value);
```

Эти функции обращаются к эмулируемой шине PC I/O, а не к аппаратным регистрам RP2xxx.

---

## 5. Память DOS

`dos_mem.h` предоставляет выделение conventional memory и преобразование адресов:

```c
void *dos_alloc_low(size_t size);
uint16_t dos_ptr_segment(const void *ptr);
uint32_t dos_ptr_linear(const void *ptr);
```

### `dos_alloc_low`

Выделяет блок conventional memory DOS и возвращает native pointer в guest RAM.

Используйте его для буферов, которые должны быть видимы real-mode DOS/BIOS API.

### `dos_ptr_segment`

Преобразует выровненный по параграфу указатель на conventional memory DOS в
DOS-сегмент. Возвращает ноль, если указатель находится вне conventional RAM
или не выровнен по параграфу.

Типичный пример:

```c
void *buf = dos_alloc_low(4096);
uint16_t seg = dos_ptr_segment(buf);
/* pass seg:0 to a BIOS/DOS interface */
```

### `dos_ptr_linear`

Возвращает точный 20-битный гостевой физический/линейный адрес любого
указателя внутри первого MiB guest RAM либо `UINT32_MAX` при ошибке.

Используйте это при программировании ISA DMA, когда важен точный адрес байта.

---

## 6. API файлов и каталогов DOS

Нативные приложения не используют объекты `FILE` firmware/newlib для файлов
DOS. Файловый слой основан на DOS handles.

### 6.1 Низкоуровневые файловые дескрипторы

Заголовки:

```text
fcntl.h
io.h
direct.h
sys/stat.h
```

Публичный интерфейс:

```c
int open(const char *path, int flags, ...);
int read(int handle, void *buffer, unsigned count);
int write(int handle, const void *buffer, unsigned count);
int close(int handle);

int32_t lseek(int handle, int32_t offset, int origin);
int32_t filelength(int handle);
int fstat(int handle, struct stat *info);

int access(const char *path, int mode);
int mkdir(const char *path, int mode);
int remove(const char *path);
```

Сейчас поддерживаются следующие флаги open:

```c
O_RDONLY
O_WRONLY
O_CREAT
O_TRUNC
O_BINARY
```

`O_BINARY` сейчас равен нулю в compatibility-заголовке. Не предполагайте, что
native stream layer выполняет характерное для host-среды преобразование CR/LF.

Длинные операции `read()` / `write()` являются cooperative service points:
между порциями DOS I/O runtime выполняет yield, чтобы таймеры, эмулируемые
устройства и ожидающие guest IRQ продолжали обслуживаться, пока core 0 занят
нативным процессом.

### 6.2 `stdio.h`

`FILE` намеренно является opaque-типом:

```c
typedef struct native_dos_FILE FILE;
```

Доступный интерфейс включает:

```c
FILE *fopen(...);
int fclose(...);

size_t fread(...);
size_t fwrite(...);
int fseek(...);
long ftell(...);
void rewind(...);
int feof(...);

int fputc(...);
int fputs(...);
int putchar(...);
int puts(...);
int getchar(void);

int printf(...);
int vprintf(...);
int fprintf(...);
int sprintf(...);
int vsnprintf(...);

int sscanf(...);
int fscanf(...);
```

Политика форматирования/сканирования:

- владение DOS stream и реальный файловый I/O остаются в native DOS runtime;
- нейтральные примитивы форматирования/сканирования по возможности должны повторно использовать backend libc прошивки;
- `vsnprintf` является базовым backend форматирования;
- `vsscanf` является базовым backend сканирования;
- не создавайте в `apps/api` второй независимый parser libc.

Такое разделение не позволяет передавать объекты native DOS `FILE *` в libc
прошивки, но позволяет повторно использовать libc для грамматики форматов.

---

## 7. Общая совместимость с libc

Такие заголовки, как `string.h`, `stdlib.h`, `ctype.h` и `math.h`, объявляют
только функции, которые действительно предоставляет native runtime.

**Не** считайте эти заголовки алиасами libc из toolchain.

Важные моменты:

- публичные `malloc/calloc/realloc/free` остаются частью userspace libc API, но
  само выделение памяти с API v17 выполняет ядро через слоты 111..115;
- allocator сначала использует приватный PSRAM-интервал текущего native EXEC,
  затем DOS conventional memory; его состояние принадлежит процессу и корректно
  сохраняется/восстанавливается при nested EXEC;
- `malloc_largest_block()` возвращает максимум из крупнейшего свободного блока
  process-local PSRAM heap и крупнейшего доступного DOS-блока;
- `dos_malloc_set_policy()` выбирает поведение userspace-обёртки при OOM:
  вернуть `NULL`, выполнить `exit(1)` либо сначала вывести сообщение и выйти;
- `exit()` — операция native process/CRT, а не завершение процесса Pico SDK;
- `memcpy/memset/memcmp/memmove` используют явные firmware/SRAM-сервисы, а не произвольные реализации flash libc;
- сгенерированные компилятором `__aeabi_*` и математические операции предоставляются `dos-api-math.c` / `dos-api-divmod.S`.

При добавлении отсутствующей стандартной функции сначала определите, к какой
категории она относится:

1. нейтральная операция, подходящая для одного примитива системной таблицы прошивки;
2. DOS-специфичный адаптер (файлы, консоль, процесс);
3. тривиальный локальный inline/helper.

Не следует автоматически заново реализовывать целые подсистемы libc в userspace.

---

## 8. Завершение процесса

### 8.1 Состояние завершения DOS

`dos_process.h` предоставляет:

```c
void dos_process_exit(int status) __attribute__((noreturn));
bool dos_termination_requested(void);
```

`dos_process_exit()` — принадлежащий ядру путь завершения, используемый legacy
ELF runtime.

`dos_termination_requested()` важен для очистки C runtime. Если программа уже
завершилась через DOS-семантику (например, TSR termination), обработка
destructors/fini не должна разрушать резидентное состояние.

### 8.2 `exit()`

Приложения вызывают стандартный:

```c
exit(status);
```

Runtime выбирает соответствующий внутренний механизм:

- legacy ELF: unwind к принадлежащему ядру native main trampoline;
- EZ: локальный unwind к EZ CRT main trampoline, так что снаружи это выглядит
  как возврат `status` из `main()`, после чего выполняется обычная userspace fini sequence.

Код приложения не должен самостоятельно сохранять/восстанавливать native ARM SP.

---

## 9. Кооперативное выполнение и `dos_yield()`

Нативное приложение выполняется синхронно на core 0. Пока его `main()` активен,
обычный внешний CPU loop эмулятора приостановлен.

Поэтому нативные приложения, ожидающие/опрашивающие время или устройства,
должны достигать cooperative service points.

```c
uint32_t now_us = dos_yield();
```

Service point:

1. обслуживает эмулируемые устройства и host input;
2. позволяет выполнять cooperative native timer services;
3. позволяет ожидающим guest hardware IRQ handlers выполняться по обычному пути
   PIC -> IVT, не возобновляя замороженный CS:IP родительского процесса;
4. возвращает микросекундные часы эмулятора.

Выполнение guest IRQ использует синтетическую границу возврата. Финальный
`IRET` возвращается на эту границу, после чего guest CPU slice немедленно
останавливается; выполнение по приостановленному адресу родителя не допускается.

### Практическое правило

Любой native busy wait обязан выполнять yield.

Bad:

```c
while (ticcount < target)
    ;
```

Good:

```c
while (ticcount < target)
    TSM_Yield();     /* or dos_yield(), depending on subsystem */
```

Long DOS reads/writes already yield internally between chunks.

---

## 10. Кооперативный сервис таймеров (`TSM_*`)

Runtime предоставляет cooperative timer API в стиле DMX, используемый такими
портами, как DOOM:

```c
void TSM_Install(int rate);
int TSM_NewService(int (*service)(void), int rate, int priority, int pause);
void TSM_DelService(int id);
void TSM_PauseService(int id);
void TSM_ResumeService(int id);
void TSM_Remove(void);
void TSM_Yield(void);
uint32_t TSM_YieldTime(void);
```

`TSM_Yield()` обслуживает таймеры и отбрасывает возвращённое время;
`TSM_YieldTime()` выполняет тот же service point и возвращает текущее время
эмулятора в микросекундах.

Callbacks выполняются в обычном контексте приложения в service points. Это не
асинхронные timer IRQ callbacks RP2xxx.

Так сделано намеренно: произвольный C-код приложения не выполняется конкурентно
с тем же native stack/runtime state.

---

## 11. Нативная замена векторов прерываний

`dos_vect.h` предоставляет нативный аналог механизма владения DOS
`_dos_getvect/_dos_setvect`:

```c
typedef bool (*dos_native_vector_handler_t)(void *cpu);

typedef struct dos_native_vector {
    uint16_t intno;
    uint16_t old_off;
    uint16_t old_seg;
    dos_native_vector_handler_t old_handler;
    bool installed;
} dos_native_vector_t;

bool dos_native_setvect(dos_native_vector_t *state,
                        unsigned intno,
                        dos_native_vector_handler_t handler);

void dos_native_restorevect(dos_native_vector_t *state);
```

Нативную ARM-функцию нельзя непосредственно записать в x86 IVT. Поэтому при
установке сохраняются **оба** вытесняемых уровня:

- старый x86 IVT vector;
- старый native BIOS entry `handlers[intno]`.

Затем IVT entry направляется на стандартный native BIOS hook FFE0, а ARM callback
устанавливается в `handlers[]`.

При восстановлении возвращаются оба вытесненных владельца.

Неявного chaining нет. Это соответствует обычной DOS-семантике владения
`setvect`: приложения, полностью заменяющие IRQ handler (классический пример —
keyboard IRQ1 в DOOM), не обязаны автоматически вызывать вытесненный handler.

Перед обычным shutdown процесса установленный vector всегда следует
восстановить. Runtime процесса также должен гарантировать, что оставленные
native callbacks не переживут процесс, которому принадлежит их код.

---

## 12. Доступность оборудования

`sound_hw.h` сообщает об устройствах, реально созданных/включённых эмулятором:

```c
uint32_t sound_hw_mask(void);
```

Bits:

```text
SOUND_HW_PC_SPEAKER
SOUND_HW_ADLIB
SOUND_HW_SB16
SOUND_HW_TANDY
SOUND_HW_COVOX
SOUND_HW_MPU401
SOUND_HW_DSS
```

Используйте это вместо повторного чтения файлов конфигурации прошивки из приложения.

---

# Часть II — Сборка нативных приложений

## 13. Создание нового приложения с нуля

Есть два разумных способа начать.

### 13.1 Проект внутри дерева murm386

Создайте новый каталог рядом с `apps/test`, используйте `../api` и сначала оставьте
в программе только `main()` и общий CRT/runtime. Это самый короткий путь к
заведомо рабочей сборке.

### 13.2 Самостоятельный сторонний порт

Для большого существующего дерева исходников скопируйте `apps/api` внутрь порта, а не
переносите сам порт внутрь дерева murm386.  Практическая последовательность:

1. скопировать в проект весь каталог `apps/api`;
2. скопировать/адаптировать небольшой CMake/linker/unresolved-symbol каркас из
   `apps/test`;
3. направить `NATIVE_DOS_API_DIR` на локальную копию API;
4. сначала собрать тривиальный `main()` и проверить получение корректного EZ
   executable;
5. только после этого подключать сторонние исходники группами;
6. добавлять локальные для проекта compatibility-адаптеры платформенного кода;
7. переносить функциональность в `apps/api` только если она в общем виде
   полезна другим нативным DOS-приложениям.

Это разделяет два вида работ, которые легко смешать при портировании:

- **работа над userspace SDK** — общие средства DOS API/libc/compiler-runtime;
- **адаптация приложения** — renderer, sound backend, конфигурация, допущения
  старого компилятора, специфичное для приложения владение IRQ и т. п.

Вторая категория должна оставаться в порте. Первая относится к `apps/api`.

### 13.3 Выбирайте минимальный референсный проект

Используйте эти примеры из репозитория для разных задач:

- `apps/test` — минимальный runtime, CRT, поведение аргументов/exit и жизненный
  цикл TSR;
- `apps/doom` — сложный пример гостевой физической VGA-памяти, native IRQ
  replacement, cooperative timing, аудиооборудования и большого legacy C-порта;
- `apps/elf2ez` — исходник конвертера и пример кода, который намеренно может
  собираться и как native-DOS приложение, и как host-tool.

При самостоятельной разработке начинайте с `apps/test`, а к более крупным
портам обращайтесь только за той подсистемой, которая действительно нужна.

## 14. Общий каркас CMake

Нативное приложение сначала собирается как relocatable ARM image:

```cmake
project(myapp C CXX ASM)
pico_sdk_init()

add_executable(${PROJECT_NAME}
    main.c
    # ...
)

# In-tree:
#set(NATIVE_DOS_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../api")

# Standalone/vendored:
set(NATIVE_DOS_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/api")

include("${NATIVE_DOS_API_DIR}/native-dos-runtime.cmake")
native_dos_runtime_attach(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${NATIVE_DOS_API_DIR}"
)

configure_file(memmap.ld.in memmap.ld @ONLY)
pico_set_linker_script(${PROJECT_NAME}
    ${CMAKE_CURRENT_BINARY_DIR}/memmap.ld)

target_link_options(${PROJECT_NAME} PRIVATE
    -Xlinker --print-memory-usage
    -r
)

target_compile_definitions(${PROJECT_NAME} PRIVATE ELF_MODE)
```

Важные моменты:

- `-r` используется намеренно: промежуточный результат — ET_REL, а не обычный
  executable прошивки RP2xxx;
- не линкуйте в приложение `pico_runtime`, `pico_stdlib`, host stdio и т. п.,
  если конкретная userspace-архитектура явно этого не поддерживает;
- `-fno-jump-tables` обычно используется для Thumb-1, чтобы избежать нежелательных
  compiler runtime symbols;
- после relocatable link обязательно проверяйте unresolved symbols.

---

## 15. Проверка неразрешённых символов

Relocatable link по определению допускает unresolved symbols. У native DOS
application нет обычного dynamic linker, поэтому неожиданные unresolved
references должны приводить к ошибке сборки.

Сборка приложения должна запускать `nm -u` через `check_unresolved.cmake`.

При сборке EZ только шесть CRT pseudo-symbols границ массивов должны оставаться
unresolved в ET_REL:

```text
__ez_preinit_array_start
__ez_preinit_array_end
__ez_init_array_start
__ez_init_array_end
__ez_fini_array_start
__ez_fini_array_end
```

`elf2ez` синтезирует их после обнаружения и размещения достижимых
startup-array sections.

Любой другой unresolved symbol является ошибкой сборки.

---

# Часть III — Режим legacy ELF

## 16. Формат legacy native ELF

Legacy loader непосредственно загружает relocatable ARM ELF.

Kernel loader отвечает за значительно большее, чем обычная DOS-загрузка:

- разбирает ELF sections, symbols и relocations;
- обнаруживает достижимые зависимости;
- размещает sections;
- применяет relocations;
- находит callback версии API;
- находит metadata требований процесса;
- находит `_init`, `main`, `_fini`, signal hooks и startup arrays;
- подготавливает native и guest DOS stacks;
- выполняет последовательность CRT в коде ядра;
- владеет native recovery frame `main()`, используемым `exit()`.

Этот путь полезен для разработки и совместимости, но превращает ядро в
реализацию ELF linker/CRT.

### Требования legacy-процесса

`native_process.h` определяет структуру legacy ELF:

```c
typedef struct native_dos_process_requirements {
    uint32_t struct_size;
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
    uint32_t assigned_native_stack_size;
    uint32_t assigned_dos_stack_size;
    uint32_t app_psram_begin;
    uint32_t app_psram_end;
} native_dos_process_requirements;
```

Приложение экспортирует:

```c
native_dos_process_requirements *
__native_dos_process_requirements(void);
```

Первые поля являются входными требованиями; последующие поля заполняет loader.

Example:

```c
static native_dos_process_requirements req = {
    sizeof(req),
    64u * 1024u,
    4u * 1024u,
    0, 0, 0, 0
};

native_dos_process_requirements *
__native_dos_process_requirements(void)
{
    return &req;
}
```

---

# Часть IV — Режим EZ

## 17. Зачем нужен EZ

EZ выносит знание ELF/linker из ядра DOS.

Build-time утилита `elf2ez` выполняет дорогостоящую работу один раз:

```text
ET_REL ARM ELF
      |
      | elf2ez
      v
EZ executable (.exe)
```

После этого DOS loader работает с компактным фиксированным executable ABI,
а не разбирает ELF section/symbol/string tables.

Граница ответственности устроена так:

### За что отвечает `elf2ez`

- разбор ELF;
- обнаружение зависимостей;
- окончательное размещение image;
- разрешение внутренних relocations image;
- размещение CRT/startup arrays;
- преобразование небольшого оставшегося набора relocations, зависящих от load base, в relocation records EZ.

### За что отвечает DOS EZ loader

- проверка EZ header;
- выделение блока DOS-процесса/PSP;
- загрузка инициализированных байтов image по RVA `0x100`;
- обнуление хвоста, существующего только в памяти;
- применение компактной таблицы EZ relocations;
- выделение/назначение native и DOS stacks;
- формирование `argc/argv`;
- публикация runtime-информации процесса;
- вызов ровно одной entry point.

### За что отвечает EZ CRT

- preinit array;
- init array;
- необязательный `_init`;
- `main`;
- обычный unwind `exit()`;
- необязательный `_fini`;
- fini array в обратном порядке.

Ядру не требуется знать, где в EZ-файле находятся `main`, `_init`, `_fini`,
constructors или destructors.

---

## 18. Структура файла EZ v1

Constants:

```c
EZ_MAGIC          = 0x5a45        /* "EZ" */
EZ_FORMAT_VERSION = 1
EZ_IMAGE_RVA      = 0x00000100
```

Логическая структура файла:

```text
offset 0
+-------------------------------+
| struct ez_file_header         | 64 bytes in v1
| future header extension       |
+-------------------------------+ header_size
| initialized image             |
+-------------------------------+
| optional future file data     |
+-------------------------------+ reloc_offset
| struct ez_reloc[]             | 8 bytes each in v1
+-------------------------------+
```

Правило RVA:

```text
runtime_address = process_base + rva
```

`process_base` — native pointer, соответствующий нулевому смещению сегмента PSP.

Первый сохранённый байт image находится по адресу:

```text
process_base + EZ_IMAGE_RVA
```

Таким образом, 0x100-байтный PSP остаётся в начале блока DOS-процесса.

---

## 19. Заголовок EZ v1

Фиксированный header v1 имеет размер 64 байта и содержит:

```text
magic
version
header_size
flags
required_dos_api_version
native_stack_size
dos_stack_size
image_file_size
image_mem_size
entry_rva
reloc_offset
reloc_count
reloc_entry_size
reserved fields
```

Важная семантика:

### `required_dos_api_version`

Минимальная версия API прошивки, требуемая image. Загрузчик проверяет её
**до выполнения кода приложения**.

### `native_stack_size`

Запрошенный размер native ARM stack. Ноль означает значение loader по умолчанию; ненулевое значение является минимумом и может быть округлено вверх.

### `dos_stack_size`

Guest DOS stack, используемый когда native code входит в DOS/BIOS/guest IRQ execution. Ноль означает значение loader по умолчанию.

### `image_file_size`

Инициализированные байты, физически сохранённые в executable.

### `image_mem_size`

Полный размер image в памяти, включая zero-initialized tail.

### `entry_rva`

Единственная вызываемая native entry point, обычно `__ez_start`.

Бит 0 Thumb function pointer сохраняется.

### `reloc_offset`, `reloc_count`, `reloc_entry_size`

Расположение и формат компактной runtime relocation table.

---

## 20. Флаги образа EZ

EZ v1 определяет:

```text
EZ_FLAG_THUMB
EZ_FLAG_ARMV6M
EZ_FLAG_THUMB2
EZ_FLAG_SOFT_FLOAT
```

`ARMV6M` и `THUMB2` задают взаимно несовместимые минимальные требования к instruction set для v1.

Hard-float images не входят в текущий ABI; native DOS использует soft-float procedure-call ABI.

---

## 21. Релокации EZ

EZ relocations — **не** сырые ELF records `R_ARM_*`.

`elf2ez` разрешает всё, что зависит только от положения внутри готового image.
Загрузчик получает только оставшиеся base-dependent fixups.

EZ v1 types:

```text
0  EZ_RELOC_NONE
1  EZ_RELOC_ABS32
2  EZ_RELOC_THM_ALU_ABS_G0_NC
3  EZ_RELOC_THM_ALU_ABS_G1_NC
4  EZ_RELOC_THM_ALU_ABS_G2_NC
5  EZ_RELOC_THM_ALU_ABS_G3
6  EZ_RELOC_THM_MOVW_ABS_NC
7  EZ_RELOC_THM_MOVT_ABS
```

Каждая запись:

```c
struct ez_reloc {
    uint32_t rva;
    uint8_t type;
    uint8_t reserved[3];
};
```

и имеет размер ровно 8 байт в EZ v1.

Загрузчик обязан отклонять неизвестные ненулевые relocation types.

---

## 22. EZ CRT

EZ-сборка линкует:

```text
apps/api/crt0.c
apps/api/crt0.S
```

Каноническая entry point:

```c
int __ez_start(int argc, char **argv);
```

Порядок запуска:

```text
preinit_array
    ->
init_array
    ->
optional _init
    ->
main
    ->
optional _fini
    ->
fini_array in reverse order
```

Fini path пропускается, если DOS уже запросил завершение процесса через семантику, которая должна сохранить состояние, например TSR termination.

`exit(status)` внутри `main()` не требует от ядра знания stack frame приложения. `crt0.S` сохраняет локальный для EZ recovery SP и делает unwind обратно в CRT так, как если бы `main()` вернул `status`.

---

## 23. Требования EZ-процесса и runtime-информация

Для EZ статические требования являются build metadata, а не callback, выполняемым ядром.

`ez.h` defines:

```c
typedef struct native_ez_process_requirements {
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
} native_ez_process_requirements;
```

`crt0` предоставляет weak default со всеми нулями. Приложение может его переопределить:

```c
#include <ez.h>

const native_ez_process_requirements __native_ez_process_requirements = {
    64u * 1024u,
    4u * 1024u
};
```

`elf2ez` читает этот объект из ET_REL и копирует значения в EZ header. Во время конвертации код приложения не выполняется.

At runtime:

```c
typedef struct native_ez_process_info {
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
    uint32_t app_psram_begin;
    uint32_t app_psram_end;
} native_ez_process_info;

const native_ez_process_info *native_ez_get_process_info(void);
```

Возвращаются ресурсы, реально назначенные loader текущему EZ-процессу.

---

# Часть V — `elf2ez`

## 23. Командная строка

Типичная команда:

```text
elf2ez /y program.crt
```

Имя выходного файла получается заменой расширения исходного файла на:

```text
.exe
```

`/y` отключает запрос подтверждения перезаписи.

Без `/y` перезапись существующего выходного файла подтверждается интерактивно.

Типичная диагностика успешного выполнения:

```text
EZ image: 6804 bytes, 38 relocations, entry=00000131
```

Точные числа зависят от приложения.

---

## 24. Что делает `elf2ez`

Концептуально:

1. открывает входной ET_REL;
2. проверяет формат ARM ELF и нужные section/symbol tables;
3. находит entry symbol EZ CRT;
4. рекурсивно загружает достижимые sections/зависимости symbols;
5. размещает их начиная с `EZ_IMAGE_RVA`;
6. разрешает PC-relative и полностью image-relative relocations;
7. создаёт `ez_reloc` только когда итоговое значение всё ещё зависит от
   `process_base`;
8. строит/синтезирует диапазоны startup arrays, необходимые `crt0`;
9. читает статические требования EZ-процесса;
10. формирует 64-байтный EZ header;
11. записывает header + image + relocation table;
12. выводит итоговый размер image, число relocations и entry RVA.

Итоговый EZ-файл намеренно не содержит ELF symbol/string/section tables.

---

## 25. Host- и DOS-сборки `elf2ez`

`elf2ez.c` спроектирован так, чтобы сама логика конвертации могла собираться
в двух средах:

- как native DOS application с использованием `apps/api`;
- как host tool в `tools/elf2ez` с небольшим host shim.

Host shim должен дублировать только небольшой platform layer, необходимый
утилите (file descriptors и constants), но не on-disk структуры EZ. Типы
формата и metadata процесса берутся из авторитетных заголовков `apps/api` проекта.

Именно host tool обычно вызывается CMake приложения после получения ET_REL.

---

# Часть VI — Рекомендуемый CMake-процесс для EZ

## 26. Сборка `.exe`

Рекомендуемый итоговый процесс:

```text
C/C++/ASM
   |
   | GCC -r
   v
program.elf / ET_REL
   |
   | unresolved-symbol check
   v
program.crt               temporary staging name
   |
   | tools/elf2ez /y
   v
program.exe               EZ v1
```

Пример CMake:

```cmake
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../../bin/${CMAKE_BUILD_TYPE}")
set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../apps/compiled")

if(WIN32)
    set(ELF2EZ_TOOL "${CMAKE_SOURCE_DIR}/../../tools/elf2ez.exe")
else()
    set(ELF2EZ_TOOL "${CMAKE_SOURCE_DIR}/../../tools/elf2ez")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../api/native-dos-runtime.cmake)
native_dos_runtime_attach(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../api/crt0.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../api/crt0.S
)

target_link_options(${PROJECT_NAME} PRIVATE
    -Xlinker --print-memory-usage
    -r
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    ELF_MODE
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DINPUT=$<TARGET_FILE:${PROJECT_NAME}>
        -DALLOW_UNRESOLVED=__ez_preinit_array_start,__ez_preinit_array_end,__ez_init_array_start,__ez_init_array_end,__ez_fini_array_start,__ez_fini_array_end
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_unresolved.cmake

    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/${BUILD_NAME}.crt

    COMMAND ${ELF2EZ_TOOL}
        /y ${COMPILED_DIR}/${BUILD_NAME}.crt

    COMMAND ${CMAKE_COMMAND} -E rm -f
        ${COMPILED_DIR}/${BUILD_NAME}.crt
)
```

### О `ELF_MODE`

Исторически `ELF_MODE` означает в исходниках приложения «native ARM DOS port,
а не Watcom/x86 build». Поэтому он может присутствовать и при создании
промежуточного ET_REL, который сразу конвертируется в EZ.

Не следует трактовать каждый `#ifdef ELF_MODE` как «конечный файл на диске —
ELF». В перспективе код, реально зависящий от **формата loader**, должен
использовать format-specific abstraction, а не перегружать этот legacy define
исходного порта.

---

# Часть VII — Чек-лист портирования

## 27. Чек-лист нового нативного приложения

### Runtime/сборка

- [ ] выбрать либо прямое использование in-tree API, либо один согласованный
      снимок `apps/api`;
- [ ] для standalone-порта задать один `NATIVE_DOS_API_DIR` и строить от него
      все пути API/CRT;
- [ ] держать специфичные для приложения shims вне локальной копии API;
- [ ] фиксировать/обновлять ревизию локального API как единое целое;
- [ ] подключить `native-dos-runtime.cmake`;
- [ ] использовать relocatable link `-r`;
- [ ] использовать `-fno-jump-tables`, где это требуется для Thumb-1;
- [ ] выполнять проверку unresolved symbols;
- [ ] для EZ: линковать `crt0.c` + `crt0.S`;
- [ ] для EZ: запускать host `elf2ez` после сборки ET_REL;
- [ ] не линковать в приложение произвольные runtime-библиотеки Pico SDK.

### Память

- [ ] различать native pointers и гостевые физические адреса;
- [ ] использовать `dos_phys_*`, когда нужна семантика VGA/device registers;
- [ ] использовать `dos_video_get_buffer()` только в коде, который намеренно знает layout raw renderer buffer;
- [ ] использовать `dos_alloc_low()` для conventional memory, видимой real-mode/DMA;
- [ ] проверять правила 64-КиБ границы ISA DMA там, где они применимы;
- [ ] запрашивать достаточные размеры native и DOS stack;
- [ ] использовать назначенный приложению диапазон PSRAM, а не считать, что
      приложению принадлежит вся PSRAM.

### Тайминг/прерывания

- [ ] не использовать native busy loop без cooperative yield;
- [ ] для длинных вычислительных циклов явно предусматривать service strategy;
- [ ] использовать `dos_native_setvect()`, когда native code заменяет x86 interrupt;
- [ ] восстанавливать векторы при shutdown;
- [ ] не обходить PIC/IVT только ради доставки IRQ callback.

### libc

- [ ] использовать только объявления из `apps/api`;
- [ ] если отсутствует нейтральный примитив libc, предпочитать один backend
      прошивки второй реализации parser/formatter;
- [ ] сохранять семантику DOS `FILE` в DOS-адаптере;
- [ ] verify no unresolved newlib/Pico SDK symbols remain.

### Завершение

- [ ] обычный `return` из `main()` допустим;
- [ ] обычный `exit(status)` допустим;
- [ ] DOS termination/TSR paths не должны после этого выполнять обычный fini teardown.

---

# Часть VIII — Нативные TSR-приложения

## 32. `apps/test` как референс для TSR

`apps/test` — рекомендуемый референс для части нативного TSR, связанной с
**временем жизни процесса и владением памятью**. Он намеренно достаточно мал,
чтобы важные DOS-шаги были видны без платформенного кода большого приложения.

Его не следует воспринимать как готовый framework для interrupt-driven TSR:
у настоящего TSR, устанавливающего резидентные IRQ/service callbacks, появляются
дополнительные вопросы владения и выгрузки, описанные ниже.

Референсная программа использует ключ вида:

```text
test.exe -r
```

для перехода в resident path. Основные шаги следующие.

### 32.1 Получение текущего PSP

У нативной программы по-прежнему есть обычный DOS PSP. Тест запрашивает его у
DOS через INT 21h/AH=51h:

```c
PC *machine = get_PC();
CPU *cpu = machine->cpu;

cpu->gprx[regax].r16 = 0x5100;
bios_intcall(cpu, 0x21, "test.exe get PSP");
psp_seg = cpu->gprx[regbx].r16;
```

Использовать DOS-сервис предпочтительнее, чем вводить собственное предположение
о том, где находится текущий PSP.

### 32.2 Определение размера резидентного основного блока

`PSP:0002` содержит сегмент сразу за основным блоком памяти процесса:

```c
end_seg =
    *(volatile uint16_t *)dos_guest_far_ptr(psp_seg, 2);
```

При **текущей организации native runtime** ни один startup stack не входит в
этот основной MCB:

- guest DOS stack выделяется отдельным DOS-блоком;
- native ARM stack берётся из PSRAM native-stack arena.

Поэтому резидентный размер основного блока вычисляется просто:

```c
resident_paragraphs = end_seg - psp_seg;
```

Не надо вычитать из этого значения зашитый размер native-stack или DOS-stack.
Это относилось только к старым экспериментальным layout, где стеки находились
внутри MCB процесса.

После unwind TSR-запроса runtime может независимо освободить startup-only
выделения стеков, сохранив основной резидентный MCB.

### 32.3 Освобождение environment, если он не нужен TSR

DOS EXEC child обычно владеет отдельным environment-блоком, на который
указывает `PSP:002Ch`.

INT 21h/AH=31h сохраняет DOS-блоки, принадлежащие дочернему процессу. Поэтому
небольшой TSR, которому унаследованный environment не нужен, должен явно
освободить его перед переходом в резидентное состояние:

```c
volatile uint16_t *ps_env =
    (volatile uint16_t *)dos_guest_far_ptr(psp_seg, 0x2c);
uint16_t env_seg = *ps_env;

if (env_seg != 0) {
    union REGS regs = {0};
    struct SREGS sregs;

    segread(&sregs);
    sregs.es = env_seg;
    regs.h.ah = 0x49;             /* free DOS memory block */
    int386x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag)
        return 3;

    *ps_env = 0;
}
```

Это необязательно. Не освобождайте environment, если резидентный код
намеренно будет использовать его позже.

### 32.4 Запрос DOS на завершение с оставлением резидентом

Само оставление программы резидентной выполняется стандартным DOS-сервисом:

```c
cpu->gprx[regax].r16 = 0x3100;  /* AH=31h, AL=exit code */
cpu->gprx[regdx].r16 = resident_paragraphs;
bios_intcall(cpu, 0x21, "test.exe TSR");
```

Сам native bridge должен сделать unwind обратно через C-код, поэтому управление
может вернуться к месту вызова, хотя DOS уже пометил дочерний процесс как
terminated/resident. Приложение обязано считать это терминальным состоянием:
нельзя продолжать обычную работу программы или запускать второй нормальный
shutdown sequence.

Runtime/CRT проверяет DOS termination state и в этом случае подавляет обычный
fini path.

Поэтому простой resident helper должен немедленно вернуть управление после
того, как bridge `bios_intcall()` завершил unwind.

## 33. Что остаётся резидентным

Основной MCB процесса является резидентным образом. Код/данные, которые
понадобятся позже, должны находиться в сохраняемом образе либо в другом
намеренно оставленном allocation.

Нельзя оставлять в резидентном состоянии указатели на:

- native ARM startup stack;
- отдельный guest DOS startup stack;
- environment-блок, который был явно освобождён;
- обычные временные allocations, освобождаемые termination path;
- локальные automatic variables, C-frame которых исчезает при unwind.

Для resident control blocks, структур сохранения векторов, callback state и
подобных данных предпочтительны static/global объекты внутри резидентного
образа.

## 34. Перехватчики прерываний и сервисов TSR

TSR, устанавливающий native callback, обязан оставить резидентными и код
callback, и всё состояние, которое он разыменовывает.

`dos_native_setvect()` позволяет заменить DOS/x86 interrupt на native ARM
handler, сохранив информацию о вытесненном IVT/native-handler и владении им.

У обычного foreground-приложения shutdown восстанавливает вектор. Для TSR этот
hook как раз и должен остаться установленным, поэтому resident path **не
должен** восстанавливать его перед AH=31h.

Отсюда сразу возникает проблема выгрузки: будущий uninstaller не может
освободить резидентный образ, пока не убедится, что TSR всё ещё владеет
вектором, и не восстановит вытесненный handler. Такой ownership protocol надо
проектировать явно; нельзя просто освободить MCB, за которым остаётся живой
ARM callback.

Сейчас `apps/test` демонстрирует механику resident memory/lifetime, но не полный
install/query/uninstall protocol для резидентного IRQ-сервиса.

## 35. Почему `apps/test` полезен и для обычной разработки

Его CMake-конфигурация линкует тот же EZ CRT (`crt0.c`/`crt0.S`) и подключает
`native-dos-runtime.cmake`, при этом исходник остаётся маленьким. Поэтому это
лучший референс для копирования при создании:

- новой native utility;
- каркаса standalone-порта;
- regression test для нового API slot;
- теста exit/argument/CRT;
- теста жизненного цикла TSR.

Используйте большое приложение как исходный шаблон только тогда, когда его
дополнительный platform layer действительно нужен.

---

## 28. Типичные ошибки

### Direct VGA dereference

Неправильно:

```c
memset((void *)0xA0000, 0, 65536);
```

Правильно:

```c
for (uint32_t p = 0xA0000; p < 0xB0000; ++p)
    dos_phys_write8(p, 0);
```

### Waiting for an exact timer tick

Неправильно:

```c
target = ticcount + 30;
while (ticcount != target)
    TSM_Yield();
```

A yield can advance more than one tick.

Правильно:

```c
start = ticcount;
while ((unsigned)(ticcount - start) < 30u)
    TSM_Yield();
```

### Подключение целой host libc

Отсутствующая возможность `sscanf()` — не причина линковать в приложение
отдельную libc. Экспортируйте/используйте нейтральный примитив прошивки
(`vsscanf`), а адаптацию DOS stream оставляйте локальной.

### Treating parent x86 `CS:IP` as runnable during native execution

Родительский процесс приостановлен до возврата нативного дочернего процесса.
Выполнение guest IRQ во время `dos_yield()` должно возвращаться на синтетическую
границу и никогда не продолжать поток инструкций родителя.

---

## 29. Карта исходников

Основные файлы:

```text
apps/api/dos_api_version.h      API version
apps/api/dos-api.h              low-level PC/CPU entry points
apps/api/dos-api-sdtfn.c        DOS/libc/process adapters
apps/api/dos-api-math.c         math + compiler-runtime wrappers
apps/api/dos-api-divmod.S       exact EABI divmod trampolines
apps/api/dos_math_api.h         math system-table slot map

apps/api/dos_phys.h             guest physical memory
apps/api/dos_video.h            прямой доступ к raw VGA backing buffer
apps/api/dos_mem.h              conventional DOS memory
apps/api/dos_yield.h            cooperative service point
apps/api/dos_process.h          process exit/termination state
apps/api/dos_vect.h             native interrupt-vector replacement
apps/api/sound_hw.h             enabled emulated sound devices

apps/api/stdio.h                DOS stream compatibility
apps/api/stdlib.h               minimal stdlib surface
apps/api/string.h               string/memory surface
apps/api/fcntl.h                open flags
apps/api/io.h                   fd I/O
apps/api/direct.h               directory API
apps/api/sys/stat.h             minimal stat API

apps/api/ez.h                   EZ v1 on-disk/process ABI
apps/api/crt0.h                 EZ CRT entry contract
apps/api/crt0.c                 EZ C startup/cleanup
apps/api/crt0.S                 EZ main/exit stack trampoline

apps/api/native-dos-runtime.cmake
                                common native userspace build module

apps/elf2ez/elf2ez.c            converter source
tools/elf2ez                    host converter build/output

apps/test/CMakeLists.txt         minimal EZ/runtime build reference
apps/test/test.c                 minimal application + TSR lifecycle reference

src/dos_api.c                   firmware system table
src/fdos/task.c                 DOS native ELF/EZ loaders/process execution
```

---

## 30. Правила стабильности ABI

При расширении native DOS API:

Для standalone-портов важно помнить: локальная копия `apps/api` является
снимком SDK. Заголовки, реализации, CRT и определения версий должны относиться
к одной ревизии; не смешивайте без необходимости файлы разных поколений API.

1. никогда не менять порядок существующих слотов системной таблицы;
2. добавлять новый слот в конец;
3. увеличивать `DOS_API_VERSION`;
4. обновлять авторитетный публичный заголовок;
5. обеспечивать запись требуемой версии в EZ-файлы из их входной metadata;
6. сохранять фиксированные размеры on-disk структур EZ v1;
7. добавлять новое поведение EZ через явные version/flags/extended-header
   механизмы, а не молча менять трактовку v1;
8. сохранять старые префиксы `native_dos_process_requirements` с помощью `struct_size`;
9. не раскрывать firmware-private layout C-структур, если он намеренно не
   объявлен частью публичного ABI.

---

## 31. ELF и EZ: краткое сравнение

| Свойство | Legacy ELF | EZ |
|---|---|---|
| Парсер файла в ядре | полный ELF | фиксированный EZ header |
| Symbol table нужна при загрузке | да | нет |
| Section table нужна при загрузке | да | нет |
| Поиск зависимостей | ядро | `elf2ez` |
| Общие ELF relocations | ядро | `elf2ez` |
| Runtime relocations | производные ELF | компактные типы EZ |
| Последовательность CRT | принадлежит ядру | userspace `crt0` |
| Ядро знает о `main` | да | нет |
| Entry points процесса | несколько найденных symbols | один `entry_rva` |
| Требование API | callback/metadata executable | статический EZ header |
| Требования stack | legacy requirements record | статическая EZ metadata |
| Сложность loader | высокая | низкая |
| Предпочтительный конечный формат | compatibility/debug | **да** |

Для новых приложений используйте EZ как обычный формат распространения, если
конкретная задача loader/debugging не требует непосредственного legacy ELF.
Для нового дерева исходников самый короткий практический путь обычно такой:
скопировать `apps/api`, взять небольшой build skeleton из `apps/test`, проверить
тривиальный EZ `main()`, а затем постепенно подключать реальное приложение.
