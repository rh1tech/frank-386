# Supaplex — vyřešeno: jeden cyklus DMA, který neposunul kanál

**Datum:** 2026-09-03 · **Stav:** příčina nalezena, opravena a ověřena na hardwaru
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz / PSRAM 166

Navazuje na `FRANK_SUPAPLEX_OPEN.md` (co bylo vyloučeno a kudy jsem šel) a na
`FRANK_PREHISTORIK2_SOLVED.md` (druhá z těch dvou padajících her; jiná
příčina). Všechno níže je měřené na desce.

---

## 1. Příčina

DSP příkaz **`0xE2`** je „DMA identification": karta na něj provede **jeden
cyklus DMA** a zapíše bajt do paměti. Ovladače podle toho zjišťují, na kterém
kanálu karta visí.

Skutečný 8237 po každém cyklu **posune adresu kanálu**. Emulátor to nedělal:
`i8257_dma_write_memory()` počítá cíl jako `page<<16 | now[ADDR] + pos`,
volající pro `0xE2` předával `pos = 0` a kanálem nikdo nepohnul — takže
**každý zápis přes `0xE2` trefil tentýž bajt**.

`BLASTER.SND` Supaplexu naprogramuje kanál 1 na zápis **dvou** bajtů do svého
škrábacího místa na `CS:0x45` a pošle `0xE2` dvakrát:

```
port 0x0A val 0x05   ; maskovat kanál 1
port 0x0B val 0x45   ; režim: zápis do paměti, single, increment
port 0x0C            ; vynulovat flip-flop
port 0x02 val 0x95   ; adresa nízký bajt
port 0x02 val 0x85   ; adresa vysoký bajt     → 0x8595
port 0x83 val 0x06   ; stránka kanálu 1       → fyzicky 0x68595
port 0x03 val 0x01   ; počet = 2 bajty
port 0x03 val 0x00
port 0x0A val 0x01   ; odmaskovat
```

Na hardwaru padnou ty dva bajty na `0x68595` a `0x68596`. V emulátoru padly
**oba na `0x68595`** a druhý bajt zůstal, jaký byl.

### Co z toho vzniklo

Slovo na `CS:0x45` je zároveň dosažitelné jako položka rozskokové tabulky:

```asm
0725  cmp bx, 0x0e
0728  jae 0x73e            ; index je hlídaný
0736  shl bx, 1
0738  call word ptr [bx + 0x39]    ; bx = 6 → 0x39 + 12 = 0x45
```

S půlkou aktualizovaného slova ukázalo na **`0x9407`**. Tam v tom segmentu
nejsou instrukce, ale nuly — a `00 00` je `add [bx+si], al`, dvoubajtové.
Stopa proto jde po dvou bajtech až na `0xFFFF`, přeteče na `0x0001` a ty
`add` zápisy mezitím rozmlátí kód ovladače (past na jeden hlídaný bajt
napočítala **6114 zásahů**). Když v tom stavu přijde přerušení klávesnice,
`IRET` obsluhy hry vrátí řízení doprostřed té spouště:

- **s EMM386** runaway zachytí V86 monitor a program se restartuje na
  obrazovku se zadáním kódu ochrany;
- **bez EMM386** to skončí smyčkou na `#UD` (`exc_hist[6] = 22 577 419`).

Obojí je tentýž pád viděný z jiné strany. Proto se to léta popisovalo dvěma
různými příznaky.

---

## 2. Oprava

`src/i8257.c` + `src/i8257.h` + `src/sb16.c`:

```c
/* Ruční cyklus DMA musí posunout kanál, jako to dělá 8237. */
void     i8257_dma_advance(IsaDma *obj, int nchan, int len);
uint32_t i8257_dma_get_pos(IsaDma *obj, int nchan);
```

a v obsluze `0xE2`:

```c
i8257_dma_write_memory(s->isa_dma, s->dma, &(s->e2_valadd),
                       (int)i8257_dma_get_pos(s->isa_dma, s->dma), 1);
i8257_dma_advance(s->isa_dma, s->dma, 1);
```

Pozice se předává proto, že pomocné funkce sčítají `now[ADDR] + pos`; obsluha
přenosu předává absolutní pozici, kterou dostala, takže volající, který dělá
jediný cyklus sám, musí předat totéž — jinak každý takový cyklus skončí na
stejném bajtu.

### Nutná souběžná oprava v `src/pc.c`

Řadič DMA 1 nebyl dekódovaný na svém **aliasu `0x10–0x1F`**. Čip má čtyři
adresní vodiče a chip select složený z toho, že vyšší bity jsou nula, takže na
skutečném PC vybírá DMA 1 **každý** port od `0x00` do `0x1F`. Ovladač
Supaplexu čte počítadlo kanálu 1 na portu **`0x13`** — v okně selhání jsem
naměřil **40 960 čtení**, než se to spravilo; po opravě je tam jediné.

---

## 3. Ověření na hardwaru

Čistý start, level 1, pět dávek pohybu (vpravo / dolů / vlevo):

```
cs_base 0x47640  ip 0x54e5      ; hra běží ve své hlavní smyčce
ud_hit 0   ud_reason 0          ; žádná výjimka
```

