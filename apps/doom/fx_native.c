/*
 * Native-FDOS multi-voice Sound Blaster SFX backend.
 *
 * The original Apogee mixer depended on x86 IRQ handlers and DPMI.  Native
 * ARM code instead keeps one 8-bit mono auto-init DMA ring running through
 * the existing emulated 8237 + SB DSP and fills that ring cooperatively from
 * a 140-Hz TSM service.  No host IRQ callback enters client code.
 *
 * Eight DOOM voices are mixed in software.  Volume and priority are applied;
 * stereo left/right gains are folded to mono because this first continuous
 * DMA path is 8-bit mono.  Pitch setters are still accepted but resampling
 * pitch offsets are left for the next stage.
 */

#include <stdint.h>
#include <stddef.h>

#include <conio.h>
#include <dos_mem.h>
#include <stdlib.h>
#include <string.h>

#include "dmx.h"
#include "fx_man.h"

#define SB_DSP_RESET          0x06
#define SB_DSP_READ           0x0a
#define SB_DSP_WRITE          0x0c
#define SB_DSP_READ_STATUS    0x0e

#define SB_CMD_SET_TC         0x40
#define SB_CMD_SET_BLOCK      0x48
#define SB_CMD_DMA8_AUTO      0x1c
#define SB_CMD_HALT_DMA8      0xd0
#define SB_CMD_SPEAKER_ON     0xd1
#define SB_CMD_SPEAKER_OFF    0xd3
#define SB_CMD_EXIT_AUTO8     0xda

#define NATIVE_FX_VOICES      8
#define NATIVE_FX_MIX_RATE    11025u
#define NATIVE_FX_RING_SIZE   4096u
#define NATIVE_FX_RING_MASK   (NATIVE_FX_RING_SIZE - 1u)
#define NATIVE_FX_BLOCK_SIZE  128u
#define NATIVE_FX_LEAD        384u
#define NATIVE_FX_SERVICE_RATE 140

#if (NATIVE_FX_RING_SIZE & (NATIVE_FX_RING_SIZE - 1u)) != 0
#error NATIVE_FX_RING_SIZE must be a power of two
#endif

typedef struct
{
    uint16_t mask_port;
    uint16_t mode_port;
    uint16_t clear_port;
    uint16_t page_port;
    uint16_t addr_port;
    uint16_t count_port;
    uint8_t channel_select;
} native_dma8_ports_t;

typedef struct
{
    const unsigned char *data;
    uint32_t length;
    uint32_t pos_fp;
    uint32_t step_fp;
    uint32_t source_rate;
    int pitch_cents;
    int handle;
    int priority;
    int volume;
    int active;
} native_fx_voice_t;

static fx_blaster_config native_sb;
static native_fx_voice_t native_voices[NATIVE_FX_VOICES];
static unsigned char *native_dma_alloc;
static unsigned char *native_dma_buffer;
static uint32_t native_dma_linear;
static uint32_t native_mix_write_pos;
static uint32_t native_mix_rate = NATIVE_FX_MIX_RATE;
static int native_fx_volume = 255;
static int native_fx_next_handle = 1;
static int native_fx_service_id = -1;
static int native_dma_running;

static int native_dma8_ports(unsigned channel, native_dma8_ports_t *p)
{
    static const uint8_t page_ports[4] = { 0x87, 0x83, 0x81, 0x82 };
    static const uint8_t addr_ports[4] = { 0x00, 0x02, 0x04, 0x06 };
    static const uint8_t count_ports[4] = { 0x01, 0x03, 0x05, 0x07 };

    if (!p || channel > 3 || channel == 2)
        return -1;

    p->mask_port = 0x0a;
    p->mode_port = 0x0b;
    p->clear_port = 0x0c;
    p->page_port = page_ports[channel];
    p->addr_port = addr_ports[channel];
    p->count_port = count_ports[channel];
    p->channel_select = (uint8_t)channel;
    return 0;
}

static int native_sb_write(uint8_t value)
{
    uint16_t port = (uint16_t)native_sb.Address + SB_DSP_WRITE;
    unsigned i;

    for (i = 0; i < 0x10000u; ++i)
    {
        if ((inp(port) & 0x80u) == 0)
        {
            outp(port, value);
            return 0;
        }
    }
    return -1;
}

