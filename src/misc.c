#include "misc.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifdef RP2350_BUILD
// RP2350: time functions provided by platform_rp2350.c
#include "pico/stdlib.h"
#include "platform_rp2350.h"
#include "ff.h"  // FatFS for SD card access
#include "debug.h"
#ifndef EIO
#define EIO 5
#endif
#else
#include <time.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__wasm__) && !defined(RP2350_BUILD)
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#endif
#ifdef BUILD_ESP32
#include "driver/uart.h"
#endif

#if !defined(_WIN32) && !defined(__wasm__) && !defined(RP2350_BUILD)
static void CtrlC(int _)
{
	exit( 0 );
}

static void ResetKeyboardInput()
{
	// Re-enable echo, etc. on keyboard.
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag |= ICANON | ECHO;
	tcsetattr(0, TCSANOW, &term);
}

// Override keyboard, so we can capture all keyboard input for the VM.
void CaptureKeyboardInput()
{
	// Hook exit, because we want to re-enable keyboard.
#ifndef BUILD_ESP32
	atexit(ResetKeyboardInput);
	signal(SIGINT, CtrlC);
#endif

	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag &= ~(ICANON | ECHO | ISIG); // Disable echo as well
	tcsetattr(0, TCSANOW, &term);
}

static int ReadKBByte()
{
#ifdef BUILD_ESP32
	char data;
	if (uart_read_bytes(0, &data, 1, 20 / portTICK_PERIOD_MS) > 0) {
		return data;
	}
	return -1;
#else
	char rxchar = 0;
	int rread = read(fileno(stdin), (char*)&rxchar, 1);
	if( rread > 0 ) // Tricky: getchar can't be used with arrow keys.
		return rxchar;
	else
		abort();
#endif
}

static int IsKBHit()
{
#ifdef BUILD_ESP32
	size_t len;
	if (uart_get_buffered_data_len(0, &len) == ESP_OK) {
		if (len)
			return 1;
	}
	return 0;
#else
	int byteswaiting;
	ioctl(0, FIONREAD, &byteswaiting);
	return !!byteswaiting;
#endif
}
#endif

/* sysprog21/semu */
struct U8250 {
	uint8_t dll, dlh;
	uint8_t lcr;
	uint8_t ier;
	uint8_t mcr;
	uint8_t ioready;
	int out_fd;
	uint8_t in;

	int irq;
	void *pic;
	void (*set_irq)(void *pic, int irq, int level);
};

U8250 *u8250_init(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
	U8250 *s = malloc(sizeof(U8250));
	memset(s, 0, sizeof(U8250));
	s->out_fd = 1;

	s->irq = irq;
	s->pic = pic;
	s->set_irq = set_irq;
	return s;
}

struct CMOS {
	uint8_t data[128];
	int index;
	int irq;
	uint32_t irq_timeout;
	uint32_t irq_period;
	void *pic;
	void (*set_irq)(void *pic, int irq, int level);
};

static int bin2bcd(int a)
{
	return ((a / 10) << 4) | (a % 10);
}

static int month_from_str(const char *m)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) {
        if (strncmp(m, months + i * 3, 3) == 0)
            return i + 1;
    }
    return 1;
}

/* ---- виртуальный RTC ---------------------------------------------------
 * Раньше cmos_update_time() при КАЖДОМ чтении порта CMOS перезаписывал
 * регистры времени значениями __DATE__/__TIME__. Следствия: часы стояли
 * на месте, а INT 1Ah AH=03h/05h (то есть DOS TIME и DATE) молча
 * откатывались на ближайшем же чтении.
 *
 * Теперь храним привязку: момент time_us_64() и соответствующее ему
 * время. На чтении время вычисляется как привязка плюс прошедшие
 * секунды, на записи привязка пересчитывается - поэтому установка
 * времени держится, а часы идут.
 *
 * Календарь - алгоритм Хауарда Хиннанта; сверен с gmtime() на 50000
 * суток (1970-01-01 .. 2106) включая високосные годы и век 2100.
 */
static int bcd2bin(int a)
{
	return ((a >> 4) & 0x0F) * 10 + (a & 0x0F);
}

static int32_t rtc_days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
	y -= m <= 2;
	const int32_t era = (y >= 0 ? y : y - 399) / 400;
	const uint32_t yoe = (uint32_t)(y - era * 400);
	const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int32_t)doe - 719468;
}

