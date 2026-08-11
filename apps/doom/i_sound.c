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

// I_SOUND.C

#include <stdio.h>
#include "doomdef.h"
#include "dmx.h"
#include "sounds.h"
#include "i_sound.h"

#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp1, _wp2, _wp3, _wp4;
#else
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9, _wp10;
extern int _wp11, _wp12, _wp13, _wp14, _wp15, _wp16, _wp17, _wp18, _wp19;
#endif

#if (APPVER_DOOMREV >= AV_DR_DM12)
/*
===============
=
= I_StartupTimer
=
===============
*/

static int tsm_ID;

void I_StartupTimer (void)
{
#ifndef NOTIMER
	extern int I_TimerISR(void);

	printf("I_StartupTimer()\n");
	// installs master timer.  Must be done before StartupTimer()!
	TSM_Install(SND_TICRATE);
	tsm_ID = TSM_NewService (I_TimerISR, 35, 0, 0); // max priority
	if (tsm_ID == -1)
	{
		I_Error("Can't register 35 Hz timer w/ DMX library");
	}
#endif
}

void I_ShutdownTimer (void)
{
	TSM_DelService(tsm_ID);
	TSM_Remove();
}
#endif

/*
 *
 *                           SOUND HEADER & DATA
 *
 *
 */

#if (APPVER_DOOMREV < AV_DR_DM1666P)
// sound information
const char *dnames[] = {"None",
			"PC_Speaker",
			"Adlib",
			"Sound_Blaster",
#if (APPVER_DOOMREV >= AV_DR_DM12)
			"ProAudio_Spectrum16",
#endif
			"Gravis_Ultrasound",
			"MPU",
			//"AWE32"
			};
#endif

#if (APPVER_DOOMREV < AV_DR_DM12)
const char snd_prefixen[] = { '\0', 'P', 'A', 'S', 'S', 'M' };
#elif (APPVER_DOOMREV < AV_DR_DM1666P)
const char snd_prefixen[] = { 'P', 'P', 'A', 'S', 'S', 'S', 'M' };
#elif (APPVER_DOOMREV < AV_DR_DM18)
const char snd_prefixen[] = { 'P', 'P', 'A', 'S', 'S', 'S', 'M',
  'M', 'M', 'S'};
#else
const char snd_prefixen[] = { 'P', 'P', 'A', 'S', 'S', 'S', 'M',
  'M', 'M', 'S', 'S', 'S'};
#endif

int snd_DesiredMusicDevice, snd_DesiredSfxDevice;
#if (APPVER_DOOMREV < AV_DR_DM12)
int snd_MusicDevice,    // current music card # (index to dmxCodes)
	snd_SfxDevice,      // current sfx card # (index to dmxCodes)
	snd_MaxVolume;      // maximum volume for sound
#else
int snd_MusicDevice,    // current music card # (index to dmxCodes)
	snd_SfxDevice,      // current sfx card # (index to dmxCodes)
	snd_MaxVolume,      // maximum volume for sound
	snd_MusicVolume;    // maximum volume for music
#endif
int dmxCodes[NUM_SCARDS]; // the dmx code for a given card

#if (APPVER_DOOMREV < AV_DR_DM12)
musicinfo_t *snd_mus = NULL;
musicinfo_t *snd_mus2 = NULL;
sfxcache_t sfxlump[NUMSFX];
musiccache_t muslump[NUMMUSIC];
#endif

int     snd_SBport, snd_SBirq, snd_SBdma;       // sound blaster variables
int     snd_Mport;                              // midi variables

extern boolean  snd_MusicAvail, // whether music is available
		snd_SfxAvail;   // whether sfx are available

#if (APPVER_DOOMREV < AV_DR_DM12)
void I_PauseSong(void)
{
	if (snd_mus)
		MUS_PauseSong(snd_mus->cache->handle);
}

void I_ResumeSong(void)
{
	if (snd_mus)
		MUS_ResumeSong(snd_mus->cache->handle);
}

void I_SetMusicVolume(int volume)
{
  MUS_SetMasterVolume(volume);
}

