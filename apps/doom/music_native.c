/*
 * Native-FDOS MIDI music backend for the restored Apogee MUSIC interface.
 *
 * The emulator already exposes an MPU-401 UART at the guest-visible port
 * selected by DOOM (normally 0x330).  The backend therefore only needs to
 * sequence a Standard MIDI File and write MIDI bytes to that port.
 *
 * Timing follows the DMX model: a 140-Hz TSM service is dispatched
 * asynchronously from the native TSR0 timer hook on core0.
 * No host IRQ, DPMI task manager or second hardware timer is involved.
 *
 * The same sequencer feeds either the MPU-401 UART or a native OPL2 driver.
 * The OPL2 path consumes the 13-byte-per-instrument bank prepared by DMX from
 * the GENMIDI lump, so the original DOOM instrument definitions are preserved.
 */

#include <stdint.h>
#include <stddef.h>

#include <conio.h>
#include <stdlib.h>
#include <string.h>

#include "dmx.h"
#include "music.h"
#include "sndcards.h"

#define NATIVE_MUSIC_RATE        140
#define NATIVE_MIDI_CHANNELS     16
#define NATIVE_TICK_FRAC_BITS    16
#define NATIVE_TICK_FRAC_ONE     (1u << NATIVE_TICK_FRAC_BITS)
#define NATIVE_OPL_VOICES        9
#define NATIVE_OPL_PORT          0x388

typedef struct
{
    const unsigned char *ptr;
    const unsigned char *end;
    uint32_t next_tick;
    uint8_t running_status;
    uint8_t ended;
} native_midi_track_t;

typedef struct
{
    uint8_t active;
    uint8_t channel;
    uint8_t key;
    uint8_t velocity;
    uint32_t age;
} native_opl_voice_t;

static const uint8_t opl_operator_offset[9][2] =
{
    { 0, 3 }, { 1, 4 }, { 2, 5 },
    { 8,11 }, { 9,12 }, {10,13 },
    {16,19 }, {17,20 }, {18,21 }
};

static const uint16_t opl_fnum[12] =
{
    0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
    0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287
};

int MUSIC_ErrorCode = MUSIC_Ok;

static int music_device = NumSoundCards;
static uint16_t music_port;
static int music_service_id = -1;
static int music_playing;
static int music_paused;
static int music_loop;
static int music_context;
static int music_volume = 255;
static int music_channel_volume[NATIVE_MIDI_CHANNELS];

static native_midi_track_t *music_tracks;
static unsigned music_track_count;
static unsigned music_division;
static uint32_t music_tempo_us = 500000u;
static uint64_t music_tick_fp;
static uint32_t music_song_ticks;

static int music_fade_active;
static int music_fade_start;
static int music_fade_target;
static unsigned music_fade_step;
static unsigned music_fade_steps;

static unsigned char *music_timbre_bank;
static native_opl_voice_t opl_voices[NATIVE_OPL_VOICES];
static uint8_t opl_program[NATIVE_MIDI_CHANNELS];
static int16_t opl_pitchbend[NATIVE_MIDI_CHANNELS];
static uint32_t opl_voice_age;
#if DMX_DIAG
static int opl_diag_first_note;
#endif

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static int read_vlq(const unsigned char **pp,
                    const unsigned char *end,
                    uint32_t *value)
{
    const unsigned char *p = *pp;
    uint32_t v = 0;
    int count = 0;

    while (p < end && count < 4)
    {
        uint8_t b = *p++;
        v = (v << 7) | (uint32_t)(b & 0x7fu);
        ++count;
        if ((b & 0x80u) == 0)
        {
            *pp = p;
            *value = v;
            return 0;
        }
    }

    return -1;
}

static void opl_write(uint8_t reg, uint8_t value)
{
    int delay;

    outp(NATIVE_OPL_PORT, reg);
    for (delay = 0; delay < 6; ++delay)
        (void)inp(NATIVE_OPL_PORT);
    outp(NATIVE_OPL_PORT + 1, value);
    for (delay = 0; delay < 27; ++delay)
        (void)inp(NATIVE_OPL_PORT);
}

