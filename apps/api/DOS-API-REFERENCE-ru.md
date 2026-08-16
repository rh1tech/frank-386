# Справочник по Native DOS API

Это русская версия `DOS-API-REFERENCE.md`. Имена API, файлов, символов,
директив CMake, ABI-термины и фрагменты кода намеренно оставлены в исходном
виде, чтобы документ можно было использовать непосредственно при разработке.

Этот документ описывает the native ARM application layer under `apps/api`, the
legacy relocatable-ELF execution path, the EZ executable format, and the normal
`elf2ez` build/conversion workflow.

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
> утилиту. Подключайте `dos_api_version.h` / the project EZ headers and use
> `DOS_API_VERSION`. Таблица только расширяется в конец: номера существующих слотов нельзя
> перенумеровывать.

---

## 1. Модель выполнения

Нативная DOS-программа всё равно является DOS-процессом:

- it has a PSP and DOS ownership/lifetime;
- DOS file handles, current directory, DTA, environment and child process
  semantics remain DOS semantics;
- low/conventional memory is guest x86 memory;
- the application code itself executes natively as ARM/Thumb code on core 0;
- the parent DOS/x86 execution context is suspended while the native child is
  running.

Поэтому приложение одновременно существует в двух адресных пространствах:

1. **нативное ARM-адресное пространство** — обычные C-объекты, native stack,
   код;
2. **гостевое физическое x86/DOS-пространство** — PSP, IVT, BDA, VGA-память,
   conventional memory, DMA-буферы и структуры DOS.

Не разыменовывайте гостевой физический адрес such as `0xA0000` or `0xB8000` as
an ARM pointer. Use the guest-memory API.

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

`native_dos_runtime_attach()` adds the libc/compiler-runtime bridge:

```text
dos-api-sdtfn.c
dos-api-math.c
dos-api-divmod.S
```

and compiles the C runtime bridge with `-fno-builtin`. This is intentional:
these files define symbols such as `memcpy`, `printf`, compiler EABI helpers,
etc.; GCC must not silently replace their bodies with calls back into a
toolchain libc.

Приложение должно подключать только конкретный публичный заголовок, нужный
подсистеме. Avoid including `dos-api.h` merely to get one small service because
it exposes emulator structures (`PC`, `CPU`) that can collide with application
names and unnecessarily couples the application to firmware internals.

### 2.1 Два поддерживаемых варианта структуры проекта

API можно использовать как непосредственно из дерева, так и как локально скопированный userspace SDK.

**In-tree layout:**

```text
murm386/
    apps/
        api/
        myapp/
            CMakeLists.txt
            ...
```

The application can use `../api`.

**Standalone/vendored layout:**

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

A standalone project does not need to copy itself into the murm386 source
tree.  Instead it can set one API-root variable and use paths relative to it:

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

A copied `stdio.h` from one API revision combined with an older
`dos-api-sdtfn.c`, for example, can compile while referring to a system-table
service which the copied runtime does not wrap correctly.

Относитесь к локальной копии API как к снимку небольшого SDK:

```text
third_party/native-dos-api/
    ...exact copy of apps/api...
```

Record which murm386/API revision it came from.  When updating it, replace or
merge the **whole API snapshot coherently**, then rebuild and rerun the
unresolved-symbol check.

Application-specific compatibility code should normally remain outside this
directory.  If a missing function or service is genuinely generic, add it to
the canonical `apps/api` first and then refresh the vendored copy.  This avoids
turning every large port into a private fork of the runtime.

### 2.3 Что не входит в копируемый API

Standalone-приложению всё равно нужен небольшой проектный/сборочный каркас,
который сам по себе не является частью `apps/api`:

- `CMakeLists.txt` приложения;
- подходящий linker script/template для relocatable link (`memmap.ld.in`);
- проверка unresolved symbols;
- host-бинарник `elf2ez` либо путь к host-утилите из репозитория.

Для нового проекта `apps/test` лучше подходит как исходный каркас
чем `apps/doom`: it has a small source set and exercises the same runtime/CRT
mechanisms without carrying a large game's platform layer.

---

## 3. Системная таблица

The firmware system table starts at:

```c
#define DOS_OS_API_SYS_TABLE_BASE ((void *)0x10100000ul)
```

