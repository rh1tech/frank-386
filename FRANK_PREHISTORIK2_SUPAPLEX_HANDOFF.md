# Prehistorik 2 a Supaplex — předání vyšetřování

> **VYŘEŠENO 2026-09-03:** viz `FRANK_PREHISTORIK2_SOLVED.md`. Příčinou byl
> zápis hry do ROM BIOSu, který emulátor nezahazoval — antidebug kontrola
> Prehistoriku si tím rozbila vlastní kód. Kapitola 3 (tabulka vyloučených)
> platí dál; chybný byl až závěr dalšího kola, že zápis emulovaným
> procesorem je vyloučen.


**Datum:** 2026-09-02 · **Stav:** nevyřešeno, příčina nenalezena

> **Pokračování:** `FRANK_PREHISTORIK2_BYTE_CORRUPTION.md` — od 2. 9. večer
> je příčina zúžená na jediný přepsaný bajt kódu hry (`10BB:62B8`, `50` → `FB`).
> Kapitoly 3 a 5 níže platí dál, ale JIT do tabulky vyloučených přibyl až tam.
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz, PSRAM 166 MHz, I2S, HDMI

Tenhle dokument shrnuje celodenní pátrání po pádu dvou her. **Příčinu jsem
nenašel.** Hodnota je proto hlavně v tom, co je *vyloučeno* a *jak* — aby to
další kolo nemuselo opakovat — a v popisu diagnostiky, která je ve stromu
připravená.

Všechno níže je měřené na hardwaru přes SWD, ne odvozené. Kde jde o domněnku,
je to výslovně napsáno.

---

## 1. Příznaky, jak je hlásí uživatel

### Prehistorik 2 (`C:\HRY\PREHIST2`, `PRE2.EXE`)

- Hra naběhne a hraje.
- **Jakmile se chvíli pohybuje postavou, zamrzne.** Nemusí ani stisknout
  mezerník; stačí pohyb.
- **Postava se před zamrznutím posouvá, ale nemá animaci chůze** — jen klouže.
- Původně se zdálo, že spouštěčem je mezerník (použití kyje); pozdější
  pokusy to nepotvrdily, stačí pohyb.

### Supaplex (`C:\HRY\SUPAPLEX`, `SUPAPLEX.EXE` přes `RUNME.BAT`)

- Hudba hraje, hra běží.
- **Jakmile se ve hře stiskne klávesa, hra se restartuje** na úvodní obrazovku
  se zadáním kódu ochrany proti kopírování.
- **Při druhém pokusu přijde barevné blikání a zamrznutí.**
- Dříve to bylo popisováno jako „spadne, když se má ozvat zvukový efekt“ —
  což je pravděpodobně tatáž věc viděná z jiné strany, protože stisk klávesy
  ve hře zvukový efekt vyvolá.

### Společné

Obě hry jsou jediné dvě, kde uživatel tohle chování pozoruje. Ostatní hry na
stejné desce jedou.

---

## 2. Co je potvrzeno měřením

### 2.1 Není to regrese z 2026-09-02

Kontrolní firmware **`bin/MinSizeRel/DUAL-OPL2-504MHz-P166-I2S.elf`
(2026-08-31 21:45)** — tedy před všemi dnešními zásahy i před změnou
`drivers/vga/vga_hw.c` (2026-09-02 00:45) — **se chová stejně.**

Ověřeno uživatelem přímo na desce.

### 2.2 Podpis pádu: skok do prázdné paměti a smyčka na #UD

Obě hry skončí tím, že řízení skočí na adresu, kde není kód, ale výplň `0xFF`.
Tam narazí na neplatné kódování `FF FF`, emulátor korektně vyhodí `#UD`,
obsluha INT 6 v BIOSu je **holý `IRET`**, takže se řízení vrátí na tutéž
adresu — a takhle donekonečna.

**Supaplex, zachyceno záchytem prvního #UD:**