static void rtc_civil_from_days(int32_t z, int32_t *py, uint32_t *pm, uint32_t *pd)
{
	z += 719468;
	const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
	const uint32_t doe = (uint32_t)(z - era * 146097);
	const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int32_t y = (int32_t)yoe + era * 400;
	const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const uint32_t mp = (5 * doy + 2) / 153;
	const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
	const uint32_t m = mp + (mp < 10 ? 3 : -9);
	*py = y + (m <= 2);
	*pm = m;
	*pd = d;
}

static uint32_t rtc_anchor_sec;   /* секунды с 1970-01-01 в момент привязки */
static uint64_t rtc_anchor_us;    /* time_us_64() в момент привязки        */
static uint32_t rtc_cached_sec;   /* для какой секунды уже заполнен data[] */
static int      rtc_cache_valid;

static uint32_t rtc_now_sec(void)
{
	uint64_t el = (time_us_64() - rtc_anchor_us) / 1000000u;
	return rtc_anchor_sec + (uint32_t)el;
}

static void rtc_fields_from_sec(CMOS *s, uint32_t secs)
{
	uint32_t days = secs / 86400u;
	uint32_t tod  = secs % 86400u;
	int32_t y; uint32_t m, d;
	rtc_civil_from_days((int32_t)days, &y, &m, &d);
	s->data[0x00] = bin2bcd((int)(tod % 60));
	s->data[0x02] = bin2bcd((int)((tod / 60) % 60));
	s->data[0x04] = bin2bcd((int)(tod / 3600));
	s->data[0x06] = bin2bcd((int)((days + 4) % 7) + 1); /* 1 = воскресенье */
	s->data[0x07] = bin2bcd((int)d);
	s->data[0x08] = bin2bcd((int)m);
	s->data[0x09] = bin2bcd((int)(y % 100));
	s->data[0x32] = bin2bcd((int)(y / 100));
}

/* Пересчитать привязку по текущему содержимому регистров времени.
   Вызывается после гостевой записи в любой из них. */
static void rtc_anchor_from_fields(CMOS *s)
{
	int sec  = bcd2bin(s->data[0x00]); if (sec  > 59) sec  = 59;
	int min  = bcd2bin(s->data[0x02]); if (min  > 59) min  = 59;
	int hour = bcd2bin(s->data[0x04]); if (hour > 23) hour = 23;
	int day  = bcd2bin(s->data[0x07]); if (day  < 1)  day  = 1;
	                                   if (day  > 31) day  = 31;
	int mon  = bcd2bin(s->data[0x08]); if (mon  < 1)  mon  = 1;
	                                   if (mon  > 12) mon  = 12;
	int yy   = bcd2bin(s->data[0x09]); if (yy   > 99) yy   = 99;
	int cc   = bcd2bin(s->data[0x32]); if (cc < 19 || cc > 20) cc = 20;
	int year = cc * 100 + yy;
	int32_t days = rtc_days_from_civil(year, (uint32_t)mon, (uint32_t)day);
	if (days < 0) days = 0;
	rtc_anchor_sec  = (uint32_t)days * 86400u +
	                  (uint32_t)(hour * 3600 + min * 60 + sec);
	rtc_anchor_us   = time_us_64();
	rtc_cache_valid = 0;
}

static void cmos_update_time(CMOS *s)
{
	/* REG_B бит 7 (SET) - гость просил заморозить часы на время записи. */
	if (s->data[11] & 0x80)
		return;
	uint32_t now = rtc_now_sec();
	if (rtc_cache_valid && now == rtc_cached_sec)
		return;
	rtc_fields_from_sec(s, now);
	rtc_cached_sec  = now;
	rtc_cache_valid = 1;
}

static int rtc_is_time_reg(int i)
{
	return i == 0x00 || i == 0x02 || i == 0x04 ||
	       i == 0x06 || i == 0x07 || i == 0x08 ||
	       i == 0x09 || i == 0x32;
}

/* Стартовая привязка: время сборки прошивки. Реального источника времени
   на плате нет (platform_set_time_offset() никто не вызывает), поэтому
   дата сборки - лучшее доступное приближение, и оно хотя бы правдоподобно
   для DOS. Дальше часы идут от неё и переустанавливаются гостем. */
static void cmos_time_init(CMOS *s)
{
	const char *d = __DATE__;
	int month = month_from_str(d);
	int day = (d[4] == ' ') ? (d[5] - '0') : (10 * (d[4] - '0') + (d[5] - '0'));
	int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 +
	           (d[9] - '0') * 10 + (d[10] - '0');
	const char *t = __TIME__;
	int hour = (t[0] - '0') * 10 + (t[1] - '0');
	int min  = (t[3] - '0') * 10 + (t[4] - '0');
	int sec  = (t[6] - '0') * 10 + (t[7] - '0');

	int32_t days = rtc_days_from_civil(year, (uint32_t)month, (uint32_t)day);
	if (days < 0) days = 0;
	rtc_anchor_sec  = (uint32_t)days * 86400u +
	                  (uint32_t)(hour * 3600 + min * 60 + sec);
	rtc_anchor_us   = time_us_64();
	rtc_cache_valid = 0;
	cmos_update_time(s);
}

