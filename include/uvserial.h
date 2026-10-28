/*
* UAE - The Un*x Amiga Emulator
*
* United Video (Prevue Networks) multiserial Zorro II card
*
*/

#ifndef UAE_UVSERIAL_H
#define UAE_UVSERIAL_H

#include "uae/types.h"

extern bool uvserial_init(struct autoconfig_info *aci);
extern int log_uvserial;

#endif /* UAE_UVSERIAL_H */
