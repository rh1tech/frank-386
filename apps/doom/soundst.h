//
// Copyright (C) 1993-1996 Id Software, Inc.
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
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

// soundst.h

#ifndef __SOUNDSTH__
#define __SOUNDSTH__

#define S_MAX_VOLUME		127

// when to clip out sounds
// Doesn't fit the large outdoor areas.
#define S_CLIPPING_DIST		(1200*0x10000)

// when sounds should be max'd out
#define S_CLOSE_DIST		(200*0x10000)


#define S_ATTENUATOR		((S_CLIPPING_DIST-S_CLOSE_DIST)>>FRACBITS)

#define NORM_PITCH     		128
#define NORM_PRIORITY		64
#define NORM_VOLUME    		snd_MaxVolume

#define S_PITCH_PERTURB		1
#define NORM_SEP			128
#define S_STEREO_SWING		(96*0x10000)

// % attenuation from front to back
#define S_IFRACVOL			30

#define NA				0
#define S_NUMCHANNELS		2

#if (APPVER_DOOMREV < AV_DR_DM12)
typedef struct
{
	int handle; // music handle once registered
	int lumpnum; // lump number of music
	void *data; // music data
} musiccache_t;

typedef struct
{
	char *name; // up to 6-character name
	musiccache_t *cache;
} musicinfo_t;
#else
typedef struct
{
	char *name; // up to 6-character name
	int lumpnum; // lump number of music
	void *data; // music data
	int handle; // music handle once registered
} musicinfo_t;
#endif


#if (APPVER_DOOMREV < AV_DR_DM12)

typedef struct
{
	int lumpnum; // lump number of sfx
	void *data; // sound data
} sfxcache_t;

typedef struct sfxinfo_s
{
	char *name; // up to 6-character name
	int singularity; // Sfx singularity (only one at a time)
	int priority; // Sfx priority
	struct sfxinfo_s *link; // referenced sound if a link
	int pitch; // pitch if a link
	int volume; // volume if a link
	sfxcache_t *cache;
} sfxinfo_t;

typedef struct
{
	sfxinfo_t *sfxinfo; // sound information (if null, channel avail.)
	void *origin; // origin of sound
	int f_8;
	int handle; // handle of the sound being played
} channel_t;

#else
typedef struct sfxinfo_s
{
	char *name; // up to 6-character name
	int singularity; // Sfx singularity (only one at a time)
	int priority; // Sfx priority
	struct sfxinfo_s *link; // referenced sound if a link
	int pitch; // pitch if a link
	int volume; // volume if a link
	void *data; // sound data
	int usefulness; // Determines when a sound should be cached out
	int lumpnum; // lump number of sfx
} sfxinfo_t;

typedef struct
{
	sfxinfo_t *sfxinfo; // sound information (if null, channel avail.)
	void *origin; // origin of sound
	int handle; // handle of the sound being played
} channel_t;
#endif



enum
{
    Music,
    Sfx,
    SfxLink
};

enum
{
    PC=1,
    Adlib=2,
    SB=4,
    Midi=8
}; // cards available

enum
{
    sfxThrowOut=-1,
    sfxNotUsed=0
};

void S_Start(void);
void S_StartSound(void *origin, int sound_id);
#if (APPVER_DOOMREV < AV_DR_DM17)
void S_StartSoundAtVolume(mobj_t *origin, int sound_id, int volume);
#else
void S_StartSoundAtVolume(void *origin, int sound_id, int volume);
#endif
void S_StopSound(void *origin);
void S_StartMusic(int music_id);
void S_ChangeMusic(int music_id, int looping);
void S_StopMusic(void);
void S_PauseSound(void);
void S_ResumeSound(void);
void S_UpdateSounds(void *listener);
void S_SetMusicVolume(int volume);
void S_SetSfxVolume(int volume);
void S_Init(int,int);

//--------
//SOUND IO
//--------
#define FREQ_LOW		0x40
#define FREQ_NORM		0x80
#define FREQ_HIGH		0xff

#if (APPVER_DOOMREV < AV_DR_DM12)
void I_PauseSong(void);
void I_ResumeSong(void);
void I_SetMusicVolume(int volume);
void I_SetSfxVolume(int volume);
void I_SetMasterVolume(int volume);
void I_GetMusicLumpNum(musicinfo_t *mus);
void I_StopSong(void);
void I_RegisterSong(musicinfo_t *mus);
void I_StartSongNoLoop(musicinfo_t *mus);
void I_StartSongLoop(musicinfo_t *mus);
void I_StartSong(musicinfo_t *mus, boolean looping);
void I_GetSfxLumpNum(sfxinfo_t *sfx);
void I_StartSound(channel_t *channel, int vol, int sep, int pitch, int priority);
void I_StopSound(channel_t *channel);
int I_SoundIsPlaying(channel_t *channel);
void I_UpdateSoundParams(channel_t *channel, int vol, int sep);
#else

void I_SetMusicVolume(int volume);
void I_SetSfxVolume(int volume);

//
//  MUSIC I/O
//
void I_PauseSong(int handle);
void I_ResumeSong(int handle);

//
// Called by anything that wishes to start music.
//  plays a song, and when the song is done,
//  starts playing it again in an endless loop.
// Horrible thing to do, considering.
int I_PlaySong(int handle, int looping);


// stops a song over 3 seconds.
void I_StopSong(int handle);

// registers a song handle to song data
int I_RegisterSong(void *data);

// see above then think backwards
void I_UnRegisterSong(int handle);

// is the song playing?
int I_QrySongPlaying(int handle);


//
//  SFX I/O
//
void I_SetChannels(int channels);

int I_GetSfxLumpNum (sfxinfo_t*);


// Starts a sound in a particular sound channel.
#if (APPVER_DOOMREV < AV_DR_DM1666P)
int I_StartSound(void *data, int vol, int sep, int pitch, int priority);
#else
int I_StartSound(int id, void* data, int vol, int sep, int pitch, int priority);
#endif


// Updates the volume, separation,
//  and pitch of a sound channel.
#if (APPVER_DOOMREV < AV_DR_DM1666P)
void I_UpdateSoundParams(int handle, int vol, int sep);
#else
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch);
#endif


// Stops a sound channel.
void I_StopSound(int handle);

// Called by S_*()'s to see if a channel is still playing.
// Returns 0 if no longer playing, 1 if playing.
int I_SoundIsPlaying(int handle);
#endif


// the complete set of sound effects
extern sfxinfo_t S_sfx[];

// the complete set of music
extern musicinfo_t S_music[];

#endif