static int native_sb_reset(void)
{
    uint16_t base = (uint16_t)native_sb.Address;
    unsigned i;

    outp(base + SB_DSP_RESET, 1);
    for (volatile unsigned delay = 0; delay < 128u; ++delay)
        ;
    outp(base + SB_DSP_RESET, 0);

    for (i = 0; i < 0x10000u; ++i)
    {
        if ((inp(base + SB_DSP_READ_STATUS) & 0x80u) &&
            inp(base + SB_DSP_READ) == 0xaau)
            return 0;
    }
    return -1;
}

static int native_dma_buffer_ensure(void)
{
    uintptr_t address;
    uintptr_t aligned;

    if (native_dma_buffer)
        return 0;

    /*
     * ISA 8-bit DMA may not cross a 64-KiB boundary.  A 4-KiB ring needs at
     * most 4095 bytes of padding to move past the end of the current 64-KiB
     * window, so 8 KiB is sufficient.  The previous code always rounded up
     * to the next 64-KiB boundary and therefore allocated 128 KiB just to
     * obtain a 4-KiB ring.
     */
    native_dma_alloc = (unsigned char *)malloc(NATIVE_FX_RING_SIZE + 0x0fffu);
#if DMX_DIAG
    if (!native_dma_alloc) {
        DMX_Diag("SB DMA: allocation of %u bytes failed\n",
                 (unsigned)(NATIVE_FX_RING_SIZE + 0x0fffu));
        return -1;
    }
#endif
    address = (uintptr_t)native_dma_alloc;
    aligned = address;

    if ((aligned & 0xffffu) > (0x10000u - NATIVE_FX_RING_SIZE))
        aligned = (aligned + 0xffffu) & ~(uintptr_t)0xffffu;

    native_dma_buffer = (unsigned char *)aligned;
    native_dma_linear = dos_ptr_linear(native_dma_buffer);
    if (native_dma_linear == UINT32_MAX)
    {
#if DMX_DIAG
        DMX_Diag("SB DMA: dos_ptr_linear failed buffer=%p\n",
                 native_dma_buffer);
#endif
        free(native_dma_alloc);
        native_dma_alloc = NULL;
        native_dma_buffer = NULL;
        return -1;
    }

    memset(native_dma_buffer, 128, NATIVE_FX_RING_SIZE);
    return 0;
}

static int native_dma8_program_autoinit(void)
{
    native_dma8_ports_t p;
    uint32_t addr;
    uint16_t count;

    if (native_dma_buffer_ensure() != 0)
        return -1;
    if (native_dma8_ports((unsigned)native_sb.Dma8, &p) != 0) {
#if DMX_DIAG
        DMX_Diag("SB DMA: unsupported channel %d\n", native_sb.Dma8);
#endif
        return -1;
    }

    addr = native_dma_linear;
#if DMX_DIAG
    DMX_Diag("SB DMA: channel=%d linear=0x%06lx size=%u\n",
             native_sb.Dma8, (unsigned long)addr,
             (unsigned)NATIVE_FX_RING_SIZE);
#endif
    count = (uint16_t)(NATIVE_FX_RING_SIZE - 1u);

    outp(p.mask_port, 0x04u | p.channel_select);
    outp(p.clear_port, 0);

    /* Single-cycle direction=memory->device plus 8237 auto-initialize. */
    outp(p.mode_port, 0x58u | p.channel_select);

    outp(p.addr_port, (uint8_t)addr);
    outp(p.addr_port, (uint8_t)(addr >> 8));
    outp(p.page_port, (uint8_t)(addr >> 16));

    outp(p.count_port, (uint8_t)count);
    outp(p.count_port, (uint8_t)(count >> 8));
    outp(p.mask_port, p.channel_select);

    return 0;
}