Wrappers in `apps/api` indirect through this table.

Important fixed slots include:

| Slot | Service |
|---:|---|
| 0 | `get_PC()` |
| 1 | native BIOS handler table (`handlers[]`) |
| 2 | `bios_intcall()` |
| 3..5 | guest physical read 8/16/32 |
| 6..8 | guest physical write 8/16/32 |
| 9 | PSRAM size/service used by the public PSRAM wrapper |
| 10 | firmware `vsnprintf` backend |
| 11 | native process exit/unwind |
| 12 | cooperative `dos_yield()` |
| 13..18 | early compiler/division helpers retained by ABI |
| 19..100 | math and ARM EABI/compiler-runtime services |
| 101 | diagnostic latch |
| 102 | firmware `vsscanf` backend |
| 103 | SRAM-resident native-app `memcpy` |
| 104 | shared SRAM-resident `memset` (`nf_memset`) |
| 105 | SRAM-resident native-app `memcmp` |
| 106 | native DOS termination state |
| 107 | current EZ process information |

Only append new services. A program requiring a newer slot must declare a
newer `DOS_API_VERSION`; the loader rejects it before execution on an older
firmware.

Slots 19..100 are defined symbolically in `dos_math_api.h`. Do not duplicate
their numeric constants elsewhere.

---

## 4. Core low-level services

### 4.1 `dos-api.h`

Use this header only when direct access to the emulator CPU/PC object is
actually required.

```c
PC *pc = get_PC();
bios_intcall(pc->cpu, 0x21, "owner string");
```

It also defines the conventional guest-RAM mapping helpers:

```c
void *dos_guest_linear_ptr(uint32_t linear);
void *dos_guest_far_ptr(uint16_t seg, uint16_t off);
```

Guest RAM is mapped to the native application at `DOS_GUEST_RAM_BASE`
(`0x11000000` in the current ABI). These helpers are for standard DOS data
structures that genuinely reside in conventional guest RAM.

### 4.2 `dos_phys.h`

Use for **physical/emulated memory accesses**, particularly memory whose
semantics are implemented by devices:

```c
uint8_t  dos_phys_read8(uint32_t addr);
uint16_t dos_phys_read16(uint32_t addr);
uint32_t dos_phys_read32(uint32_t addr);

void dos_phys_write8(uint32_t addr, uint8_t value);
void dos_phys_write16(uint32_t addr, uint16_t value);
void dos_phys_write32(uint32_t addr, uint32_t value);
```

Examples:

```c
dos_phys_write8(0xA0000, pixel);   /* VGA aperture */
dos_phys_write16(0xB8000, cell);   /* text VRAM */
```

This is different from `dos_guest_far_ptr()`: a direct native pointer does not
reproduce VGA plane/write-mode/device semantics.

### 4.3 `conio.h`

Native port I/O:

```c
uint8_t  inp(uint16_t port);
uint16_t inpw(uint16_t port);
void outp(uint16_t port, uint8_t value);
void outpw(uint16_t port, uint16_t value);
```

These access the emulated PC I/O bus, not RP2xxx hardware registers.

---

## 5. DOS memory

`dos_mem.h` provides conventional-memory allocation and address conversion:

```c
void *dos_alloc_low(size_t size);
uint16_t dos_ptr_segment(const void *ptr);
uint32_t dos_ptr_linear(const void *ptr);
```

### `dos_alloc_low`

Allocates a DOS conventional-memory block and returns a native pointer into
guest RAM.

Use it for buffers that must be visible to real-mode DOS/BIOS APIs.

### `dos_ptr_segment`

Converts a paragraph-aligned pointer returned from DOS conventional memory to
a DOS segment. It returns zero when the pointer is outside conventional RAM or
is not paragraph aligned.

Typical use:

```c
void *buf = dos_alloc_low(4096);
uint16_t seg = dos_ptr_segment(buf);
/* pass seg:0 to a BIOS/DOS interface */
```

### `dos_ptr_linear`

Returns the exact 20-bit guest physical/linear address of any pointer inside
the first MiB guest RAM, or `UINT32_MAX` on failure.

Use this for ISA DMA programming where the precise byte address matters.

