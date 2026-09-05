# Textový režim: rozdělená obrazovka, krok CRTC a předvolba řádku

**Datum:** 2026-09-03 · **Stav:** opraveno a ověřeno na hardwaru (Prehistorik 2, intro)
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz / PSRAM 166, výstup HDMI

Úvodní animace Prehistorika 2 ukazovala na desce **jen běžící text nahoře** a
zbytek obrazovky byl černý — chybělo logo HYBRID, obě tečkované vlnovky
i „PRESENTS / PREHISTORIK 2 (C) TITUS".

---

## 1. Co to intro vlastně je

Není to grafický režim. Je to **textový režim 80×25 s předefinovaným fontem**:
logo i vlnovky jsou nakreslené znaky, kterým hra přepsala tvary, a v paměti to
poznáte podle souvislé řady kódů znaků jdoucích za sebou (`a0 a0 a1 a1 a2 a3…`).
`gfx_submode` přitom hlásil 3 (mode 13h) — je to jen zbytek z předchozího
režimu, rozhoduje `current_mode == 1`.

Změřený stav CRTC v tu chvíli:

| registr | hodnota | význam |
|---|---|---|
| Line Compare (CR18/07/09) | **129** | rozdělená obrazovka |
| CR0C/0D start | 0x0616 | posouvá se, scroller |
| CR13 offset | **41** → 82 buněk/řádek | virtuální šířka pro plynulý posuv |
| CR08 preset row scan | **5–7** | svislý posuv o část znaku |
| CR09 max scan | 15 → výška znaku 16 | |
| AR10 | 0x24 | bit 5: pod zlomem se panning neuplatní |
| AR13 | 4 | vodorovný posuv o pixely |

Celé intro tedy stojí na rozdělené obrazovce: nahoře plynule rolující vzkaz,
pod zlomem logo od adresy 0.

## 2. Tři chyby v `render_text_line()`

1. **Rozdělenou obrazovku textový renderer vůbec neuměl.** Vždycky četl od
   `frame_vram_offset`, takže všechno pod zlomem četlo za koncem dat horní
   oblasti — tedy prázdno. To je celá ta černá plocha.
2. **Krok mezi řádky byl natvrdo 80 buněk.** `text_stride_cells` se
   aktualizovalo jen na hraně vertikálního zatemnění ve `pre_render_line()`,
   kterou cesta přes HDMI nikdy neuvidí; zůstávalo tam 80, zatímco CRTC říkalo
   82. Každý další řádek se tak četl o dvě buňky dřív.
3. **Výška znaku byla natvrdo 16** (`line >> 4`) a **předvolba řádku (CR08) se
   ignorovala**, takže horní oblast seděla svisle o několik pixelů jinde.

## 3. Oprava

Do snímkového latche (v obou rendererech, ale rozhoduje `drivers/hdmi/hdmi.c`)
přibylo `frame_preset_row`, `frame_text_char_h` a hlavně **`text_cols` /
`text_stride_cells` z `CR01` a `CR13`** — tam, kde se už spolehlivě latchuje
zbytek stavu snímku.

`render_text_line()` teď počítá:

```c
uint32_t ch_h      = frame_text_char_h ? frame_text_char_h : 16u;
uint32_t base_cell = frame_vram_offset;
uint32_t v         = line + frame_preset_row;
if (frame_line_compare >= 0 && (int)line > frame_line_compare) {
    base_cell = 0;                                   /* adresa se nuluje */
    v = line - (uint32_t)(frame_line_compare + 1);   /* i čítač řádku znaku */
}
uint32_t char_row   = v / ch_h;
uint32_t glyph_line = v % ch_h;
```

Zlom je `> line_compare`, ne `>=` — čítač se nuluje **až za** řádkem shody,
stejně jako u grafické cesty.

Pevná mez `char_row < 25` je nahrazená kontrolou, že se řádek vejde do
65 536 buněk `gfx_buffer`.

## 4. Co zatím zůstává nedodělané

