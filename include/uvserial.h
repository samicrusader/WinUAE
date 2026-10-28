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

/* od-win32/parser.cpp: slot 0..3 = unit data, 4..7 = unit CD level feed */
bool uvsock_open(int slot, const TCHAR *addr);
void uvsock_close(int slot);
bool uvsock_connected(int slot);
int uvsock_read(int slot, uae_u8 *buf, int size);
void uvsock_write(int slot, const uae_u8 *buf, int size);

#endif /* UAE_UVSERIAL_H */
