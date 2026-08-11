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

#ifndef __SOUND__
#define __SOUND__

#define SND_TICRATE             140             // tic rate for updating sound
#define SND_MAXSONGS    40              // max number of songs in game
#define SND_SAMPLERATE  11025   // sample rate of sound effects

typedef enum
{
  snd_none,
  snd_PC,
  snd_Adlib,
  snd_SB,
#if (APPVER_DOOMREV >= AV_DR_DM12)
  snd_PAS,
#endif
  snd_GUS,
  snd_MPU,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  snd_MPU2,
  snd_MPU3,
  snd_AWE,
#if (APPVER_DOOMREV >= AV_DR_DM18)
  snd_ENS,
  snd_CODEC,
#endif
#endif
  NUM_SCARDS
} cardenum_t;

#endif
