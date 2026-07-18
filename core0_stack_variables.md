# Переменные на стеке core0

Основание анализа: исходники, файлы GCC `-fstack-usage` (`.su`) и linker map из предоставленной Release-сборки.

`.su` содержит размер полного кадра функции, но не разбивку по переменным. Поэтому:

- точный размер указан для явно объявленных массивов и объектов с однозначно вычисляемым размером;
- `~` означает оценку по типу, конфигурации FatFs и/или полному кадру функции;
- мелкие объекты размером 1–8 байт исключены;
- статические и глобальные объекты не включены, поскольку они не находятся на стеке;
- функции, выполняемые только на core1 (`core1_entry` и его видеозвуковой цикл), исключены.

| Исходник | Функция | Переменная | Размер, байт |
|---|---|---:|---:|
| `src/main.c` | `load_rom` | `fp` (`FIL`) | ~592 |
| `src/main.c` | `init_emulator` | `fp` (`FIL`) | ~592 |
| `src/bios/bios_05h.c` | `bios_05h` | `fp` (`FIL`) | ~592 | 
| `src/config_save.c` | `config_save_all` | `fp` (`FIL`) | ~592 |
| `src/main.c` | `init_hardware` | `fp` (`FIL`) | ~592 |
| `src/ide.c` | `ide_attach` | `f` (`FIL`) | ~592 |
| `src/pc.c` | `emulink_data_read` | `buf` | 512 |
| `src/pc.c` | `emulink_data_write` | `buf` | 512 |
| `src/ide.c` | `ide_data_read_string` | `buf` | 512 |
| `src/ide.c` | `ide_data_write_string` | `buf` | 512 |
| `src/netredirect.c` | `redirector_handler_impl` | `file_info` (`FILINFO`) | ~284 |
| `src/netredirect.c` | `redirector_handler_impl` | `find_fileinfo` (`FILINFO`) | ~284 |
| `src/main.c` | `load_rom` | `path` | 256 |
| `src/main.c` | `init_emulator` | `bios_path` | 256 |
| `src/bios/bios_05h.c` | `bios_05h` | `line` | 256 |
| `src/netredirect.c` | `redirector_handler_impl` | `path` | 256 |
| `src/netredirect.c` | `redirector_handler_impl` | `guest_path` | 256 |
| `src/ini.c` | `ini_parse_stream` | `line` | 200 |
| `src/diskui.c` | `scan_disk_images` | `fno` (`FILINFO`) | ~284 |
| `src/main.c` | `init_hardware` | `dir` (`DIR`) | ~76 |
| `src/diskui.c` | `scan_disk_images` | `dir` (`DIR`) | ~76 |
| `src/config_save.c` | `config_save_all` | `line` | 80 |
| `src/main.c` | `init_emulator` | `detail` | 64 |
| `src/ini.c` | `ini_parse_stream` | `section` | 50 |
| `src/ini.c` | `ini_parse_stream` | `prev_name` | 50 |
| `src/fdos/initdisk.c` | `ProcessDisk` | `PTable` | ~48 |
| `src/fdos/fcom/fcom.c` | `builtin_copy` | `parse_storage` | ~36 |
| `src/main.c` | `init_hardware` | `detail` | 32 |
| `src/diskui.c` | `scan_disk_images` | `temp` | 32 |
| `src/ini.c` | `ini_parse_stream` | `abyss` | 16 |
| `src/ide.c` | `ide_attach` | `chsbuf` | 14 |
| `src/fdos/initdisk.c` | `ProcessDisk` | `driveParam` | ~12 |

## Наиболее тяжёлые кадры функций по `.su`

Это размер всего кадра, а не одной переменной:

| Функция | Кадр, байт |
|---|---:|
| `init_emulator` | 1272 |
| `bios_05h` | 920 |
| `load_rom` | 904 |
| `redirector_handler_impl` | 864 |
| `init_hardware` | 728 |
| `config_save_all` | 712 |
| `ide_attach` | 680 |
| `emulink_data_read` | 584 |
| `emulink_data_write` | 584 |
| `ide_data_read_string` | 576 |
| `ide_data_write_string` | 576 |
| `ProcessDisk` | 448 |
| `scan_disk_images` | 440 |
| `ini_parse_stream` | 392 |