static int opl_patch_for(int channel, int key)
{
    if (channel == 9)
        return (key + 128) & 255;
    return opl_program[channel] & 127;
}

static const unsigned char *opl_timbre(int patch)
{
    if (!music_timbre_bank)
        return NULL;
    return music_timbre_bank + ((unsigned)patch * 13u);
}

static void opl_key_off(int voice)
{
    opl_write((uint8_t)(0xb0 + voice), 0);
    opl_voices[voice].active = 0;
}

static int opl_alloc_voice(void)
{
    int i;
    int oldest = 0;

    for (i = 0; i < NATIVE_OPL_VOICES; ++i)
        if (!opl_voices[i].active)
            return i;

    for (i = 1; i < NATIVE_OPL_VOICES; ++i)
        if (opl_voices[i].age < opl_voices[oldest].age)
            oldest = i;

    opl_key_off(oldest);
    return oldest;
}

static uint8_t opl_level(uint8_t original, int channel, int velocity)
{
    /*
     * OPL total-level is attenuation: 0 = loudest, 63 = silent.
     *
     * Keep the whole product until the final division.  The previous code
     * first reduced velocity*channel_volume*music_volume to an integer in
     * the range 0..1.  For every value below the absolute maximum it became
     * zero, so the carrier was programmed with TL=63 and nearly all AdLib
     * notes were completely muted.
     *
     * Maximum numerator:
     *   63 * 127 * 127 * 255 = 259,112,? < UINT32_MAX
     * so 32-bit unsigned arithmetic is sufficient.
     */
    unsigned loudness = 63u - (original & 0x3fu);
    uint32_t scaled;

    scaled = (uint32_t)loudness
           * (uint32_t)(unsigned)velocity
           * (uint32_t)(unsigned)music_channel_volume[channel]
           * (uint32_t)(unsigned)music_volume;
    scaled /= (127u * 127u * 255u);

    if (scaled > 63u)
        scaled = 63u;

    return (uint8_t)((original & 0xc0u) | (63u - (unsigned)scaled));
}

static void opl_program_voice(int voice, int channel, int key, int velocity)
{
    int patch = opl_patch_for(channel, key);
    const unsigned char *t = opl_timbre(patch);
    unsigned op0 = opl_operator_offset[voice][0];
    unsigned op1 = opl_operator_offset[voice][1];
    int note;
    int octave;
    unsigned fnum;
    int bend;

    if (!t)
        return;

#if DMX_DIAG
    if (!opl_diag_first_note) {
        DMX_Diag("AdLib note: voice=%d ch=%d key=%d vel=%d prog=%u patch=%d "
                 "vol=%d chvol=%d\n",
                 voice, channel, key, velocity,
                 (unsigned)opl_program[channel], patch,
                 music_volume, music_channel_volume[channel]);
        DMX_Diag("AdLib timbre: %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x\n",
                 t[0], t[1], t[2], t[3], t[4], t[5], t[6],
                 t[7], t[8], t[9], t[10], t[11], t[12]);
        opl_diag_first_note = 1;
    }
#endif
    opl_write((uint8_t)(0xa0 + voice), 0);
    opl_write((uint8_t)(0xb0 + voice), 0);

    opl_write((uint8_t)(0x20 + op0), t[0]);
    opl_write((uint8_t)(0x20 + op1), t[1]);
    opl_write((uint8_t)(0x60 + op0), t[4]);
    opl_write((uint8_t)(0x60 + op1), t[5]);
    opl_write((uint8_t)(0x80 + op0), t[6]);
    opl_write((uint8_t)(0x80 + op1), t[7]);
    opl_write((uint8_t)(0xe0 + op0), t[8]);
    opl_write((uint8_t)(0xe0 + op1), t[9]);
    opl_write((uint8_t)(0x40 + op0),
              (t[10] & 1u) ? opl_level(t[2], channel, velocity) : t[2]);
    opl_write((uint8_t)(0x40 + op1), opl_level(t[3], channel, velocity));
    opl_write((uint8_t)(0xc0 + voice), t[10]);

    if (channel == 9)
        note = (int)(int8_t)t[11] - 12;
    else
        note = key + (int)(int8_t)t[11] - 12;

    bend = opl_pitchbend[channel];
    note += bend / 4096;
    if (note < 0) note = 0;
    if (note > 95) note = 95;

    octave = note / 12;
    if (octave > 7) octave = 7;
    fnum = opl_fnum[note % 12];
#if DMX_DIAG
    if (opl_diag_first_note == 1) {
        DMX_Diag("AdLib pitch: note=%d octave=%d fnum=0x%03x "
                 "op0=%u op1=%u levels=%02x/%02x feedback=%02x\n",
                 note, octave, fnum, op0, op1,
                 opl_level(t[2], channel, velocity),
                 opl_level(t[3], channel, velocity), t[10]);
        opl_diag_first_note = 2;
    }
#endif
    opl_write((uint8_t)(0xa0 + voice), (uint8_t)fnum);
    opl_write((uint8_t)(0xb0 + voice),
              (uint8_t)(((fnum >> 8) & 3u) | ((unsigned)octave << 2) | 0x20u));

    opl_voices[voice].active = 1;
    opl_voices[voice].channel = (uint8_t)channel;
    opl_voices[voice].key = (uint8_t)key;
    opl_voices[voice].velocity = (uint8_t)velocity;
    opl_voices[voice].age = ++opl_voice_age;
}

