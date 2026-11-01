/*
* UAE - The Un*x Amiga Emulator
*
* Hayes-style modem on a serial line, with Telnet (or raw TCP) as the phone
* line. A TCP connection to the modem's listener is an incoming call: the
* modem says RING (and auto-answers on S0), ATA answers, ATDhost[:port]
* dials out. Used by the built-in serial port (win32.serial_port=MODEM://...)
* and by UV card units (uvserial_portN=MODEM://...).
*
*   MODEM://[host:port][/raw][/speed=N]
*
*   host:port  listen there for calls; leave it out for a dial-out-only modem
*   /raw       plain TCP, no Telnet negotiation or IAC escaping
*   /speed=N   the number after CONNECT (default: the DTE rate the Amiga set)
*/

#ifndef UAE_SERMODEM_H
#define UAE_SERMODEM_H

#include "uae/types.h"

struct sermodem;

struct sermodem *sermodem_open(const TCHAR *name, const TCHAR *spec);
void sermodem_free(struct sermodem *m);

/* call regularly (every few scanlines): calls, rings, dialling, the +++ guard time */
void sermodem_poll(struct sermodem *m);
/* bytes from the modem to the computer (results, echo, line data) */
int sermodem_avail(struct sermodem *m);
int sermodem_read(struct sermodem *m, uae_u8 *buf, int size);
/* bytes from the computer to the modem (AT commands, or line data when on line) */
void sermodem_write(struct sermodem *m, const uae_u8 *buf, int size);
/* modem status lines as the computer sees them */
bool sermodem_carrier(struct sermodem *m);
bool sermodem_ring(struct sermodem *m);
/* computer's DTR (AT&D2: dropping it on line hangs up) */
void sermodem_dtr(struct sermodem *m, bool on);
/* computer's baud rate, the default CONNECT speed */
void sermodem_set_dte_rate(struct sermodem *m, int baud);

#endif /* UAE_SERMODEM_H */