---

## 6. DOS file and directory API

Native applications do not use firmware/newlib `FILE` objects for DOS files.
The file layer is backed by DOS handles.

### 6.1 Low-level file descriptors

Headers:

```text
fcntl.h
io.h
direct.h
sys/stat.h
```

Public surface:

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

Supported open flags currently include:

```c
O_RDONLY
O_WRONLY
O_CREAT
O_TRUNC
O_BINARY
```

`O_BINARY` is currently zero in the compatibility header. Do not assume that
the native stream layer performs host-style CR/LF translation.

Long `read()` / `write()` operations are cooperative service points: the
runtime yields between DOS I/O chunks so timers, emulated devices and pending
guest IRQ work continue to progress while a native process owns core 0.

### 6.2 `stdio.h`

`FILE` is deliberately opaque:

```c
typedef struct native_dos_FILE FILE;
```

Available interface includes:

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

Formatting/scanning policy:

- DOS stream ownership and actual file I/O remain in the native DOS runtime;
- neutral formatting/scanning primitives should reuse the firmware libc
  backend where practical;
- `vsnprintf` is the basic formatting backend;
- `vsscanf` is the basic scanning backend;
- do not grow a second independent libc parser in `apps/api`.

This split avoids passing native DOS `FILE *` objects to firmware libc, while
still reusing libc for format grammar.

---

## 7. General libc compatibility

Headers such as `string.h`, `stdlib.h`, `ctype.h`, and `math.h` expose only
functions the native runtime actually supplies.

Do **not** treat these headers as aliases for the toolchain libc.

Notable points:

- `malloc/calloc/realloc/free` are native DOS runtime implementations;
- `exit()` is a native process/CRT operation, not Pico SDK process exit;
- `memcpy/memset/memcmp` use explicit firmware/SRAM services rather than
  arbitrary flash libc implementations;
- compiler-generated `__aeabi_*` and math operations are provided by
  `dos-api-math.c` / `dos-api-divmod.S`.

When adding a missing standard function, first decide which category it belongs
to:

1. neutral operation suitable for one firmware system-table primitive;
2. DOS-specific adapter (files, console, process);
3. trivial local inline/helper.

Do not automatically reimplement full libc subsystems in userspace.

---

## 8. Process termination

### 8.1 DOS termination state

`dos_process.h` provides:

```c
void dos_process_exit(int status) __attribute__((noreturn));
bool dos_termination_requested(void);
```

`dos_process_exit()` is the kernel-owned termination path used by the legacy
ELF runtime.

`dos_termination_requested()` is important for C runtime cleanup. If the
program has already terminated through DOS semantics (for example TSR
termination), destructors/fini processing must not tear down resident state.

### 8.2 `exit()`

Applications call the standard:

```c
exit(status);
```

The runtime selects the correct underlying mechanism:

- legacy ELF: unwind to the kernel-owned native main trampoline;
- EZ: unwind locally to the EZ CRT main trampoline, so `main()` appears to have
  returned `status`, then execute the normal userspace fini sequence.

Application code must not save/restore the native ARM SP itself.

---

## 9. Cooperative execution and `dos_yield()`

A native application executes synchronously on core 0. While its `main()` is
active, the ordinary outer emulator CPU loop is suspended.

Therefore native applications that wait/poll for time or devices must reach
cooperative service points.

```c
uint32_t now_us = dos_yield();
```

The service point:

1. services emulated devices/host input;
2. allows cooperative native timer services to run;
3. allows pending guest hardware IRQ handlers to execute through the normal
   PIC -> IVT path without resuming the frozen parent process CS:IP;
4. returns the emulator microsecond clock.

The guest-IRQ execution uses a synthetic return boundary. The final `IRET`
returns to that boundary and the guest CPU slice stops immediately; the
suspended parent execution address is not allowed to run.

### Practical rule

Any native busy wait must yield.

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

## 10. Cooperative timer service (`TSM_*`)

The runtime provides the DMX-style cooperative timer API used by ports such as
DOOM:

```c
void TSM_Install(int rate);
int TSM_NewService(int (*service)(void), int rate, int priority, int pause);
void TSM_DelService(int id);
void TSM_PauseService(int id);
void TSM_ResumeService(int id);
void TSM_Remove(void);
void TSM_Yield(void);
```

