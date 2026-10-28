/*
* UAE - The Un*x Amiga Emulator
*
* Hayes-style modem with Telnet (or raw TCP) as the phone line.
* See include/sermodem.h. Timing (rings, the +++ guard time, S7) runs in
* emulated time, so the guest's own pauses are measured the way it made them.
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"
#include "custom.h"
#include "sersock.h"
#include "sermodem.h"

#define OUTBUF 4096
#define CMDMAX 80
#define MAXRINGS 20

enum { M_IDLE, M_RINGING, M_ANSWERING, M_DIALING, M_ONLINE, M_ONLINE_CMD };
static const TCHAR *statename[] = { _T("idle"), _T("ringing"), _T("answering"), _T("dialling"), _T("on line"), _T("on line, command mode") };

#define MR_OK 0
#define MR_CONNECT 1
#define MR_RING 2
#define MR_NOCARRIER 3
#define MR_ERROR 4
#define MR_NODIALTONE 6
#define MR_BUSY 7
#define MR_NOANSWER 8
#define MR_NONE -1
static const char *resultname[] = { "OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE", "BUSY", "NO ANSWER" };

/* Telnet */
#define T_SE 240
#define T_SB 250
#define T_WILL 251
#define T_WONT 252
#define T_DO 253
#define T_DONT 254
#define T_IAC 255
#define TOPT_BINARY 0
#define TOPT_ECHO 1
#define TOPT_SGA 3
enum { TN_DATA, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };

struct sermodem
{
	TCHAR name[64];
	struct sersock *listen;		/* incoming calls */
	struct sersock *dial;		/* our own outgoing call */
	bool incoming;				/* the call in progress came in on listen */
	bool raw;
	int speed, dte;
	int state;
	uae_u8 S[256];
	bool echo, verbose, quiet;
	int amp_c, amp_d;
	uae_u8 cmd[CMDMAX];
	int cmdlen;
	uae_u8 lastcmd[CMDMAX];
	int lastcmdlen;
	uae_u8 out[OUTBUF];
	int outhead, outtail;
	double t_ring_next, t_deadline, t_last_tx, t_plus, t_ri;
	int plus;
	int rings;
	bool dtr;
	/* Telnet state for the current call */
	int tn_state;
	uae_u8 tn_cmd;
	bool tn_cr;
	bool us[256], him[256];
};

static double emu_ms(void)
{
	return (double)get_cycles() / CYCLE_UNIT / (currprefs.ntscmode ? CHIPSET_CLOCK_NTSC : CHIPSET_CLOCK_PAL) * 1000.0;
}

static int out_count(struct sermodem *m)
{
	int n = m->outtail - m->outhead;
	return n < 0 ? n + OUTBUF : n;
}

static int out_room(struct sermodem *m)
{
	return OUTBUF - 1 - out_count(m);
}

static void out_put(struct sermodem *m, const uae_u8 *p, int n)
{
	while (n-- > 0) {
		int next = (m->outtail + 1) % OUTBUF;
		if (next == m->outhead)
			return;
		m->out[m->outtail] = *p++;
		m->outtail = next;
	}
}

static void out_str(struct sermodem *m, const char *s)
{
	out_put(m, (const uae_u8*)s, (int)strlen(s));
}

static void out_crlf(struct sermodem *m)
{
	uae_u8 b[2] = { m->S[3], m->S[4] };
	out_put(m, b, 2);
}

static int speed_of(struct sermodem *m)
{
	if (m->speed > 0)
		return m->speed;
	if (m->dte > 0)
		return m->dte;
	return 2400;
}

static void result(struct sermodem *m, int code)
{
	char tmp[64];
	if (code < 0 || m->quiet)
		return;
	if (m->verbose) {
		if (code == MR_CONNECT)
			sprintf(tmp, "CONNECT %d", speed_of(m));
		else
			strcpy(tmp, resultname[code]);
		out_crlf(m);
		out_str(m, tmp);
		out_crlf(m);
	} else {
		uae_u8 b = m->S[3];
		sprintf(tmp, "%d", code);
		out_str(m, tmp);
		out_put(m, &b, 1);
	}
	write_log(_T("%s: -> %S\n"), m->name, code == MR_CONNECT ? tmp : resultname[code]);
}

