# Prehistorik 2 — vyřešeno: zápis do ROM BIOSu

**Datum:** 2026-09-03 · **Stav:** příčina nalezena, opravena a ověřena na hardwaru
**Deska:** FRANK 386 (Z2 / RP2350B), měřeno na 504 MHz / PSRAM 166 MHz

Uzavírá `FRANK_PREHISTORIK2_SUPAPLEX_HANDOFF.md` a
`FRANK_PREHISTORIK2_BYTE_CORRUPTION.md`. Ty popisují dvě kola pátrání, ve
kterých se příčina nenašla; tenhle dokument popisuje, co to bylo, proč to
předchozí metoda nemohla chytit, a jak je to opravené.

---

## 1. Příčina

Prehistorik 2 se brání debuggeru tímhle kódem — je v kódovém segmentu hry na
`10BB:96FF` a běží **při načítání levelu**:

```asm
10BB:96FF  33 c0           xor  ax, ax
10BB:9701  8e d8           mov  ds, ax
10BB:9703  c5 1e 0c 00     lds  bx, [000C]      ; vektor INT 3 → F000:06F4
10BB:9707  8a 07           mov  al, [bx]        ; první bajt obsluhy INT 3
10BB:9709  80 37 55        xor  byte [bx], 55   ; ★ zápis do ROM BIOSu
10BB:970C  2a 07           sub  al, [bx]
10BB:970E  2e 00 06 b8 62  add  cs:[62B8], al   ; ★ přičte rozdíl do vlastního kódu
```

Na skutečném PC leží obsluha INT 3 v **ROM BIOSu**. Zápis na `10BB:9709` se
tedy neprojeví, `sub al,[bx]` odečte tutéž hodnotu, jakou `mov al,[bx]` načetl,
a `al` vyjde **nula**. `add cs:[62B8], al` pak nepřičte nic a kód hry zůstane
nedotčený. Když si někdo přesměruje INT 3 do RAM (typicky debugger), zápis se
propíše, `al` vyjde nenulové a hra si rozbije vlastní kód. To je celý smysl té
kontroly.

**V emulátoru byla oblast BIOSu obyčejná zapisovatelná RAM**, takže se hra
chovala, jako by pod debuggerem běžela vždycky.

### Řetěz, každý článek změřený na desce

| krok | naměřeno |
|---|---|
| `F000:06F4` původně | `0x00` |
| po herním `xor … 0x55` | `0x55` (přečteno přes SWD) |
| `al = 0x00 − 0x55` | `0xAB` |
| `[62B8] = 0x50 + 0xAB` | **`0xFB`** — přesně pozorované poškození |

`0xFB` je `sti`. Rutina na `62B8` proto uloží tři registry místo čtyř, ale
vysype čtyři, `SP` skončí o dva bajty výš, `ret` si vezme cizí slovo a řízení
uteče do tabulky vektorů přerušení, kde skončí ve smyčce na `#UD` — přesně
podpis popsaný v obou předchozích dokumentech.

---

## 2. Proč to dvě kola nešlo chytit

Dvě nezávislé chyby v metodě, obě stejného druhu: **měřilo se v nesprávném
okamžiku, ne nesprávnou věcí.**

1. **Softwarová past v `pstore` se ozbrojovala až v levelu.** Poškození ale
   nastane *při načítání levelu*, tedy dřív. Past proto byla vždycky němá a z
   toho se vyvodilo „zápis emulovaným procesorem je vyloučen" — což byl přesně
   ten jediný chybný závěr, o který se opíralo celé druhé kolo. Zápis
   emulovaným procesorem to celou dobu byl.