void cmos_set_floppy_types(CMOS *c, uint8_t type_a, uint8_t type_b) {
    if (!c) return;
    c->data[0x10] = ((type_a & 0xF) << 4) | (type_b & 0xF);
}

CMOS *cmos_init(long mem_size, int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
	CMOS *c = malloc(sizeof(CMOS));
	memset(c, 0, sizeof(CMOS));
	c->irq = irq;
	c->pic = pic;
	c->set_irq = set_irq;

	cmos_time_init(c);
	c->data[0x10] = 0x44;  /* floppy: A=1.44M(4), B=1.44M(4) */
	c->data[10] = 0x26;
	c->data[11] = 0x02;
	c->data[12] = 0x00;
	c->data[13] = 0x80;
	/* Standard AT CMOS memory fields.
	 * 15h/16h: base memory in KiB (normally 640 KiB)
	 * 17h/18h: extended memory above 1 MiB in KiB, capped at 65535
	 * 30h/31h: mirror of 17h/18h
	 * 34h/35h: memory above 16 MiB in 64-KiB units
	 */
	uint32_t total_kb = mem_size > 0 ? (uint32_t)mem_size >> 10 : 0;
	uint32_t base_kb = total_kb < 640u ? total_kb : 640u;
	uint32_t ext_kb = total_kb > 1024u ? total_kb - 1024u : 0u;
	if (ext_kb > 0xffffu)
		ext_kb = 0xffffu;

	c->data[0x15] = (uint8_t)(base_kb & 0xff);
	c->data[0x16] = (uint8_t)(base_kb >> 8);
	c->data[0x17] = (uint8_t)(ext_kb & 0xff);
	c->data[0x18] = (uint8_t)(ext_kb >> 8);
	c->data[0x30] = c->data[0x17];
	c->data[0x31] = c->data[0x18];

	uint32_t above16_64k =
		mem_size > 16 * 1024 * 1024 ? ((uint32_t)mem_size - 16u * 1024u * 1024u) >> 16 : 0u;
	if (above16_64k > 0xffffu)
		above16_64k = 0xffffu;
	c->data[0x34] = (uint8_t)(above16_64k & 0xff);
	c->data[0x35] = (uint8_t)(above16_64k >> 8);
	return c;
}

static void u8250_update_interrupts(U8250 *uart)
{
	if (uart->ier & uart->ioready) {
		uart->set_irq(uart->pic, uart->irq, 1);
	} else {
		uart->set_irq(uart->pic, uart->irq, 0);
	}
}

uint8_t u8250_reg_read(U8250 *uart, int off)
{
	uint8_t val;
	switch (off) {
	case 0:
		if (uart->lcr & (1 << 7)) { /* DLAB */
			val = uart->dll;
			break;
		}
		val = uart->in;
		uart->ioready &= ~1;
		u8250_update_interrupts(uart);
		break;
	case 1:
		if (uart->lcr & (1 << 7)) { /* DLAB */
			val = uart->dlh;
			break;
		}
		val = uart->ier;
		break;
	case 2:
		val = (uart->ier & uart->ioready) ? 0 : 1;
		break;
	case 3:
		val = uart->lcr;
		break;
	case 4:
		val = uart->mcr;
		break;
	case 5:
		/* LSR = no error, TX done & ready */
		val = 0x60 | (uart->ioready & 1);
		break;
	case 6:
		/* MSR = carrier detect, no ring, data ready, clear to send. */
		val = 0xb0;
		break;
		/* no scratch register, so we should be detected as a plain 8250. */
	default:
		val = 0;
	}
	return val;
}

void u8250_reg_write(U8250 *uart, int off, uint8_t val)
{
	switch (off) {
	case 0:
		if (uart->lcr & (1 << 7)) {
			uart->dll = val;
			break;
		} else {
#if !defined(__wasm__) && !defined(RP2350_BUILD)
			ssize_t r;
			do {
				r = write(uart->out_fd, &val, 1);
			} while (r == -1 && errno == EINTR);
#elif defined(DEBUG_ENABLED)
			putchar(val);
#endif
		}
		break;
	case 1:
		if (uart->lcr & (1 << 7)) {
			uart->dlh = val;
			break;
		} else {
			uart->ier = val;
			if (uart->ier & 2)
				uart->ioready |= 2;
			else
				uart->ioready &= ~2;
			u8250_update_interrupts(uart);
		}
		break;
	case 3:
		uart->lcr = val;
		break;
	case 4:
		uart->mcr = val;
		break;
	}
}

