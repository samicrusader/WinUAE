/*
* UAE - The Un*x Amiga Emulator
*
* Serial lines over TCP: a listener that takes one client, or a client that
* connects out and, unless told not to, reconnects with back-off after the
* far end goes away or a connect fails. Everything is non-blocking, so a dead
* or slow peer never stalls emulation.
*
* Address syntax, shared by every user (built-in serial port, UV card units,
* the Hayes modem):
*
*   [TCP://]host:port[/option...]
*
*   /connect      connect out to host:port instead of listening on it
*   /noreconnect  with /connect: try once, never reconnect
*   /wait         listener: block emulation start until a client connects
*                 (upstream WinUAE's option; only the built-in port honours it)
*
* An empty host means 127.0.0.1.
*/

#ifndef UAE_SERSOCK_H
#define UAE_SERSOCK_H

#include "uae/types.h"

struct sersock;

struct sersock_spec
{
	TCHAR host[256];
	TCHAR port[32];
	bool connect;			/* /connect */
	bool reconnect;			/* cleared by /noreconnect */
	bool wait;				/* /wait */
	bool raw;				/* /raw (modem: no Telnet) */
	int speed;				/* /speed=N (modem: CONNECT text), 0 = DTE rate */
	bool haveaddr;			/* host:port was given */
};

#define SERSOCK_EV_CONNECTED	1	/* a client was accepted, or a connect completed */
#define SERSOCK_EV_DISCONNECTED	2	/* the peer went away (or was hung up locally) */
#define SERSOCK_EV_FAILED		4	/* a connect attempt failed */
#define SERSOCK_EV_REFUSED		8	/* ... and the far end refused it */

/* parse "[TCP://|MODEM://]host:port[/opt...]"; false on a malformed address */
bool sersock_parse(const TCHAR *spec, struct sersock_spec *s);
/* true if spec is a TCP address with /connect */
bool sersock_is_client(const TCHAR *spec);

struct sersock *sersock_listen(const TCHAR *name, const TCHAR *host, const TCHAR *port);
struct sersock *sersock_connect(const TCHAR *name, const TCHAR *host, const TCHAR *port, bool reconnect);
/* open from a parsed spec: /connect -> client, otherwise listener */
struct sersock *sersock_open(const TCHAR *name, const struct sersock_spec *s);
void sersock_free(struct sersock *s);

/* accept / connect progress / reconnect timer / pending output; returns connected */
bool sersock_poll(struct sersock *s);
bool sersock_connected(struct sersock *s);
/* events since the last call (SERSOCK_EV_*) */
int sersock_events(struct sersock *s);
/* bytes waiting to be read (0 if none or not connected) */
int sersock_avail(struct sersock *s);
/* up to size bytes; 0 when there is nothing yet; a closed peer is dropped */
int sersock_read(struct sersock *s, uae_u8 *buf, int size);
/* queue bytes for the peer; dropped (like an unplugged line) when nobody is connected */
void sersock_write(struct sersock *s, const uae_u8 *buf, int size);
/* drop the current peer; a reconnecting client tries again after its back-off */
void sersock_hangup(struct sersock *s);
/* listener: refuse callers beyond the first, sending them msg first (NULL = leave them queued) */
void sersock_set_busy_message(struct sersock *s, const char *msg);

#endif /* UAE_SERSOCK_H */
