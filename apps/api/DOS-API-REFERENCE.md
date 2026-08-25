# Native DOS API Reference

This document describes the public native-application interface exported by
murm386/FDOS. It intentionally does not document application-specific
compatibility layers, build tutorials, `elf2ez`, or third-party libraries.

Current ABI version: **21** (`DOS_API_VERSION`).

## 1. ABI and system table

Native applications call firmware services through the read-only system table
at `DOS_OS_API_SYS_TABLE_BASE` (`0x10100000`). Public headers in `apps/api`
contain the typed wrappers. Applications should use those headers instead of
indexing the table directly.

The table is append-only. A new entry is added at the end and
`DOS_API_VERSION` is increased when a new public firmware service is required.
Existing slot meanings must not change.

Important public slots:

| Slot | Public interface | Header |
|---:|---|---|
| 0 | `get_PC()` | `dos-api.h` |
| 2 | `bios_intcall()` | `dos-api.h` |
| 3..8 | physical guest reads/writes | `dos_phys.h` |
| 9 | `psram_size()` | `psram.h` |
| 10 | formatting backend | runtime implementation |
| 11 | native process exit | `dos_process.h` |
| 12 | `dos_yield()` | `dos_yield.h` |
| 101 | diagnostic latch | `dos_diag.h` |
| 102 | scanning backend | runtime implementation |
| 103..105,109 | SRAM memory primitives | runtime implementation |
| 106 | termination-request state | `dos_process.h` |
| 107 | current EZ process info | `ez.h` |
| 108,110 | DOS memory information | runtime implementation |
| 111..115 | native process allocator | `stdlib.h` runtime |
| 116 | `dos_video_get_buffer()` | `dos_video.h` |
| 117 | `set_tsr0_callback()` | `tsr_callback.h` |
| 118 | `set_tsr1_callback()` | `tsr_callback.h` |
| 119 | `dos_keyboard_get_event()` | `dos_keyboard.h` |

Slots 13..100 are compiler-runtime/math backends used by the native runtime;
applications normally reach them through C operators and `math.h`.

## 2. CPU, BIOS and guest physical memory

`dos-api.h` exposes low-level access to the emulator instance and BIOS call
bridge:

```c
PC *get_PC(void);
void bios_intcall(CPU *cpu, int intnum, const char *owner);
```

These are low-level interfaces. Prefer higher-level DOS/runtime wrappers when
they exist.

`dos_phys.h` accesses guest physical memory through the active murm386 memory
backend:

```c
uint8_t  dos_phys_read8(uint32_t addr);
uint16_t dos_phys_read16(uint32_t addr);
uint32_t dos_phys_read32(uint32_t addr);
void dos_phys_write8(uint32_t addr, uint8_t value);
void dos_phys_write16(uint32_t addr, uint16_t value);
void dos_phys_write32(uint32_t addr, uint32_t value);
```

Use these for guest physical addresses. Do not create a native pointer from a
guest address unless the relevant API explicitly returns one.

## 3. Cooperative platform service point

`dos_yield.h`:

```c
uint32_t dos_yield(void);
```

A native application owns core0 while it executes. `dos_yield()` gives the
emulator a cooperative service point and returns the current emulator time in
microseconds.

`dos_yield()` is a platform primitive; it is **not** a timer scheduler.
Application/library timer schedulers such as DMX TSM belong to their respective
compatibility libraries, not to the DOS API.

## 4. Native timer/service callback hooks

`tsr_callback.h`:

```c
typedef void (*tsr_callback_t)(void);

tsr_callback_t set_tsr0_callback(tsr_callback_t callback);
tsr_callback_t set_tsr1_callback(tsr_callback_t callback);
```

Both setters atomically replace the current callback and return the displaced
callback. Chaining is explicit:

```c
static tsr_callback_t previous;

static void handler(void)
{
    /* short native service */
    if (previous)
        previous();
}

previous = set_tsr0_callback(handler);
```

Calling the previous callback continues the chain. Omitting that call suppresses
the displaced work.

### TSR0

TSR0 runs on core0 from the existing high-frequency timer path, before the
default guest timer work. The default callback services the emulated PIT/IRQ0
path. A replacement should normally call the previous callback unless it
intentionally owns/suppresses that work.

The callback executes asynchronously with respect to foreground native code.
Keep it bounded and do not use DOS/file/stdio operations from it.

### TSR1

TSR1 runs on core1 from the video scanline DMA IRQ path. It is intended for
very short realtime service work independent of core0.

A slow TSR1 callback misses video deadlines and can produce corrupted or lost
video output. Do not perform blocking work, DOS calls, allocation, formatted
I/O, or other long operations from TSR1.

