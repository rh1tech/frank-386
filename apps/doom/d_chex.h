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

#ifndef D_CHEX_H
#define D_CHEX_H

//
//	M_Menu.C
//
#undef QUITMSG
#define QUITMSG	"Don't give up now...do \nyou still wish to quit?"
#undef LOADNET
#define LOADNET 	"you can't do load while in a net quest!\n\n"PRESSKEY
#undef QLOADNET
#define QLOADNET	"you can't quickload during a netquest!\n\n"PRESSKEY
#undef QSPROMPT
#define QSPROMPT 	"quicksave over your quest named\n\n'%s'?\n\n"PRESSYN
#undef QLPROMPT
#define QLPROMPT	"do you want to quickload the quest named\n\n'%s'?\n\n"PRESSYN

#undef NEWGAME
#define NEWGAME	\
"you can't start a new quest\n"\
"while in a network quest.\n\n"PRESSKEY

#undef NIGHTMARE
#define NIGHTMARE	\
"Careful, this will be tough.\n"\
"Do you wish to continue?\n\n"PRESSYN

#undef SWSTRING
#define SWSTRING	\
"this is Chex(R) Quest. look for\n\n"\
"future levels at www.chexquest.com.\n\n"PRESSKEY

#undef NETEND
#define NETEND	"you can't end a netquest!\n\n"PRESSKEY
#undef ENDGAME
#define ENDGAME	"are you sure you want to end the quest?\n\n"PRESSYN

//
//	P_inter.C
//
#undef GOTARMOR
#define GOTARMOR	"Picked up the Chex(R) Armor."
#undef GOTMEGA
#define GOTMEGA	"Picked up the Super Chex(R) Armor!"
#undef GOTHTHBONUS
#define GOTHTHBONUS	"Picked up a glass of water."
#undef GOTARMBONUS
#define GOTARMBONUS	"Picked up slime repellent."
#undef GOTSTIM
#define GOTSTIM	"Picked up a bowl of fruit."
#undef GOTMEDINEED
#define GOTMEDINEED	"Picked up some needed vegetables!"
#undef GOTMEDIKIT
#define GOTMEDIKIT	"Picked up a bowl of vegetables."
#undef GOTSUPER
#define GOTSUPER	"Supercharge Breakfast!"

#undef GOTBLUECARD
#define GOTBLUECARD	"Picked up a blue key."
#undef GOTYELWCARD
#define GOTYELWCARD	"Picked up a yellow key."
#undef GOTREDCARD
#define GOTREDCARD	"Picked up a red key."
#undef GOTBLUESKUL
#define GOTBLUESKUL	GOTBLUECARD
#undef GOTYELWSKUL
#define GOTYELWSKUL	GOTYELWCARD
#undef GOTREDSKULL
#define GOTREDSKULL	GOTREDCARD

#undef GOTSUIT
#define GOTSUIT	"Slimeproof Suit"

#undef GOTCLIP
#define GOTCLIP	"Picked up a mini zorch recharge."
#undef GOTCLIPBOX
#define GOTCLIPBOX	"Picked up a mini zorch pack."
#undef GOTROCKET
#define GOTROCKET	"Picked up a zorch propulsor recharge."
#undef GOTROCKBOX
#define GOTROCKBOX	"Picked up a zorch propulsor pack."
#undef GOTCELL
#define GOTCELL	"Picked up a phasing zorcher recharge."
#undef GOTCELLBOX
#define GOTCELLBOX	"Picked up a phasing zorcher pack."
#undef GOTSHELLS
#define GOTSHELLS	"Picked up a large zorcher recharge."
#undef GOTSHELLBOX
#define GOTSHELLBOX	"Picked up a large zorcher pack."
#undef GOTBACKPACK
#define GOTBACKPACK	"Picked up a Zorchpak!"

#undef GOTBFG9000
#define GOTBFG9000	"You got the LAZ Device!"
#undef GOTCHAINGUN
#define GOTCHAINGUN	"You got the Rapid Zorcher!"
#undef GOTCHAINSAW
#define GOTCHAINSAW	"You got the Super Bootspork!"
#undef GOTLAUNCHER
#define GOTLAUNCHER	"You got the Zorch Propulsor!"
#undef GOTPLASMA
#define GOTPLASMA	"You got the Phasing Zorcher!"
#undef GOTSHOTGUN
#define GOTSHOTGUN	"You got the Large Zorcher!"
#undef GOTSHOTGUN2
#define GOTSHOTGUN2	"You got the Super Large Zorcher!"

