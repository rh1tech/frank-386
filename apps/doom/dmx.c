//
// Copyright (C) 2015-2017 Alexey Khokholov (Nuke.YKT)
// Copyright (C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#include "dmx.h"
#include "sound_hw.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "fx_man.h"
#include "music.h"
#include "mus2mid.h"
#include "pcfx.h"

unsigned short divisors[] = {
    0,
    6818, 6628, 6449, 6279, 6087, 5906, 5736, 5575,
    5423, 5279, 5120, 4971, 4830, 4697, 4554, 4435,
    4307, 4186, 4058, 3950, 3836, 3728, 3615, 3519,
    3418, 3323, 3224, 3131, 3043, 2960, 2875, 2794,
    2711, 2633, 2560, 2485, 2415, 2348, 2281, 2213,
    2153, 2089, 2032, 1975, 1918, 1864, 1810, 1757,
    1709, 1659, 1612, 1565, 1521, 1478, 1435, 1395,
    1355, 1316, 1280, 1242, 1207, 1173, 1140, 1107,
    1075, 1045, 1015,  986,  959,  931,  905,  879,
     854,  829,  806,  783,  760,  739,  718,  697,
     677,  658,  640,  621,  604,  586,  570,  553,
     538,  522,  507,  493,  479,  465,  452,  439,
     427,  415,  403,  391,  380,  369,  359,  348,
     339,  329,  319,  310,  302,  293,  285,  276,
     269,  261,  253,  246,  239,  232,  226,  219,
     213,  207,  201,  195,  190,  184,  179,
};

typedef struct {
    unsigned int length;
    unsigned short priority;
    unsigned short data[];
} pcspkmuse_t;

typedef struct {
    unsigned short id;
    unsigned short length;
    unsigned char data[];
} dmxpcs_t;

static pcspkmuse_t *pcspkmuse;
int pcshandle = 0;

static void pcspkmuse_done(unsigned long callbackval)
{
    (void)callbackval;

    if (pcspkmuse)
    {
        free(pcspkmuse);
        pcspkmuse = NULL;
    }
}

fx_blaster_config dmx_blaster;

void *mus_data = NULL;
char *mid_data = NULL;

int mus_loop = 0;
int dmx_mus_port = 0;
int dmx_sdev = 0;
int dmx_mdev = NumSoundCards;

#if DMX_DIAG
#define DMX_DIAG_FILE "sounddiag.txt"

void DMX_DiagReset(void)
{
    FILE *f = fopen(DMX_DIAG_FILE, "w");
    if (f)
        fclose(f);
}

void DMX_Diag(const char *format, ...)
{
    char line[192];
    va_list ap;
    FILE *f;

    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);

    printf("%s", line);

    f = fopen(DMX_DIAG_FILE, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}
#endif

static int dmx_music_started;
static int dmx_fx_started;
static int dmx_pcfx_started;
int mus_rate = 140;
int mus_active = 0;
int mus_fadeout = 0;
int mus_mastervolume = 127;

#if DMX_DIAG
static int dmx_diag_song_registered;
static int dmx_diag_song_played;
#endif

