# Supaplex — otevřené, a je to jiná chyba než u Prehistorika

> **VYŘEŠENO 2026-09-03 — viz `FRANK_SUPAPLEX_SOLVED.md`.**
> Příčinou byl DSP příkaz `0xE2` („DMA identification"): dělá jeden cyklus DMA,
> ale emulátor po něm neposunul kanál, takže dva takové zápisy trefily tentýž
> bajt. Ovladač si tím dostal do svého slova na `CS:0x45` půlku hodnoty,
> rozskok `call word ptr [bx+0x39]` skončil na `0x9407` v nulách a odtud se to
> rozpadlo. Opraveno v `i8257.c`/`sb16.c`, plus nutné dekódování aliasu 8237 na
> `0x10-0x1F` v `pc.c`. Ověřeno na desce.
>
> **Kapitoly níže čti jako záznam cesty, ne jako závěry** — kapitola 9 obsahuje
> chybnou domněnku o „zkráceném načtení ovladače", která je hned nad ní
> opravená.

**Datum:** 2026-09-03 · **Stav:** nevyřešeno; zúženo, co to **není**
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz / PSRAM 166, firmware s ochranou
ROM ze `FRANK_PREHISTORIK2_SOLVED.md`

Prehistorik 2 a Supaplex se celou dobu vyšetřovaly společně, protože byly
jediné dvě hry, které na téhle desce padaly. Prehistorik je vyřešený. **Na
Supaplex ta oprava nezabrala a měření ukazuje, že jde o jiný mechanismus.**

---

## 1. Jak to reprodukovat

```
cd \hry\supaplex{ENTER}
runme{ENTER}                 ~40 s, skončí na obrazovce "CODE ENTRY"
1 2 3 {ENTER}                tři libovolné číslice; kód se nekontroluje
{SPACE}                      z menu do levelu
šipka vpravo                 ← tady to selže
```

**Každý test musí začínat čistým spuštěním hry.** Diff mezi dvěma různými běhy
je bezcenný a stav po předchozím selhání taky — obojí mě v tomhle kole stálo
jedno měření.

### Jak se to má chovat správně (podle uživatele)

Tohle je důležité, protože to mění formulaci problému. **Návrat z levelu
nastává jedině při zabití postavy** — jinak nikdy. A i tehdy má hra skončit
v **hlavním menu**, ne na obrazovce se zadáním kódu.

Vada je tedy dvojí a je potřeba je rozlišovat:

1. **Postava umře, i když by neměla.** Pohyb vpravo hned na startu levelu 1
   Murphyho zabít nemá — jen prokopává „base". Sem patří i podezření ze
   starého handoffu, že se ztrácí scancode uvolnění klávesy a postava jede
   dál, dokud do něčeho nenarazí.
2. **Po smrti hra skočí na obrazovku s kódem místo do menu.** To znamená, že
   se de facto spustila znovu od začátku — což je buď chyba v řízení toku, nebo
   větev „ochrana neprošla → restart".

Zatím není jasné, jestli jsou to dvě chyby, nebo jedna se dvěma projevy.

### Dvě různá selhání podle toho, jakou klávesou se hýbe

| klávesa | co firmware pošle | co se stane |
|---|---|---|
| kurzorová šipka (`{RIGHT}`) | `E0 4D` (rozšířená) | hra se vrátí na obrazovku s kódem, **bez výjimky** |
| numerická šestka (`KP6`) | `4D` (bez prefixu) | `#UD` na `6855:5BF0`, a v jednom běhu **reboot hosta** |

`ps2_put_keycode()` posílá všechny kódy od 96 výš jako `E0` + kód set 1;
cokoli pod 96 jde jako holý scancode ([i8042.c:537-558](src/i8042.c#L537-L558)).
Do `claude_handoff/swdkey.py` proto přibyl numerický blok (`KP0`–`KP9`), aby
šlo obojí testovat.

---

## 2. Co je vyloučeno

| # | Podezřelý | Jak vyvráceno |
|---|---|---|
| 1 | **Zápis do ROM BIOSu** (příčina u Prehistorika) | Log zahozených zápisů na `0x110B4A00` zůstal po celém běhu Supaplexu **na nule**. Supaplex na oblast `0xF0000–0xFFFFF` vůbec nesahá. |
| 2 | **Runaway / špatný skok** (podpis Prehistorika) | Ve variantě s kurzorovou šipkou je `ud_hit = 0` — žádná výjimka. Kruh přenosů řízení ukazuje ustálený, zdravý cyklus V86 ↔ trap EMM386 (selektor `0x0048`, monitor na segmentu `0x1248`). Hra se na obrazovku s kódem vrací **vědomě**, není to zásek. |

---

## 3. Co je naměřeno

- Kódový segment hry: **`0x4764`** (báze `0x47640`). Čekací smyčka na klávesu
  je `4764:88FF`, přesně jak ji popisuje starý handoff.
- Diff kódové oblasti `0x47640..0x60000` **v jednom běhu**, před pohybem a po
  selhání: 635 změněných bajtů ve 20 shlucích. Většina vypadá na herní data
  (skóre, sprity, fonty). Jediná změna tvarem podobná chybě u Prehistorika:

  ```
  seg+0x16979   len 1   před: c6   po: cd
  ```

  `0xC6` je `mov r/m8, imm8`, `0xCD` je `int`. **Není ověřené, že je to kód a
  ne data** — na to chybí referenční běh, kde hra funguje.
- Při selhání se znovu čte z disku (mění se 512bajtový sektorový buffer na
  `0x04500`), což odpovídá tomu, že se hra vrací na úvodní obrazovku.

---

## 4. Kde pokračovat

Pořadí je podle poměru cena/výtěžek.

1. **Ozbrojit softwarovou past na `seg+0x16979`** (fyzicky `0x5DFB9`) přes
   `wp_lo`/`wp_hi` **před** pohybem, v jednom čistém běhu. To je přesně ten
   postup, který Prehistorika rozlousknul na první pokus, a odpoví na obojí
   naráz: jestli je ten bajt kód, a kdo ho píše. Pozor na chybu z minulého
   kola — past musí být nahozená **dřív**, než se stav vytvoří.
2. **Zjistit, kde se hra rozhodne vrátit.** Selhání je vědomá větev, takže se
   dá chytit pastí na vstup do čekací smyčky `4764:88FF`. Diagnostika zatím
   past na shodu IP nemá; přidat ji je pár řádků a kruh instrukcí (16384
   položek, ~16 ms) pak ukáže, co hra testovala těsně předtím.
3. **Ochrana proti kopírování je hlavní podezřelý.** Návrat na obrazovku s
   kódem je přesně to, co dobová ochrana dělá, když test neprojde. Stojí za to
   projít `SUPAPLEX.EXE` na to, co ten test čte — u Prehistorika to byl vektor
   INT 3 a ROM.
4. **Uživatelské pozorování, které zatím nemá vysvětlení:** číslo zobrazené na
   obrazovce "CODE ENTRY" bylo po selhání „hausnummero". Jestli je to normálně
   náhodné číslo pro každé spuštění, neříká to nic; jestli ne, je to stopa po
   poškození dat.

---

## 5. Co je potřeba mít po ruce

- `claude_handoff/swdgfx.py <elf> out.png` — grafická obrazovka hosta přes SWD.
  Supaplex je celou dobu v grafickém režimu, takže `swdscreen.py` je k ničemu.
- `claude_handoff/mon.py <elf> --status` — `cs_base`, `ip`, `ud_hit`,
  `ud_reason` a hlídaný bajt jedním příkazem.
- `claude_handoff/swdkey.py` — nově i `KP0`–`KP9`, viz kapitola 1.

---

## 8. Kolo 2026-09-03 večer — zúženo na inicializaci Sound Blasteru

**Stav: příčina pořád nenalezena.** Ale okruh je výrazně menší a repro je
mnohem rychlejší.

### 8.1 Co je rozhodnuto měřením

| # | Zjištění | Jak |
|---|---|---|
| 1 | **Není to pád.** Hra se restartuje od svého vstupního bodu. | Past na shodu `CS:IP` sepnula na `47640:0010` i `47640:0000`. Do rozsahu `0x10..0x60` přitom v celém kódovém segmentu nic neskáče, takže se tam řízení dostalo dálkovým přenosem. |
| 2 | Restart provede **`IRET` v segmentu `0x6855`** s návratovým rámcem `4764:0000`. | Kruh dálkových přenosů: `68550:03e0 -> 4764`, `stk0 = 47640000`. |
| 3 | Segment `0x6855` je **ovladač Sound Blasteru** (`BLASTER.SND`). | DSP protokol v kódu: `[0x30]` je bázový port, `+0x0C` zápis, `+0x0E` stav, `out 0x0A,5` maskuje DMA kanál 1. |
| 4 | **Vypnutí Sound Blasteru chybu odstraní úplně.** | `pc->sb16_enabled = 0` přes SWD: level se dohraje a skončí **v hlavním menu**, jak má, včetně zapsaného času do žebříčku. |
| 5 | Není to OPL. | `adlib_enabled = 0`, `sb16_enabled = 1` → chyba přetrvává. |
| 6 | **Pád přichází při inicializaci SB, ne při hraní.** | Uživatel: na config page spadne hned, v levelu se to projeví návratem na code page. Sedí na kruh událostí — těsně před restartem je celá detekční sekvence ovladače. |
| 7 | Verze hry na tom nezáleží. | Uživatel vyzkoušel původní i přepatchovanou verzi pro nové PC; stejný výsledek. |
| 8 | Ochrana proti kopírování v tomhle výtisku je **vykuchaná**, takže návrat na code page není její selhání. | `89c3 cmp cx,[0xde34]` / `89c7 jne 0x89c9` — skok míří na následující instrukci, `[0xd9b]=1` se nastaví vždy. |

### 8.2 Detekční sekvence projde bez chyby

Záznam událostí SB těsně před restartem (čistý, po opravě kolize nástroje —
viz 8.4):

```
reset DSP -> 0xAA           ; potvrzení resetu
0xE0 0xC6  -> 0x39          ; invertovaný bajt sedí
0xF2                        ; žádost o IRQ, IRQ5 přijde i se potvrdí
reset DSP -> 0xAA
0xE0 0xAA  -> 0x55
0xE1       -> 04 05         ; verze DSP 4.05
0xE2 0xBA / 0xE2 0x94       ; identifikace DMA kanálu
0xE4 0xAA / 0xE8 -> 0xAA    ; testovací registr sedí
0x40 0x64                   ; časová konstanta = 6410 Hz
0x14 0x00 0x00              ; jednobajtový přenos přes DMA
   -> DMA_RUN 1 bajt, IRQ5 nahoru, potvrzeno
0xD1                        ; zapnout reproduktor
   ... 154 ms ticha ...
zápis do DMA, čtení stavu 0x00, a pak restart
```

Všechno projde. Ta **154ms mezera** na konci je zatím nejlepší stopa: vypadá
jako vypršení čekání.

### 8.3 Dvě odchylky od hardwaru, které to ale nejsou

Obě jsou skutečné a obě jsem otestoval na desce. **Ani jedna hru nespraví** a
je poctivé to říct, ne je vydávat za řešení.

1. **`base+0x0E` slučoval dvě různé věci.** Vracel bit 7 („čeká bajt ke
   čtení") i tehdy, když jen visel IRQ. Ovladač má přesně takovou čekací
   smyčku, takže by mohl přečíst bajt, který nevznikl. Opraveno, otestováno,
   **nepomohlo → vráceno** (nevalidovaná změna chování SB by mohla rozbít
   jinou hru; ponecháno v `src/sb16.c.readstatus-attempt`).
2. **Reset DSP nerušil naplánované přerušení.** `reset()` nuluje
   `mixer_regs[0x82]`, ale nechával `aux_pending`. Přerušení pak přišlo se
   zahozeným stavovým bitem, potvrzení na `base+0x0E` je na tom bitu závislé,
   takže by se linka nesundala — a protože je 8259 hranově citlivý, další
   přerušení SB by už nikdy nepřišlo. **Opraveno a ponecháno** (reset musí
   naplánované přerušení zrušit, na tom nemůže nic záviset), ale hru to
   nespravilo.

Vyloučeno bylo i podezření na re-entranci obsluhy přes EOI: PIC je správně
hranově citlivý (`i8259.c`, `pic_set_irq1`), takže držená linka nové přerušení
nevyvolá.

### 8.4 Poučení o nástrojích

- **Kruh instrukcí nejde zvětšit.** `CMakeLists.txt` dává hostovi
  `EMU_MEM_SIZE_MB=8` a PSRAM je 8 MB, takže žádná „volná paměť nad hostem"
  neexistuje. Přesun kruhu na `0x11400000` uložil ring doprostřed paměti hosta
  a deska šla do boot smyčky. Test „zapíšu vzorek a přečtu ho zpátky" tohle
  **nedokáže** — jen říká, že host ten bajt zrovna nepoužil. Číst se má
  konfigurace, ne paměť.
- **Kruh událostí koliduje s histogramem portů.** První umístění na
  `BASE+0x14000` nechalo `frank_diag_port()` psát skrz záznamy a výpis
  obsahoval nemožné řádky (`irq 5 level 131072`, neexistující druhy událostí).
  Teď je na `BASE+0x11000`; ta mezera je volná jen dokud se nepoužívá stínová
  kopie a hlídač zápisu, které tam mají svoje struktury.
- Do `audiodiag.h` přibyla **past na shodu `CS:IP`** (`trap_cs`/`trap_ip`,
  `ud_reason = 9`). Bez ní se tenhle druh chyby chytit nedá: není tu žádná
  výjimka ani špatný skok, na který by šlo zmrazit.

### 8.5 Kde pokračovat

1. **Použít rychlé repro:** config page, ne level. Pád přijde hned při
   inicializaci SB, takže odpadá celý nájezd přes kód a mezerník.
2. Zjistit, **na co ovladač těch 154 ms čeká** a proč to nepřijde. Kandidát je
   smyčka `6855:009F` (`in al,dx` / `or al,al` / `jns` / `loop`), která v
   zachyceném běhu protočila **1167 otáček**; hned za ní následuje
   `call 0x447`, což je vypínací cesta ovladače — maskuje DMA kanál, volá
   `0x2C9`, a to obnoví původní vektor přerušení a zamaskuje IRQ na PIC.
   Jinými slovy: **ovladač to vzdává a vypíná se.** Otázka je, co mu chybí.
3. Až bude příčina jasná, ověřit i **Tyrian 2000**, protože ten je důvod, proč
   v `sb16.c` vznikl obchvat s příkazem `0x80` a s hlášením stavu na
   `base+0x0E`.

---

## 9. OPRAVA ZÁVĚRU: ovladač zkrácený NENÍ — a proč to tak vypadalo

> **Pozor, čti dřív než cokoli z kapitoly 9 použiješ.** Kapitola níže tvrdí, že
> se `BLASTER.SND` nahrává do paměti zkrácený na 4 752 bajtů. **To je
> nesprávné** a bylo to vyvráceno měřením ještě téhož večera:
>
> - DOS soubor přečte celý: `debug blaster.snd` → `CX = 991B` = 39 195 bajtů.
> - Kód, který je v souboru na offsetu `0x94BA`, **v paměti hosta je** — jeho
>   bajty (`b9 ff ff 80 3e 91 00 00 75 08 fe 06 91 00`) se najdou na
>   `0x6785A`, což odpovídá bázi `0x5E3A0`, tedy **segmentu `0x5E3A`**. Ten se
>   v trailu skutečně objevuje (`5e3a0:8fbc`).
>
> Modul je tedy nahraný **celý**, jen ho ovladač adresuje **dvěma různými
> segmenty**: `0x5E3A` je jeho začátek a `0x6737` ukazuje o `0x8FD0` bajtů
> dál, na jeho pozdější část. To, co jsem měřil jako „4 752 bajtů obsahu za
> segmentem 0x6737", je prostě zbytek modulu od offsetu `0x8FD0` do konce
> souboru — žádné zkrácení.
>
> **Co z kapitoly 9 platí dál** (a je změřené): řetěz selhání v 9.1 — rozskok
> `call word ptr [bx+0x39]` na `6737:0738` skočí na `0x9407`, tam jsou nuly,
> ty se vykonávají jako `add [bx+si], al` po dvou bajtech až za konec
> segmentu, a přepíšou kód ovladače (6114 zápisů do hlídaného bajtu). Přepsaný
> bajt na `+0x2D0` je tedy **následek, ne příčina**. Platí i celá kapitola 9.5
> (tři samostatné opravy v `sb16.c`, `i8257.c` a `pc.c`).
>
> **Otevřená otázka pro další kolo:** rozskoková tabulka na `6737:0039`
> obsahuje `[0]=074a [1]=074e [2]=076a [3]=0785 [4]=07ab [5]=07c2 [6]=94ba
> [7]=082a …`, index je hlídaný na `< 14`, a přesto se skočilo na **`0x9407`**,
> což v té tabulce **není**. Buď se tabulka mezi otiskem a voláním změnila,
> nebo se čte s jinou bází, než předpokládám. To je nejkratší cesta k příčině:
> nahodit hlídač zápisu na tu tabulku (`0x67370+0x39`, 28 bajtů) v čistém běhu
> **před** spuštěním chyby.

---

## 9-star. Původní (částečně chybný) rozbor: „ovladač zvuku se nahraje zkrácený"

**Datum:** 2026-09-03 pozdě večer · **Stav:** příčina identifikována, oprava
zatím ne (chybí zjistit, *proč* je čtení krátké)

### 9.1 Řetěz, každý článek změřený na desce

1. Hra volá ovladač zvuku přes `INT 80h`, vektor míří na `6737:0000`
   (segment ovladače; bez EMM386, s ním je to `0x6855`).
2. Vstup ovladače končí rozskokem podle tabulky:

   ```asm
   0725  cmp bx, 0x0e
   0728  jae 0x73e            ; index je hlídaný
   072a  cmp bx, 4
   072d  jb  0x736
   072f  cmp byte [0x65], 0
   0734  je  0x73e
   0736  shl bx, 1
   0738  call word ptr [bx + 0x39]   ; tabulka 14 ukazatelů na offsetu 0x39
   0749  retf
   ```

3. Kruh instrukcí ukazuje, že `0x0738` normálně skáče na `0x074E`, `0x076A`,
   `0x0785`, `0x07C2` — a jednou na **`0x9407`**.
4. Na `0x9407` nejsou instrukce, ale **nuly**. Pozná se to podle tvaru stopy:
   jde po **dvou bajtech** bez jediného skoku až na `0xFFFF`, protože `00 00`
   je `add [bx+si], al`. Pak IP přeteče na `0x0001` a běží dál.
5. Ty `add` zápisy přepisují paměť. Past na jeden hlídaný bajt kódu ovladače
   (`+0x2D0`) zaznamenala **6114 zápisů**, všechny z `6737:0094`, tedy z té
   nulové oblasti. **Přepsaný bajt je následek, ne příčina** — stálo mě to
   několik kol, než jsem to rozlišil.
6. Když v tomhle stavu přijde přerušení klávesnice, obsluha hry doběhne
   normálně (`4646:0394` je `IRET`, ne řetězící skok) a vrátí řízení tam, kde
   host byl — tedy do nul. S EMM386 to zachytí V86 monitor a program se
   restartuje na obrazovku s kódem; bez EMM386 uteče do prázdna a skončí ve
   smyčce na `#UD` (`exc_hist[6] = 22 577 419`).

### 9.2 Měření, které to uzavírá

DOS hlásí velikosti souborů:

```
BLASTER  SND   39 195 bajtů
ADLIB    SND    5 354 bajtů
```

Otisk paměti pořízený **v levelu, před spuštěním chyby**:

```
segment ovladače 0x6737: poslední nenulový bajt na +0x128F  =  4 752 bajtů
za tím 147,8 kB nul až k dalšímu bloku DOSu na 0x8D530
rozskoková tabulka na +0x39:
   [0] 074a  [1] 074e  [2] 076a  [3] 0785  [4] 07ab  [5] 07c2
   [6] 94ba  [7] 082a  [8] 0817  [9] 08f4  [10] 08cb [11] 08db
   [12] 08ff [13] 093e
```

Tabulka patří modulu o velikosti ~39 kB (ukazuje na `0x94BA`), ale v paměti
jsou jen **4 752 bajty**. Ovladač se tedy nahrál zkrácený a první příkaz,
jehož obsluha leží výš, skočí do nul.

### 9.3 Proč se to jeví jako „chyba Sound Blasteru"

Protože `BLASTER.SND` je ten zkrácený soubor. Odtud všechna dřívější
pozorování:

- `sb16_enabled = 0` chybu odstraní úplně a level pak skončí **v hlavním
  menu**, jak má.
- `adlib_enabled = 0` nepomůže — `ADLIB.SND` je jiný, malý soubor.
- Chyba přichází „hned při inicializaci Sound Blasteru", protože tehdy hra
  poprvé zavolá příkaz, jehož obsluha ve zkráceném obrazu chybí.
- Nezáleží na verzi hry (uživatel zkusil původní i přepatchovanou).

### 9.4 Co zbývá

**Zjistit, proč je čtení souboru krátké.** Hra čte `BLASTER.SND` přes DOS,
tedy `INT 21h` → `INT 13h` → emulace IDE. Krátké čtení na téhle cestě je
vážná chyba, která se může týkat i jiného softwaru, jen se u něj neprojeví
tak nápadně.

Postup, který se nabízí: v čistém běhu nahodit hlídač zápisu na cílový buffer
(`0x67370`) a sledovat, kolik bajtů tam DOS doopravdy uloží, případně hlídat
cestu `io_read_string` / `INS` (značka `0x1451`), kterou disková PIO čtení
zapisují do `phys_mem` mimo `pstore`.

### 9.5 Změny v `sb16.c` z tohohle kola

Tři věci se opravily cestou. **Ani jedna hru nespravila**, ale všechny jsou
samostatně správné:

1. **Reset DSP nerušil naplánované přerušení.** `reset()` nuloval
   `mixer_regs[0x82]`, ale nechával `aux_pending`; přerušení pak přišlo se
   zahozeným stavovým bitem, potvrzení na `base+0x0e` je na tom bitu závislé,
   takže by se linka nesundala a další přerušení SB by už nepřišlo. Opraveno,
   a `sb16_poll()` teď stavový bit nastavuje spolu s linkou.
2. **DSP příkaz `0xE2` neposouval kanál DMA.** Provádí jeden cyklus DMA mimo
   obsluhu přenosu, takže si adresu i počet musí posunout sám; ovladače podle
   toho identifikují kanál. Přidána `i8257_dma_advance()`.
3. **8237 nebyl dekódovaný na svém aliasu `0x10-0x1F`.** Čip má čtyři adresní
   vodiče, takže na skutečném PC vybírá řadič DMA 1 každý port od `0x00` do
   `0x1F`. Ovladač Supaplexu čte počítadlo kanálu 1 na `0x13` — naměřeno
   **40 960 čtení** v okně selhání, než se to spravilo; po opravě je tam
   jediné čtení.

Vráceno bylo naopak zúžení bitu 7 na `base+0x0E` (hlásil „data k dispozici"
i když jen visel IRQ). Je to odchylka od hardwaru, ale nevalidovaná změna
chování SB by mohla rozbít jinou hru — leží v `src/sb16.c.readstatus-attempt`.

Do `sb16.c` přibylo ještě **odložení vyvolání IRQ na jádro 0**: `sb_set_irq()`
se volalo z `sb16_getsample()` na jádru 1, tedy měnilo IRR/ISR řadiče
přerušení, zatímco jádro 0 mohlo zrovna přerušení potvrzovat, a nic to
nezamykalo. Sound Blaster je jediné zařízení, které přerušuje z druhého jádra.
Chybu to nespravilo, ale je to skutečná závodní podmínka a stojí za to ji mít
zavřenou.