static uint16_t native_dma8_count(void)
{
    native_dma8_ports_t p;
    uint16_t lo, hi;

    if (native_dma8_ports((unsigned)native_sb.Dma8, &p) != 0)
        return 0xffffu;

    outp(p.clear_port, 0);
    lo = inp(p.count_port);
    hi = inp(p.count_port);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t native_dma_read_pos(void)
{
    uint16_t count = native_dma8_count();

    if (count >= NATIVE_FX_RING_SIZE)
        return native_mix_write_pos & NATIVE_FX_RING_MASK;

    return (NATIVE_FX_RING_SIZE - 1u - (uint32_t)count)
         & NATIVE_FX_RING_MASK;
}

static native_fx_voice_t *native_find_voice(int handle)
{
    int i;

    for (i = 0; i < NATIVE_FX_VOICES; ++i)
        if (native_voices[i].active && native_voices[i].handle == handle)
            return &native_voices[i];

    return NULL;
}

static native_fx_voice_t *native_alloc_voice(int priority)
{
    native_fx_voice_t *victim = NULL;
    int i;

    for (i = 0; i < NATIVE_FX_VOICES; ++i)
    {
        native_fx_voice_t *v = &native_voices[i];
        if (!v->active)
            return v;
        if (!victim || v->priority < victim->priority)
            victim = v;
    }

    if (victim && priority >= victim->priority)
        return victim;

    return NULL;
}

/*
 * Convert DMX pitch offset (cents) to a 16.16 source-position increment.
 *
 * DOOM's restored DMX shim maps its 0..255 pitch byte to approximately
 * -2400..+2400 cents.  Avoid libm: use exact Q16 ratios at semitone
 * boundaries and linear interpolation for the remaining 0..99 cents.
 * Interpolation error over one semitone is small and is far below the
 * resolution of the original 8-bit SFX playback path.
 */
static uint32_t native_pitch_step(uint32_t source_rate, int cents)
{
    static const uint32_t semitone_q16[] =
    {
    16384u, 17358u, 18390u, 19484u, 20643u, 21870u, 23170u, 24548u,
    26008u, 27554u, 29193u, 30929u, 32768u, 34716u, 36781u, 38968u,
    41285u, 43740u, 46341u, 49097u, 52016u, 55109u, 58386u, 61858u,
    65536u, 69433u, 73562u, 77936u, 82570u, 87480u, 92682u, 98193u,
    104032u, 110218u, 116772u, 123715u, 131072u, 138866u, 147123u, 155872u,
    165140u, 174960u, 185364u, 196386u, 208064u, 220436u, 233544u, 247431u,
    262144u, 277732u
    };
    int semitone;
    int fraction;
    unsigned index;
    uint32_t r0, r1;
    uint32_t ratio;
    uint64_t step;

    if (cents < -2400)
        cents = -2400;
    else if (cents > 2400)
        cents = 2400;

    /* Floor division, so fraction is always 0..99 even for negative cents. */
    semitone = cents / 100;
    fraction = cents % 100;
    if (fraction < 0)
    {
        --semitone;
        fraction += 100;
    }

    index = (unsigned)(semitone + 24);
    r0 = semitone_q16[index];
    r1 = semitone_q16[index + 1u];
    ratio = r0 + (uint32_t)(((uint64_t)(r1 - r0)
                             * (unsigned)fraction) / 100u);

    step = (uint64_t)source_rate * ratio;
    step /= native_mix_rate;

    if (step == 0)
        step = 1;
    if (step > UINT32_MAX)
        step = UINT32_MAX;

    return (uint32_t)step;
}

static void native_voice_set_pitch(native_fx_voice_t *v, int cents)
{
    v->pitch_cents = cents;
    v->step_fp = native_pitch_step(v->source_rate, cents);
}

static unsigned char native_mix_one_sample(void)
{
    int mix = 0;
    int i;

    for (i = 0; i < NATIVE_FX_VOICES; ++i)
    {
        native_fx_voice_t *v = &native_voices[i];
        uint32_t pos;
        int sample;

        if (!v->active)
            continue;

        pos = v->pos_fp >> 16;
        if (pos >= v->length)
        {
            v->active = 0;
            continue;
        }

        sample = (int)v->data[pos] - 128;
        sample = sample * v->volume * native_fx_volume / (255 * 255);
        mix += sample;

        v->pos_fp += v->step_fp;
        if ((v->pos_fp >> 16) >= v->length)
            v->active = 0;
    }

    if (mix < -128)
        mix = -128;
    else if (mix > 127)
        mix = 127;

    return (unsigned char)(mix + 128);
}

static void native_mix_block(uint32_t start, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < length; ++i)
        native_dma_buffer[(start + i) & NATIVE_FX_RING_MASK] =
            native_mix_one_sample();
}

static int native_fx_service(void)
{
    uint32_t read_pos;
    uint32_t ahead;

    if (!native_dma_running || !native_dma_buffer)
        return 0;

    /* Reading the 8-bit status register acknowledges any SB block IRQ. */
    (void)inp((uint16_t)native_sb.Address + SB_DSP_READ_STATUS);

    read_pos = native_dma_read_pos();
    ahead = (native_mix_write_pos - read_pos) & NATIVE_FX_RING_MASK;

    while (ahead < NATIVE_FX_LEAD)
    {
        native_mix_block(native_mix_write_pos, NATIVE_FX_BLOCK_SIZE);
        native_mix_write_pos =
            (native_mix_write_pos + NATIVE_FX_BLOCK_SIZE)
            & NATIVE_FX_RING_MASK;
        ahead += NATIVE_FX_BLOCK_SIZE;
    }

    return 0;
}

