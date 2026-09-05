# Rozdělená obrazovka VGA — tři chyby v jedné cestě

**Datum:** 2026-09-03 · **Stav:** opraveno a ověřeno na hardwaru (Supaplex)
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz / PSRAM 166, výstup HDMI

Stavový pruh Supaplexu se drží dole přes **rozdělenou obrazovku** (CRTC Line
Compare). Chovaly se špatně tři různé věci a projevovaly se postupně, jak jsem
je odkrýval.

---

## 1. Porovnání v nesprávných jednotkách

```c
if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare)
```

`frame_line_compare` je v **zobrazovaných** řádcích, `src_line` ve
**zdrojových**. V režimu 320×200 má displej 400 řádků a každý zdrojový se
kreslí dvakrát, takže `src_line` dojde jen do 199 — a Supaplex žádá o zlom na
řádku **351**. Podmínka tedy nikdy neplatila a k rozdělení vůbec nedošlo.

Spodek se pak kreslil z rolujícího offsetu, ten po `& 0xFFFF` přetekl
(`0xA8BD + 199*122 = 0x10823` → `0x0823`) a náhodou spadl na obrázek panelu —
proto byl panel vidět, ale posunutý a useknutý. Že panel opravdu leží na
adrese 0, jsem ověřil vykreslením videopaměti od nuly.

**Oprava:** porovnávat v zobrazovaných řádcích a bod zlomu převést do
zdrojových **stejným pravidlem, jakým se odvozuje `src_line`** (`>>2`, `>>1`,
poměrem, nebo 1:1 podle výšky režimu) — viz `line_compare_src()`.

## 2. Panning se aplikoval i pod zlomem

Attribute Mode Control (**AR10, bit 5**) říká, že úspěšná shoda na Line Compare
vynuluje registr pixel panningu pro oblast pod zlomem. Právě proto spodní pruh
při rolování nešoupá. Renderer panning aplikoval na všechny řádky, takže se
pruh při pohybu doleva/doprava trhal.

Změřeno: Supaplex ten bit **nastavuje** (`frame_panning_split_off = 1`).

**Oprava:** bit se latchuje jednou za snímek spolu se zbytkem stavu a v oblasti
pod zlomem se panning nepoužije.

## 3. Oblast pod zlomem byla o řádek vyšší

Čítač adres se nuluje **až za** řádkem shody, takže první řádek pruhu je
`line_compare + 1`. Začínal o řádek dřív, což dalo 25 zdrojových řádků místo
24 — a ten přebývající četl za konec panelu do herních dlaždic za ním. Na
obrazovce to byl proužek pravidelných červeno-zelených bloků přes celou šířku
pod panelem.

**Oprava:** `(int)line > frame_line_compare` a bod zlomu se převádí z
`frame_line_compare + 1`.

---

## Kde to je

Renderery jsou **dva** a je potřeba opravit oba — deska jede přes HDMI, takže
oprava jen ve `vga_hw.c` se na obrazovce vůbec neprojeví (naletěl jsem na to):

- `drivers/hdmi/hdmi.c` — `render_gfx_line_ega320()` a `render_gfx_line_ega640()`
- `drivers/vga/vga_hw.c` — `render_gfx_line_ega()`

Nová globální proměnná `frame_panning_split_off` je definovaná ve `vga_hw.c`
vedle ostatního stavu snímku a `hdmi.c` ji bere přes `extern`, stejně jako
`frame_line_compare` a spol.

## Co jsem nechal být

**256barevná cesta** (`render_gfx_line_vga_planar256`) měla jednotky správně už
předtím a má u toho vysvětlující komentář. Chyby 2 a 3 v ní ale nejspíš jsou
taky — nesahal jsem na ni, protože ji nemám na čem otestovat a měnit naslepo
chování režimu, který dneska funguje, se mi nezdá. Až se najde hra, která
rozdělenou obrazovku v režimu 13h používá, patří to tam doplnit.

Stejně tak **CGA cesty** (`render_gfx_line_cga*`) mají chybu 1 doslova stejným
výrazem.

## Ověření

Supaplex, level 1, posuv doleva i doprava:

- pruh drží pozici na pixel (porovnány snímky z kamery při posuvu oběma směry),
- pod ním nejsou žádná rezidua,
- panel je celý a na svém místě.
