#ifndef _DMX_H_
#define _DMX_H_

/*
 * DMX API surface used by the original DOOM i_sound.c.
 *
 * Native FDOS implements only the backends which correspond to hardware
 * actually exposed by murm386.  Keeping this header backend-neutral lets the
 * original high-level sound code compile without pulling the old DPMI DMX
 * implementation into every translation unit.
 */

typedef int SFX_HANDLE;

#if DMX_DIAG
/* Startup diagnostics: mirror short sound-init lines to screen and file. */
void DMX_DiagReset(void);
void DMX_Diag(const char *format, ...);
#else
#define DMX_DiagReset()
#define DMX_Diag(...)
#endif

/* Low-level AdLib backend hook still required by AL_SetCard(). */
void AL_RegisterTimbreBank(unsigned char *timbres);

/* Timer service API used by i_sound.c. */
void TSM_Install(int rate);
int TSM_NewService(int (*service)(void), int rate, int priority, int pause);
void TSM_DelService(int id);
void TSM_Remove(void);
void TSM_Yield(void);
void TSM_Lock(void);
void TSM_Unlock(void);

void MUS_PauseSong(int handle);
void MUS_ResumeSong(int handle);
void MUS_SetMasterVolume(int volume);
int MUS_RegisterSong(void *data);
int MUS_UnregisterSong(int handle);
int MUS_QrySongPlaying(int handle);
int MUS_StopSong(int handle);
int MUS_ChainSong(int handle, int next);
int MUS_PlaySong(int handle, int volume);
int MUS_FadeInSong(int handle, int ms);
int MUS_FadeOutSong(int handle, int ms);

int SFX_PlayPatch(void *data, int pitch, int sep, int vol, int unk1, int priority);
void SFX_StopPatch(int handle);
int SFX_Playing(int handle);
void SFX_SetOrigin(int handle, int pitch, int sep, int vol);

int SB_Detect(int *port, int *irq, int *dma, int *unk);
void SB_SetCard(int port, int irq, int dma);
int AL_Detect(int *port, int *unk);
void AL_SetCard(int port, void *data);
int MPU_Detect(int *port, int *unk);
void MPU_SetCard(int port);

int DMX_Init(int rate, int maxsongs, int music_device, int sfx_device);
void DMX_DeInit(void);
void WAV_PlayMode(int channels, int samplerate);

/* Legacy entry points remain declared for the non-ELF original DOS path. */
int GF1_Detect(void);
void GF1_SetMap(void *data, int len);
int CODEC_Detect(int *port, int *dma);
int ENS_Detect(void);

#define AHW_PC_SPEAKER      1
#define AHW_ADLIB           2
#define AHW_AWE32           4
#define AHW_SOUND_BLASTER   8
#define AHW_MPU_401        16
#define AHW_ULTRA_SOUND    32
#define AHW_MEDIA_VISION   64
#define AHW_ENSONIQ       256
#define AHW_CODEC         512

#endif /* _DMX_H_ */