2. **Hardwarový watchpoint (DWT) mazal OpenOCD.** Při každém připojení
   `cortex_m` cíl vynuluje `DWT_FUNCTION` všech komparátorů. Firmware ho
   ozbrojí jednou (`mon_armed` je západka), a první `mdw` po ozbrojení ho zase
   shodí. Protože se klávesy do hosta posílají taky přes OpenOCD, byl
   komparátor v praxi vypnutý pokaždé, když se na desku sáhlo. Ověřeno: bajt se
   prokazatelně změnil (`0x0c` → `0x70`) a `mon_hits` zůstalo **0**.

   Navíc má `DWT_FUNCTION` na Cortex-M33 (ARMv8-M) jiné kódování než na
   ARMv7-M: pole `MATCH[3:0]` a `ACTION[5:4]`, kde zápisový watchpoint je
   `MATCH = 0b0101` a debug událost `ACTION = 0b01`, tedy `0x15`. Firmware
   zapisoval `0x16`, což je na ARMv8-M watchpoint na **čtení**.

**Poučení pro příště:** dokud není známý *okamžik* poškození, nemá smysl
zpřesňovat *nástroj*. Levný způsob, jak okamžik najít, je porovnávat jeden bajt
po jednotlivých krocích scénáře (titulní obrazovka → level → pohyb), ne až po
zásekru.

### Vedlejší, ale důležité: „občasnost" nikdy neexistovala