Level se dohrál a skončil **v hlavním menu** se zapsaným časem
(`AIDEN 000:11:09.91`) — tedy tak, jak se hra podle uživatele chovat má.
Potvrzeno i uživatelem na monitoru.

Před opravou skončil každý pokus návratem na obrazovku s kódem (s EMM386),
respektive smyčkou na `#UD` (bez něj).

---

## 4. Další opravy, které při tom vznikly

Všechny jsou samostatně správné, ale **žádná z nich hru nespravila** — je
poctivé to říct, aby je někdo v budoucnu nepovažoval za příčinu:

1. **Reset DSP nerušil naplánované přerušení.** `reset()` nuloval
   `mixer_regs[0x82]`, ale nechával `aux_pending`. Přerušení pak přišlo se
   zahozeným stavovým bitem, potvrzení na `base+0x0e` je na tom bitu závislé,
   takže by se linka nesundala — a protože je 8259 hranově citlivý, další
   přerušení Sound Blasteru by už nikdy nepřišlo. Opraveno; `sb16_poll()` teď
   stavový bit nastavuje spolu s linkou.
2. **IRQ Sound Blasteru se vyvolávalo z jádra 1.** `sb_set_irq()` se volá z
   `sb16_getsample()` v 44,1kHz obsluze na jádru 1 a měnilo IRR/ISR řadiče
   přerušení, zatímco jádro 0 mohlo zrovna přerušení potvrzovat; nic to
   nezamykalo. Sound Blaster je jediné zařízení, které přerušuje z druhého
   jádra. Jádro 1 teď jen požádá a `pc_step()` na jádru 0 přerušení vyvolá.
3. **Zúžení bitu 7 na `base+0x0E`** (hlásil „data k dispozici" i když jen visel
   IRQ) bylo **vráceno**: je to sice odchylka od hardwaru i od DOSBoxu, ale
   nevalidovaná změna chování SB by mohla rozbít jinou hru. Leží v
   `src/sb16.c.readstatus-attempt`.

---

## 5. Slepé uličky, na které jsem naletěl

Stojí za zapsání, protože každá vypadala přesvědčivě a stála jedno kolo:

- **„Ovladač se nahrává zkrácený."** Segment ovladače měl za sebou jen 4 752
  bajtů obsahu a jeho tabulka ukazovala na kód 38 kB dovnitř modulu. Vyvráceno:
  `debug blaster.snd` → `CX = 991B`, tedy DOS přečte celých 39 195 bajtů, a kód
  ze souborového offsetu `0x94BA` se v paměti **najde** na `0x6785A`. Modul je
  nahraný celý; jen ho ovladač adresuje dvěma segmenty (`0x5E3A` je začátek,
  `0x6737`/`0x6855` ukazuje o `0x8FD0` dál).
- **„Přepsaný bajt kódu ovladače."** Past na jeden bajt (`+0x2D0`) chytila 6114
  zápisů — ale všechny z `6737:0094`, tedy už z divokého běhu v nulách. Byl to
  **následek**, ne příčina. Přesně stejný tvar jako u Prehistorika, a proto
  svůdný.
- **Past `ret_cs`** chytila legitimní návrat startovní rutiny; dokument na to
  varoval a měl pravdu.
- **Re-entrance obsluhy přes EOI** — vyloučeno čtením kódu: PIC je správně
  hranově citlivý (`i8259.c`, `pic_set_irq1`).

Společné poučení: **než se začne opravovat nástroj, musí být jasné, v jakém
okamžiku se chyba děje.** Trojí zpřesňování pastí nepomohlo; pomohlo teprve
zmrazení na `CS:IP` a pak filtrovaná stopa portů, která ukázala pořadí
přístupů.

---

## 6. Nástroje, které při tom vznikly

- **`claude_handoff/sp_run.py`** — z resetu desky až do rozehraného levelu
  jedním příkazem. Bez toho se pravidlo „každé měření z čistého spuštění"
  nedodržuje.
- **`claude_handoff/swdgfx.py`** — grafická obrazovka hosta přes SWD do PNG
  (EGA planární i chain-4).
- **`claude_handoff/mon.py`** — `--status`, `--arm`, `--dump`, `--peek`.
- **past na shodu `CS:IP`** v `audiodiag.h` (`trap_cs`/`trap_ip`,
  `ud_reason = 9`) — bez ní se tenhle druh chyby chytit nedá, protože tu není
  žádná výjimka ani špatný skok, na který by šlo zmrazit.
- **filtrovaná stopa portů** (`portlog_on`, kruh na `BASE+0x16000`) —
  uspořádaný sled přístupů na DMA, PIC a SB. To byl nástroj, který nakonec
  ukázal, na co ovladač čeká.

> **Pozor na kolize nástrojů.** Kruh událostí jsem nejdřív umístil na
> `BASE+0x14000`, kde do něj psal histogram portů, a výpis obsahoval nemožné
> řádky (`irq 5 level 131072`). Kruh instrukcí jsem zase zkusil zvětšit na
> `0x11400000` v domnění, že je nad pamětí hosta volná PSRAM — jenže
> `CMakeLists.txt` dává hostovi `EMU_MEM_SIZE_MB=8` a deska šla do boot smyčky.
> Test „zapíšu vzorek a přečtu ho zpátky" to **neodhalí**; číst se má
> konfigurace, ne paměť.