static int native_fx_start_dma(void)
{
    unsigned tc;
    uint16_t block = (uint16_t)(NATIVE_FX_BLOCK_SIZE - 1u);

    if (native_dma8_program_autoinit() != 0) {
#if DMX_DIAG
        DMX_Diag("SB start: DMA programming failed\n");
#endif
        return -1;
    }

    tc = 256u - (1000000u / native_mix_rate);
    if (tc > 255u)
        tc = 255u;

    if (native_sb_write(SB_CMD_SET_TC) != 0 ||
        native_sb_write((uint8_t)tc) != 0 ||
        native_sb_write(SB_CMD_SET_BLOCK) != 0 ||
        native_sb_write((uint8_t)block) != 0 ||
        native_sb_write((uint8_t)(block >> 8)) != 0 ||
        native_sb_write(SB_CMD_SPEAKER_ON) != 0 ||
        native_sb_write(SB_CMD_DMA8_AUTO) != 0) {
#if DMX_DIAG
        DMX_Diag("SB start: DSP command sequence failed tc=%u block=%u\n",
                 tc, (unsigned)block + 1u);
#endif
        return -1;
    }

    native_mix_write_pos = NATIVE_FX_LEAD;
    memset(native_dma_buffer, 128, NATIVE_FX_RING_SIZE);
    native_dma_running = 1;
    return 0;
}

int FX_SetupSoundBlaster(fx_blaster_config blaster,
                         int *MaxVoices, int *MaxSampleBits, int *MaxChannels)
{
    native_sb = blaster;

    if (MaxVoices) *MaxVoices = NATIVE_FX_VOICES;
    if (MaxSampleBits) *MaxSampleBits = 8;
    if (MaxChannels) *MaxChannels = 1;

    {
        int rc = native_sb_reset();
#if DMX_DIAG
        DMX_Diag("SB setup: port=0x%x irq=%d dma=%d reset=%s\n",
                 native_sb.Address, native_sb.Interrupt, native_sb.Dma8,
                 rc == 0 ? "ok" : "FAIL");
#endif
        return rc == 0 ? FX_Ok : FX_BlasterError;
    }
}

int FX_Init(int SoundCard, int numvoices, int numchannels,
            int samplebits, unsigned mixrate)
{
    (void)numvoices;
    (void)numchannels;
    (void)samplebits;

    if (SoundCard != SoundBlaster)
        return FX_InvalidCard;

    if (mixrate >= 4000u && mixrate <= 44100u)
        native_mix_rate = mixrate;
    else
        native_mix_rate = NATIVE_FX_MIX_RATE;

    memset(native_voices, 0, sizeof(native_voices));

    DMX_Diag("SB FX init: card=%d rate=%u voices=%d\n",
             SoundCard, native_mix_rate, NATIVE_FX_VOICES);

    if (native_sb_reset() != 0) {
        DMX_Diag("SB FX init: DSP reset FAIL\n");
        return FX_BlasterError;
    }
    DMX_Diag("SB FX init: DSP reset ok\n");

    if (native_fx_start_dma() != 0)
        return FX_BlasterError;
    DMX_Diag("SB FX init: DMA/DSP playback started\n");

    if (native_fx_service_id < 0)
        native_fx_service_id = TSM_NewService(native_fx_service,
                                               NATIVE_FX_SERVICE_RATE,
                                               1, 0);
    if (native_fx_service_id < 0) {
        DMX_Diag("SB FX init: TSM service registration FAIL\n");
        return FX_Error;
    }

    DMX_Diag("SB FX init: service id=%d ok\n", native_fx_service_id);
    return FX_Ok;
}

int FX_Shutdown(void)
{
    native_dma8_ports_t p;

    memset(native_voices, 0, sizeof(native_voices));

    if (native_fx_service_id >= 0)
    {
        TSM_DelService(native_fx_service_id);
        native_fx_service_id = -1;
    }

    if (native_dma_running)
    {
        native_sb_write(SB_CMD_EXIT_AUTO8);
        native_sb_write(SB_CMD_HALT_DMA8);
    }
    native_sb_write(SB_CMD_SPEAKER_OFF);

    if (native_dma8_ports((unsigned)native_sb.Dma8, &p) == 0)
        outp(p.mask_port, 0x04u | p.channel_select);

    native_dma_running = 0;

    if (native_dma_alloc)
        free(native_dma_alloc);
    native_dma_alloc = NULL;
    native_dma_buffer = NULL;
    native_dma_linear = 0;
    return FX_Ok;
}