void I_SetSfxVolume(int volume)
{
  snd_MaxVolume = volume;
}

void I_SetMasterVolume(int volume)
{
	I_SetMusicVolume(volume);
	I_SetSfxVolume(volume);
}

void I_GetMusicLumpNum(musicinfo_t *mus)
{
	char namebuf[9];
	int i;
	musiccache_t *cache;
	sprintf(namebuf, "d_%s", mus->name);

	i = mus - S_music;
	cache = &muslump[i];
	mus->cache = cache;
	cache->lumpnum = W_GetNumForName(namebuf);
	cache->handle = -1;
}

void I_StopSong(void)
{
	musiccache_t *cache;
	if (snd_mus2)
	{
		cache = snd_mus2->cache;
		if (MUS_QrySongPlaying(cache->handle))
		{
			if (MUS_StopSong(cache->handle) < 0)
				fprintf(stderr, "could not stop song\n");
		}
		if (MUS_UnregisterSong(cache->handle) < 0)
			fprintf(stderr, "could not un-register song\n");
		cache->handle = -1;
		snd_mus2 = NULL;
	}
	if (snd_mus)
	{
		cache = snd_mus->cache;
		if (cache->handle != -1)
		{
			if (MUS_QrySongPlaying(cache->handle))
			{
				if (MUS_FadeOutSong(cache->handle, 1000) < 0)
					fprintf(stderr, "could not fadeoutsong\n");
			}
		}
		snd_mus2 = snd_mus;
	}
	snd_mus = NULL;
}

void I_RegisterSong(musicinfo_t *mus)
{
	musiccache_t* cache;
	if (!mus->cache)
		I_GetMusicLumpNum(mus);
	cache = mus->cache;

	if (cache->handle == -1)
	{
		cache->data = W_CacheLumpNum(cache->lumpnum, PU_MUSIC);
		cache->handle = MUS_RegisterSong(cache->data);
		if (cache->handle < 0)
			fprintf(stderr, "could not register song\n");
	}
}

void I_StartSongNoLoop(musicinfo_t *mus)
{
	musiccache_t* cache;
	I_StopSong();
	I_RegisterSong(mus);
	cache = mus->cache;
	if (MUS_ChainSong(cache->handle, -1) < 0)
		fprintf(stderr, "could not chain song\n");

	if (MUS_PlaySong(cache->handle, snd_MaxVolume) < 0)
	{
		fprintf(stderr, "interesting- no music\n");
		return;
	}
	snd_mus = mus;
}

void I_StartSongLoop(musicinfo_t *mus)
{
	musiccache_t* cache;
	if (snd_mus == mus)
		return;
	I_StopSong();
	I_RegisterSong(mus);
	cache = mus->cache;
	if (MUS_ChainSong(cache->handle, cache->handle) < 0)
		fprintf(stderr, "could not chain song\n");

	if (MUS_PlaySong(cache->handle, snd_MaxVolume) < 0)
	{
		fprintf(stderr, "interesting- no music\n");
		return;
	}
	snd_mus = mus;
}

void I_StartSong(musicinfo_t *mus, boolean looping)
{
	musiccache_t* cache;
	musiccache_t* cache2;
	if (mus == snd_mus)
		return;
	if (snd_mus2 && snd_mus2 != snd_mus)
	{
		cache = snd_mus2->cache;
		if (MUS_UnregisterSong(cache->handle) < 0)
			fprintf(stderr, "could not un-register song\n");
		cache->handle = -1;
	}
	I_RegisterSong(mus);
	cache = mus->cache;
	if (looping)
	{
		if (MUS_ChainSong(cache->handle, cache->handle) < 0)
			fprintf(stderr, "could not chain song\n");
	}
	else
	{
		if (MUS_ChainSong(cache->handle, -1) < 0)
			fprintf(stderr, "could not unchain song\n");
	}

	if (snd_mus)
	{
		cache2 = snd_mus->cache;
		if (cache2->handle != -1 && MUS_QrySongPlaying(cache2->handle))
			MUS_StopSong(cache2->handle);
		snd_mus2 = snd_mus;
	}
	else
	{
		if (MUS_PlaySong(cache->handle, snd_MaxVolume) < 0)
			fprintf(stderr, "interesting- no music\n");
	}
	if (MUS_PlaySong(cache->handle, snd_MaxVolume) < 0)
		fprintf(stderr, "interesting- no music\n");
	snd_mus = mus;
}