static void set_state(struct sermodem *m, int st)
{
	if (m->state != st)
		write_log(_T("%s: %s -> %s\n"), m->name, statename[m->state], statename[st]);
	m->state = st;
}

static struct sersock *callsock(struct sermodem *m)
{
	return m->incoming ? m->listen : m->dial;
}

static void reset_regs(struct sermodem *m)
{
	memset(m->S, 0, sizeof m->S);
	m->S[0] = 0;
	m->S[2] = '+';
	m->S[3] = 13;
	m->S[4] = 10;
	m->S[5] = 8;
	m->S[6] = 2;
	m->S[7] = 50;
	m->S[8] = 2;
	m->S[10] = 14;
	m->S[12] = 50;
	m->echo = true;
	m->verbose = true;
	m->quiet = false;
	m->amp_c = 1;
	m->amp_d = 2;
}

/* ---- Telnet ---- */

static void tn_send(struct sermodem *m, uae_u8 cmd, uae_u8 opt)
{
	uae_u8 b[3] = { T_IAC, cmd, opt };
	sersock_write(callsock(m), b, 3);
}

static void tn_start(struct sermodem *m)
{
	m->tn_state = TN_DATA;
	m->tn_cr = false;
	memset(m->us, 0, sizeof m->us);
	memset(m->him, 0, sizeof m->him);
	if (m->raw)
		return;
	/* character at a time, 8-bit clean both ways, the Amiga echoes */
	m->us[TOPT_ECHO] = m->us[TOPT_SGA] = m->us[TOPT_BINARY] = true;
	m->him[TOPT_SGA] = m->him[TOPT_BINARY] = true;
	tn_send(m, T_WILL, TOPT_ECHO);
	tn_send(m, T_WILL, TOPT_SGA);
	tn_send(m, T_DO, TOPT_SGA);
	tn_send(m, T_WILL, TOPT_BINARY);
	tn_send(m, T_DO, TOPT_BINARY);
}

static void tn_option(struct sermodem *m, uae_u8 cmd, uae_u8 opt)
{
	switch (cmd)
	{
	case T_DO:
		if (opt == TOPT_BINARY || opt == TOPT_SGA || opt == TOPT_ECHO) {
			if (!m->us[opt]) {
				m->us[opt] = true;
				tn_send(m, T_WILL, opt);
			}
		} else {
			tn_send(m, T_WONT, opt);
		}
		break;
	case T_DONT:
		if (m->us[opt]) {
			m->us[opt] = false;
			tn_send(m, T_WONT, opt);
		}
		break;
	case T_WILL:
		if (opt == TOPT_BINARY || opt == TOPT_SGA) {
			if (!m->him[opt]) {
				m->him[opt] = true;
				tn_send(m, T_DO, opt);
			}
		} else {
			tn_send(m, T_DONT, opt);
		}
		break;
	case T_WONT:
		if (m->him[opt]) {
			m->him[opt] = false;
			tn_send(m, T_DONT, opt);
		}
		break;
	}
}

/* line bytes -> data bytes; returns the count written to out */
static int tn_decode(struct sermodem *m, const uae_u8 *in, int n, uae_u8 *out)
{
	int o = 0;
	if (m->raw) {
		memcpy(out, in, n);
		return n;
	}
	for (int i = 0; i < n; i++) {
		uae_u8 b = in[i];
		switch (m->tn_state)
		{
		case TN_DATA:
			if (b == T_IAC) {
				m->tn_state = TN_IAC;
				break;
			}
			/* NVT end of line is CR NUL or CR LF; drop the NUL unless binary */
			if (b == 0 && m->tn_cr && !m->him[TOPT_BINARY]) {
				m->tn_cr = false;
				break;
			}
			m->tn_cr = b == 13;
			out[o++] = b;
			break;
		case TN_IAC:
			if (b == T_IAC) {
				out[o++] = b;
				m->tn_cr = false;
				m->tn_state = TN_DATA;
			} else if (b >= T_WILL && b <= T_DONT) {
				m->tn_cmd = b;
				m->tn_state = TN_OPT;
			} else if (b == T_SB) {
				m->tn_state = TN_SB;
			} else {
				m->tn_state = TN_DATA;		/* NOP, AYT, GA, ...: ignored */
			}
			break;
		case TN_OPT:
			tn_option(m, m->tn_cmd, b);
			m->tn_state = TN_DATA;
			break;
		case TN_SB:
			if (b == T_IAC)
				m->tn_state = TN_SB_IAC;
			break;
		case TN_SB_IAC:
			m->tn_state = b == T_SE ? TN_DATA : TN_SB;
			break;
		}
	}
	return o;
}