void u8250_update(U8250 *uart)
{
#if !defined(_WIN32) && !defined(__wasm__) && !defined(RP2350_BUILD)
	if (IsKBHit()) {
		if (!(uart->ioready & 1)) {
			uart->in = ReadKBByte();
			uart->ioready |= 1;
			u8250_update_interrupts(uart);
		}
	}
#else
	(void)uart;  // Suppress unused parameter warning
#endif
}

#define CMOS_FREQ 32768
#define RTC_REG_A               10
#define RTC_REG_B               11
#define RTC_REG_C               12
#define RTC_REG_D               13
#define REG_A_UIP 0x80
#define REG_B_SET 0x80
#define REG_B_PIE 0x40
#define REG_B_AIE 0x20
#define REG_B_UIE 0x10

static uint32_t cmos_get_timer(CMOS *s)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * CMOS_FREQ +
		((uint64_t)ts.tv_nsec * CMOS_FREQ / 1000000000);
}

static void cmos_update_timer(CMOS *s)
{
	int period_code;

	period_code = s->data[RTC_REG_A] & 0x0f;
	if ((s->data[RTC_REG_B] & REG_B_PIE) &&
	    period_code != 0) {
		if (period_code <= 2)
			period_code += 7;
		s->irq_period = 1 << (period_code - 1);
		s->irq_timeout = (cmos_get_timer(s) + s->irq_period) &
			~(s->irq_period - 1);
	}
}

void __not_in_flash_func(cmos_update_irq)(CMOS *s)
{
	uint32_t d;
	if (s->data[RTC_REG_B] & REG_B_PIE) {
		d = cmos_get_timer(s) - s->irq_timeout;
		if ((int32_t)d >= 0) {
			/* this is not what the real RTC does. Here we sent the IRQ
			   immediately */
			s->data[RTC_REG_C] |= 0xc0;
			s->set_irq(s->pic, s->irq, 1);
			s->set_irq(s->pic, s->irq, 0);
			/* update for the next irq */
			s->irq_timeout += s->irq_period;
		}
	}
}

uint8_t cmos_ioport_read(CMOS *cmos, int addr)
{
	if (addr == 0x70)
		return 0xff;
	cmos_update_time(cmos);
	uint8_t val = cmos->data[cmos->index];
	return val;
}

void cmos_ioport_write(CMOS *cmos, int addr, uint8_t val)
{
	if (addr == 0x70)
		cmos->index = val & 0x7f;
	else {
		CMOS *s = cmos;
		switch(s->index) {
		case RTC_REG_A:
			s->data[RTC_REG_A] = (val & ~REG_A_UIP) |
				(s->data[RTC_REG_A] & REG_A_UIP);
			cmos_update_timer(s);
			break;
		case RTC_REG_B:
			s->data[s->index] = val;
			cmos_update_timer(s);
			break;
		default:
			if (rtc_is_time_reg(s->index)) {
				/* освежить остальные поля, чтобы привязка считалась
				   от актуального времени, а не от устаревшего */
				cmos_update_time(s);
				s->data[s->index] = val;
				rtc_anchor_from_fields(s);
			} else {
				s->data[s->index] = val;
			}
			break;
		}
	}
}

uint8_t cmos_set(void *cmos, int addr, uint8_t val)
{
	CMOS *s = cmos;
	if (addr < 128) {
		s->data[addr] = val;
	}
	return val;
}

uint8_t cmos_get(void* cmos, int addr)
{
	return (addr >= 0 && addr < 128) ? ((CMOS*)cmos)->data[addr] : 0;
}

/* Пересчитать CMOS checksum (0x10..0x2D) и записать в 0x2E/0x2F.
 * Вызывать после любых изменений CMOS, до старта BIOS. */
void cmos_update_checksum(void *cmos)
{
	CMOS *s = cmos;
	uint16_t sum = 0;
	for (int i = 0x10; i <= 0x2D; i++)
		sum += s->data[i];
	s->data[0x2E] = (sum >> 8) & 0xFF;
	s->data[0x2F] = sum & 0xFF;
}

/* EMULINK removed - disk operations use INT 13h disk handler instead */
