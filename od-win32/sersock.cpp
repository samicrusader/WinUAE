/*
* UAE - The Un*x Amiga Emulator
*
* Serial lines over TCP (Win32 sockets): listener or reconnecting client.
* See include/sersock.h for the address syntax.
*/

#include "sysconfig.h"

#include <winsock2.h>
#include <Ws2tcpip.h>
#include <windows.h>

#include "sysdeps.h"
#include "sersock.h"

#define SERSOCK_TXBUF 16384
#define RECONNECT_FIRST_MS 500
#define RECONNECT_MAX_MS 10000
#define CONNECT_TIMEOUT_MS 10000

struct sersock
{
	TCHAR name[64];
	TCHAR host[256], port[32];
	bool client, reconnect;
	bool wsa;
	SOCKET lsock;			/* listener */
	SOCKET conn;			/* the peer, or a connect in progress */
	bool connecting;
	bool dead;				/* a one-shot client that has given up */
	uae_u64 connect_start;
	uae_u64 next_try;
	int backoff;
	int failures;
	int events;
	const char *busymsg;
	bool overflow_logged;
	uae_u8 tx[SERSOCK_TXBUF];
	int txlen;
};

static uae_u64 now_ms(void)
{
	return GetTickCount64();
}

static TCHAR *skipscheme(const TCHAR *spec)
{
	if (!_tcsnicmp(spec, _T("TCP://"), 6))
		return (TCHAR*)spec + 6;
	if (!_tcsnicmp(spec, _T("MODEM://"), 8))
		return (TCHAR*)spec + 8;
	if (!_tcsnicmp(spec, _T("TCP:"), 4))
		return (TCHAR*)spec + 4;
	if (!_tcsnicmp(spec, _T("MODEM:"), 6))
		return (TCHAR*)spec + 6;
	return (TCHAR*)spec;
}

bool sersock_parse(const TCHAR *spec, struct sersock_spec *s)
{
	TCHAR tmp[512];
	TCHAR *opt, *p;

	memset(s, 0, sizeof *s);
	s->reconnect = true;
	_tcsncpy(tmp, skipscheme(spec), 511);
	tmp[511] = 0;
	opt = _tcschr(tmp, '/');
	if (opt)
		*opt++ = 0;
	while (opt && *opt) {
		TCHAR *next = _tcschr(opt, '/');
		if (next)
			*next++ = 0;
		if (!_tcsicmp(opt, _T("connect")) || !_tcsicmp(opt, _T("client")))
			s->connect = true;
		else if (!_tcsicmp(opt, _T("noreconnect")) || !_tcsicmp(opt, _T("once")))
			s->reconnect = false;
		else if (!_tcsicmp(opt, _T("reconnect")))
			s->reconnect = true;
		else if (!_tcsicmp(opt, _T("wait")))
			s->wait = true;
		else if (!_tcsicmp(opt, _T("raw")))
			s->raw = true;
		else if (!_tcsicmp(opt, _T("telnet")))
			s->raw = false;
		else if (!_tcsnicmp(opt, _T("speed="), 6))
			s->speed = _tstol(opt + 6);
		else if (opt[0])
			write_log(_T("SERSOCK: unknown option '/%s' in '%s' ignored\n"), opt, spec);
		opt = next;
	}
	if (!tmp[0])
		return true;			/* no address at all (MODEM:// without a listener) */
	p = _tcsrchr(tmp, ':');
	if (!p) {
		write_log(_T("SERSOCK: no port in '%s'\n"), spec);
		return false;
	}
	*p++ = 0;
	if (!p[0] || _tstol(p) <= 0 || _tstol(p) > 65535) {
		write_log(_T("SERSOCK: bad port in '%s'\n"), spec);
		return false;
	}
	_tcsncpy(s->port, p, 31);
	_tcsncpy(s->host, tmp[0] ? tmp : _T("127.0.0.1"), 255);
	s->haveaddr = true;
	return true;
}

bool sersock_is_client(const TCHAR *spec)
{
	struct sersock_spec s;
	if (_tcsnicmp(spec, _T("TCP:"), 4))
		return false;
	return sersock_parse(spec, &s) && s.connect && s.haveaddr;
}

static struct sersock *sersock_alloc(const TCHAR *name, const TCHAR *host, const TCHAR *port)
{
	WSADATA wsadata;
	struct sersock *s = xcalloc(struct sersock, 1);
	if (!s)
		return NULL;
	_tcsncpy(s->name, name, 63);
	_tcsncpy(s->host, host && host[0] ? host : _T("127.0.0.1"), 255);
	_tcsncpy(s->port, port, 31);
	s->lsock = INVALID_SOCKET;
	s->conn = INVALID_SOCKET;
	s->backoff = RECONNECT_FIRST_MS;
	if (WSAStartup(MAKEWORD(2, 2), &wsadata)) {
		write_log(_T("%s: WSAStartup() failed: %d\n"), s->name, WSAGetLastError());
		xfree(s);
		return NULL;
	}
	s->wsa = true;
	return s;
}