//
//	HU_stuff.C
//

#undef HUSTR_E1M1
#define HUSTR_E1M1	"E1M1: Landing Zone"
#undef HUSTR_E1M2
#define HUSTR_E1M2	"E1M2: Storage Facility"
#undef HUSTR_E1M3
#define HUSTR_E1M3	"E1M3: Experimental Lab"
#undef HUSTR_E1M4
#define HUSTR_E1M4	"E1M4: Arboretum"
#undef HUSTR_E1M5
#define HUSTR_E1M5	"E1M5: Caverns of Bazoik"
#undef HUSTR_E1M6
#define HUSTR_E1M6	HUSTR_E1M5
#undef HUSTR_E1M7
#define HUSTR_E1M7	HUSTR_E1M5
#undef HUSTR_E1M8
#define HUSTR_E1M8	HUSTR_E1M5
#undef HUSTR_E1M9
#define HUSTR_E1M9	HUSTR_E1M5

#undef HUSTR_E2M1
#define HUSTR_E2M1	HUSTR_E1M5
#undef HUSTR_E2M2
#define HUSTR_E2M2	HUSTR_E1M5
#undef HUSTR_E2M3
#define HUSTR_E2M3	HUSTR_E1M5
#undef HUSTR_E2M4
#define HUSTR_E2M4	HUSTR_E1M5
#undef HUSTR_E2M5
#define HUSTR_E2M5	HUSTR_E1M5
#undef HUSTR_E2M6
#define HUSTR_E2M6	HUSTR_E1M5
#undef HUSTR_E2M7
#define HUSTR_E2M7	HUSTR_E1M5
#undef HUSTR_E2M8
#define HUSTR_E2M8	HUSTR_E1M5
#undef HUSTR_E2M9
#define HUSTR_E2M9	HUSTR_E1M5

#undef HUSTR_E3M1
#define HUSTR_E3M1	HUSTR_E1M5
#undef HUSTR_E3M2
#define HUSTR_E3M2	HUSTR_E1M5
#undef HUSTR_E3M3
#define HUSTR_E3M3	HUSTR_E1M5
#undef HUSTR_E3M4
#define HUSTR_E3M4	HUSTR_E1M5
#undef HUSTR_E3M5
#define HUSTR_E3M5	HUSTR_E1M5
#undef HUSTR_E3M6
#define HUSTR_E3M6	HUSTR_E1M5
#undef HUSTR_E3M7
#define HUSTR_E3M7	HUSTR_E1M5
#undef HUSTR_E3M8
#define HUSTR_E3M8	HUSTR_E1M5
#undef HUSTR_E3M9
#define HUSTR_E3M9	HUSTR_E1M5

#undef HUSTR_E4M1
#define HUSTR_E4M1	HUSTR_E1M5
#undef HUSTR_E4M2
#define HUSTR_E4M2	HUSTR_E1M5
#undef HUSTR_E4M3
#define HUSTR_E4M3	HUSTR_E1M5
#undef HUSTR_E4M4
#define HUSTR_E4M4	HUSTR_E1M5
#undef HUSTR_E4M5
#define HUSTR_E4M5	HUSTR_E1M5
#undef HUSTR_E4M6
#define HUSTR_E4M6	HUSTR_E1M5
#undef HUSTR_E4M7
#define HUSTR_E4M7	HUSTR_E1M5
#undef HUSTR_E4M8
#define HUSTR_E4M8	HUSTR_E1M5
#undef HUSTR_E4M9
#define HUSTR_E4M9	HUSTR_E1M5

