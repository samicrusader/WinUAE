/*
* UAE - The Un*x Amiga Emulator
*
* Pioneer LD-V8000 LaserDisc player emulation (ASCII serial protocol)
*
*/

#ifndef UAE_PIONEERLD_H
#define UAE_PIONEERLD_H

#include "uae/types.h"

/* host -> player, one byte */
void pioneerld_put(uae_u8 b);
/* player -> host, -1 if nothing pending */
int pioneerld_get(void);

/* make sure the module is alive and hooked into vsync */
void pioneerld_activate(void);
void pioneerld_reset(void);
/* emulator start / config load: the next pioneerld_activate() powers the player on.
   Amiga resets never reset the player. */
void pioneerld_power_on(void);
/* the Amiga was reset: the player keeps going, only its frame count is resynchronised */
void pioneerld_amiga_reset(void);

/* true when the disc is playing or paused on a picture (genlock should show video) */
bool pioneerld_active(void);
bool pioneerld_video_enabled(void);

extern int log_pioneerld;

#endif /* UAE_PIONEERLD_H */
