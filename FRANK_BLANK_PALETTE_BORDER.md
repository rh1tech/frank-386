# Růžové orámování při přechodu z MODE BEGINNER (Prehistorik 2)

**Datum:** 2026-09-03 · **Stav:** příčina prokázaná, oprava nahraná, **čeká na potvrzení uživatelem**
**Deska:** FRANK 386 (Z2 / RP2350B), 504 MHz / PSRAM 166, HDMI

---

## 1. Hlášení

Na obrazovce MODE BEGINNER byly modré pruhy nahoře, dole a po straně — opraveno
dřív (letterbox se maluje `frame_border_pix` místo palety indexu 0).

Zbylo: **při překlopení z MODE BEGINNER do hry problikne asi na vteřinu
narůžovělé orámování**, nahoře, dole i vlevo. Uživatel navíc upřesnil, že tatáž
barva krátce naskočí i **jako obrysová barva textury**, než se promítne modrá —
což byla rozhodující nápověda, protože to znamená, že nejde o okraj, ale o
**celou paletu**.

## 2. Co se vyloučilo (a čím)

Vše přes SWD za běhu, bez haltování.

| hypotéza | důkaz | výsledek |
|---|---|---|
| okraj ukazuje na růžovou položku | 52 111 vzorků `frame_border_pix` + nová diagnostika `frame_border_rgb` | **ne**, `0,0,0` ve všech |
| okraj přestane být černý jen na okamžik | spouštěč nastražený přímo na „`frame_border_rgb != 0`" | **nesepnul ani jednou za 120 s** |
| rozbitá geometrie (letterbox se kreslí z VRAM) | `active_start`/`active_end`/`gfx_width`/`gfx_height` | **ne**, konstantně `40..440` |
| paleta se na výstup promítá pozdě | časová osa DAC hosta vs. `conv_color` na 624 Hz | **ne**, medián **2 ms**, nikdy hůř než ~34 ms |

Pozn.: první výpočet dal „p90 = 1317 ms", ale to byl artefakt — měřil mezeru do
další změny tabulky i tam, kde se mezitím neměnil ani DAC. Časová osa to
vyvrací: fade 11 058 → 11 643 ms mění paletu po ~82 ms a tabulka jde vždy
1–2 ms za ním.

## 3. Co to je

Snímek obrazové paměti hosta vytažený přesně v tom okamžiku (`vram13.bin` +
`dac13.bin`, dekódováno jako EGA planární 320×200) ukazuje **obrazovku MODE
BEGINNER nakreslenou v lososově růžové paletě levelu** — staré pixely ještě leží
ve VRAM, paleta už je nová:

```
#12  black
#13  MODE BEGINNER v paletě 1=(56,40,32) 2=(40,24,16) 3=(32,16,8)   <-- ta růžová
#15+ kreslí se level
```

A v témž okamžiku host **shodí PAS bit attribute controlleru** (`ar_index & 0x20`),
tedy si zatemní obraz — naměřeno okno **22 637 → 22 957 ms, 320 ms**.

Skutečná VGA v tu chvíli nezobrazuje nic. My jsme zatemnění ignorovali:
`vga_hw_set_mode()` mode 0 zahazuje (jinak by obraz zůstal černý až do rebootu),
takže se dál scanoutovalo — a bylo vidět přesně to, co by reálný stroj schoval.

## 4. Oprava

`drivers/vga/vga_hw.c` (zálohy `.before-blank-palette`):

1. **Zatemnění se respektuje ve scanoutu.** V ISR na hranici snímku se počítá
   běh snímků s `vga_get_mode() == 0` a nastavuje `frame_blank_active`.
   `render_line()` v `drivers/hdmi/hdmi.c` pak celou viditelnou oblast vyplní
   `frame_border_pix` místo obrazu.
2. **Zámek proti trvalému zčernání:** `BLANK_MAX_FRAMES = 120` (~2 s). Po jeho
   překročení se zatemnění přestane respektovat. To je celý důvod, proč se
   mode 0 kdysi zahazoval — host, který nechá PAS shozený, nesmí desku
   zčernat až do rebootu.