#undef HUSTR_CHATMACRO1
#define HUSTR_CHATMACRO1	"I'm ready to zorch!"
#undef HUSTR_CHATMACRO2
#define HUSTR_CHATMACRO2	"I'm feeling great!"
#undef HUSTR_CHATMACRO3
#define HUSTR_CHATMACRO3	"I'm getting pretty gooed up!"
#undef HUSTR_CHATMACRO4
#define HUSTR_CHATMACRO4	"Somebody help me!"
#undef HUSTR_CHATMACRO5
#define HUSTR_CHATMACRO5	"Go back to your own dimension!"
#undef HUSTR_CHATMACRO6
#define HUSTR_CHATMACRO6	"Stop that Flemoid"
#undef HUSTR_CHATMACRO7
#define HUSTR_CHATMACRO7	"I think I'm lost!"
#undef HUSTR_CHATMACRO8
#define HUSTR_CHATMACRO8	"I'll get you out of this gunk."

#undef HUSTR_TALKTOSELF1
#define HUSTR_TALKTOSELF1	"I'm feeling great."
#undef HUSTR_TALKTOSELF2
#define HUSTR_TALKTOSELF2	"I think I'm lost."
#undef HUSTR_TALKTOSELF3
#define HUSTR_TALKTOSELF3	"Oh No..."
#undef HUSTR_TALKTOSELF4
#define HUSTR_TALKTOSELF4	"Gotta break free."
#undef HUSTR_TALKTOSELF5
#define HUSTR_TALKTOSELF5	"Hurry!"

//
#undef STSTR_DQDON
#define STSTR_DQDON		"Invincible Mode On"
#undef STSTR_DQDOFF
#define STSTR_DQDOFF	"Invincible Mode Off"

#undef STSTR_KFAADDED
#define STSTR_KFAADDED	"Super Zorch Added"
#undef STSTR_FAADDED
#define STSTR_FAADDED	"Zorch Added"

#undef STSTR_CHOPPERS
#define STSTR_CHOPPERS	"... Eat Chex(R)!"

//
//	F_Finale.C
//
#undef E1TEXT
#define E1TEXT \
"Mission accomplished.\n"\
"\n" \
"Are you prepared for the next mission?\n" \
"\n" \
"\n" \
"\n" \
"\n" \
"\n" \
"\n" \
"Press the escape key to continue...\n"

#undef E2TEXT
#define E2TEXT "You've done it!"

#undef E3TEXT
#define E3TEXT "Wonderful Job!"

#undef E4TEXT
#define E4TEXT "Fantastic"

// after level 6, put this:

#undef C1TEXT
#define C1TEXT "Great!"

// After level 11, put this:

#undef C2TEXT
#define C2TEXT "Way to go!"

// After level 20, put this:

#undef C3TEXT
#define C3TEXT "Thanks for the help!"

// After level 29, put this:

#undef C4TEXT
#define C4TEXT "Great!\n"


// Before level 31, put this:

#undef C5TEXT
#define C5TEXT "Fabulous!"

// Before level 32, put this:

#undef C6TEXT
#define C6TEXT "CONGRATULATIONS!\n"

// after map 06	

#undef P1TEXT
#define P1TEXT "Nicely done!"\

// after map 11

#undef P2TEXT
#define P2TEXT "Nice Job!"

// after map 20

#undef P3TEXT
#define P3TEXT "Well done!"

// after map 30

#undef P4TEXT
#define P4TEXT "Eat Chex(R)!"

// before map 31

#undef P5TEXT
#define P5TEXT "Are you ready?"

// before map 32

#undef P6TEXT
#define P6TEXT "Were you ready?"


#undef T1TEXT
#define T1TEXT "There's more to come..."

#undef T2TEXT
#define T2TEXT "Keep up the good work!"

#undef T3TEXT
#define T3TEXT "Get ready!."

#undef T4TEXT
#define T4TEXT "Be Proud."

#undef T5TEXT
#define T5TEXT "Wow!"

#undef T6TEXT
#define T6TEXT "Great."



//
// Character cast strings F_FINALE.C
//
#undef CC_ZOMBIE
#define CC_ZOMBIE	"FLEMOIDUS COMMONUS"
#undef CC_SHOTGUN
#define CC_SHOTGUN	"FLEMOIDUS BIPEDICUS"
#undef CC_IMP
#define CC_IMP	"FLEMOIDUS BIPEDICUS WITH ARMOR"
#undef CC_DEMON
#define CC_DEMON	"FLEMOIDUS CYCLOPTIS"
#undef CC_BARON
#define CC_BARON	"THE FLEMBRANE"

#endif
