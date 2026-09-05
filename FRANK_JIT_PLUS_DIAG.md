# JIT a diagnostika najednou

**Datum:** 2026-09-03 · **Stav:** postavené a připravené, **na desku zatím
nenahrané** (na desce běží instalace Win95)

Až dosud se JIT a diagnostika do SRAM nevešly zároveň: deska naběhla do černé
obrazovky, protože `pc_new()` nedostal svou haldu. Tohle popisuje, kde ta paměť
byla a jak se získala zpátky.

---

## 1. Kde těch 8 kB bylo

Měřeno porovnáním symbolů dvou buildů (`arm-none-eabi-nm -S`):

```
                    JIT      JIT+diag     rozdíl
cpu_exec1        118 654  →  119 812      +1 158
store32          (inline) →    3 516      +3 516
celkem RAM       476 868  →  485 064      +8 196
```

Diagnostika sice **leží v PSRAM**, jak tvrdí komentář v `audiodiag.h`, ale dva
její hooky jsou vložené do kódu, který sám musí být v SRAM:

- **`frank_diag_wp()`** v `pstore8/16/32` — nafoukne `store32` natolik, že se
  z něj stane samostatná 3,5kB funkce rezidentní v SRAM;
- **`frank_diag_ip()`** v hlavní smyčce interpretu — roste `cpu_exec1`.

Zbytek instrumentace je prakticky zadarmo.

## 2. Co s tím

Dva nové přepínače, oba s výchozí hodnotou zachovávající dosavadní chování:

| přepínač | výchozí | co dělá |
|---|---|---|
| `AUDIO_DIAG_HOT` | `ON` | s `OFF` vypustí ty dva horké hooky |
| `NJIT_CODE_KB` | `16` | velikost překladové cache JITu v kB |

Sestavení kombinovaného buildu:

```bash
cmake -G Ninja -DNATIVE_JIT=ON -DAUDIO_DIAG=ON \
      -DAUDIO_DIAG_HOT=OFF -DNJIT_CODE_KB=8 ..
ninja
```

### Bilance SRAM

| build | použito | volno | `pc_new()` potřebuje > 42 728 |
|---|---|---|---|
| JIT, bez diagnostiky | 476 868 | 47 420 | ✔ |
| JIT + celá diagnostika | 485 064 | 39 224 | ✘ **nenaběhne** |
| JIT + celá diagnostika, bez horkých hooků | 480 968 | 43 320 | ✔ ale jen o 592 B |
| **JIT + diagnostika, bez horkých hooků, cache 8 kB** | **472 776** | **51 512** | ✔ rezerva 8,8 kB |

Samotné vypnutí horkých hooků by stačilo jen o 592 bajtů, což je na tuhle desku
příliš nasucho — proto ještě poloviční JIT cache. Vyjde z toho dokonce **méně**
SRAM než u čistého JIT buildu.

## 3. Co v tom buildu zůstane a co ne

**Zůstává** — a na grafiku to stačí:

- kruh dálkových přenosů řízení (`FRANK_CS_RING`)
- uspořádaná stopa portů (`portlog_on`, DMA / PIC / SB)
- záznam událostí Sound Blasteru
- zachycení první výjimky (`frank_diag_exc`, `ud_*`)
- čítače a stav videořadiče

**Odpadá** — a je potřeba na to myslet:

- kruh instrukcí (`FRANK_IP_RING`)
- **past na shodu `CS:IP`** (`trap_cs`/`trap_ip`, `ud_reason = 9`) — sedí uvnitř
  `frank_diag_ip()`
- **hlídač zápisu** (`wp_lo`/`wp_hi`, `ud_reason = 3`) — sedí uvnitř
  `frank_diag_wp()`
- stínová kopie

Jinými slovy: přesně ty dva nástroje, které rozlouskly Prehistorika i Supaplex,
v kombinovaném buildu **nejsou**. Na hledání „kdo přepsal tenhle bajt" nebo
„kudy se tam řízení dostalo" je pořád potřeba build s `AUDIO_DIAG_HOT=ON` a
JITem vypnutým.

## 4. Připravené obrazy

V `bin/prepared/` se SHA256:

- `JIT-only-504-P166-ON-BOARD.elf` — to, co je **teď na desce** (Supaplex
  i Prehistorik 2 na něm ověřeně běží)
- `JIT-plus-diag-504-P166.elf` — kombinovaný build, **nenahraný**

## 5. Mimochodem opravené

`src/audiodiag.h` měl ve větvi pro vypnutou diagnostiku v makru
`frank_diag_cs()` **literální `\n` místo pokračování řádku**. Projevilo se to
teprve teď, protože s `AUDIO_DIAG=OFF` se ta větev nikdy nepřekládala — první
build bez diagnostiky na ni okamžitě spadl.