void I_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char namebuf[9];
	int i;
	sfxcache_t *cache;

	namebuf[0] = 'd';

	if (sfx->link)
		sfx = sfx->link;

	i = sfx - S_sfx;
	cache = &sfxlump[i];
	sfx->cache = cache;

	strcpy(&namebuf[2], sfx->name);
	namebuf[1] = snd_prefixen[snd_SfxDevice];

	cache->lumpnum = W_GetNumForName(namebuf);
	cache->data = NULL;
}

void I_StartSound(channel_t *channel, int vol, int sep, int pitch, int priority)
{
	sfxcache_t *cache;
	int i;

	cache = channel->sfxinfo->cache;
	if (cache->lumpnum != -1)
	{
		cache->data = W_CacheLumpNum(cache->lumpnum, PU_STATIC);
		if (!cache->data)
			I_Error("could not cache sound lump");
		i = channel->sfxinfo - S_sfx;
		// hacks out certain PC sounds
		if (snd_SfxDevice == snd_PC
			&& ((i >= sfx_posact && i <= sfx_dmact)
				|| i == sfx_dmpain
				|| i == sfx_popain
				|| i == sfx_sawidl))
		{
			channel->handle = -1;
			return;
		}
		channel->handle = SFX_PlayPatch(cache->data, sep, pitch, vol, priority);
	}
	else
		fprintf(stderr, "Bad lump number for sound\n");
}

void I_StopSound(channel_t *channel)
{
	if (channel->handle != -1 && SFX_Playing(channel->handle))
		SFX_StopPatch(channel->handle);
}

int I_SoundIsPlaying(channel_t *channel)
{
	return SFX_Playing(channel->handle);
}

void I_UpdateSoundParams(channel_t *channel, int vol, int sep)
{
	if (SFX_Playing(channel->handle))
		SFX_SetOrigin(channel->handle, sep, vol);
}

#else

void I_PauseSong(int handle)
{
  MUS_PauseSong(handle);
}

void I_ResumeSong(int handle)
{
  MUS_ResumeSong(handle);
}

void I_SetMusicVolume(int volume)
{
  MUS_SetMasterVolume(volume);
  snd_MusicVolume = volume;
}

void I_SetSfxVolume(int volume)
{
  snd_MaxVolume = volume;
}

/*
 *
 *                              SONG API
 *
 */

int I_RegisterSong(void *data)
{
  int rc = MUS_RegisterSong(data);
#ifdef SNDDEBUG
  if (rc<0) printf("MUS_Reg() returned %d\n", rc);
#endif
  return rc;
}

void I_UnRegisterSong(int handle)
{
  int rc = MUS_UnregisterSong(handle);
#ifdef SNDDEBUG
  if (rc < 0) printf("MUS_Unreg() returned %d\n", rc);
#endif
}

int I_QrySongPlaying(int handle)
{
  int rc = MUS_QrySongPlaying(handle);
#ifdef SNDDEBUG
  if (rc < 0) printf("MUS_QrySP() returned %d\n", rc);
#endif
  return rc;
}

// Stops a song.  MUST be called before I_UnregisterSong().

void I_StopSong(int handle)
{
  int rc;
  rc = MUS_StopSong(handle);
#ifdef SNDDEBUG
  if (rc < 0) printf("MUS_StopSong() returned %d\n", rc);
#endif
  // Fucking kluge pause
  {
	int s;
	extern volatile int ticcount;
	for (s=ticcount ; ticcount - s < 10 ; );
  }
}