Callbacks execute in normal application context at service points. They are not
RP2xxx asynchronous timer IRQ callbacks.

This is intentional: arbitrary application C code is not run concurrently
against the same native stack/runtime state.

---

## 11. Native interrupt-vector replacement

`dos_vect.h` provides a native counterpart to DOS `_dos_getvect/_dos_setvect`
ownership:

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

A native ARM function cannot be written directly into the x86 IVT. Installation
therefore saves **both** displaced layers:

- old x86 IVT vector;
- old native BIOS `handlers[intno]` entry.

Then it points the IVT entry to the standard native BIOS FFE0 hook and installs
the ARM callback in `handlers[]`.

Restoration puts both displaced owners back.

There is no implicit chaining. This matches normal DOS `setvect` ownership:
applications that completely replace an IRQ handler (classic DOOM keyboard
IRQ1 is an example) do not automatically call the displaced handler.

Always restore an installed vector before normal process shutdown. The process
runtime should also ensure abandoned native callbacks cannot survive the
process that owns their code.

---

## 12. Hardware availability

`sound_hw.h` reports devices actually instantiated/enabled by the emulator:

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

Use this instead of re-reading firmware configuration files from an
application.

---

# Часть II — Сборка нативных приложений

## 13. Создание нового приложения с нуля

Есть два разумных способа начать.

### 13.1 Проект внутри дерева murm386

Создайте новый каталог рядом с `apps/test`, используйте `../api` и сначала оставьте
в программе только `main()` и общий CRT/runtime.  This gives the shortest
path to a known-good build.

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

A native application starts as a relocatable ARM image:

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

Important points:

- `-r` is intentional: the intermediate output is ET_REL, not a normal
  RP2xxx firmware executable;
- do not link `pico_runtime`, `pico_stdlib`, host stdio, etc. into the
  application unless a particular userspace design explicitly supports it;
- `-fno-jump-tables` is commonly used for Thumb-1 builds to avoid unwanted
  compiler runtime symbols;
- unresolved symbols must be checked after the relocatable link.

---

## 15. Проверка неразрешённых символов

A relocatable link accepts unresolved symbols by design. A native DOS
application has no normal dynamic linker, so unexpected unresolved references
must fail the build.

The application build should run `nm -u` through `check_unresolved.cmake`.

For an EZ build, only the six CRT array-boundary pseudo-symbols are expected to
remain unresolved in ET_REL:

```text
__ez_preinit_array_start
__ez_preinit_array_end
__ez_init_array_start
__ez_init_array_end
__ez_fini_array_start
__ez_fini_array_end
```

`elf2ez` synthesizes these after it discovers and lays out the reachable
startup-array sections.

Every other unresolved symbol is a build error.

---

# Part III — Legacy ELF mode

## 15. Legacy native ELF format

The legacy loader consumes the relocatable ARM ELF directly.

The kernel loader is responsible for much more than ordinary DOS loading:

- parses ELF sections, symbols and relocations;
- discovers reachable dependencies;
- lays sections out;
- applies relocations;
- locates the API-version callback;
- locates process-requirements metadata;
- locates `_init`, `main`, `_fini`, signal hooks and startup arrays;
- prepares native and guest DOS stacks;
- executes CRT sequencing in kernel-owned code;
- owns the native `main()` recovery frame used by `exit()`.

This path is useful for development and compatibility, but it makes the kernel
an ELF linker/CRT implementation.

### Legacy process requirements

`native_process.h` defines the legacy ELF record:

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

An application exposes:

```c
native_dos_process_requirements *
__native_dos_process_requirements(void);
```

The first fields are input requests; later fields are loader output.

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

# Part IV — EZ mode

## 16. Why EZ exists

EZ moves ELF/linker knowledge out of the DOS kernel.

The build-time `elf2ez` tool performs the expensive work once:

```text
ET_REL ARM ELF
      |
      | elf2ez
      v
EZ executable (.exe)
```

The DOS loader then handles a compact fixed executable ABI rather than parsing
ELF section/symbol/string tables.

The design boundary is:

### `elf2ez` owns