```
ud_cs    = 0x00004000     (segment 0x0400)
ud_ip    = 0x00000F42
ud_excno = 6              (#UD)
ud_flags = 0x00027852     VM=1, IF=0, IOPL=2
bajty na 0400:0F42:  ff ff ff ff ff 02 00 00 ...   (capstone nedekóduje)
exc_hist[6] = 2 493 879   ← počet opakování té smyčky
```

**Prehistorik 2, jiné zachycení:**

```
CS base 0x000F0000, IP 0xFF53, excno = 6
host kmitá mezi F000:2D2C a F000:FF53
F000:2D2C  =  ff ff ff ff ...        nepopsaná ROM
INT 6 vektor (0x18) = 53 ff 00 f0  →  F000:FF53
F000:FF53  =  cf                      holý IRET
g_mips spadlo z ~2,0 na 0,697         (režie obsluhy výjimky)
```

**Důležité:** emulátor v tomhle stavu **nic neodmítá ani nepočítá špatně**.
Poctivě provádí skok, který dostal. Chyba je v tom, **co ten skok způsobí** —
a to se zachytit nepodařilo.

### 2.3 Sekundární pozorování

- **Zaseknuté IRQ5** (Prehistorik): master PIC měl `IRR = 0x23` (IRQ0, IRQ1,
  IRQ5), `ISR = 0`, `IMR = 0xD8`, host `IF = 0`. Ruční shození bitu 5 v IRR
  přes SWD **nezměnilo nic** — byl to důsledek, ne příčina.
- **Zamrzlé ustálené stavy**, do kterých to spadne:
  - Supaplex: vlastní čekací smyčka na klávesu `4764:88FF`
    (`mov al,[0x16f9]` / `cmp al,0` / `je`)
  - DOS/COMMAND.COM: smyčka v segmentu `0x215F` kolem `006F`
  - v obou případech chodí už jen tik časovače (18,2 Hz nebo ~50 Hz)
- **Hard fault firmwaru** se objevil **jednou** u Supaplexu:
  `"FALT"`, PC `0x1002d9ca`, CFSR `0x8200` (PRECISERR + BFARVALID),
  BFAR `0x0000E1FD`.
  **Tenhle záznam považuju za nespolehlivý**: PC ukazuje na
  `spin_lock_claim_unused+10`, což je instrukce `movs r2, #24` — ta do paměti
  vůbec nesahá a přesnou chybu sběrnice způsobit nemůže. Buď byl rámec sebrán
  z rozbitého zásobníku, nebo je vadný z jiného důvodu. **Nestavět na něm.**
  (Právě kvůli tomu vznikl plnější záznam, viz kapitola 4.)

---

## 3. Co je VYLOUČENO — a jak

Tohle je nejcennější část dokumentu. Každá položka byla ověřena, ne odhadnuta.

| # | Podezřelý | Jak vyvráceno |
|---|---|---|
| 1 | **Sound Blaster** a změna škrcení DMA z 2026-09-02 | Spadlo to i s `adlib_enabled = 0` **a** `sb16_enabled = 0`. S vypnutým SB se navíc vůbec nesahá na DMA — `sb16_getsample()` je jediné, co drží DREQ, a to se nevolá. |
| 2 | **EMM386 / V86 monitor** | Stejné chování s EMM386 i bez něj. |
| 3 | **Změny planárního VGA z 2026-09-01/02** | Kontrolní firmware z 2026-08-31 se chová stejně. |
| 4 | **Diagnostika Claude v PSRAM** (leží v aperturzě 0xA0000–0xBFFFF) | Kontrolní firmware ji nemá a padá taky. |
| 5 | **PIT** | Kanál 0 je v **režimu 2** (periodický rate generator, původní cesta). Jednorázové režimy přidané 2026-09-02 se tu vůbec neuplatní. |
| 6 | **A20** | `cpu->a20_mask = 0xFFFFFFFF`, tedy zapnutá. Žádné zabalení HMA. |
| 7 | **Stín přerušení po `MOV SS` / `POP SS`** | Implementovaný, jen nenápadně: `i386.c:2435` a `i386.c:2800` dělají `stepcount++`, čímž protáhnou rozpočet kroku tak, aby následující instrukce proběhla uvnitř téhož `cpui386_step()`. Přerušení se testuje jen na jeho začátku, takže mezi ně spadnout nemůže. `LSS` `stepcount++` nemá, ale načítá SS i SP atomicky v jedné instrukci, takže tam okno není. |
| 8 | **Čtení portu 0x61** | `pcspk_ioport_read()` vrací hradlo (bit 0), data (bit 1), refresh (bit 4) a výstup kanálu 2 (bit 5) — čtení-úprava-zápis v obsluze klávesnice si tedy nic nepřepíše. |