static void opl_note_off(int channel, int key)
{
    int i;
    for (i = 0; i < NATIVE_OPL_VOICES; ++i)
        if (opl_voices[i].active && opl_voices[i].channel == channel &&
            opl_voices[i].key == key)
        {
            opl_key_off(i);
            return;
        }
}

static void opl_all_notes_off_channel(int channel)
{
    int i;
    for (i = 0; i < NATIVE_OPL_VOICES; ++i)
        if (opl_voices[i].active && opl_voices[i].channel == channel)
            opl_key_off(i);
}

static void opl_update_channel_voices(int channel)
{
    int i;
    for (i = 0; i < NATIVE_OPL_VOICES; ++i)
        if (opl_voices[i].active && opl_voices[i].channel == channel)
            opl_program_voice(i, channel, opl_voices[i].key,
                              opl_voices[i].velocity);
}

static void opl_reset(void)
{
    int reg;
    int i;
    for (reg = 1; reg <= 0xf5; ++reg)
        opl_write((uint8_t)reg, 0);
    opl_write(0x01, 0x20);
    opl_write(0xbd, 0);
    for (i = 0; i < NATIVE_OPL_VOICES; ++i)
        opl_voices[i].active = 0;
    for (i = 0; i < NATIVE_MIDI_CHANNELS; ++i)
    {
        opl_program[i] = 0;
        opl_pitchbend[i] = 0;
    }
    opl_voice_age = 0;
}

static void midi_write(uint8_t value)
{
    outp(music_port, value);
}

static void midi_all_notes_off(void)
{
    int ch;

    if (music_device == Adlib)
    {
        for (ch = 0; ch < NATIVE_MIDI_CHANNELS; ++ch)
            opl_all_notes_off_channel(ch);
        return;
    }

    if (music_device == GenMidi)
        for (ch = 0; ch < NATIVE_MIDI_CHANNELS; ++ch)
        {
            midi_write((uint8_t)(0xb0 | ch));
            midi_write(123);
            midi_write(0);
        }
}

static uint8_t scaled_channel_volume(int channel)
{
    unsigned value;

    value = (unsigned)music_channel_volume[channel]
          * (unsigned)music_volume;
    value /= (127u * 2u);

    if (value > 127u)
        value = 127u;
    return (uint8_t)value;
}

static void midi_send_channel_volumes(void)
{
    int ch;

    if (music_device == Adlib)
    {
        for (ch = 0; ch < NATIVE_MIDI_CHANNELS; ++ch)
            opl_update_channel_voices(ch);
        return;
    }

    if (music_device == GenMidi)
        for (ch = 0; ch < NATIVE_MIDI_CHANNELS; ++ch)
        {
            midi_write((uint8_t)(0xb0 | ch));
            midi_write(7);
            midi_write(scaled_channel_volume(ch));
        }
}