- ELF parsing;
- dependency discovery;
- final image layout;
- resolution of image-internal relocations;
- CRT/startup-array layout;
- conversion of the small remaining load-base-dependent relocation set to EZ
  relocation records.

### DOS EZ loader owns

- validate the EZ header;
- allocate the DOS process block/PSP;
- load initialized image bytes at RVA `0x100`;
- zero any memory-only tail;
- apply the compact EZ relocation table;
- allocate/assign native and DOS stacks;
- construct `argc/argv`;
- publish runtime process information;
- call exactly one entry point.

### EZ CRT owns

- preinit array;
- init array;
- optional `_init`;
- `main`;
- normal `exit()` unwind;
- optional `_fini`;
- reverse fini array.

The kernel never needs to know where `main`, `_init`, `_fini`, constructors or
destructors are in an EZ file.

---

## 17. EZ v1 file layout

Constants:

```c
EZ_MAGIC          = 0x5a45        /* "EZ" */
EZ_FORMAT_VERSION = 1
EZ_IMAGE_RVA      = 0x00000100
```

Logical file layout:

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

RVA convention:

```text
runtime_address = process_base + rva
```

`process_base` is the native pointer corresponding to PSP segment offset 0.

The first stored image byte is at:

```text
process_base + EZ_IMAGE_RVA
```

Therefore the 0x100-byte PSP remains at the front of the DOS process block.

---

## 18. EZ v1 header

The fixed v1 header is 64 bytes and includes:

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

Important semantics:

### `required_dos_api_version`

Minimum firmware API required by the image. The loader validates this **before
executing application code**.

### `native_stack_size`

Requested native ARM stack size. Zero means loader default; non-zero is a
minimum request and can be rounded upward.

### `dos_stack_size`

Guest DOS stack used when native code enters DOS/BIOS/guest IRQ execution.
Zero means loader default.

### `image_file_size`

Initialized bytes physically stored in the executable.

### `image_mem_size`

Total image bytes in memory, including the zero-initialized tail.

### `entry_rva`

The one callable native entry point, normally `__ez_start`.

Thumb function-pointer bit 0 is retained.

### `reloc_offset`, `reloc_count`, `reloc_entry_size`

Location and shape of the compact runtime relocation table.

---

## 19. EZ image flags

EZ v1 defines:

```text
EZ_FLAG_THUMB
EZ_FLAG_ARMV6M
EZ_FLAG_THUMB2
EZ_FLAG_SOFT_FLOAT
```

`ARMV6M` and `THUMB2` describe mutually incompatible minimum instruction-set
requirements for v1.

Hard-float images are not part of the current ABI; native DOS uses the
soft-float procedure-call ABI.

---

## 20. EZ relocations

EZ relocations are **not** raw ELF `R_ARM_*` records.

`elf2ez` resolves everything that depends only on positions inside the finished
image. The loader receives only remaining base-dependent fixups.

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

Each record is:

```c
struct ez_reloc {
    uint32_t rva;
    uint8_t type;
    uint8_t reserved[3];
};
```

and is exactly 8 bytes in EZ v1.

The loader must reject unknown non-zero relocation types.

---

## 21. EZ CRT

An EZ build links:

```text
apps/api/crt0.c
apps/api/crt0.S
```

The canonical entry point is:

```c
int __ez_start(int argc, char **argv);
```

Startup order:

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

The fini path is skipped if DOS has already requested process termination
through semantics that must preserve state, for example TSR termination.

`exit(status)` during `main()` does not require the kernel to know the
application stack frame. `crt0.S` records the EZ-local recovery SP and unwinds
back to the CRT as if `main()` returned `status`.

---

## 22. EZ process requirements and runtime information

For EZ, static requirements are build metadata, not a callback executed by the
kernel.

`ez.h` defines:

```c
typedef struct native_ez_process_requirements {
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
} native_ez_process_requirements;
```

`crt0` provides a weak all-zero default. An application can override it:

```c
#include <ez.h>

const native_ez_process_requirements __native_ez_process_requirements = {
    64u * 1024u,
    4u * 1024u
};
```

`elf2ez` reads this object from ET_REL and copies the values into the EZ
header. No application code is run during conversion.

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