void sersock_free(struct sersock *s)
{
	if (!s)
		return;
	if (s->conn != INVALID_SOCKET)
		closesocket(s->conn);
	if (s->lsock != INVALID_SOCKET)
		closesocket(s->lsock);
	if (s->wsa)
		WSACleanup();
	xfree(s);
}

static void setup_conn(SOCKET c)
{
	u_long nonblock = 1;
	const int one = 1;
	ioctlsocket(c, FIONBIO, &nonblock);
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

struct sersock *sersock_listen(const TCHAR *name, const TCHAR *host, const TCHAR *port)
{
	ADDRINFOW hints = { 0 };
	PADDRINFOW ai = NULL;
	u_long nonblock = 1;
	const int one = 1;
	struct sersock *s = sersock_alloc(name, host, port);

	if (!s)
		return NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (GetAddrInfoW(s->host, s->port, &hints, &ai) || !ai) {
		write_log(_T("%s: can't resolve %s:%s: %d\n"), s->name, s->host, s->port, WSAGetLastError());
		goto fail;
	}
	s->lsock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (s->lsock == INVALID_SOCKET) {
		write_log(_T("%s: socket() failed: %d\n"), s->name, WSAGetLastError());
		goto fail;
	}
	setsockopt(s->lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
	if (bind(s->lsock, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR || listen(s->lsock, 2) == SOCKET_ERROR) {
		write_log(_T("%s: can't listen on %s:%s: %d\n"), s->name, s->host, s->port, WSAGetLastError());
		goto fail;
	}
	ioctlsocket(s->lsock, FIONBIO, &nonblock);
	FreeAddrInfoW(ai);
	write_log(_T("%s: listening on %s:%s\n"), s->name, s->host, s->port);
	return s;
fail:
	if (ai)
		FreeAddrInfoW(ai);
	sersock_free(s);
	return NULL;
}

static void connect_failed(struct sersock *s, int err)
{
	if (s->conn != INVALID_SOCKET)
		closesocket(s->conn);
	s->conn = INVALID_SOCKET;
	s->connecting = false;
	s->events |= SERSOCK_EV_FAILED;
	if (err == WSAECONNREFUSED)
		s->events |= SERSOCK_EV_REFUSED;
	s->failures++;
	if (!s->reconnect) {
		s->dead = true;
		write_log(_T("%s: connect to %s:%s failed (%d)\n"), s->name, s->host, s->port, err);
		return;
	}
	if (s->failures <= 3 || (s->failures % 30) == 0)
		write_log(_T("%s: connect to %s:%s failed (%d), attempt %d, retrying in %d ms\n"),
			s->name, s->host, s->port, err, s->failures, s->backoff);
	s->next_try = now_ms() + s->backoff;
	s->backoff *= 2;
	if (s->backoff > RECONNECT_MAX_MS)
		s->backoff = RECONNECT_MAX_MS;
}

static void connected(struct sersock *s)
{
	s->connecting = false;
	s->events |= SERSOCK_EV_CONNECTED;
	s->backoff = RECONNECT_FIRST_MS;
	s->overflow_logged = false;
	s->txlen = 0;
	if (s->failures > 3)
		write_log(_T("%s: connected to %s:%s after %d failed attempts\n"), s->name, s->host, s->port, s->failures);
	else
		write_log(_T("%s: connected to %s:%s\n"), s->name, s->host, s->port);
	s->failures = 0;
}

static void start_connect(struct sersock *s)
{
	ADDRINFOW hints = { 0 };
	PADDRINFOW ai = NULL;
	int err;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (GetAddrInfoW(s->host, s->port, &hints, &ai) || !ai) {
		connect_failed(s, WSAGetLastError());
		return;
	}
	s->conn = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (s->conn == INVALID_SOCKET) {
		FreeAddrInfoW(ai);
		connect_failed(s, WSAGetLastError());
		return;
	}
	setup_conn(s->conn);
	s->connect_start = now_ms();
	if (connect(s->conn, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
		FreeAddrInfoW(ai);
		connected(s);
		return;
	}
	err = WSAGetLastError();
	FreeAddrInfoW(ai);
	if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
		s->connecting = true;
		return;
	}
	connect_failed(s, err);
}

struct sersock *sersock_connect(const TCHAR *name, const TCHAR *host, const TCHAR *port, bool reconnect)
{
	struct sersock *s = sersock_alloc(name, host, port);
	if (!s)
		return NULL;
	s->client = true;
	s->reconnect = reconnect;
	write_log(_T("%s: connecting to %s:%s%s\n"), s->name, s->host, s->port, reconnect ? _T(" (reconnecting)") : _T(""));
	start_connect(s);
	return s;
}

struct sersock *sersock_open(const TCHAR *name, const struct sersock_spec *sp)
{
	if (!sp->haveaddr)
		return NULL;
	if (sp->connect)
		return sersock_connect(name, sp->host, sp->port, sp->reconnect);
	return sersock_listen(name, sp->host, sp->port);
}

static void drop(struct sersock *s, const TCHAR *why)
{
	if (s->conn == INVALID_SOCKET)
		return;
	closesocket(s->conn);
	s->conn = INVALID_SOCKET;
	s->connecting = false;
	s->txlen = 0;
	s->events |= SERSOCK_EV_DISCONNECTED;
	write_log(_T("%s: %s %s\n"), s->name, s->client ? _T("disconnected from server,") : _T("client disconnected,"), why);
	if (s->client) {
		if (s->reconnect) {
			s->backoff = RECONNECT_FIRST_MS;
			s->next_try = now_ms() + s->backoff;
		} else {
			s->dead = true;
		}
	}
}

void sersock_hangup(struct sersock *s)
{
	if (s)
		drop(s, _T("hung up locally"));
}

static void flush_tx(struct sersock *s)
{
	while (s->txlen > 0 && s->conn != INVALID_SOCKET && !s->connecting) {
		int n = send(s->conn, (const char*)s->tx, s->txlen, 0);
		if (n > 0) {
			if (n < s->txlen)
				memmove(s->tx, s->tx + n, s->txlen - n);
			s->txlen -= n;
			continue;
		}
		if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			return;
		drop(s, _T("send failed"));
		return;
	}
}

bool sersock_poll(struct sersock *s)
{
	if (!s)
		return false;
	if (s->client) {
		if (s->connecting) {
			fd_set wfd, efd;
			struct timeval tv = { 0, 0 };
			FD_ZERO(&wfd);
			FD_ZERO(&efd);
			FD_SET(s->conn, &wfd);
			FD_SET(s->conn, &efd);
			int r = select(0, NULL, &wfd, &efd, &tv);
			if (r == SOCKET_ERROR) {
				connect_failed(s, WSAGetLastError());
			} else if (FD_ISSET(s->conn, &efd)) {
				int err = 0, len = sizeof err;
				getsockopt(s->conn, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
				connect_failed(s, err ? err : WSAECONNREFUSED);
			} else if (FD_ISSET(s->conn, &wfd)) {
				connected(s);
			} else if (now_ms() - s->connect_start > CONNECT_TIMEOUT_MS) {
				connect_failed(s, WSAETIMEDOUT);
			}
		} else if (s->conn == INVALID_SOCKET && !s->dead && s->reconnect && now_ms() >= s->next_try) {
			start_connect(s);
		}
	} else if (s->lsock != INVALID_SOCKET) {
		if (s->conn == INVALID_SOCKET) {
			SOCKET c = accept(s->lsock, NULL, NULL);
			if (c != INVALID_SOCKET) {
				setup_conn(c);
				s->conn = c;
				s->txlen = 0;
				s->overflow_logged = false;
				s->events |= SERSOCK_EV_CONNECTED;
				write_log(_T("%s: client connected\n"), s->name);
			}
		} else if (s->busymsg) {
			SOCKET c = accept(s->lsock, NULL, NULL);
			if (c != INVALID_SOCKET) {
				send(c, s->busymsg, (int)strlen(s->busymsg), 0);
				closesocket(c);
				write_log(_T("%s: second caller refused (busy)\n"), s->name);
			}
		}
	}
	flush_tx(s);
	return s->conn != INVALID_SOCKET && !s->connecting;
}

bool sersock_connected(struct sersock *s)
{
	return s && s->conn != INVALID_SOCKET && !s->connecting;
}

int sersock_events(struct sersock *s)
{
	if (!s)
		return 0;
	int e = s->events;
	s->events = 0;
	return e;
}

int sersock_avail(struct sersock *s)
{
	u_long avail = 0;
	char c;
	if (!sersock_connected(s))
		return 0;
	int n = recv(s->conn, &c, 1, MSG_PEEK);
	if (n == 0) {
		drop(s, _T("closed by peer"));
		return 0;
	}
	if (n == SOCKET_ERROR) {
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			drop(s, _T("receive failed"));
		return 0;
	}
	if (ioctlsocket(s->conn, FIONREAD, &avail) == SOCKET_ERROR || avail == 0)
		return 1;
	return (int)avail;
}

int sersock_read(struct sersock *s, uae_u8 *buf, int size)
{
	if (size <= 0 || !sersock_connected(s))
		return 0;
	int n = recv(s->conn, (char*)buf, size, 0);
	if (n > 0)
		return n;
	if (n == 0) {
		drop(s, _T("closed by peer"));
		return 0;
	}
	if (WSAGetLastError() != WSAEWOULDBLOCK)
		drop(s, _T("receive failed"));
	return 0;
}

void sersock_write(struct sersock *s, const uae_u8 *buf, int size)
{
	if (!sersock_connected(s) || size <= 0)
		return;
	if (s->txlen + size > SERSOCK_TXBUF) {
		flush_tx(s);
		if (!sersock_connected(s))
			return;
		if (s->txlen + size > SERSOCK_TXBUF) {
			if (!s->overflow_logged)
				write_log(_T("%s: peer is not reading, output dropped\n"), s->name);
			s->overflow_logged = true;
			size = SERSOCK_TXBUF - s->txlen;
			if (size <= 0)
				return;
		}
	}
	memcpy(s->tx + s->txlen, buf, size);
	s->txlen += size;
	flush_tx(s);
}

void sersock_set_busy_message(struct sersock *s, const char *msg)
{
	if (s)
		s->busymsg = msg;
}