static int track_read_next_delta(native_midi_track_t *track)
{
    uint32_t delta;

    if (track->ptr >= track->end)
    {
        track->ended = 1;
        return -1;
    }

    if (read_vlq(&track->ptr, track->end, &delta) != 0)
    {
        track->ended = 1;
        return -1;
    }

    track->next_tick += delta;
    return 0;
}

static int midi_message_data_length(uint8_t status)
{
    switch (status & 0xf0u)
    {
        case 0xc0:
        case 0xd0:
            return 1;

        case 0x80:
        case 0x90:
        case 0xa0:
        case 0xb0:
        case 0xe0:
            return 2;

        default:
            return -1;
    }
}

static void track_process_event(native_midi_track_t *track)
{
    const unsigned char *p = track->ptr;
    uint8_t status;
    uint8_t data[2];
    int data_len;
    int i;

    if (p >= track->end)
    {
        track->ended = 1;
        return;
    }

    if (*p & 0x80u)
    {
        status = *p++;
        if (status < 0xf0u)
            track->running_status = status;
        else
            track->running_status = 0;
    }
    else
    {
        status = track->running_status;
        if (!status)
        {
            track->ended = 1;
            return;
        }
    }

    if (status == 0xffu)
    {
        uint8_t type;
        uint32_t len;

        if (p >= track->end)
        {
            track->ended = 1;
            return;
        }

        type = *p++;
        if (read_vlq(&p, track->end, &len) != 0 ||
            len > (uint32_t)(track->end - p))
        {
            track->ended = 1;
            return;
        }

        if (type == 0x2fu)
        {
            track->ptr = p + len;
            track->ended = 1;
            return;
        }

        if (type == 0x51u && len == 3)
        {
            uint32_t tempo = ((uint32_t)p[0] << 16)
                           | ((uint32_t)p[1] << 8)
                           | p[2];
            if (tempo != 0)
                music_tempo_us = tempo;
        }

        p += len;
        track->ptr = p;
        track_read_next_delta(track);
        return;
    }

    if (status == 0xf0u || status == 0xf7u)
    {
        uint32_t len;

        if (read_vlq(&p, track->end, &len) != 0 ||
            len > (uint32_t)(track->end - p))
        {
            track->ended = 1;
            return;
        }

        /*
         * DOOM's converted MUS data does not require SysEx.  Skip it rather
         * than feeding partial file-format SysEx framing to the MPU UART.
         */
        p += len;
        track->ptr = p;
        track_read_next_delta(track);
        return;
    }

    data_len = midi_message_data_length(status);
    if (data_len < 0 || p + data_len > track->end)
    {
        track->ended = 1;
        return;
    }

    for (i = 0; i < data_len; ++i)
        data[i] = *p++;

    if ((status & 0xf0u) == 0xb0u && data[0] == 7)
    {
        int channel = status & 0x0f;
        music_channel_volume[channel] = data[1];
        data[1] = scaled_channel_volume(channel);
    }

    if (music_device == GenMidi)
    {
        midi_write(status);
        for (i = 0; i < data_len; ++i)
            midi_write(data[i]);
    }
    else if (music_device == Adlib)
    {
        int channel = status & 0x0f;
        switch (status & 0xf0u)
        {
            case 0x80:
                opl_note_off(channel, data[0]);
                break;
            case 0x90:
                if (data[1] == 0)
                    opl_note_off(channel, data[0]);
                else
                    opl_program_voice(opl_alloc_voice(), channel, data[0], data[1]);
                break;
            case 0xb0:
                if (data[0] == 7)
                    opl_update_channel_voices(channel);
                else if (data[0] == 123 || data[0] == 121)
                    opl_all_notes_off_channel(channel);
                break;
            case 0xc0:
                opl_program[channel] = data[0];
                break;
            case 0xe0:
                opl_pitchbend[channel] =
                    (int16_t)(((int)data[0] | ((int)data[1] << 7)) - 8192);
                opl_update_channel_voices(channel);
                break;
            default:
                break;
        }
    }

    track->ptr = p;
    track_read_next_delta(track);
}