void MUS_PauseSong(int handle) {
    MUSIC_Pause();
}
void MUS_ResumeSong(int handle) {
    MUSIC_Continue();
}
void MUS_SetMasterVolume(int volume) {
    mus_mastervolume = volume;
    MUSIC_SetVolume(volume * 2);
}
int MUS_RegisterSong(void *data) {
    FILE *mus;
    FILE *mid;
    unsigned int midlen;
    unsigned short len;
    mus_data = NULL;
    len = ((unsigned short*)data)[2]
        + ((unsigned short*)data)[3];
    if (mid_data)
    {
        free(mid_data);
    }
    if (memcmp(data, "MThd", 4))
    {
        mus = fopen("temp.mus", "wb");
        if (!mus)
        {
            return 0;
        }
        fwrite(data, 1, len, mus);
        fclose(mus);
        mus = fopen("temp.mus", "rb");
        if (!mus)
        {
            return 0;
        }
        mid = fopen("temp.mid", "wb");
        if (!mid)
        {
            fclose(mus);
            return 0;
        }
        if (mus2mid(mus, mid, mus_rate, dmx_mdev == Adlib || dmx_mdev == SoundBlaster))
        {
            fclose(mid);
            fclose(mus);
            return 0;
        }
        fclose(mid);
        fclose(mus);
        mid = fopen("temp.mid", "rb");
        if (!mid)
        {
            return 0;
        }
        fseek(mid, 0, SEEK_END);
        midlen = ftell(mid);
        rewind(mid);
        mid_data = malloc(midlen);
        if (!mid_data)
        {
            fclose(mid);
            return 0;
        }
        fread(mid_data, 1, midlen, mid);
        fclose(mid);
        mus_data = mid_data;
        remove("temp.mid");
        remove("temp.mus");
#if DMX_DIAG
        if (!dmx_diag_song_registered) {
            DMX_Diag("MUS register: MUS len=%u -> MIDI len=%u rate=%d\n",
                     (unsigned)len, midlen, mus_rate);
            dmx_diag_song_registered = 1;
        }
#endif
        return 0;
    }
    mus_data = data;
#if DMX_DIAG
    if (!dmx_diag_song_registered) {
        DMX_Diag("MUS register: input already MIDI\n");
        dmx_diag_song_registered = 1;
    }
#endif
    return 0;
}
int MUS_UnregisterSong(int handle) {
    return 0;
}
int MUS_QrySongPlaying(int handle) {
    if (mus_active)
    {
        if (mus_fadeout && !MUSIC_FadeActive())
        {
            MUSIC_StopSong();
            mus_active = 0;
            mus_fadeout = 0;
        }
        if (!MUSIC_SongPlaying())
        {
            mus_active = 0;
            mus_fadeout = 0;
        }
    }
    return mus_active;
}
int MUS_StopSong(int handle) {
    long status = MUSIC_StopSong();
    mus_active = 0;
    mus_fadeout = 0;
    return (status != MUSIC_Ok);
}
int MUS_ChainSong(int handle, int next) {
    mus_loop = (next == handle);
    return 0;
}

int MUS_PlaySong(int handle, int volume) {
    long status;
    if (mus_data == NULL)
    {
        return 1;
    }
    status = MUSIC_PlaySong((unsigned char*)mus_data, mus_loop);
#if DMX_DIAG
    if (!dmx_diag_song_played) {
        DMX_Diag("MUS play: loop=%d volume=%d rc=%ld\n",
                 mus_loop, volume, status);
        dmx_diag_song_played = 1;
    }
#endif
    if (status == MUSIC_Ok)
    {
        mus_active = 1;
        mus_fadeout = 0;
        //if (volume > mus_mastervolume)
            volume = mus_mastervolume;
        MUSIC_SetVolume(volume * 2);
    }
    return (status != MUSIC_Ok);
}

int MUS_FadeInSong(int handle, int ms) {
    long status;
    int target;
    if (mus_data == NULL)
    {
        return 1;
    }
    target = mus_mastervolume * 2;
    MUSIC_SetVolume(0);
    MUSIC_FadeVolume(target, ms);
    status = MUSIC_PlaySong((unsigned char*)mus_data, mus_loop);
    if (status == MUSIC_Ok)
    {
        mus_active = 1;
        mus_fadeout = 0;
    }
    return (status != MUSIC_Ok);
}

int MUS_FadeOutSong(int handle, int ms) {
    if (!mus_active)
        return 1;

    MUSIC_FadeVolume(0, ms);
    mus_fadeout = 1;
    return 0;
}