3. Režim samotný zatemnění přežije beze změny (`vga_hw_set_mode` mode 0 dál
   ignoruje), takže se po jeho konci nic nedohání.

Dřívější změny z téže session, které **problém neřešily**, ale jsou správné a
zůstávají:

- `vga_hw_new_frame_deferred()` udržuje paletu i během zatemnění (jinak by
  tabulka zamrzla na odcházející paletě).
- `vga_border_index()` preferuje položku, která je přesně `(0,0,0)`, před pouhou
  nejtmavší.
- `frame_border_rgb[3]` — barva, kterou se okraj naposledy zakódoval. Bez ní by
  se tahle diagnóza nedala uzavřít; nechat tam.

## 5. Jak to ověřit

Sekvence: `cd \hry\prehist2`, `pre2`, ~50 s, **ENTER, ENTER**, **1**,
**ENTER, ENTER**. Sledovat monitor.

Užitečné adresy (mění se s buildem, tahat přes `nm`):
`frame_border_pix`, `frame_border_rgb`, `frame_blank_active`, `current_mode`,
`gfx_submode`, `active_start`, `active_end`, `vga_state`, `conv_color`,
`gfx_buffer`.

Osvědčené postupy z téhle session:

- **Vzorkovat TCL smyčkou v OpenOCD**, ne opakovaným spouštěním. Dosažitelných
  je **500–620 Hz** i se čtením 768bajtového DAC; deska se nehaltí.
- **Spouštěč + `dump_image`**: nechat skript čekat na podmínku a v ten okamžik
  vysypat `gfx_buffer` (64 KB) a DAC (768 B). Tím se dá obrazovka hosta
  zrekonstruovat přesně, což kamera neumí.
- **EGA planární dekód** (submode 6): `gfx_buffer[addr*4 + plane]`,
  `addr = y*40 + x/8`, bit `7-(x&7)`, index = bity čtyř plane. Lineární dekód
  platí jen pro submode 3 (mode 13h, chain-4).
- Kamera: `ffmpeg -f dshow -i video="eMeet Nova"`; na hledání krátkého jevu
  nahrát video a projet ho numpy skriptem, ne prohlížet snímky.

## 6. Pozor při buildu

```bash
export PATH="/c/Users/janbr/.pico-sdk/cmake/v3.28.6/bin:\
/c/Users/janbr/.pico-sdk/ninja/v1.12.1:\
/c/Users/janbr/.pico-sdk/toolchain/13_2_Rel1/bin:$PATH"
./build.sh -Z2 -i2s -p 166 -504 --usb-hid
```

**`--usb-hid` je povinné.** `build.sh` ho má defaultně vypnutý a jméno binárky
ho nekóduje, takže build bez něj vypadá identicky — a po flashi deska přijde
o klávesnici uživatele. Stalo se to v této session.

## 7. Otevřené

- **Ověřit tuhle opravu na desce.**
- Pokud by 2 s zámek byl málo/moc, je to jediná konstanta k ladění
  (`BLANK_MAX_FRAMES`).
- `hdmi_isr_max_us` = **44 µs** při rozpočtu 32 µs na řádek a
  `hdmi_isr_over_30_count` = 872. ISR přetahuje; zatím to nic viditelného
  nezpůsobuje, ale je to nejbližší kandidát na problémy s časováním.
- `new_frame_max_us` = **1031 µs** (přestavba palety, 256 TMDS zakódování).
  Optimalizace: přestavovat jen řádek a sloupec párové tabulky patřící ke
  změněným indexům.
- Pixel panning (AR13) v textovém režimu se neuplatňuje — viz
  `FRANK_TEXT_SPLIT_SCREEN_FIX.md`.
- `-DHDMI_ADDR_LOG=ON` nefunguje — definice se přidává spustitelným cílům, ale
  `hdmi.c` se překládá do knihovny `video_driver`.