static int all_tracks_ended(void)
{
    unsigned i;

    for (i = 0; i < music_track_count; ++i)
        if (!music_tracks[i].ended)
            return 0;
    return 1;
}

static int midi_reset_tracks(unsigned char *song)
{
    const unsigned char *p = song;
    uint32_t header_len;
    uint16_t format;
    uint16_t tracks;
    uint16_t division;
    unsigned i;

    if (!song || memcmp(p, "MThd", 4) != 0)
        return MUSIC_MidiError;

    header_len = be32(p + 4);
    if (header_len < 6)
        return MUSIC_MidiError;

    format = be16(p + 8);
    tracks = be16(p + 10);
    division = be16(p + 12);

    if (format > 1 || tracks == 0 || (division & 0x8000u))
        return MUSIC_MidiError;

    if (music_tracks)
    {
        free(music_tracks);
        music_tracks = NULL;
    }

    music_tracks = (native_midi_track_t *)
        calloc(tracks, sizeof(native_midi_track_t));
    if (!music_tracks)
        return MUSIC_Error;

    music_track_count = tracks;
    music_division = division;
    music_tempo_us = 500000u;
    music_tick_fp = 0;
    music_song_ticks = 0;

    p += 8u + header_len;

    for (i = 0; i < tracks; ++i)
    {
        uint32_t length;

        if (memcmp(p, "MTrk", 4) != 0)
            return MUSIC_MidiError;

        length = be32(p + 4);
        p += 8;

        music_tracks[i].ptr = p;
        music_tracks[i].end = p + length;
        music_tracks[i].next_tick = 0;
        music_tracks[i].running_status = 0;
        music_tracks[i].ended = 0;

        if (track_read_next_delta(&music_tracks[i]) != 0)
            music_tracks[i].ended = 1;

        p += length;
    }

    for (i = 0; i < NATIVE_MIDI_CHANNELS; ++i)
        music_channel_volume[i] = 127;

    return MUSIC_Ok;
}

static int music_service(void)
{
    uint64_t increment;
    uint32_t current_tick;
    unsigned i;

    if (!music_playing || music_paused || music_track_count == 0)
        return 0;

    /*
     * 16.16 MIDI-tick accumulator.
     *
     * MIDI ticks/second = division * 1,000,000 / tempo_us.
     * The service runs at 140 Hz.  Tempo changes are applied from the next
     * service slice, so their quantization error is bounded by one 140-Hz
     * interval (~7.14 ms), matching the original DMX service cadence.
     */
    increment = ((uint64_t)music_division * 1000000ull
                 * NATIVE_TICK_FRAC_ONE)
              / ((uint64_t)music_tempo_us * NATIVE_MUSIC_RATE);
    music_tick_fp += increment;
    current_tick = (uint32_t)(music_tick_fp >> NATIVE_TICK_FRAC_BITS);
    music_song_ticks = current_tick;

    for (;;)
    {
        int progressed = 0;

        for (i = 0; i < music_track_count; ++i)
        {
            native_midi_track_t *track = &music_tracks[i];

            while (!track->ended && track->next_tick <= current_tick)
            {
                track_process_event(track);
                progressed = 1;
            }
        }

        if (!progressed)
            break;
    }

    if (music_fade_active)
    {
        ++music_fade_step;
        if (music_fade_step >= music_fade_steps)
        {
            music_volume = music_fade_target;
            music_fade_active = 0;
        }
        else
        {
            int delta = music_fade_target - music_fade_start;
            music_volume = music_fade_start
                         + (int)((int64_t)delta * music_fade_step
                                 / music_fade_steps);
        }
        midi_send_channel_volumes();
    }

    if (all_tracks_ended())
    {
        if (music_loop)
        {
            unsigned char *song = NULL;

            /*
             * Each track pointer has advanced, so recover the owning MIDI
             * image from a separately retained pointer below.
             */
            extern unsigned char *native_music_song_image;
            song = native_music_song_image;
            midi_all_notes_off();
            if (midi_reset_tracks(song) != MUSIC_Ok)
                music_playing = 0;
            else
                midi_send_channel_volumes();
        }
        else
        {
            midi_all_notes_off();
            music_playing = 0;
        }
    }

    return 0;
}