---

## 4. Diagnostika, která je ve stromu

Všechna leží v PSRAM **za nepoužívanou VGA aperturou hosta** (guest
0xA0000–0xBFFFF → `0x110a0000`–`0x110c0000`), takže **nestojí ani bajt SRAM**.
To je podstatné: RAM je na 90,95 % a `pc_new()` má rezervu pod 1 KB.

Zapíná se `-DAUDIO_DIAG=ON`. S `OFF` se všechna makra vypustí a kód se nemění.

### Rozvržení paměti

| adresa (host) | guest | obsah |
|---|---|---|
| `0x110a0000` | 0xA0000 | stínová kopie OPL3 banky 1 (2 KB, existovala předtím) |
| `0x110a1000` | 0xA1000 | `FrankDiag` — čítače a hlavičky |
| `0x110a2000` | 0xA2000 | kruh zápisů do OPL (16 KB) |
| `0x110a7000` | 0xA7000 | kruh časů IRQ0 (16 KB) |
| `0x110ad000` | 0xAD000 | kruh událostí digitálního zvuku a DMA (24 KB) |
| `0x110b5000` | 0xB5000 | histogram **zápisů** na porty, 4096 × u16 |
| `0x110b7000` | 0xB7000 | histogram **čtení** portů, 4096 × u16 |
| `0x110b9000` | 0xB9000 | hrubá stopa řízení, 512 × 16 B |
| `0x110bb000` | 0xBB000 | plný záznam hard faultu (16 slov) |
| `0x110bc000` | 0xBC000 | **kruh načtení CS**, 1024 × 16 B |

> **Pozor:** histogram čtení portů zasahuje do rozsahu textové videopaměti
> (0xB8000). Guest ji má přesměrovanou na `gfx_buffer`, takže to vadit nemá —
> ale ověřeno to není u *všech* cest (DMA a disk sahají do `phys_mem` přímo).
> Kontrolní test v tabulce výše (řádek 4) naznačuje, že to problém není.

### Klíčová pole `FrankDiag` (offsety od `0x110a1000`)

```
+0x2d0  ud_hit        1 = zachycen první #UD, kruhy zmrazeny
+0x2d4  ud_cs         CS base místa pádu
+0x2d8  ud_ip
+0x2dc  ud_excno      6 = #UD
+0x2e0  ud_excerr
+0x2e4  ud_flags      příznaky hosta v okamžiku výjimky
+0x2e8  trace_head
+0x2ec  trace_last_cs
+0x2f0  cs_head       ukazatel do kruhu načtení CS
+0x2f4  exc_hist[32]  počet výjimek podle čísla
```