static void line_send(struct sermodem *m, const uae_u8 *p, int n)
{
	struct sersock *s = callsock(m);
	if (m->raw) {
		sersock_write(s, p, n);
		return;
	}
	uae_u8 tmp[512];
	while (n > 0) {
		int o = 0;
		while (n > 0 && o < (int)sizeof tmp - 2) {
			uae_u8 b = *p++;
			n--;
			tmp[o++] = b;
			if (b == T_IAC)
				tmp[o++] = T_IAC;
		}
		sersock_write(s, tmp, o);
	}
}

/* pull line data into the output queue (on line), or throw it away (ringing) */
static void line_pull(struct sermodem *m, bool keep)
{
	struct sersock *s = callsock(m);
	uae_u8 in[512], dec[512];
	for (;;) {
		int room = keep ? out_room(m) : (int)sizeof in;
		if (room > (int)sizeof in)
			room = sizeof in;
		if (room <= 0)
			return;
		int n = sersock_read(s, in, room);
		if (n <= 0)
			return;
		n = tn_decode(m, in, n, dec);
		if (keep)
			out_put(m, dec, n);
	}
}

/* ---- call control ---- */

static void drop_call(struct sermodem *m)
{
	if (m->incoming) {
		if (sersock_connected(m->listen))
			sersock_hangup(m->listen);
		sersock_events(m->listen);
	} else if (m->dial) {
		sersock_free(m->dial);
	}
	m->dial = NULL;
	m->incoming = false;
	m->rings = 0;
	m->S[1] = 0;
	m->plus = 0;
}

static void hangup(struct sermodem *m, int code, const TCHAR *why)
{
	bool hadcall = m->state != M_IDLE;
	drop_call(m);
	set_state(m, M_IDLE);
	if (hadcall)
		write_log(_T("%s: call ended (%s)\n"), m->name, why);
	result(m, code);
}

static void go_online(struct sermodem *m)
{
	tn_start(m);
	set_state(m, M_ONLINE);
	m->rings = 0;
	m->plus = 0;
	m->t_last_tx = emu_ms();
	result(m, MR_CONNECT);
}

static void dial(struct sermodem *m, const char *s)
{
	TCHAR host[256], port[16];
	char buf[256];
	int o = 0;

	/* ATD[T|P]host[:port]: a leading T or P (tone/pulse) is dropped, as are
	   spaces, quotes and the , ; pause/stay-in-command-mode modifiers */
	while (*s == ' ')
		s++;
	if ((*s == 'T' || *s == 'P' || *s == 't' || *s == 'p') && s[1])
		s++;
	for (; *s && o < 255; s++) {
		if (*s == ' ' || *s == '"' || *s == ',' || *s == ';')
			continue;
		buf[o++] = *s;
	}
	buf[o] = 0;
	if (!o) {
		result(m, MR_ERROR);
		return;
	}
	char *colon = strrchr(buf, ':');
	if (colon) {
		*colon = 0;
		au_copy(port, 16, colon + 1);
	} else {
		_tcscpy(port, _T("23"));
	}
	au_copy(host, 256, buf);
	if (m->state == M_ONLINE_CMD || m->incoming || m->dial) {
		result(m, MR_ERROR);
		return;
	}
	write_log(_T("%s: dialling %s:%s\n"), m->name, host, port);
	m->dial = sersock_connect(m->name, host, port, false);
	if (!m->dial) {
		result(m, MR_NODIALTONE);
		return;
	}
	m->incoming = false;
	m->t_deadline = emu_ms() + m->S[7] * 1000.0;
	set_state(m, M_DIALING);
}