This returns the resources actually assigned by the loader to the current EZ
process.

---

# Часть V — `elf2ez`

## 23. Командная строка

Typical command:

```text
elf2ez /y program.crt
```

Output filename is derived by replacing the source extension with:

```text
.exe
```

`/y` suppresses the overwrite prompt.

Without `/y`, an existing output file is confirmed interactively.

Typical successful diagnostic:

```text
EZ image: 6804 bytes, 38 relocations, entry=00000131
```

The exact numbers depend on the application.

---

## 24. Что делает `elf2ez`

Conceptually:

1. open the ET_REL input;
2. validate ARM ELF format and relevant section/symbol tables;
3. find the EZ CRT entry symbol;
4. recursively load reachable sections/symbol dependencies;
5. lay them out beginning at `EZ_IMAGE_RVA`;
6. resolve PC-relative and fully image-relative relocations;
7. emit an `ez_reloc` only when the final value still depends on
   `process_base`;
8. build/synthesize startup-array ranges required by `crt0`;
9. read static EZ process requirements;
10. construct the 64-byte EZ header;
11. write header + image + relocation table;
12. report final image size, relocation count and entry RVA.

The final EZ file deliberately contains no ELF symbol/string/section tables.

---

## 25. Host- и DOS-сборки `elf2ez`

`elf2ez.c` is designed so the actual conversion logic can be built in two
environments:

- as a native DOS application using `apps/api`;
- as a host tool under `tools/elf2ez` using a small host shim.

The host shim must duplicate only the tiny platform layer needed by the tool
(file descriptors and constants), not EZ on-disk structures. Format and process
metadata types come from the project's authoritative `apps/api` headers.

The host tool is what application CMake normally invokes after producing ET_REL.

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

Representative CMake:

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

### About `ELF_MODE`

`ELF_MODE` historically means "native ARM DOS port rather than Watcom/x86
build" in application source. It can therefore still appear while producing
the ET_REL intermediate that is immediately converted to EZ.

Do not interpret every `#ifdef ELF_MODE` as "the final file on disk is ELF".
Long term, code that really depends on **loader format** should use a
format-specific abstraction rather than overloading this legacy source-port
define.

---

# Часть VII — Чек-лист портирования

## 27. Чек-лист нового нативного приложения

### Runtime/сборка

- [ ] choose either direct in-tree API use or one coherent vendored `apps/api`
      snapshot;
- [ ] for a standalone port, define one `NATIVE_DOS_API_DIR` and make all API/CRT
      paths derive from it;
- [ ] keep application-specific shims outside the vendored API directory;
- [ ] record/update the vendored API revision as a unit;
- [ ] attach `native-dos-runtime.cmake`;
- [ ] use relocatable link `-r`;
- [ ] use `-fno-jump-tables` where required by the Thumb-1 target;
- [ ] run unresolved-symbol validation;
- [ ] for EZ: link `crt0.c` + `crt0.S`;
- [ ] for EZ: run host `elf2ez` after the ET_REL build;
- [ ] do not link arbitrary Pico SDK runtime libraries into the app.

### Память

- [ ] различать native pointers и гостевые физические адреса;
- [ ] использовать `dos_phys_*` для VGA/device memory;
- [ ] использовать `dos_alloc_low()` для conventional memory, видимой real-mode/DMA;
- [ ] проверять правила 64-КиБ границы ISA DMA там, где они применимы;
- [ ] запрашивать достаточные размеры native и DOS stack;
- [ ] use the assigned application PSRAM interval rather than assuming all
      PSRAM belongs to the app.

### Тайминг/прерывания

- [ ] не использовать native busy loop без cooperative yield;
- [ ] для длинных вычислительных циклов явно предусматривать service strategy;
- [ ] use `dos_native_setvect()` when native code replaces an x86 interrupt;
- [ ] восстанавливать векторы при shutdown;
- [ ] do not bypass PIC/IVT merely to deliver an IRQ callback.

### libc

- [ ] использовать только объявления из `apps/api`;
- [ ] if a neutral libc primitive is missing, prefer one firmware backend over a
      second parser/formatter implementation;
- [ ] keep DOS `FILE` semantics in the DOS adapter;
- [ ] verify no unresolved newlib/Pico SDK symbols remain.