void I_PlaySong(int handle, boolean looping)
{
  int rc;
  rc = MUS_ChainSong(handle, looping ? handle : -1);
#ifdef SNDDEBUG
  if (rc < 0) printf("MUS_ChainSong() returned %d\n", rc);
#endif
  rc = MUS_PlaySong(handle, snd_MusicVolume);
#ifdef SNDDEBUG
  if (rc < 0) printf("MUS_PlaySong() returned %d\n", rc);
#endif

}

/*
 *
 *                                 SOUND FX API
 *
 */

// Gets lump nums of the named sound.  Returns pointer which will be
// passed to I_StartSound() when you want to start an SFX.  Must be
// sure to pass this to UngetSoundEffect() so that they can be
// freed!


int I_GetSfxLumpNum(sfxinfo_t *sound)
{
  char namebuf[9];

  if (sound->link) sound = sound->link;
  sprintf(namebuf, "d%c%s", snd_prefixen[snd_SfxDevice], sound->name);
  return W_GetNumForName(namebuf);

}

#if (APPVER_DOOMREV < AV_DR_DM1666P)
int I_StartSound (void *data, int vol, int sep, int pitch, int priority)
#else
int I_StartSound (int id, void *data, int vol, int sep, int pitch, int priority)
#endif
{
  // hacks out certain PC sounds
  if (snd_SfxDevice == snd_PC
	&& (data == S_sfx[sfx_posact].data
	||  data == S_sfx[sfx_bgact].data
	||  data == S_sfx[sfx_dmact].data
	||  data == S_sfx[sfx_dmpain].data
	||  data == S_sfx[sfx_popain].data
	||  data == S_sfx[sfx_sawidl].data)) return -1;

  else
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	return SFX_PlayPatch(data, sep, pitch, vol, priority);
#else
	return SFX_PlayPatch(data, sep, pitch, vol, 0, 100);
#endif

}

void I_StopSound(int handle)
{
//  extern volatile long gDmaCount;
//  long waittocount;
  SFX_StopPatch(handle);
//  waittocount = gDmaCount + 2;
//  while (gDmaCount < waittocount) ;
}

int I_SoundIsPlaying(int handle)
{
  return SFX_Playing(handle);
}

#if (APPVER_DOOMREV < AV_DR_DM1666P)
void I_UpdateSoundParams(int handle, int vol, int sep)
{
  SFX_SetOrigin(handle, sep, vol);
}
#else
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
  SFX_SetOrigin(handle, pitch, sep, vol);
}
#endif

#endif

/*
 *
 *                                                      SOUND STARTUP STUFF
 *
 *
 */

//
// Why PC's Suck, Reason #8712
//