static void answer(struct sermodem *m)
{
	if (m->state == M_RINGING) {
		go_online(m);
	} else if (m->state == M_IDLE) {
		/* off hook, wait S7 seconds for a caller */
		m->t_deadline = emu_ms() + m->S[7] * 1000.0;
		set_state(m, M_ANSWERING);
	} else {
		result(m, MR_ERROR);
	}
}

/* ---- AT commands ---- */

static int getnum(const char *s, int *i)
{
	int v = 0;
	while (isdigit((uae_u8)s[*i])) {
		v = v * 10 + (s[*i] - '0');
		(*i)++;
	}
	return v;
}

static void info(struct sermodem *m, int n)
{
	const char *t;
	switch (n)
	{
	case 0: t = "2400"; break;
	case 3: t = "WinUAE Hayes modem emulation (Telnet line)"; break;
	case 4: t = "WinUAE"; break;
	default: t = "WinUAE modem"; break;
	}
	out_crlf(m);
	out_str(m, t);
	out_crlf(m);
}

static void command_line(struct sermodem *m, const uae_u8 *line, int len)
{
	char s[CMDMAX + 1];
	int i, j = 0, at;
	char logline[CMDMAX + 1];

	memcpy(logline, line, len);
	logline[len] = 0;
	/* hunt for AT */
	for (i = 0; i + 1 < len; i++) {
		if (toupper(line[i]) == 'A' && toupper(line[i + 1]) == 'T')
			break;
	}
	if (i + 1 >= len)
		return;
	at = i + 2;
	write_log(_T("%s: <- %S\n"), m->name, logline);
	memcpy(m->lastcmd, line, len);
	m->lastcmdlen = len;
	for (i += 2; i < len; i++)
		s[j++] = toupper(line[i]);
	s[j] = 0;

	i = 0;
	while (s[i]) {
		char c = s[i++];
		switch (c)
		{
		case ' ':
			break;
		case 'A':
			answer(m);
			return;
		case 'D':
			/* the dial string keeps its case (host names): s[i] is line[at + i] */
			{
				char orig[CMDMAX + 1];
				memcpy(orig, line + at, len - at);
				orig[len - at] = 0;
				dial(m, orig + i);
			}
			return;
		case 'E':
			m->echo = getnum(s, &i) != 0;
			break;
		case 'H':
			if (getnum(s, &i) == 0 && m->state != M_IDLE) {
				drop_call(m);
				set_state(m, M_IDLE);
				write_log(_T("%s: call ended (ATH)\n"), m->name);
			}
			break;
		case 'I':
			info(m, getnum(s, &i));
			break;
		case 'O':
			getnum(s, &i);
			if (m->state == M_ONLINE_CMD) {
				set_state(m, M_ONLINE);
				m->plus = 0;
				m->t_last_tx = emu_ms();
				result(m, MR_CONNECT);
			} else {
				result(m, MR_NOCARRIER);
			}
			return;
		case 'Q':
			m->quiet = getnum(s, &i) != 0;
			break;
		case 'V':
			m->verbose = getnum(s, &i) != 0;
			break;
		case 'Z':
			getnum(s, &i);
			if (m->state != M_IDLE) {
				drop_call(m);
				set_state(m, M_IDLE);
			}
			reset_regs(m);
			break;
		case 'S':
		{
			int r = getnum(s, &i);
			if (s[i] == '=') {
				i++;
				m->S[r & 255] = (uae_u8)getnum(s, &i);
			} else if (s[i] == '?') {
				char tmp[8];
				i++;
				sprintf(tmp, "%03d", m->S[r & 255]);
				out_crlf(m);
				out_str(m, tmp);
				out_crlf(m);
			}
			break;
		}
		case '&':
		{
			char sub = s[i];
			if (sub)
				i++;
			int n = getnum(s, &i);
			if (sub == 'F')
				reset_regs(m);
			else if (sub == 'C')
				m->amp_c = n;
			else if (sub == 'D')
				m->amp_d = n;
			/* everything else (&K, &W, &V ...) is accepted and ignored */
			break;
		}
		case '\\': case '%': case '#': case '*': case '+': case '$': case '-': case ')': case '"':
			if (s[i] && isalpha((uae_u8)s[i]))
				i++;
			if (s[i] == '=')
				i++;
			getnum(s, &i);
			break;
		default:
			if (isalpha((uae_u8)c)) {
				getnum(s, &i);			/* B C L M N P T W X Y ...: accepted, ignored */
				break;
			}
			result(m, MR_ERROR);
			return;
		}
	}
	result(m, MR_OK);
}