void FX_SetVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    native_fx_volume = volume;
}

int FX_GetVolume(void)
{
    return native_fx_volume;
}

int FX_PlayRaw(char *ptr, unsigned long length, unsigned rate,
               int pitchoffset, int vol, int left, int right,
               int priority, unsigned long callbackval)
{
    native_fx_voice_t *v;
    unsigned gain;

    (void)callbackval;

    if (!ptr || !length || rate < 1000u || rate > 44100u)
        return FX_Error;

    TSM_Lock();
    v = native_alloc_voice(priority);
    if (!v)
    {
        TSM_Unlock();
        return FX_Error;
    }

    gain = ((unsigned)(left < 0 ? 0 : left)
          + (unsigned)(right < 0 ? 0 : right)) / 2u;
    if (gain > 255u)
        gain = 255u;

    v->data = (const unsigned char *)ptr;
    v->length = (uint32_t)length;
    v->pos_fp = 0;
    v->source_rate = rate;
    native_voice_set_pitch(v, pitchoffset);
    v->priority = priority;
    v->volume = (int)(((unsigned)(vol < 0 ? 0 : vol) * gain) / 255u);
    if (v->volume > 255)
        v->volume = 255;

    ++native_fx_next_handle;
    if (native_fx_next_handle <= 0)
        native_fx_next_handle = 1;
    v->handle = native_fx_next_handle;
    v->active = 1;

    native_fx_service();
    {
        int handle = v->handle;
        TSM_Unlock();
        return handle;
    }
}

int FX_StopSound(int handle)
{
    native_fx_voice_t *v;

    TSM_Lock();
    v = native_find_voice(handle);
    if (!v)
    {
        TSM_Unlock();
        return FX_Error;
    }

    v->active = 0;
    TSM_Unlock();
    return FX_Ok;
}

int FX_StopAllSounds(void)
{
    memset(native_voices, 0, sizeof(native_voices));
    return FX_Ok;
}

int FX_SoundActive(int handle)
{
    return native_find_voice(handle) != NULL;
}

int FX_SoundsPlaying(void)
{
    int i, count = 0;

    for (i = 0; i < NATIVE_FX_VOICES; ++i)
        if (native_voices[i].active)
            ++count;
    return count;
}

int FX_SetPan(int handle, int vol, int left, int right)
{
    native_fx_voice_t *v = native_find_voice(handle);
    unsigned gain;

    if (!v)
        return FX_Error;

    gain = ((unsigned)(left < 0 ? 0 : left)
          + (unsigned)(right < 0 ? 0 : right)) / 2u;
    if (gain > 255u)
        gain = 255u;
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    v->volume = (int)((unsigned)vol * gain / 255u);
    return FX_Ok;
}

int FX_SetPitch(int handle, int pitchoffset)
{
    native_fx_voice_t *v = native_find_voice(handle);

    if (!v)
        return FX_Error;

    native_voice_set_pitch(v, pitchoffset);
    return FX_Ok;
}

int FX_SetFrequency(int handle, int frequency)
{
    native_fx_voice_t *v = native_find_voice(handle);

    if (!v || frequency <= 0)
        return FX_Error;

    v->source_rate = (uint32_t)frequency;
    native_voice_set_pitch(v, v->pitch_cents);
    return FX_Ok;
}

int FX_VoiceAvailable(int priority)
{
    return native_alloc_voice(priority) != NULL;
}

int FX_EndLooping(int handle)
{
    return native_find_voice(handle) ? FX_Ok : FX_Error;
}

int FX_SetupCard(int SoundCard, fx_device *device)
{
    if (SoundCard != SoundBlaster)
        return FX_InvalidCard;
    if (device)
    {
        device->MaxVoices = NATIVE_FX_VOICES;
        device->MaxSampleBits = 8;
        device->MaxChannels = 1;
    }
    return FX_Ok;
}

char *FX_ErrorString(int ErrorNumber)
{
    (void)ErrorNumber;
    return "native FDOS multi-voice Sound Blaster backend";
}