The callback code and all state it dereferences must remain resident for as
long as the hook is installed.

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

The call reads the host keyboard-event queue before i8042/guest IRQ1 delivery.
`keycode` is a Linux input keycode; `is_down` is 1 for press and 0 for release.
It returns `1` when an event is returned, `0` when the queue is empty, and `-1`
for invalid arguments or flags.

The two flag bits are independent:

| Flags | Result |
|---|---|
| `0` | peek the oldest event |
| `DOS_KEYBOARD_EVENT_CONSUME` | consume the oldest event |
| `DOS_KEYBOARD_EVENT_NEWEST` | peek the newest event |
| `DOS_KEYBOARD_EVENT_CONSUME | DOS_KEYBOARD_EVENT_NEWEST` | consume the newest event |

Consuming the newest event leaves older queued events intact. The operation is
atomic with respect to the PS/2 timer producer.

## 6. Video backing store

`dos_video.h`:

```c
uint8_t *dos_video_get_buffer(uint32_t *size);
```

Returns the raw native backing buffer used by the VGA renderer and optionally
its physical size. This bypasses VGA aperture translation, latches, write
modes, masks and plane logic.

It is therefore only suitable for code that explicitly understands the current
backing-store layout. For emulated VGA semantics use guest physical accesses
through the normal VGA aperture instead.

## 7. DOS conventional memory

`dos_mem.h`:

```c
void *dos_alloc_low(size_t size);
void dos_free_low(void *ptr);
uint16_t dos_ptr_segment(const void *ptr);
uint32_t dos_ptr_linear(const void *ptr);
```

`dos_alloc_low()` allocates DOS conventional memory suitable for real-mode
interfaces. `dos_ptr_segment()` returns a paragraph segment for aligned DOS
blocks. `dos_ptr_linear()` returns an exact conventional-memory linear address
or `UINT32_MAX` when the pointer is outside the first MiB of guest RAM.

The standard `malloc/calloc/realloc/free` interface is separate: since API v17
allocation is owned by the native process allocator (slots 111..115), which can
use process-local native memory and DOS memory according to the runtime policy.

## 8. Process and EZ state

`dos_process.h`:

```c
void dos_process_exit(int status) __attribute__((noreturn));
bool dos_termination_requested(void);
```

`dos_termination_requested()` lets CRT/fini code avoid destroying state that a
DOS termination path has intentionally made resident.

`ez.h` exposes the current EZ process information through slot 107. This is
runtime/process information; the EZ file format and `elf2ez` command-line tool
are documented outside this API reference.

## 8. Native interrupt-vector ownership

`dos_vect.h` provides the native equivalent of DOS getvect/setvect ownership:

```c
typedef bool (*dos_native_vector_handler_t)(void *cpu);

bool dos_native_setvect(dos_native_vector_t *state,
                        unsigned intno,
                        dos_native_vector_handler_t handler);
void dos_native_restorevect(dos_native_vector_t *state);
```

A native ARM address cannot be written directly into the x86 IVT. The wrapper
therefore preserves both the displaced x86 vector and the displaced native BIOS
handler and restores both on teardown.

## 9. Files, directories, console and libc subset

The headers `io.h`, `fcntl.h`, `direct.h`, `stdio.h`, `conio.h`, `string.h`,
`stdlib.h`, `ctype.h` and `math.h` provide the native runtime subset actually
implemented by FDOS. They are not aliases for the host/Pico SDK libc.

Notable file interfaces include DOS-style `open/read/write/lseek/close`,
directory helpers and a native `FILE` wrapper for `fopen/fread/fwrite/fseek`,
formatted I/O and scanning.

Long DOS I/O paths use `dos_yield()` as a generic emulator service point. They
do not know about application-specific timer schedulers.

## 10. Hardware availability and I/O

`sound_hw.h`:

```c
uint32_t sound_hw_mask(void);
```

reports the sound devices instantiated/enabled by the emulator. `conio.h`
provides port I/O wrappers (`inp`, `inpw`, `outp`, ...).

Applications should query hardware through the public runtime interface instead
of reading murm386 configuration files directly.

## 11. Diagnostics

With `DIAG` enabled, `dos_diag.h` exposes a low-cost diagnostic latch through
slot 101. Writing the latch is a direct native store and does not enter DOS,
stdio, FatFS or a scheduler.

## 12. API boundary

`apps/api` contains platform/DOS/runtime interfaces reusable by unrelated native
applications.

It must not contain compatibility implementations for third-party application
libraries. In particular, **DMX TSM is not a DOS API**. The native DMX/TSM
replacement used by Doom lives under `apps/doom` and is implemented on top of
the generic `set_tsr0_callback()` and `dos_yield()` primitives.