static void command_byte(struct sermodem *m, uae_u8 b)
{
	if (m->echo)
		out_put(m, &b, 1);
	if (b == m->S[3]) {
		int len = m->cmdlen;
		m->cmdlen = 0;
		if (len > 0)
			command_line(m, m->cmd, len);
		return;
	}
	if (b == m->S[5]) {
		if (m->cmdlen > 0)
			m->cmdlen--;
		return;
	}
	if (b == 10 || b == 0)
		return;
	if (m->cmdlen < CMDMAX)
		m->cmd[m->cmdlen++] = b;
	if (m->cmdlen == 2 && toupper(m->cmd[0]) == 'A' && m->cmd[1] == '/') {
		m->cmdlen = 0;
		if (m->lastcmdlen > 0) {
			uae_u8 tmp[CMDMAX];
			memcpy(tmp, m->lastcmd, m->lastcmdlen);
			command_line(m, tmp, m->lastcmdlen);
		}
	}
}

/* ---- public ---- */

struct sermodem *sermodem_open(const TCHAR *name, const TCHAR *spec)
{
	struct sersock_spec sp;
	struct sermodem *m;

	if (!sersock_parse(spec, &sp))
		return NULL;
	m = xcalloc(struct sermodem, 1);
	if (!m)
		return NULL;
	_tcsncpy(m->name, name, 63);
	m->raw = sp.raw;
	m->speed = sp.speed;
	m->dtr = true;
	reset_regs(m);
	if (sp.haveaddr) {
		m->listen = sersock_listen(m->name, sp.host, sp.port);
		if (!m->listen) {
			xfree(m);
			return NULL;
		}
		sersock_set_busy_message(m->listen, "\r\nBUSY\r\n");
	}
	write_log(_T("%s: Hayes modem, %s, %s\n"), m->name, sp.haveaddr ? _T("takes calls") : _T("dial-out only"), m->raw ? _T("raw TCP") : _T("Telnet"));
	return m;
}

void sermodem_free(struct sermodem *m)
{
	if (!m)
		return;
	if (m->dial)
		sersock_free(m->dial);
	if (m->listen)
		sersock_free(m->listen);
	xfree(m);
}

void sermodem_poll(struct sermodem *m)
{
	if (!m)
		return;
	double now = emu_ms();

	if (m->listen) {
		sersock_poll(m->listen);
		int ev = sersock_events(m->listen);
		if (ev & SERSOCK_EV_CONNECTED) {
			if (m->state == M_IDLE && m->dtr) {
				write_log(_T("%s: incoming call\n"), m->name);
				m->incoming = true;
				m->rings = 0;
				m->t_ring_next = now;
				set_state(m, M_RINGING);
			} else if (m->state == M_ANSWERING) {
				m->incoming = true;
				go_online(m);
			} else {
				const char *busy = "\r\nBUSY\r\n";
				sersock_write(m->listen, (const uae_u8*)busy, (int)strlen(busy));
				sersock_hangup(m->listen);
				sersock_events(m->listen);
				write_log(_T("%s: caller refused, modem %s\n"), m->name, statename[m->state]);
			}
		}
		if ((ev & SERSOCK_EV_DISCONNECTED) && m->incoming) {
			if (m->state == M_RINGING) {
				m->incoming = false;
				m->rings = 0;
				m->S[1] = 0;
				set_state(m, M_IDLE);
				write_log(_T("%s: caller hung up while ringing\n"), m->name);
			} else if (m->state == M_ONLINE || m->state == M_ONLINE_CMD) {
				hangup(m, MR_NOCARRIER, _T("caller hung up"));
			}
		}
	}
	if (m->dial) {
		sersock_poll(m->dial);
		int ev = sersock_events(m->dial);
		if (m->state == M_DIALING) {
			if (ev & SERSOCK_EV_CONNECTED)
				go_online(m);
			else if (ev & SERSOCK_EV_FAILED)
				hangup(m, (ev & SERSOCK_EV_REFUSED) ? MR_BUSY : MR_NOCARRIER, _T("dial failed"));
		} else if ((ev & SERSOCK_EV_DISCONNECTED) && (m->state == M_ONLINE || m->state == M_ONLINE_CMD)) {
			hangup(m, MR_NOCARRIER, _T("far end hung up"));
		}
	}

	switch (m->state)
	{
	case M_RINGING:
		line_pull(m, false);
		if (now >= m->t_ring_next) {
			if (m->rings >= MAXRINGS) {
				const char *na = "\r\nNO ANSWER\r\n";
				sersock_write(m->listen, (const uae_u8*)na, (int)strlen(na));
				hangup(m, MR_NONE, _T("no answer"));
				break;
			}
			m->rings++;
			m->S[1] = m->rings;
			m->t_ri = now;
			result(m, MR_RING);
			m->t_ring_next = now + 6000.0;
			if (m->S[0] && m->rings >= m->S[0] && m->dtr) {
				write_log(_T("%s: auto-answer on ring %d\n"), m->name, m->rings);
				go_online(m);
			}
		}
		break;
	case M_ONLINE_CMD:
		/* line data waits for ATO, but notice the far end hanging up */
		sersock_avail(callsock(m));
		break;
	case M_ANSWERING:
	case M_DIALING:
		if (now >= m->t_deadline)
			hangup(m, MR_NOCARRIER, _T("S7 timeout"));
		break;
	case M_ONLINE:
		if (m->plus == 3 && now - m->t_plus >= m->S[12] * 20.0) {
			m->plus = 0;
			set_state(m, M_ONLINE_CMD);
			result(m, MR_OK);
			break;
		}
		line_pull(m, true);
		break;
	}
}

