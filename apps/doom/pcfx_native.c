/*
 * Native-FDOS PC speaker backend for the restored DMX PCFX interface.
 *
 * The original Apogee PCFX driver used a hardware timer ISR.  Native ARM
 * applications use the DMX-compatible asynchronous TSM scheduler on core0.
 * PCFX therefore needs no separate host timer and no DPMI code.
 *
 * DMX passes PC speaker data as a byte stream containing little-endian
 * 16-bit PIT channel-2 divisors, one divisor per 140-Hz service tick.
 */

#include <stdint.h>

#include <conio.h>

#include "dmx.h"
#include "pcfx.h"

#define PCFX_SERVICE_RATE 140
#define PCFX_HANDLE       PCFX_MinVoiceHandle

static const unsigned char *pcfx_data;
static unsigned long pcfx_length;
static unsigned long pcfx_pos;
static int pcfx_priority;
static int pcfx_active;
static int pcfx_service_id = -1;
static int pcfx_total_volume = PCFX_MaxVolume;
static void (*pcfx_callback)(unsigned long);
static unsigned long pcfx_callback_value;

static void pcfx_speaker_off(void)
{
    outp(0x61, (uint8_t)(inp(0x61) & ~3u));
}

static void pcfx_set_divisor(uint16_t divisor)
{
    uint8_t gate;

    if (divisor == 0)
    {
        pcfx_speaker_off();
        return;
    }

    /*
     * PIT channel 2, lobyte/hibyte, square-wave generator, binary counter.
     * This is the standard PC speaker programming sequence and therefore goes
     * through the existing guest-visible PIT/port emulation.
     */
    outp(0x43, 0xb6);
    outp(0x42, (uint8_t)divisor);
    outp(0x42, (uint8_t)(divisor >> 8));

    gate = inp(0x61);
    outp(0x61, (uint8_t)(gate | 3u));
}

static void pcfx_finish(void)
{
    pcfx_speaker_off();
    pcfx_active = 0;
    pcfx_data = 0;
    pcfx_length = 0;
    pcfx_pos = 0;

    if (pcfx_callback)
        pcfx_callback(pcfx_callback_value);
}

static int pcfx_service(void)
{
    uint16_t divisor;

    if (!pcfx_active)
        return 0;

    if (pcfx_pos + 1 >= pcfx_length)
    {
        pcfx_finish();
        return 0;
    }

    divisor = (uint16_t)pcfx_data[pcfx_pos]
            | ((uint16_t)pcfx_data[pcfx_pos + 1] << 8);
    pcfx_pos += 2;

    pcfx_set_divisor(divisor);

    if (pcfx_pos >= pcfx_length)
        pcfx_finish();

    return 0;
}

char *PCFX_ErrorString(int ErrorNumber)
{
    (void)ErrorNumber;
    return "native FDOS PC speaker backend";
}

int PCFX_Stop(int handle)
{
    TSM_Lock();
    if (handle != PCFX_HANDLE || !pcfx_active)
    {
        TSM_Unlock();
        return PCFX_VoiceNotFound;
    }

    pcfx_finish();
    TSM_Unlock();
    return PCFX_Ok;
}

void PCFX_UseLookup(int use, unsigned value)
{
    /*
     * The restored DMX layer already converted note numbers to PIT divisors
     * before calling PCFX_Play(), so the old optional PCFX lookup table is not
     * used by this backend.
     */
    (void)use;
    (void)value;
}

int PCFX_VoiceAvailable(int priority)
{
    return !pcfx_active || priority >= pcfx_priority;
}

int PCFX_Play(PCSound *sound, int priority, unsigned long callbackval)
{
    if (!sound || sound->length < 2)
        return PCFX_Error;

    TSM_Lock();
    if (pcfx_active)
    {
        if (priority < pcfx_priority)
        {
            TSM_Unlock();
            return PCFX_NoVoices;
        }
        pcfx_finish();
    }

    pcfx_data = (const unsigned char *)sound->data;
    pcfx_length = sound->length;
    pcfx_pos = 0;
    pcfx_priority = priority;
    pcfx_callback_value = callbackval;
    pcfx_active = 1;

    /*
     * Do not wait for the first 140-Hz deadline before producing sound.
     * Program the first divisor immediately, then let asynchronous TSM advance
     * the remaining samples.
     */
    pcfx_service();

    TSM_Unlock();
    return PCFX_HANDLE;
}

int PCFX_SoundPlaying(int handle)
{
    return handle == PCFX_HANDLE && pcfx_active;
}

int PCFX_SetTotalVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > PCFX_MaxVolume)
        volume = PCFX_MaxVolume;

    /*
     * The classic one-bit PC speaker has no amplitude control.  Keep the
     * logical DMX volume value for API compatibility.
     */
    pcfx_total_volume = volume;
    return PCFX_Ok;
}

int PCFX_GetTotalVolume(void)
{
    return pcfx_total_volume;
}

void PCFX_SetCallBack(void (*function)(unsigned long))
{
    pcfx_callback = function;
}

int PCFX_Init(void)
{
    pcfx_speaker_off();
    pcfx_active = 0;
    pcfx_data = 0;
    pcfx_length = 0;
    pcfx_pos = 0;
    pcfx_priority = 0;
    pcfx_callback_value = 0;

    if (pcfx_service_id < 0)
        pcfx_service_id = TSM_NewService(pcfx_service,
                                         PCFX_SERVICE_RATE, 1, 0);

    return pcfx_service_id >= 0 ? PCFX_Ok : PCFX_Error;
}

int PCFX_Shutdown(void)
{
    if (pcfx_active)
        pcfx_finish();
    else
        pcfx_speaker_off();

    if (pcfx_service_id >= 0)
    {
        TSM_DelService(pcfx_service_id);
        pcfx_service_id = -1;
    }

    return PCFX_Ok;
}

void PCFX_UnlockMemory(void)
{
}

int PCFX_LockMemory(void)
{
    return PCFX_Ok;
}