**Pixel panning (AR13) se v textovém režimu neuplatňuje.** Scroller se proto
posouvá po celých znacích (8 pixelů) místo po pixelu. Obraz je kompletní a
správný, jen ten pohyb je hrubší. Doplnit to znamená posouvat celý řádek o
0–7 pixelů, což je v časově kritickém rendereru netriviální — a jednou už jsem
si termín scanline rozbil (per-word wrap v `hdmi.c`), takže to chce měřit.

## 5. Ověření

Kamera na monitor po opravě: logo HYBRID, obě vlnovky, „PRESENTS" i
„PREHISTORIK 2 (C) TITUS" jsou na svých místech, scroller běží nahoře.
Latchované hodnoty přečtené přes SWD: `text_stride_cells = 82`,
`char_h = 16`, `line_compare = 129`, `preset_row = 5`.

## 6. Mimochodem

Po flashi se deska jednou zasekla ještě před startem emulátoru — `vga_state`
zůstalo NULL a jádro stálo v `clock_configure_internal()`
(`runtime_init_clocks`). `POWMAN_CHIP_RESET` = `HAD_RUN_LOW`, CFSR = 0, tedy
žádná výjimka; byl to zásek přepínání hodin po resetu přes RUN. Obyčejný
`reset run` to spravil. **Nezaměňovat s chybou v tom, co se zrovna nahrálo.**

---

## 7. Paleta textového režimu (druhá oprava téhož večera)

Po zprovoznění rozdělené obrazovky bylo intro celé vidět, ale **v jiných
barvách**: logo „" magentové místo žlutého, vlnovky zlaté místo
bílých, a scroller namíchaný jinak.

Příčina: textový režim si na HDMI bral 16 barev z **natvrdo zapsané CGA
tabulky**. Při přepnutí do textového režimu se dokonce ta tabulka aktivně
obnovovala (`required_to_repair_text_pal` → `memcpy(conv_color, conv_color2, …)`),
takže paletu nahranou hostem nebylo možné vidět vůbec.

Změřeno v DAC hosta v tu chvíli (6bitové složky):

```
6: (63,63,21) žlutá      7: (63,63,63) bílá
9..14: 9,18,28,38,48,57 šedá rampa   (probíhá prolínačka)
```

Grafické režimy EGA to dělaly správně už předtím — `vga_get_palette16()`
(respektuje paletu attribute controlleru i AR14) → `vga_hw_set_palette16()`.
Textový režim teď dělá totéž, jen přes vlastní `vga_hw_set_text_palette()`:
textový režim totiž balí **dva 4bitové pixely do bajtu**, takže potřebuje
párovou 256položkovou tabulku, kterou `vga_hw_set_palette16()` staví jen pro
submode 2.

`required_to_repair_text_pal` teď znamená „přestav paletu", ne „obnov CGA".

**Cena:** přestavba je 256 zakódování TMDS, `new_frame_max_us` vyskočilo na
**1030 µs**. Během prolínačky se to děje každý snímek. Je to stejná cena, jakou
už platí režimy EGA16, a běží to v odložené části mimo obsluhu řádku, ale je to
kandidát na optimalizaci (stačilo by přestavovat jen řádek a sloupec tabulky
patřící ke změněným indexům).

## 8. Poznámka k `HDMI_ADDR_LOG`

Přepínač `-DHDMI_ADDR_LOG=ON` **nikdy nefungoval**. `hdmi.c` se překládá jednou
do sdílené knihovny `video_driver`, kdežto `target_compile_definitions()` v
`CMakeLists.txt` přidává `HDMI_ADDR_LOG_ENABLED=1` k **spustitelným cílům**,
takže se do `hdmi.c` nedostane. Ověřeno dvakrát: v binárce není konstanta
`0x110a2000` ani jednou a značka zapsaná přes SWD na tu adresu přežila.
Kdo to bude chtít použít, musí definici přidat knihovně `video_driver`.
