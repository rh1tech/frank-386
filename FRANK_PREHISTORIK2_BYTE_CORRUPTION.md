# Prehistorik 2 — jeden přepsaný bajt

> **VYŘEŠENO 2026-09-03:** viz `FRANK_PREHISTORIK2_SOLVED.md`. Řádek 2
> tabulky v kapitole 3 („zápis emulovaným procesorem — vyloučeno") je
> **nesprávný**: hlídač byl ozbrojován až po načtení levelu, ale poškození
> vzniká *při* načítání levelu, takže past byla vždycky pozdě. Pisatelem je
> hra sama, instrukcí `add cs:[62B8],al` na `10BB:970E`.


**Datum:** 2026-09-02/03 · **Stav:** příčina zúžena na jediný bajt, pisatel
zatím nezachycen
**Deska:** FRANK 386 (Z2 / RP2350B), měřeno na 504/P166, 504/P84, 378/P133

Navazuje na `FRANK_PREHISTORIK2_SUPAPLEX_HANDOFF.md`. Ten popisuje, co bylo
vyloučeno do 2. 9. odpoledne; tenhle dokument popisuje, co se našlo potom.
Všechno níže je měřené na hardwaru, ne odvozené.

---

## 1. Co se přesně děje

Hra se nezasekne na špatném skoku. **Zasekne se proto, že se jí pod rukama
změní jeden bajt vlastního kódu**, a všechno ostatní je už jen důsledek:

```
10BB:62B8   50            push ax          ← v paměti se změní na FB (sti)
10BB:62B9   51            push cx
10BB:62BA   52            push dx
10BB:62BB   55            push bp
...
10BB:62EE   5D 5A 59 58   pop bp/dx/cx/ax  ← čtyři popy proti třem pushům
10BB:62F2   C3            ret              ← vezme návratovou adresu o 2 bajty vedle
```

`sti` na `SP` nesahá, takže se do zásobníku uloží jen tři registry, ale vysypou
se čtyři. `SP` skončí o dva bajty výš a `ret` si vezme slovo, které tam nepatří.

Zachyceno pastí přímo v interpretu (stopa vede `SP` u každé instrukce):

```
63C1  SP=01DA   call 0x62b8
62B8  SP=01D8   ← call korektně uložil návratovou adresu
62B9  SP=01D8   ← push ax SP nesnížil  (v paměti tam je FB, ne 50)
62BA  SP=01D6   ✓
62BB  SP=01D4   ✓
62BC  SP=01D2   ✓
62F2  SP=01DA   ← ret čte 0x10B8A místo 0x10B88
```

Zásobník je přitom **nepoškozený** — 64 bajtů kolem `SS:SP` sedí přesně tak,
jak je hra ukládala:

```
0x10B88: 63C4   ← správná návratová adresa (call na 63C1)
0x10B8A: 0000   ← sem ukazuje SP, odsud ret vzal nulu
0x10B8C: 0320   ← ax uložené na 63BC
0x10B8E: 5B57   ← návratová adresa volajícího
```

Ze 175 instrukcí `push` v okně stopy selhala **jediná** — ta na přepsaném
bajtu.

### Zbytek řetězu

`ret` skočí na offset `0x0000` vlastního segmentu. Tam leží data, ve kterých
se `ff 1f` dekóduje jako `lcall [bx]`, což pošle řízení do segmentu `0000`,
kde host vykonává tabulku vektorů přerušení jako kód. Odtud už jen:

```
10BB:0002 -> seg 0000            far call přes nesmyslný ukazatel
0000:0002 -> seg 0000  x2944     běh po IVT
0000:0009 -> seg 0004            a nakonec #UD na 0004:10C0
```

`#UD` vektoruje na INT 6, kde je v BIOSu holý `IRET`, takže se to zacyklí — to
je ten „zásek", při kterém **hudba dál hraje**, protože přerušení se do smyčky
pořád dostanou.

S EMM386 v paměti to někdy místo smyčky zachytí V86 monitor a vypíše
`EMM386 has detected error #04 in an application at memory address 0000:070F`;
je to tentýž pád viděný o kus dřív.

---

## 2. Ten bajt

| | |
|---|---|
| guest | `10BB:62B8` |
| fyzicky | `0x16E68` |
| hostitelská adresa (PSRAM) | `0x11016E68` |
| bylo | `0x50` (`push ax`) |
| je | `0xFB` (`sti`) |

**Pokaždé stejná adresa a stejná hodnota.** Ověřeno ve třech nezávislých
bězích. Čtení přes cacheovanou (`0x11…`) i necacheovanou (`0x15…`) aperturu
PSRAM dává shodně `0xFB`, takže to není artefakt XIP cache — v paměti to
opravdu je.

**Jak poznat poškození od normálního běhu:** diff kódového segmentu mezi
začátkem levelu a záseknutým stavem **v témže běhu** dá osm změněných míst,
z toho šest jsou proměnné, které si hra drží uvnitř kódu:

| offset | co to je |
|---|---|
| `1D42`, `2680`–`2695`, `3A06`, `45A2`, `6778` | proměnné hry (mění se i v čistém běhu; `45A2` píše kód na `45D7`, `3A06` kód na `3A18`) |
| **`62B8`** | **poškození** |

Pozor: diff mezi *různými* běhy dá stovky rozdílů a je bezcenný — herní stav
se liší. Jediné, co něco znamená, je diff v jednom běhu.

---

## 3. Co je vyloučeno — a jak

| # | Podezřelý | Jak vyvráceno |
|---|---|---|
| 1 | **Native JIT** | Zásek se reprodukoval s `-DNATIVE_JIT=OFF -DBLOCK_JIT=OFF`. (V předchozím kole byl JIT celou dobu zapnutý a v tabulce vyloučených chyběl.) |
| 2 | **Zápis emulovaným procesorem** | Hlídač v `pstore8/16/32` pokrýval celých 7 kB kódu (`10BB:5000..6A00`, s výjimkou proměnné na `6778`) a v běhu, který skončil zásekem, nezaznamenal **ani jeden** zápis. |
| 3 | **DMA** | Hook v `i8257_dma_write_memory()` (obě větve, značka `0xDA00`) — ticho. |
| 4 | **Řetězcové čtení portů** (`INS`, disk PIO) | Hook na cestě `io_read_string`, která zapisuje přímo do `phys_mem` mimo `pstore` (značka `0x1451`) — ticho. |
| 5 | **Časování PSRAM** | Stejné poškození na `PSRAM_SPEED=84` i `133` jako na `166`. |
| 6 | **Takt jádra / přetaktování** | Stejné poškození na 378 MHz jako na 504 MHz. |
| 7 | **XIP cache** | Cacheovaná i necacheovaná apertura čtou shodně. |
| 8 | **Chain-4 větev `vga_mem_write()`** | Kontrolu mezí sice **nemá** (na rozdíl od obou ostatních větví), ale `bank_offset` je omezený maskou na `0x30000` a `addr` na `0xFFFF`, takže přes 256 kB `gfx_buffer` nepřeteče. Stojí za opravu, ale tohle nezpůsobuje. |
| 9 | **Buffery firmwaru v PSRAM** | V PSRAM leží jen paměť hosta, stínová banka OPL3 (`0x110a0000`), diagnostika (`0x110a1000`+) a záznam faultu (`0x110bb000`); EGA320 cache i EMS jsou vypnuté. Nic z toho nesahá na `0x16E68`. |

Poškození je **občasné**: některý běh se zasekne hned v první dávce pohybu,
jiný přežije třicet dávek bez jediné změny bajtu.

---

## 4. Diagnostika, která je ve stromu

Zapíná se `-DAUDIO_DIAG=ON`, s `OFF` se vše vypustí. Vše leží v PSRAM za
nepoužívanou VGA aperturou hosta, takže to nestojí ani bajt SRAM. RAM s celou
sadou: **86,2 %** (bez ní 85,4 %).

### Kruhy a záznamy

| adresa | obsah |
|---|---|
| `0x110a1000` | `FrankDiag` — čítače, hlavičky, nastavení pastí |
| `0x110a2000` | **kruh instrukcí**, 16384 × u32; položka = `IP | (SP << 16)`, značka `0xFFFFFFFF` následovaná bází CS |
| `0x110b2000` | kruh zásahů hlídače zápisu, 64 × 16 B `{addr, val, cs_base, ip}` |
| `0x110b2400` | 64 bajtů zásobníku kolem `SS:SP` při pasti na `ret` (od `ssp-32`) |
| `0x110b2800` | stínová kopie 7 kB kódu |
| `0x110bb000` | plný záznam hard faultu |
| `0x110bb800` | záznam hardwarového watchpointu: `"MONW"`, PC, LR, r0–r3, xPSR |
| `0x110bc000` | kruh načtení CS, 512 × 32 B `{t, from_base, from_ip, to_sel|opak<<16, ssp, stk0, stk1, flags}` |

### Pole `FrankDiag` (offsety od `0x110a1000`)

```
+0x2d0 ud_hit        1 = zmrazeno
+0x2d4 ud_cs         +0x2d8 ud_ip     +0x2dc ud_excno
+0x2e4 ud_flags      +0x2f0 cs_head   +0x2f4 exc_hist[32]
+0x374 ud_reason     proc se zmrazilo (viz níže)
+0x378 ud_ssp        +0x37c ud_stk0   +0x380 ud_stk1
+0x384 ip_head       +0x388 ip_last_cs
+0x38c wp_lo         +0x390 wp_hi     +0x394 wp_head   +0x398 wp_total
+0x39c ret_cs        segment, na který platí past na near ret
+0x3a0 shadow_base   +0x3a4 shadow_armed  +0x3ac shadow_addr
+0x3b0 shadow_was    +0x3b4 shadow_now
+0x3b8 wp_skip_lo[4] +0x3c8 wp_skip_hi[4]
+0x3d8 mon_addr      +0x3dc mon_armed
```

### Spouštěče (`ud_reason`)

| | |
|---|---|
| 1 | první `#UD` |
| 2 | jeden přenos řízení se zopakoval 65535× (zaseknutý host bez `#UD`) |
| 3 | zápis do hlídaného rozsahu (`wp_lo`..`wp_hi` mimo `wp_skip`) |
| 4 | near `ret` na offset < `0x10` v segmentu `ret_cs` |
| 5 | stínová kopie našla změnu |
| 6 | hardwarový watchpoint RP2350 |

Všechny spouštěče zmrazí **všechny** kruhy naráz, takže stopa instrukcí za
nimi je použitelná.

### Poznámky k použití

- **Kruh CS slučuje opakování se zpětným pohledem na čtyři položky.** Bez toho
  se ring plnil střídavým cyklem (`#UD` → INT 6 → `IRET` → `#UD`) a čtyři
  zachycení po sobě ukázala jen následek. Se slučováním pokryje celý boot
  i rozehraný level 277 položkami z 512.
- **Past na `ret` musí být omezená na segment hry** (`ret_cs`), jinak se
  spustí už při bootu DOSu — COMMAND.COM se legitimně vrací na `215F:0096`
  s návratovou adresou `0x0006`.
- **Hlídač zápisu ozbrojuj až v levelu.** Při načítání hry přes něj projede
  zavaděč (EMM386 kopíruje obraz, `12480:3303`) a past se spustí na tom.
- **Výjimky `wp_skip`** existují proto, že hra si drží proměnné uvnitř
  kódového segmentu; bez nich se hlídač spustí v první snímek.

---

## 5. Jak to reprodukovat

Cesta: `C:\HRY\PREHIST2\PRE2.EXE` (pozor, `C:\HRY\PREHIST` je **jednička**).
PSP hry je na segmentu `0x108B`, kódový segment `0x10BB` (báze `0x10BB0`).

Přes `claude_handoff/swdkey.py` (ELF musí odpovídat nahranému firmwaru):

```
cd \hry\prehist2{ENTER}      počkat ~4 s
pre2{ENTER}                  počkat ~45 s (378 MHz: ~50 s)
{ENTER} ×3                   dvě intro obrazovky a titulní (~9 s mezi nimi)
--hold 1 / --release 1       obrazovka „1 - START LEVEL 1"; občas je potřeba
                             zopakovat, tapnutí se někdy ztratí
{ENTER}                      „MODE BEGINNER"
{ENTER}                      level se načítá ~20 s
```

Pak držet šipku vpravo ve dvousekundových dávkách a mezi nimi číst `ud_hit`.
Zásek přijde typicky do desáté dávky, ale není zaručený.

`swdkey.py` teď zná i číslice jako tokeny (`--hold 1`), protože tapnutí `1`
tahle obrazovka často zahodí.

**Provozní past, na kterou jsem naletěl dvakrát:** shell si mezi příkazy
resetuje pracovní adresář. Každý příkaz, který volá `swdkey.py`, musí začínat
`cd /c/Users/janbr/Desktop/tiny386/frank-386`, jinak volání tiše selžou
a člověk „hraje" hru, která vůbec neběží.

---

## 6. Kde pokračovat

Deska je **teď připravená přesně na tenhle pokus**: běží 378/P133 firmware
s celou diagnostikou, hra je v levelu 1 a **hardwarový watchpoint RP2350 je
ozbrojený na `0x11016E68`** (`mon_addr`). Stačí hrát, dokud se nezasekne, a pak
přečíst:

```bash
mdw 0x110bb800 10     # "MONW", PC, LR, r0-r3 - kdo ten bajt zapsal
mdw 0x110a1374 1      # ud_reason: 6 = watchpoint, 1 = zase jen #UD
mdw 0x11016e68 1      # a jestli tam FB vůbec je
```

Watchpoint je jednorázový (obsluha ho vypne), takže po zásahu je potřeba znovu
zapsat `mon_addr` a vynulovat `mon_armed`.

Pokud watchpoint mlčí a bajt se přesto změní, zbývá jediné vysvětlení, které
zatím není vyloučené: **zápis z druhého jádra**. DWT je per-core a tenhle hlídá
jádro 0. Další krok by pak byl ozbrojit stejný komparátor i na jádru 1 (video
a zvuk běží tam) — v `main.c` je `frank_mon_arm()` volaná z hlavní smyčky,
stačí ji zavolat i ze startu jádra 1.

Druhá cesta, kdyby watchpoint selhal úplně: rozšířit stínovou kopii přes celý
kódový segment (28 kB) s výjimkami na těch šest proměnných a porovnávat ji
řidčeji. Pak sice nezjistíme pisatele, ale dostaneme přesný okamžik zápisu
a stopa instrukcí za ním ukáže, co host dělal.

---

## 7. Co bylo dnes opraveno mimo tohle

- **`pc.c`** — `_pc_io_read16()` a `_pc_io_read32()` vracely ve větvi
  `default:` nulu, takže každé 16bitové čtení portu bez vlastní obsluhy
  přečetlo 0. Rozpadá se to teď na bajtové (resp. slovní) cykly, jak to dělá
  sběrnice ISA a jak to už dělala zápisová cesta. **Opravilo Skunny
  (`C:\HRY\SKUNNYBC\FOREST.EXE`)**, které se zaseklo na `in ax, 0x40`; týmž
  tahem se spraví i `in ax, 0x3da` a slovní čtení adresních registrů DMA.
  Ověřeno na desce. Bod návratu `snapshots/2026-09-02-io16/`.
- V `claude_handoff/swdkey.py` přibyly číslice do tabulky kláves.

Chain-4 větev `vga_mem_write()` bez kontroly mezí (kapitola 3, řádek 8) je
skutečná díra, jen ne tahle — stojí za samostatnou opravu.