/*
 * Kept non-static because music_service() needs the original SMF image when a
 * looping song reaches end-of-track after all track cursors have advanced.
 */
unsigned char *native_music_song_image;

char *MUSIC_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber)
    {
        case MUSIC_Ok: return "OK";
        case MUSIC_FMNotDetected: return "AdLib/OPL2 backend error";
        case MUSIC_MidiError: return "invalid MIDI data";
        case MUSIC_MPU401Error: return "MPU-401 backend error";
        default: return "native FDOS MUSIC backend error";
    }
}

int MUSIC_Init(int SoundCard, int Address)
{
    MUSIC_ErrorCode = MUSIC_Ok;
    music_device = SoundCard;
    music_port = (uint16_t)Address;
    music_playing = 0;
    music_paused = 0;
    music_loop = 0;
    music_fade_active = 0;

    if (SoundCard == Adlib)
    {
        music_port = NATIVE_OPL_PORT;
        opl_reset();
    }
    else if (SoundCard != GenMidi)
    {
        MUSIC_ErrorCode = MUSIC_InvalidCard;
        return MUSIC_ErrorCode;
    }

    /*
     * Reset the MPU and request UART mode.  The murm386 MPU implementation
     * accepts the same guest-visible command/data ports as DOS software.
     */
    if (SoundCard == GenMidi)
    {
        outp((uint16_t)(music_port + 1), 0xff);
        outp((uint16_t)(music_port + 1), 0xd0);
    }

    if (music_service_id < 0)
        music_service_id = TSM_NewService(music_service,
                                          NATIVE_MUSIC_RATE, 1, 0);

    if (music_service_id < 0)
    {
        MUSIC_ErrorCode = MUSIC_TaskManError;
        return MUSIC_ErrorCode;
    }

    MUSIC_ResetMidiChannelVolumes();
    return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
    MUSIC_StopSong();

    if (music_service_id >= 0)
    {
        TSM_DelService(music_service_id);
        music_service_id = -1;
    }

    if (music_tracks)
    {
        free(music_tracks);
        music_tracks = NULL;
    }
    music_track_count = 0;

    return MUSIC_Ok;
}

void MUSIC_SetMaxFMMidiChannel(int channel)
{
    (void)channel;
}

void MUSIC_SetVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 255)
        volume = 255;

    music_volume = volume;
    midi_send_channel_volumes();
}

void MUSIC_SetMidiChannelVolume(int channel, int volume)
{
    if (channel < 0 || channel >= NATIVE_MIDI_CHANNELS)
        return;

    if (volume < 0)
        volume = 0;
    if (volume > 127)
        volume = 127;

    music_channel_volume[channel] = volume;

    if (music_device == GenMidi)
    {
        midi_write((uint8_t)(0xb0 | channel));
        midi_write(7);
        midi_write(scaled_channel_volume(channel));
    }
    else if (music_device == Adlib)
    {
        opl_update_channel_voices(channel);
    }
}

void MUSIC_ResetMidiChannelVolumes(void)
{
    int ch;

    for (ch = 0; ch < NATIVE_MIDI_CHANNELS; ++ch)
        music_channel_volume[ch] = 127;

    midi_send_channel_volumes();
}

int MUSIC_GetVolume(void)
{
    return music_volume;
}

void MUSIC_SetLoopFlag(int loopflag)
{
    music_loop = loopflag != 0;
}

int MUSIC_SongPlaying(void)
{
    return music_playing;
}

void MUSIC_Continue(void)
{
    music_paused = 0;
}

void MUSIC_Pause(void)
{
    music_paused = 1;
}

int MUSIC_StopSong(void)
{
    TSM_Lock();
    if (music_playing)
        midi_all_notes_off();

    music_playing = 0;
    music_paused = 0;
    music_fade_active = 0;
    native_music_song_image = NULL;

    if (music_tracks)
    {
        free(music_tracks);
        music_tracks = NULL;
    }
    music_track_count = 0;
    TSM_Unlock();
    return MUSIC_Ok;
}