int SFX_PlayPatch(void *vdata, int pitch, int sep, int vol, int unk1, int priority) {
    unsigned int rate;
    unsigned long len;
    unsigned char *data = (unsigned char*)vdata;
    unsigned int type = (data[1] << 8) | data[0];
    dmxpcs_t *dmxpcs = (dmxpcs_t*)vdata;
    unsigned short i;
    if (type == 0)
    {
        size_t bytes;

        /*
         * The old static buffer reserved space for the maximum possible
         * 65536 PC-speaker samples: 131080 bytes of permanent .bss even when
         * PC Speaker was disabled.  Allocate only the current converted lump.
         * PCFX keeps using the buffer asynchronously, so its completion
         * callback owns the corresponding free().
         */
        if (pcspkmuse)
        {
            if (pcshandle > 0 && PCFX_SoundPlaying(pcshandle))
                PCFX_Stop(pcshandle);
            else
                pcspkmuse_done(0);
        }

        bytes = sizeof(*pcspkmuse)
              + (size_t)dmxpcs->length * sizeof(pcspkmuse->data[0]);
        pcspkmuse = (pcspkmuse_t *)malloc(bytes);
        if (!pcspkmuse)
            return -1;

        pcspkmuse->length = dmxpcs->length * 2;
        pcspkmuse->priority = 100;
        for (i = 0; i < dmxpcs->length; i++)
            pcspkmuse->data[i] = divisors[dmxpcs->data[i]];

        pcshandle = PCFX_Play((PCSound *)pcspkmuse, 100, 0);
        if (pcshandle < 0)
        {
            pcspkmuse_done(0);
            return pcshandle;
        }

        return pcshandle | 0x8000;
    }
    else if (type == 3)
    {
        rate = (data[3] << 8) | data[2];
        len = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
        if (len <= 48) {
            return -1;
        }
        len -= 32;
        return FX_PlayRaw(data + 24, len, rate, ((pitch - 128) * 2400) / 128, vol * 2, ((254 - sep) * vol) / 63, ((sep)* vol) / 63, 127-priority, 0);
    }
    return 0;
}
void SFX_StopPatch(int handle) {
    if (handle & 0x8000)
    {
        PCFX_Stop(handle & 0x7fff);
        return;
    }
    FX_StopSound(handle);
}
int SFX_Playing(int handle) {
    if (handle & 0x8000)
    {
        return PCFX_SoundPlaying(handle & 0x7fff);
    }
    return FX_SoundActive(handle);
}
void SFX_SetOrigin(int handle, int  pitch, int sep, int vol) {
    if (handle & 0x8000)
    {
        return;
    }
    FX_SetPan(handle, vol * 2, ((254 - sep) * vol) / 63, ((sep)* vol) / 63);
    FX_SetPitch(handle, ((pitch - 128) * 2400) / 128);
}
int GF1_Detect(void) {
    return 0; //FIXME
}
void GF1_SetMap(void *data, int len) {
    FILE *ini = fopen("ULTRAMID.INI", "wb");
    if (ini) {
        fwrite(data, 1, len, ini);
        fclose(ini);
    }
}
int SB_Detect(int *port, int *irq, int *dma, int *unk) {
    (void)unk;

    /*
     * Native FDOS does not probe ISA hardware here.  The emulator has already
     * instantiated (or not instantiated) the SB16 device, and that state is
     * available through the public PC structure via sound_hw_mask().
     *
     * Keep the port/IRQ/DMA selected by DOOM's configuration: those values
     * are the guest-visible interface that the backend will program.
     */
    if (!(sound_hw_mask() & SOUND_HW_SB16) || !port || !irq || !dma) {
        return -1;
    }

    dmx_blaster.Type = fx_SB16;
    dmx_blaster.Address = *port;
    dmx_blaster.Interrupt = *irq;
    dmx_blaster.Dma8 = *dma;
    dmx_blaster.Dma16 = *dma;
#if DMX_DIAG
    DMX_Diag("SB detect: port=0x%04x irq=%d dma=%d hw=0x%02x\n",
             dmx_blaster.Address, dmx_blaster.Interrupt, dmx_blaster.Dma8,
             sound_hw_mask());
#endif
    return 0;
}
void SB_SetCard(int port, int irq, int dma) { } //FIXME
int AL_Detect(int *port, int *unk) {
    (void)unk;

    if (!port || !(sound_hw_mask() & SOUND_HW_ADLIB))
        return -1;

    /* AdLib's guest-visible base port is fixed by the ISA interface. */
    *port = 0x388;
    return 0;
}
void AL_SetCard(int port, void *data) {
    unsigned char *cdata;
    unsigned char *tmb;
    int i;
    cdata = (unsigned char *)data;
    tmb = malloc(13 * 256);
    memset(tmb, 0, 13 * 256);
    if (!tmb)
    {
        return;
    }
    for (i = 0; i < 128; i++)
    {
        tmb[i * 13 + 0] = cdata[8 + i * 36 + 4 + 0];
        tmb[i * 13 + 1] = cdata[8 + i * 36 + 4 + 7];
        tmb[i * 13 + 2] = cdata[8 + i * 36 + 4 + 4]
                        | cdata[8 + i * 36 + 4 + 5];
        tmb[i * 13 + 3] = cdata[8 + i * 36 + 4 + 11] & 192;
        tmb[i * 13 + 4] = cdata[8 + i * 36 + 4 + 1];
        tmb[i * 13 + 5] = cdata[8 + i * 36 + 4 + 8];
        tmb[i * 13 + 6] = cdata[8 + i * 36 + 4 + 2];
        tmb[i * 13 + 7] = cdata[8 + i * 36 + 4 + 9];
        tmb[i * 13 + 8] = cdata[8 + i * 36 + 4 + 3];
        tmb[i * 13 + 9] = cdata[8 + i * 36 + 4 + 10];
        tmb[i * 13 + 10] = cdata[8 + i * 36 + 4 + 6];
        tmb[i * 13 + 11] = cdata[8 + i * 36 + 4 + 14] + 12;
        tmb[i * 13 + 12] = 0;
    }
    for (i = 128; i < 175; i++)
    {
        tmb[(i + 35) * 13 + 0] = cdata[8 + i * 36 + 4 + 0];
        tmb[(i + 35) * 13 + 1] = cdata[8 + i * 36 + 4 + 7];
        tmb[(i + 35) * 13 + 2] = cdata[8 + i * 36 + 4 + 4]
                               | cdata[8 + i * 36 + 4 + 5];
        tmb[(i + 35) * 13 + 3] = cdata[8 + i * 36 + 4 + 11] & 192;
        tmb[(i + 35) * 13 + 4] = cdata[8 + i * 36 + 4 + 1];
        tmb[(i + 35) * 13 + 5] = cdata[8 + i * 36 + 4 + 8];
        tmb[(i + 35) * 13 + 6] = cdata[8 + i * 36 + 4 + 2];
        tmb[(i + 35) * 13 + 7] = cdata[8 + i * 36 + 4 + 9];
        tmb[(i + 35) * 13 + 8] = cdata[8 + i * 36 + 4 + 3];
        tmb[(i + 35) * 13 + 9] = cdata[8 + i * 36 + 4 + 10];
        tmb[(i + 35) * 13 + 10] = cdata[8 + i * 36 + 4 + 6];
        tmb[(i + 35) * 13 + 11] = cdata[8 + i * 36 + 3]
                                + cdata[8 + i * 36 + 4 + 14] + 12;
        tmb[(i + 35) * 13 + 12] = 0;
    }
    AL_RegisterTimbreBank(tmb);
    free(tmb);
}
int MPU_Detect(int *port, int *unk) {
    (void)unk;

    if (!port || !(sound_hw_mask() & SOUND_HW_MPU401)) {
        return -1;
    }

    return 0;
}
void MPU_SetCard(int port) {
    dmx_mus_port = port;
}
int DMX_Init(int rate, int maxsng, int mdev, int sdev) {
    long status;
    long device = NumSoundCards;

    (void)maxsng;
    mus_rate = rate;
    dmx_sdev = sdev;
    dmx_mdev = NumSoundCards;
    dmx_music_started = 0;
    dmx_fx_started = 0;
    dmx_pcfx_started = 0;
#if DMX_DIAG
    DMX_Diag("DMX init: rate=%d music_code=%d sfx_code=%d\n",
             rate, mdev, sdev);
#endif
    /*
     * DMX code 0 means "None".  Do not turn it into NumSoundCards and call
     * MUSIC_Init(): that starts the native sequencer even when DEFAULT.CFG
     * explicitly disabled music.
     */
    if (mdev != 0)
    {
        switch (mdev) {
        case AHW_ADLIB:
            /*
             * AdLib music remains OPL music even when SFX use a Sound
             * Blaster.  The SB contains an OPL-compatible FM block, but the
             * native music backend is selected as Adlib; SoundBlaster is an
             * FX device and MUSIC_Init() intentionally does not accept it.
             */
            device = Adlib;
            printf("  DMX_Init Adlib\n");
            break;
        case AHW_SOUND_BLASTER:
            device = SoundBlaster;
            printf("  DMX_Init SoundBlaster\n");
            break;
        case AHW_MPU_401:
            device = GenMidi;
            printf("  DMX_Init MPU401/GenMidi\n");
            break;
        case AHW_ULTRA_SOUND:
            device = UltraSound;
            printf("  DMX_Init UltraSound\n");
            break;
        default:
            return -1;
        }

        dmx_mdev = device;
        if (device == SoundBlaster)
        {
            int MaxVoices;
            int MaxBits;
            int MaxChannels;

            FX_SetupSoundBlaster(dmx_blaster, &MaxVoices,
                                 &MaxBits, &MaxChannels);
        }

        status = MUSIC_Init(device, dmx_mus_port);
#if DMX_DIAG
        DMX_Diag("MUSIC init: device=%ld port=0x%x rc=%ld\n",
                 device, dmx_mus_port, status);
#endif
        if (status != MUSIC_Ok)
            return -1;

        dmx_music_started = 1;
        MUSIC_SetVolume(0);
    }

    if (sdev & AHW_PC_SPEAKER)
    {
        PCFX_Init();
        dmx_pcfx_started = 1;
        PCFX_SetCallBack(pcspkmuse_done);
        PCFX_SetTotalVolume(255);
        PCFX_UseLookup(0, 0);
    }

    return mdev | sdev;
}