Poškození nastane **pokaždé** a **vždy stejně**, při načítání levelu. Náhodné
bylo jen to, kdy hra tu poškozenou rutinu poprvé zavolá — což je při kontaktu s
nepřítelem. Odtud „jednou hned, jindy až po třiceti dávkách" i hlášení v
[86Box #2594](https://github.com/86Box/86Box/issues/2594), kde popisují zásek
při zásahu nepřítelem.

---

## 3. Oprava

`src/i386.c` — `pstore8/16/32` zahodí zápis do `0xF0000–0xFFFFF`, pokud
procesor neběží v 32bitovém chráněném režimu:

```c
#define GUEST_ROM_BASE 0xf0000u
#define GUEST_ROM_SIZE 0x10000u

static inline bool in_rom(uword addr)
{
	return (addr - GUEST_ROM_BASE) < GUEST_ROM_SIZE;
}

static inline bool rom_write_allowed(CPUI386 *cpu)
{
	return (cpu->cr0 & 1) && !(cpu->flags & VM);
}
```

### Proč zrovna tahle podmínka

Zablokovat oblast natvrdo **znemožní boot** — vyzkoušeno, deska se zastaví po
7107 instrukcích. SeaBIOS si totiž drží měnitelné proměnné v F segmentu a
během POSTu do nich píše, přesně jako to skutečný čipset dovolí, dokud jsou
PAM registry otevřené. Všech devět zápisů, které boot udělá, je změřeno:

```
0xf7f28 <- 1          cs_base 0  ip 0x0f365d
0xf30c8 <- 0x800000   cs_base 0  ip 0x0ef0cd
0xf7304 <- 1, 2       cs_base 0  ip 0x0e974a
0xf7320..0xf7330      cs_base 0  ip 0x0e9758..0x0e9777
```

Všechny mají plochý kódový segment a `EIP` hluboko nad `0xFFFF`, což je
dosažitelné jen v chráněném režimu. DOS i hry běží v reálném nebo V86 režimu,
kde má skutečný stroj v té oblasti ROM. Podmínka na režim ty dva případy
odděluje přesně tak, jak to vyšlo z měření.

**Správná odpověď by byla emulovat PAM registry i440FX.** Nejde to: tenhle
emulátor hostitelský můstek vůbec neregistruje (`pci_register_device()` v
`i440fx_init()` je zakomentované), takže zápisy hosta do PAM nemají kam
dopadnout a není co číst zpátky. Kdyby můstek přibyl, je tohle místo, kde se to
má nahradit.

### Zahozené zápisy se logují

32 položek `{adresa, hodnota, CS báze, IP}` na guest `0xB4800` (host
`0x110B4800`), s celkovým počtem hned za koncem kruhu na `0x110B4A00`. Leží v
mezeře mezi stínovou kopií a histogramy portů, takže to nestojí ani bajt SRAM.
Regrese je tím vidět místo aby byla tichá.

---

## 4. Ověření na hardwaru

Po opravě, čerstvý boot → `PRE2.EXE` → level 1:

```
0x11016e68:  50 51 52 55     kód hry netknutý (push ax/cx/dx/bp)
0x110f06f4:  00              bajt BIOSu netknutý
zahozené zápisy: 1
   addr=0x000F06F4  val=0x55  cs_base=0x00010BB0  ip=0x9709
```

Ten jediný zahozený zápis je přesně herní `xor byte [bx],0x55`. Hra se rozehrála
a nezasekla se; potvrzeno i uživatelem na monitoru.

Před opravou byl na `0x11016E68` po načtení levelu vždy `0xFB`.

---

## 5. Co zkontrolovat dál

- **Supaplex** má stejné příznaky (zásek při stisku klávesy ve hře, restart na
  obrazovku s kódem) a je to druhá ze dvou her, které na téhle desce padaly.
  Stojí za to zkusit, jestli ho tahle oprava spraví taky — mechanismus
  „ochrana proti kopírování sahá na ROM" je pro hry té doby typický.
- **VGA BIOS na `0xC0000–0xC7FFF`** je taky ROM a chráněný není. Rozšířit to
  tam je jednoduché, ale chce to opatrnost: `0xC8000–0xEFFFF` jsou UMB bloky,
  které EMM386 rozdává a DOS do nich nahrává ovladače, takže tam se blokovat
  **nesmí**.
- **DMA a diskové přenosy** do oblasti ROM zahazované nejsou. Skutečný stroj by
  je taky zahodil, ale žádný pozorovaný problém na to zatím neukazuje.
- **`frank_mon_arm()`** má na Cortex-M33 špatné kódování `DWT_FUNCTION`
  (`0x16` = watchpoint na čtení; správně je `0x15`) a firmware si komparátor
  neobnovuje, takže ho každé připojení OpenOCD vypne. Pokud se má hardwarový
  watchpoint ještě někdy použít, musí se opravit obojí — jinak mlčí a to mlčení
  vypadá jako výsledek.

---

## 6. Nástroje, které v tomhle kole přibyly

- **`claude_handoff/swdgfx.py`** — vyrenderuje **grafickou** obrazovku hosta z
  `gfx_buffer` přes SWD do PNG. `swdscreen.py` umí jen textový režim, což je
  přesně ta horší polovina: hra je vždycky v grafickém režimu. Zvládá EGA
  planární 16 barev (submode 2 a 6) i chain-4 256 barev (submode 3). Bez toho
  se hra řídí naslepo — a to je chyba, která v tomhle kole stála nejvíc času.
- **`claude_handoff/mon.py`** — `--status`, `--arm`, `--dump`, `--peek` nad
  diagnostikou; adresu hlídaného bajtu si odvodí z živé báze CS, protože
  segment, na který se hra nahraje, se mění s tím, co má DOS rezidentní.
- **`src/main.c`** — watchpoint zůstává ozbrojený a jeho obsluha se sama
  přezbrojuje, drží kruh posledních 32 zásahů, běží na **obou jádrech**
  (`mon_c1`) a má nejvyšší nastavitelnou prioritu. Přibyl i kontrolní odečet
  bajtu v hlavní smyčce (`mon_nowp`), který rozliší „nikdo z jader to
  nenapsal" od „watchpoint nefungoval". Kódování `DWT_FUNCTION` opravené
  není — viz kapitola 5.

---

## 7. Sekvence kláves, kterou hra potřebuje

Pro příští kolo, ať se to nemusí hádat (`claude_handoff/swdkey.py`):

```
cd \hry\prehist2{ENTER}      ~4 s
pre2{ENTER}                  ~50 s na 504 MHz
{ENTER}                      první infopage
{ENTER}                      druhá — teprve teď se nahraje kódový segment hry
--hold 1 / --release 1       „1 - START LEVEL 1"; tapnutí se často ztratí,
                             drž aspoň 1 s a klidně zopakuj
{ENTER}                      „MODE BEGINNER"
{ENTER}                      level se načítá ~25 s  ← tady vzniká poškození
```

**Před druhým `{ENTER}` není kódový segment hry v paměti** — hledat ho tam
nemá smysl a prázdný výsledek není závada.