int MUSIC_PlaySong(unsigned char *song, int loopflag)
{
    int rc;

    if (music_device != GenMidi && music_device != Adlib)
        return MUSIC_InvalidCard;

    TSM_Lock();
    MUSIC_StopSong();

    rc = midi_reset_tracks(song);
    if (rc != MUSIC_Ok)
    {
        MUSIC_ErrorCode = rc;
        TSM_Unlock();
        return rc;
    }

    if (music_device == Adlib)
        DMX_Diag("AdLib play: tracks=%u division=%u tempo=%lu bank=%s\n",
                 music_track_count, music_division,
                 (unsigned long)music_tempo_us,
                 music_timbre_bank ? "yes" : "NO");

    native_music_song_image = song;
    music_loop = loopflag != 0;
    music_paused = 0;
    music_playing = 1;
    music_fade_active = 0;

    midi_send_channel_volumes();

    /*
     * Process all tick-zero events immediately: program changes, controllers
     * and notes need not wait for the first cooperative 140-Hz service slice.
     */
    {
        unsigned i;
        for (i = 0; i < music_track_count; ++i)
        {
            while (!music_tracks[i].ended &&
                   music_tracks[i].next_tick == 0)
                track_process_event(&music_tracks[i]);
        }
    }

    TSM_Unlock();
    return MUSIC_Ok;
}

void MUSIC_SetContext(int context)
{
    music_context = context;
}

int MUSIC_GetContext(void)
{
    return music_context;
}

void MUSIC_SetSongTick(unsigned long PositionInTicks)
{
    /*
     * Random seeking is not needed by DOOM's DMX wrapper.  Keep the API
     * deterministic rather than pretending to seek without reconstructing all
     * controller/program state.
     */
    (void)PositionInTicks;
}

void MUSIC_SetSongTime(unsigned long milliseconds)
{
    (void)milliseconds;
}

void MUSIC_SetSongPosition(int measure, int beat, int tick)
{
    (void)measure;
    (void)beat;
    (void)tick;
}

void MUSIC_GetSongPosition(songposition *pos)
{
    if (!pos)
        return;

    memset(pos, 0, sizeof(*pos));
    pos->tickposition = music_song_ticks;

    if (music_division)
        pos->milliseconds =
            (unsigned long)(((uint64_t)music_song_ticks
                             * music_tempo_us)
                            / ((uint64_t)music_division * 1000u));
}

void MUSIC_GetSongLength(songposition *pos)
{
    if (!pos)
        return;

    /*
     * DOOM's wrapper never queries song length.  Report the current known
     * position rather than scanning and mutating the active track state.
     */
    MUSIC_GetSongPosition(pos);
}

int MUSIC_FadeVolume(int tovolume, int milliseconds)
{
    if (tovolume < 0)
        tovolume = 0;
    if (tovolume > 255)
        tovolume = 255;

    if (milliseconds <= 0)
    {
        MUSIC_SetVolume(tovolume);
        music_fade_active = 0;
        return MUSIC_Ok;
    }

    music_fade_start = music_volume;
    music_fade_target = tovolume;
    music_fade_step = 0;
    music_fade_steps =
        ((unsigned)milliseconds * NATIVE_MUSIC_RATE + 999u) / 1000u;

    if (music_fade_steps == 0)
        music_fade_steps = 1;

    music_fade_active = 1;
    return MUSIC_Ok;
}

int MUSIC_FadeActive(void)
{
    return music_fade_active;
}

void MUSIC_StopFade(void)
{
    music_fade_active = 0;
}

void MUSIC_RerouteMidiChannel(int channel,
                              int cdecl (*function)(int event, int c1, int c2))
{
    (void)channel;
    (void)function;
}

void MUSIC_RegisterTimbreBank(unsigned char *timbres)
{
    AL_RegisterTimbreBank(timbres);
}

void AL_RegisterTimbreBank(unsigned char *timbres)
{
    if (!timbres)
        return;

    if (!music_timbre_bank)
        music_timbre_bank = (unsigned char *)malloc(13u * 256u);

    if (music_timbre_bank)
        memcpy(music_timbre_bank, timbres, 13u * 256u);
}