### Завершение

- [ ] обычный `return` из `main()` допустим;
- [ ] обычный `exit(status)` допустим;
- [ ] DOS termination/TSR paths must not run normal fini teardown afterward.

---

# Часть VIII — Нативные TSR-приложения

## 32. `apps/test` как референс для TSR

`apps/test` — рекомендуемый референс для части нативного TSR, связанной с
**временем жизни процесса и владением памятью**. Он намеренно достаточно мал,
чтобы важные DOS-шаги были видны без платформенного кода большого приложения.

Его не следует воспринимать как готовый framework для interrupt-driven TSR:
у настоящего TSR, устанавливающего резидентные IRQ/service callbacks, появляются
дополнительные вопросы владения и выгрузки, описанные ниже.

The reference program uses a switch such as:

```text
test.exe -r
```

to enter its resident path.  Its important steps are the following.

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

Wrong:

```c
memset((void *)0xA0000, 0, 65536);
```

Correct:

```c
for (uint32_t p = 0xA0000; p < 0xB0000; ++p)
    dos_phys_write8(p, 0);
```

### Waiting for an exact timer tick

Wrong:

```c
target = ticcount + 30;
while (ticcount != target)
    TSM_Yield();
```

A yield can advance more than one tick.

Correct:

```c
start = ticcount;
while ((unsigned)(ticcount - start) < 30u)
    TSM_Yield();
```

### Loading a whole host libc

A missing `sscanf()` feature is not a reason to link an independent libc into
the application. Export/use the neutral firmware primitive (`vsscanf`) and keep
DOS stream adaptation local.

### Treating parent x86 `CS:IP` as runnable during native execution

The parent process is suspended until the native child returns. Guest IRQ
execution during `dos_yield()` must return to a synthetic boundary, never fall
through into the parent instruction stream.

---

## 29. Карта исходников

The important files are:

```text
apps/api/dos_api_version.h      API version
apps/api/dos-api.h              low-level PC/CPU entry points
apps/api/dos-api-sdtfn.c        DOS/libc/process adapters
apps/api/dos-api-math.c         math + compiler-runtime wrappers
apps/api/dos-api-divmod.S       exact EABI divmod trampolines
apps/api/dos_math_api.h         math system-table slot map

apps/api/dos_phys.h             guest physical memory
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

When extending the native DOS API:

For standalone ports, first remember that a vendored `apps/api` copy is an SDK
snapshot.  Keep its headers, implementations, CRT and version definitions from
the same revision; do not casually mix files from different API generations.

1. never reorder an existing system-table slot;
2. append the new slot;
3. increment `DOS_API_VERSION`;
4. update the authoritative public header;
5. make EZ files record the required version from their input metadata;
6. keep on-disk EZ v1 structure sizes fixed;
7. reserve new EZ behavior through explicit version/flags/extended header
   mechanisms rather than silently changing v1 interpretation;
8. preserve old `native_dos_process_requirements` prefixes using `struct_size`;
9. do not expose firmware-private C structure layout unless it is intentionally
   declared part of the public ABI.

---

## 31. ELF и EZ: краткое сравнение

| Property | Legacy ELF | EZ |
|---|---|---|
| File parser in kernel | full ELF | fixed EZ header |
| Symbol table needed at load | yes | no |
| Section table needed at load | yes | no |
| Dependency discovery | kernel | `elf2ez` |
| General ELF relocations | kernel | `elf2ez` |
| Runtime relocations | ELF-derived | compact EZ types |
| CRT sequencing | kernel-owned | userspace `crt0` |
| `main` known to kernel | yes | no |
| Process entry points | several discovered symbols | one `entry_rva` |
| API requirement | executable callback/metadata | static EZ header |
| Stack requirements | legacy requirements record | static EZ metadata |
| Loader complexity | high | low |
| Preferred final distribution | compatibility/debug | **yes** |

Для новых приложений используйте EZ как обычный формат распространения, если
specific loader/debugging task requires direct legacy ELF execution.  For a
new source tree, the shortest practical path is usually: copy/vendor
`apps/api`, copy the small `apps/test` build skeleton, prove a trivial EZ
`main()`, then integrate the real application incrementally.