void DMX_DeInit(void) {
    if (dmx_music_started) {
        MUSIC_Shutdown();
        dmx_music_started = 0;
    }
    if (dmx_fx_started) {
        FX_Shutdown();
        dmx_fx_started = 0;
    }
    if (dmx_pcfx_started) {
        PCFX_Shutdown();
        pcspkmuse_done(0);
        dmx_pcfx_started = 0;
    }
    remove("ULTRAMID.INI");
    if (mid_data)
    {
        free(mid_data);
        mid_data = NULL;
    }
}

void WAV_PlayMode(int channels, int samplerate) {
    long device;
    long status;

    /* DMX sound-device code 0 is a real disabled state. */
    if (dmx_sdev == 0)
        return;

    switch (dmx_sdev) {
    case AHW_SOUND_BLASTER:
        device = SoundBlaster;
        break;
    case AHW_ULTRA_SOUND:
        device = UltraSound;
        break;
    default:
        return;
    }

    if (device == SoundBlaster) {
        int MaxVoices;
        int MaxBits;
        int MaxChannels;

        FX_SetupSoundBlaster(dmx_blaster, &MaxVoices,
                             &MaxBits, &MaxChannels);
    }

    status = FX_Init(device, channels, 2, 16, samplerate);
#if DMX_DIAG
    DMX_Diag("FX init result: device=%ld voices=%d rate=%d rc=%ld\n",
             device, channels, samplerate, status);
#endif
    if (status == FX_Ok) {
        dmx_fx_started = 1;
        FX_SetVolume(255);
    }
}

int CODEC_Detect(int *a, int *b)
{
    return 1;
}
int ENS_Detect(void)
{
    return 1;
}
int MV_Detect(void)
{
    return 1;
}