int sermodem_avail(struct sermodem *m)
{
	if (!m)
		return 0;
	if (out_count(m) == 0 && m->state == M_ONLINE)
		line_pull(m, true);
	return out_count(m);
}

int sermodem_read(struct sermodem *m, uae_u8 *buf, int size)
{
	int n = 0;
	if (!m)
		return 0;
	if (out_count(m) < size && m->state == M_ONLINE)
		line_pull(m, true);
	while (n < size && m->outhead != m->outtail) {
		buf[n++] = m->out[m->outhead];
		m->outhead = (m->outhead + 1) % OUTBUF;
	}
	return n;
}

void sermodem_write(struct sermodem *m, const uae_u8 *buf, int size)
{
	if (!m || size <= 0)
		return;
	double now = emu_ms();
	switch (m->state)
	{
	case M_ONLINE:
	{
		double guard = m->S[12] * 20.0;
		uae_u8 esc = m->S[2];
		for (int i = 0; i < size; i++) {
			if (esc < 128 && buf[i] == esc) {
				if (m->plus == 0)
					m->plus = (now - m->t_last_tx >= guard) ? 1 : 0;
				else if (m->plus < 3)
					m->plus++;
				else
					m->plus = 0;
				m->t_plus = now;
			} else {
				m->plus = 0;
			}
			m->t_last_tx = now;
		}
		line_send(m, buf, size);
		break;
	}
	case M_DIALING:
	case M_ANSWERING:
		/* any character aborts */
		hangup(m, MR_NOCARRIER, _T("aborted by a key"));
		break;
	default:
		for (int i = 0; i < size; i++)
			command_byte(m, buf[i]);
		break;
	}
}

bool sermodem_carrier(struct sermodem *m)
{
	if (!m)
		return false;
	if (m->amp_c == 0)
		return true;
	return m->state == M_ONLINE || m->state == M_ONLINE_CMD;
}

bool sermodem_ring(struct sermodem *m)
{
	if (!m || m->state != M_RINGING)
		return false;
	return emu_ms() - m->t_ri < 2000.0;
}

void sermodem_dtr(struct sermodem *m, bool on)
{
	if (!m || m->dtr == on)
		return;
	m->dtr = on;
	if (!on && m->amp_d >= 2 && (m->state == M_ONLINE || m->state == M_ONLINE_CMD || m->state == M_DIALING))
		hangup(m, MR_NOCARRIER, _T("DTR dropped"));
}

void sermodem_set_dte_rate(struct sermodem *m, int baud)
{
	if (m)
		m->dte = baud;
}