void I_sndArbitrateCards(void)
{
  // boolean gus, adlib, pc, sb, midi, ensoniq, codec;
#if (APPVER_DOOMREV < AV_DR_DM18)
  boolean gus, adlib, pc, sb, midi;
#else
  boolean codec, ensoniq, gus, adlib, pc, sb, midi;
#endif
  int i, rc, mputype, p, opltype, wait, dmxlump;

  snd_MaxVolume = 127;

  snd_MusicDevice = snd_DesiredMusicDevice;
  snd_SfxDevice = snd_DesiredSfxDevice;

  // check command-line parameters- overrides config file
  //
  if (M_CheckParm("-nosound")) snd_MusicDevice = snd_SfxDevice = snd_none;
  if (M_CheckParm("-nosfx")) snd_SfxDevice = snd_none;
  if (M_CheckParm("-nomusic")) snd_MusicDevice = snd_none;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
#if (APPVER_DOOMREV < AV_DR_DM12)
  if (snd_MusicDevice == snd_SB)
#else
  if (snd_MusicDevice == snd_SB || snd_MusicDevice == snd_PAS)
#endif
	snd_MusicDevice = snd_Adlib;
  if (snd_MusicDevice > snd_MPU)
	snd_MusicDevice = snd_MPU;
#else
  if (snd_MusicDevice > snd_MPU && snd_MusicDevice <= snd_MPU3)
	snd_MusicDevice = snd_MPU;
  if (snd_MusicDevice == snd_SB)
	snd_MusicDevice = snd_Adlib;
  if (snd_MusicDevice == snd_PAS)
	snd_MusicDevice = snd_Adlib;
#endif

  // figure out what i've got to initialize
  //
  gus = snd_MusicDevice == snd_GUS || snd_SfxDevice == snd_GUS;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
  sb = snd_SfxDevice == snd_SB;
#else
  sb = snd_SfxDevice == snd_SB || snd_MusicDevice == snd_SB;
#endif
#if (APPVER_DOOMREV >= AV_DR_DM18)
  ensoniq = snd_SfxDevice == snd_ENS ;
  codec = snd_SfxDevice == snd_CODEC ;
#endif
  adlib = snd_MusicDevice == snd_Adlib ;
  pc = snd_SfxDevice == snd_PC;
  midi = snd_MusicDevice == snd_MPU;

#if (APPVER_DOOMREV >= AV_DR_DM18)
  // initialize whatever i've got
  //
  if (ensoniq)
  {
	if (devparm)
	  printf("ENSONIQ\n");
	if (ENS_Detect())
	  printf("Dude.  The ENSONIQ ain't responding.\n");
  }
  if (codec)
  {
	if (devparm)
	  printf("CODEC p=0x%x, d=%d\n", snd_SBport, snd_SBdma);
	if (CODEC_Detect(&snd_SBport, &snd_SBdma))
	  printf("CODEC.  The CODEC ain't responding.\n");
  }
#endif
  if (gus)
  {
	if (devparm)
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "GUS\n");
#else
	  printf("GUS\n");
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	fprintf(stderr, "GUS1\n");
#endif
	if (GF1_Detect())
#if (APPVER_DOOMREV < AV_DR_DM12)
		fprintf(stderr, "Dude.  The GUS ain't responding.\n");
#else
		printf("Dude.  The GUS ain't responding.\n");
#endif
	else
	{
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	  fprintf(stderr, "GUS2\n");
	  if (commercial)
	    dmxlump = W_GetNumForName("dmxgusc");
	  else
#endif
	    dmxlump = W_GetNumForName("dmxgus");
#if (APPVER_DOOMREV >= AV_DR_DM12)
	  GF1_SetMap(W_CacheLumpNum(dmxlump, PU_CACHE), lumpinfo[dmxlump].size);
#endif
	}

  }
  if (sb)
  {
	if(devparm)
	{
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "cfg p=0x%x, i=%d, d=%d\n",
#else
	  printf("cfg p=0x%x, i=%d, d=%d\n",
#endif
	  snd_SBport, snd_SBirq, snd_SBdma);
	}
	if (SB_Detect(&snd_SBport, &snd_SBirq, &snd_SBdma, 0))
	{
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "SB isn't responding at p=0x%x, i=%d, d=%d\n",
#else
	  printf("SB isn't responding at p=0x%x, i=%d, d=%d\n",
#endif
	  snd_SBport, snd_SBirq, snd_SBdma);
	}
	else SB_SetCard(snd_SBport, snd_SBirq, snd_SBdma);

	if(devparm)
	{
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "SB_Detect returned p=0x%x,i=%d,d=%d\n",
#else
	  printf("SB_Detect returned p=0x%x,i=%d,d=%d\n",
#endif
	  snd_SBport, snd_SBirq, snd_SBdma);
	}
  }

  if (adlib)
  {
	if(devparm)
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "Adlib\n");
#else
	  printf("Adlib\n");
#endif
	if (AL_Detect(&wait,0))
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "Dude.  The Adlib isn't responding.\n");
#else
	  printf("Dude.  The Adlib isn't responding.\n");
#endif
	else
		AL_SetCard(wait, W_CacheLumpName("genmidi", PU_STATIC));
  }

  if (midi)
  {
	if (devparm)
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "Midi\n");
#else
	  printf("Midi\n");
#endif
	if (devparm)
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "cfg p=0x%x\n", snd_Mport);
#else
	  printf("cfg p=0x%x\n", snd_Mport);
#endif

	if (MPU_Detect(&snd_Mport, &i))
#if (APPVER_DOOMREV < AV_DR_DM12)
	  fprintf(stderr, "The MPU-401 isn't reponding @ p=0x%x.\n", snd_Mport);