`exc_hist[13]` (#GP) běžně dosahuje 150–300 tisíc — to jsou trapy I/O ve V86
od EMM386, je to normální.

### Kde jsou hooky

- `src/i386.c` — makra `THROW` a `THROW0` volají `frank_diag_exc()`.
  **Pozor:** `cpu_setexc()` **není** cesta, kterou emulátor výjimky vyhazuje;
  hook jen na ni nezachytí nic. To stálo jedno kolo.
- `src/i386.c` — `set_seg()` volá `frank_diag_cs()` při každém načtení CS.
- `src/i386.c` — `cpui386_step()` volá `frank_diag_trace()` (hrubá stopa).
- `src/pc.c` — `pc_io_read/write*` volají `frank_diag_port()`.
- `src/main.c` — `frank_fault_record()` ukládá celý výjimkový rámec a všechny
  stavové registry; s `-DFAULT_PARK=ON` **zaparkuje místo restartu**, aby se
  dal zásobník projít přes SWD (jinak si `watchdog_reboot()` zahladí stopy).
- `src/main.c` — `__wrap_abort()` (přes `-Wl,--wrap=abort`) zapíše do watchdog
  scratch `"ABRT"` a **návratovou adresu volajícího**. Pokrývá všech 14 volání
  `abort()` v binárce. Watchdog scratch přežije reset.

### Markery ve watchdog scratch (`0x400d800c`)

| magic | význam |
|---|---|
| `0x46414C54` `"FALT"` | hard fault; +4 PC, +8 CFSR, +12 BFAR |
| `0x41425254` `"ABRT"` | `abort()`; +4 návratová adresa, +8 `g_diag_stage` |
| `0x53485554` `"SHUT"` | čisté vypnutí hosta |

**Scratch se nemaže resetem** — před pokusem ho vynuluj, jinak si starý záznam
spleteš s novým.

### Postup měření

```bash
# co se stalo
mdw 0x4010002c 1      # POWMAN_CHIP_RESET: 0x20000 = brownout, 0x40000 = debug reset
mdw 0x400d800c 4      # watchdog markery
mdw 0x200303f4 3      # g_diag_free_heap, g_diag_pc_new_failed, g_diag_stage
                      # (adresy se posouvají po každém buildu — vytáhnout nm!)

# běží emulace?
mdw <g_mips> 2        # dvakrát s odstupem; mění-li se, host jede
mdw <cpu+0x144> 2     # cpu->cycle, přesný počet instrukcí

# kde je host
mdw <cpu+0x20> 1      # ip
mdw <cpu+0x58> 1      # seg[CS].base
mdw <cpu+0x28> 1      # flags: bit9 = IF, bit17 = VM
mdw <cpu+0x1b8> 1     # a20_mask
```

`pc` je globální ukazatel; `pc->cpu` je první pole, `pc->pic` na +4,
`pc->pit` na +8, `pc->adlib_enabled` na +0x84, `pc->sb16_enabled` na +0x88.

Pomocné skripty v `claude_handoff/`: `swdscreen.py` (čte textovou obrazovku
hosta), `swdkey.py` (píše do hosta přes klávesnicový kruh). **Obojí potřebuje
ELF, který přesně odpovídá tomu, co je nahrané v desce** — po přebuildu se
posunou symboly a klávesy tiše přestanou chodit (`--status` hlásí `magic BAD`).

---

## 5. Proč se to nepodařilo zachytit

Metoda byla: kruh posledních načtení CS + zmrazení na prvním `#UD`.
Selhala čtyřikrát a pokaždé zachytila **ustálený stav po pádu**, ne přechod.

Důvody, v pořadí jak se ukázaly:

1. Když pád neprodukuje `#UD` (a některé varianty ho neprodukují), **není na
   co zmrazit**. Ruční zmrazení zápisem `ud_hit = 1` přes SWD funguje, ale
   trvá kolem sekundy.
2. V zaseknutém stavu host generuje **~150 záznamů za sekundu** — na každý tik
   časovače tři (přerušení dovnitř, monitor ho odrazí, skok zpět). Kruh o 1024
   položkách proto pokryje jen **4–7 sekund**, což je na člověka ve smyčce
   málo.
3. Slučování opakování **nepomohlo**, protože ten nečinný vzorec není
   opakování jedné položky, ale **cyklus tří různých** (A→B, B→C, C→A).
   Sousední záznamy se nikdy neshodují. Změna klíče slučování z trojice
   *(odkud-segment, odkud-IP, cíl)* na dvojici *(odkud-segment, cíl)* na tom
   nic nezměnila.

**Doporučení pro další kolo:** zmrazení musí spouštět **firmware sám**, ne
člověk. Například když `cpu->cycle` roste, ale po N milisekundách nepřibyl
žádný nový *odlišný* přenos řízení — to je definice „host přestal postupovat“.
Tím se z rovnice odstraní ta sekundová prodleva, která byla celou dobu tím
úzkým hrdlem.

Druhá možnost: zaznamenávat ne *načtení CS*, ale **cíle skoků, které míří mimo
načtené moduly** — tedy detekovat ten špatný skok v okamžiku, kdy nastane.

---

## 6. Co víme o Supaplexu z jeho binárky

Disk byl připojen jako `G:\HRY\SUPAPLEX`, soubory jen čteny (nic zapsáno).

- `SUPAPLEX.EXE`, 45 948 B — **originál, ne speed fix**.
  MZ hlavička: 512 B, obraz 45 436 B, vstup `CS:IP = 0AFF:0010`,
  `SS:SP = 58D4:0080`, **0 relokací**.
- `RUNME.BAT`: `assign a=c` / `supaplex` / `assign` — hra čeká, že běží z A:.
- Zvukové ovladače se natahují ze souborů: `ADLIB.SND`, `BLASTER.SND` (39 KB),
  `SAMPLE.SND` (36 KB), `ROLAND.SND`, `BEEP.SND`.

### Obsluha klávesnice (INT 9), file offset `0x004CC`

```asm
push ax / push bx / push cx / push ds
mov  ax, 0x3f60          ; pevně daný datový segment
mov  ds, ax
in   al, 0x60            ; scancode
mov  cl, al
mov  bl, al
in   al, 0x61            ; XT potvrzení: bit 7 nahoru a zase dolů
or   al, 0x80
out  0x61, al
and  al, 0x7f
out  0x61, al
xor  al, al
xor  bh, bh
shl  bl, 1               ; bit uvolnění do CF
cmc                      ; CF=1 stisk, 0 uvolnění
rcl  al, 1               ; al = 1 / 0
shr  bl, 1               ; bl = scancode bez bitu 7
mov  [bx + 0x166d], al   ; ★ TABULKA STAVU DRŽENÝCH KLÁVES
test cl, 0x80
jne  release
mov  [0x16f9], cl        ; poslední stisknutý scancode
cmp  cl, 0x54
jne  done
mov  word [0x1664], 1
mov  word [0x166a], 1
done:
release:
mov  byte [0x16f9], 0
mov  al, 0x20 / out 0x20, al   ; EOI
pop ds / pop cx / pop bx / pop ax / iret
```

**Tabulka na `[bx + 0x166D]` je zásadní:** hra podle ní ví, které klávesy jsou
držené. Ztratí-li se scancode **uvolnění**, zůstane v ní jednička a **postava
jede dál** — přesně příznak „klouže bez animace“ u Prehistorika. Je to
domněnka, ale dobře padne na pozorování.

Odkazy na `[0x16F9]` v binárce:

```
file 0x030a7 / image 0x02ea7   mov al, [0x16f9]     čtenář
file 0x0387f / image 0x0367f   mov al, [0x16f9]     čtenář
file 0x088a7 / image 0x086a7   mov al, [0x16f9]     ★ čekací smyčka
file 0x00510 / image 0x00310   mov byte [0x16f9], 0
file 0x030ae / image 0x02eae   mov byte [0x16f9], 0
```

Čekací smyčka na `0x88A7` živě odpovídá `4764:88FF`:

```asm
88ff  mov al, [0x16f9]
8902  cmp al, 0
8904  je  0x88ff
8906  cmp al, 0x1c        ; Enter
890a  jmp 0x8998
890d  cmp al, 0x0b        ; '0'
8911  mov ah, 0x30
```

---

## 7. Vnější kontext

**86Box issue #2594** — [github.com/86Box/86Box/issues/2594](https://github.com/86Box/86Box/issues/2594)

Hlásí u **Prehistorika 2** zamrznutí při skoku (šipka nahoru) a při zásahu
nepřítelem. Jiný emulátor, jiný čipset (420TX), AWE32. **Otevřené,
bez diagnózy.** Naznačuje to, že jde o vlastnost té hry, kterou snese reálný
hardware a přesná emulace ne — a že to nebude triviální.

**cilliemalan/supaplex** — disassembly `SPFIX63.EXE`, tedy „speed fix“ verze.
Hardware nedokumentuje. **Speed fix nedoporučuju**: řeší, že hra běží na
*rychlých* strojích moc rychle, kdežto tahle deska jede kolem 1,9 MIPS, což je
zhruba 386SX — tedy blízko cílovému stroji hry.

---

## 8. Nesouvisející opravy z téhož dne (baseline)

Ať je jasné, na čem se měří. Všechny ověřené na hardwaru:

- **`i8254.c`** — pole režimu 8254 je tříbitové a čip dekóduje `110`/`111` jako
  režimy 2/3. Emulátor ukládal surovou trojici, takže legální „režim 3“
  zapsaný jako `111` (což dělá Disney's Aladdin) minul `switch` a spadl do
  `default: abort()`. Doplněno dekódování; `abort()` odstraněn, režimy 0 a 4
  dělají korektní jednorázový puls.
- **`vga.c`** — dva `abort()` na nepodporované bpp nahrazeny vykreslením černé.
- **`adlib.c` / `adlib.h`** — kruhový buffer po vzorcích místo po dávkách
  (Electro Body používá OPL registr 0x40 jako 6bitový DAC na 8,5 kHz) a
  převzorkování 49716 → 44100 Hz (naměřený posun +210 centů proti očekávaným
  +208).
- **`pc.c`** — jemnější kontrola PIT při reloadu < 512.
- **`sb16.c`** — přenos DMA škrcený náskokem 20 ms před přehráváním místo
  `free = dma_len`; dodělaný příkaz DSP `0x80` (ticho + přerušení), který
  chyběl a kvůli kterému Tyrian hlásil „ERROR 253“.
- **`hdmi.c`** — řídicí symboly přesunuty z `0xFC–0xFF` na `0xD4–0xD7`
  (staré hodnoty znamenaly „bílá vedle bílé“, což barvilo bílý text).
- **`emu8950.c`** — `SAMPLE_BUF_SIZE` 1024 → 64; **+4 160 B volné haldy**.
- **`pc.c`** — `_pc_io_read16()` i `_pc_io_read32()` měly v `default:`
  `return 0`, takže **každé 16bitové čtení portu, pro který tam není vlastní
  větev, přečetlo nulu**. Osmibitové zařízení odpoví na sběrnici ISA na slovní
  cyklus dvěma bajtovými cykly, na `addr` a `addr+1`; zápisová cesta to tak už
  dělala, čtecí ne. Skunny (`C:\HRY\SKUNNYBC\FOREST.EXE`) se tím zavěsil
  natvrdo — jeho časovací smyčka čte `in ax, 0x40` dvakrát a točí se, dokud se
  obě hodnoty rovnají, a nula se od nuly neliší nikdy. Opraveno rozpadem na
  bajtové (u 32 bitů slovní) cykly; ověřeno na desce, hra běží. Týmž tahem se
  spraví i `in ax, 0x3da` (čekání na zpětný běh paprsku) a slovní čtení
  adresních registrů DMA, které do teď vracelo nuly.

**Body návratu:** `snapshots/2026-09-02-io16/` (a starší `-final`, `-audio-fixed`,
`-sram-headroom`, `-hdmi-ctrl`, `-pit-mode`), každý s `known-good.elf`,
zdrojáky a `SHA256SUMS.txt`.

---

## 9. Provozní poznámky

- Flash na `adapter speed 4000` občas selže (`timed out while waiting for
  target halted`, `Examination failed`). **Na 1000–2000 kHz to projde.**
- `pc_new()` potřebuje o chlup víc než **42 728 B** haldy a v aktuálním buildu
  jich dostane **47 720**. Když se to překročí, `g_diag_stage` uvázne na 9
  (`DIAG_PRE_PC_NEW`), `g_diag_pc_new_failed` zůstane **0** (protože se
  `pc_new()` vůbec nevrátí — spadne uvnitř na alokaci) a deska stojí u černé
  obrazovky. `-DSUBSYS_PROFILE=ON` tenhle strop překročí a **nenabootuje**.
- Podle poznámky v `main.c` **čtení desky přes SWD za běhu ji opakovaně
  shodilo**; rozpočet je „jeden dotaz, až když už stojí“, ne opakované
  dotazování za běhu. Proto tu není žádný automatický poller.