#else
	  printf("The MPU-401 isn't reponding @ p=0x%x.\n", snd_Mport);
#endif
	else MPU_SetCard(snd_Mport);
  }

}

// inits all sound stuff

void I_StartupSound (void)
{
  int rc, i;

  if (devparm)
#if (APPVER_DOOMREV < AV_DR_DM12)
	fprintf(stderr, "I_StartupSound: forking sound daemon.\n");
#else
	printf("I_StartupSound: Hope you hear a pop.\n");
#endif

  // initialize dmxCodes[]
  dmxCodes[0] = 0;
  dmxCodes[snd_PC] = AHW_PC_SPEAKER;
  dmxCodes[snd_Adlib] = AHW_ADLIB;
  dmxCodes[snd_SB] = AHW_SOUND_BLASTER;
#if (APPVER_DOOMREV >= AV_DR_DM12)
  dmxCodes[snd_PAS] = AHW_MEDIA_VISION;
#endif
  dmxCodes[snd_GUS] = AHW_ULTRA_SOUND;
  dmxCodes[snd_MPU] = AHW_MPU_401;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  dmxCodes[snd_AWE] = AHW_AWE32;
#if (APPVER_DOOMREV >= AV_DR_DM18)
  dmxCodes[snd_ENS] = AHW_ENSONIQ;
  dmxCodes[snd_CODEC] = AHW_CODEC;
#endif
#endif

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  // inits sound library timer stuff
  I_StartupTimer();
#endif

  // pick the sound cards i'm going to use
  //
  I_sndArbitrateCards();

  if (devparm)
  {
#if (APPVER_DOOMREV < AV_DR_DM12)
	printf("Music device #%d & dmxCode=%d\n", snd_MusicDevice,
	  dmxCodes[snd_MusicDevice]);
	printf("Sfx device #%d & dmxCode=%d\n", snd_SfxDevice,
	  dmxCodes[snd_SfxDevice]);
#else
	printf("  Music device #%d & dmxCode=%d\n", snd_MusicDevice,
	  dmxCodes[snd_MusicDevice]);
	printf("  Sfx device #%d & dmxCode=%d\n", snd_SfxDevice,
	  dmxCodes[snd_SfxDevice]);
#endif
  }

#if (APPVER_DOOMREV >= AV_DR_DM12 && APPVER_DOOMREV < AV_DR_DM1666P)
  // inits sound library timer stuff
  I_StartupTimer();
#endif

  // inits DMX sound library
#if (APPVER_DOOMREV < AV_DR_DM12)
  if (snd_MusicDevice != snd_none || snd_SfxDevice != snd_none)
  {
	fprintf(stderr, "calling DMX_Init\n");
	rc = DMX_Init(SND_TICRATE, SND_MAXSONGS, dmxCodes[snd_MusicDevice],
		dmxCodes[snd_SfxDevice]);
  }
  else
	  rc = 0;

  snd_MusicAvail = ((dmxCodes[snd_MusicDevice] & rc) != 0) != 0;
  snd_SfxAvail = ((dmxCodes[snd_SfxDevice] & rc) != 0) != 0;

  if (devparm)
	printf("DMX_Init() returned %d\n", rc);
#else
  printf("  calling DMX_Init\n");
  rc = DMX_Init(SND_TICRATE, SND_MAXSONGS, dmxCodes[snd_MusicDevice],
	dmxCodes[snd_SfxDevice]);

  if (devparm)
	printf("  DMX_Init() returned %d\n", rc);
#endif

}

// shuts down all sound stuff

void I_ShutdownSound (void)
{
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  S_PauseSound();
  {
	int s;
	extern volatile int ticcount;
	for (s=ticcount + 30; s != ticcount ; );
  }
#endif
#if (APPVER_DOOMREV < AV_DR_DM12)
  if (snd_MusicAvail || snd_SfxAvail)
#endif
  DMX_DeInit();
}

void I_SetChannels(int channels)
{
  WAV_PlayMode(channels, SND_SAMPLERATE);
}
