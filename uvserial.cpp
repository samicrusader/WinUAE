/*
* UAE - The Un*x Amiga Emulator
*
* United Video / Prevue Networks multiserial Zorro II card.
*
* Four RS232 ports built from two SCN68681 class DUARTs. The board answers
* autoconfig as manufacturer 0x2179, product 1, and UVSerial.device finds it
* with expansion.library FindConfigDev(). Registers are byte wide on the odd
* byte lane, so register n of DUART d lives at
*
*     base + ((d * 16) + n) * 2 + 1
*
* giving 32 registers between base+0x01 and base+0x3f. DUART 0 serves units
* 0 and 1, DUART 1 serves units 2 and 3. The board interrupts on INT6/EXTER.
*
* Reconstructed from UVSerial.device 1.275 (6.6.94), which is the only piece
* of software that ever addresses this hardware.
*
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"
#include "rommgr.h"
#include "devices.h"
#include "debug.h"
#include "uvserial.h"
#include "pioneerld.h"

#define BOARD_SIZE 65536
#define UVSER_PORTS 4
#define RXFIFO_SIZE 512

int log_uvserial = 1;

/* 68681 status register */
#define SR_RXRDY	0x01
#define SR_FFULL	0x02
#define SR_TXRDY	0x04
#define SR_TXEMT	0x08

/* 68681 interrupt status / mask register */
#define ISR_TXRDYA	0x01
#define ISR_RXRDYA	0x02
#define ISR_DBA		0x04
#define ISR_CRDY	0x08
#define ISR_TXRDYB	0x10
#define ISR_RXRDYB	0x20
#define ISR_DBB		0x40
#define ISR_IPC		0x80

/* what a port is wired to */
#define BACKEND_NONE	0
#define BACKEND_LD		1

struct uvchannel
{
	int port;				/* 0..3, the unit number UVSerial.device uses */
	int backend;
	uae_u8 mr[2];
	int mrptr;
	uae_u8 csr;				/* baud rate selects */
	bool rxen, txen;
	uae_u8 sr;
	uae_u8 rxfifo[RXFIFO_SIZE];
	int rxhead, rxtail;
};

struct uvduart
{
	struct uvchannel ch[2];
	uae_u8 acr;
	uae_u8 imr;
	uae_u8 isr;
	uae_u8 opcr;
	uae_u8 opr;
	uae_u8 ivr;
	uae_u16 ctr;
};

static struct uvduart duart[2];
static bool uvserial_active;
static int poll_divider;

static const int baudtable_set2[16] = {
	75, 110, 134, 150, 300, 600, 1200, 1050,
	2400, 4800, 7200, 9600, 19200, 0, 0, 0
};
static const int baudtable_set1[16] = {
	50, 110, 134, 200, 300, 600, 1200, 1050,
	2000, 2400, 4800, 1800, 9600, 38400, 0, 0
};

static int rxfifo_count(struct uvchannel *c)
{
	int n = c->rxtail - c->rxhead;
	if (n < 0)
		n += RXFIFO_SIZE;
	return n;
}

static void rxfifo_put(struct uvchannel *c, uae_u8 v)
{
	int next = (c->rxtail + 1) % RXFIFO_SIZE;
	if (next == c->rxhead) {
		if (log_uvserial)
			write_log(_T("UVSER: port %d receive overrun\n"), c->port);
		return;
	}
	c->rxfifo[c->rxtail] = v;
	c->rxtail = next;
}

static int rxfifo_get(struct uvchannel *c)
{
	if (c->rxhead == c->rxtail)
		return -1;
	uae_u8 v = c->rxfifo[c->rxhead];
	c->rxhead = (c->rxhead + 1) % RXFIFO_SIZE;
	return v;
}

static void update_sr(struct uvchannel *c)
{
	c->sr &= ~(SR_RXRDY | SR_FFULL);
	if (rxfifo_count(c) > 0)
		c->sr |= SR_RXRDY;
	if (rxfifo_count(c) >= RXFIFO_SIZE - 1)
		c->sr |= SR_FFULL;
	/* the emulated transmitter is never busy */
	if (c->txen)
		c->sr |= SR_TXRDY | SR_TXEMT;
	else
		c->sr &= ~(SR_TXRDY | SR_TXEMT);
}

static void update_isr(struct uvduart *d)
{
	uae_u8 isr = d->isr & (ISR_CRDY | ISR_IPC | ISR_DBA | ISR_DBB);
	if (d->ch[0].sr & SR_TXRDY)
		isr |= ISR_TXRDYA;
	if (d->ch[0].sr & SR_RXRDY)
		isr |= ISR_RXRDYA;
	if (d->ch[1].sr & SR_TXRDY)
		isr |= ISR_TXRDYB;
	if (d->ch[1].sr & SR_RXRDY)
		isr |= ISR_RXRDYB;
	d->isr = isr;
}

static void uvserial_rethink(void)
{
	if (!uvserial_active)
		return;
	for (int i = 0; i < 2; i++) {
		struct uvduart *d = &duart[i];
		update_sr(&d->ch[0]);
		update_sr(&d->ch[1]);
		update_isr(d);
		if (d->isr & d->imr) {
			safe_interrupt_set(IRQ_SOURCE_UVSERIAL, i, true);
		}
	}
}

static void backend_write(struct uvchannel *c, uae_u8 v)
{
	switch (c->backend)
	{
	case BACKEND_LD:
		pioneerld_put(v);
		break;
	default:
		if (log_uvserial > 1)
			write_log(_T("UVSER: port %d tx %02x discarded\n"), c->port, v);
		break;
	}
}

static void backend_poll(struct uvchannel *c)
{
	switch (c->backend)
	{
	case BACKEND_LD:
		for (;;) {
			int v = pioneerld_get();
			if (v < 0)
				break;
			rxfifo_put(c, (uae_u8)v);
		}
		break;
	default:
		break;
	}
}

static void uvserial_hsync(void)
{
	if (!uvserial_active)
		return;
	poll_divider++;
	if (poll_divider < 64)
		return;
	poll_divider = 0;
	for (int i = 0; i < 2; i++) {
		backend_poll(&duart[i].ch[0]);
		backend_poll(&duart[i].ch[1]);
	}
	devices_rethink_all(uvserial_rethink);
}

static void channel_command(struct uvchannel *c, uae_u8 v)
{
	/* bits 3:0 enable / disable, bits 6:4 miscellaneous command.
	 * UVSerial.device writes the bare values 2,3,4,5 during reset, which only
	 * touch the enable bits, so handle both halves and stay permissive. */
	if (v & 0x01)
		c->rxen = true;
	if (v & 0x02)
		c->rxen = false;
	if (v & 0x04)
		c->txen = true;
	if (v & 0x08)
		c->txen = false;
	switch ((v >> 4) & 7)
	{
	case 1:		/* reset MR pointer */
		c->mrptr = 0;
		break;
	case 2:		/* reset receiver */
		c->rxen = false;
		c->rxhead = c->rxtail = 0;
		break;
	case 3:		/* reset transmitter */
		c->txen = false;
		break;
	case 4:		/* reset error status */
		c->sr &= 0x0f;
		break;
	default:
		break;
	}
	update_sr(c);
}

static uae_u8 uvserial_reg_get(int duartnum, int reg)
{
	struct uvduart *d = &duart[duartnum];
	struct uvchannel *c = &d->ch[(reg >= 8) ? 1 : 0];
	int r = reg & 7;
	uae_u8 v = 0;

	if (reg == 4) {
		/* IPCR: no input port change, all inputs low (CTS asserted) */
		return 0x0f;
	}
	if (reg == 5) {
		update_sr(&d->ch[0]);
		update_sr(&d->ch[1]);
		update_isr(d);
		return d->isr;
	}
	if (reg == 13) {
		/* input port. keep CTS/DSR asserted for both channels */
		return 0x00;
	}
	if (reg == 14 || reg == 15) {
		/* start / stop counter */
		return 0;
	}
	if (reg == 12)
		return d->ivr;

	switch (r)
	{
	case 0:		/* MR1/MR2 */
		v = c->mr[c->mrptr];
		if (c->mrptr == 0)
			c->mrptr = 1;
		break;
	case 1:		/* SR */
		update_sr(c);
		v = c->sr;
		break;
	case 2:		/* BRG test */
		v = 0;
		break;
	case 3:		/* RHR */
	{
		int b = rxfifo_get(c);
		v = b < 0 ? 0 : (uae_u8)b;
		update_sr(c);
		update_isr(d);
		break;
	}
	case 6:
		v = (uae_u8)(d->ctr >> 8);
		break;
	case 7:
		v = (uae_u8)d->ctr;
		break;
	default:
		v = 0;
		break;
	}
	return v;
}

static void uvserial_reg_put(int duartnum, int reg, uae_u8 v)
{
	struct uvduart *d = &duart[duartnum];
	struct uvchannel *c = &d->ch[(reg >= 8) ? 1 : 0];
	int r = reg & 7;

	if (reg == 4) {
		d->acr = v;
		return;
	}
	if (reg == 5) {
		d->imr = v;
		uvserial_rethink();
		return;
	}
	if (reg == 12) {
		d->ivr = v;
		return;
	}
	if (reg == 13) {
		d->opcr = v;
		return;
	}
	if (reg == 14) {
		d->opr |= v;
		return;
	}
	if (reg == 15) {
		d->opr &= ~v;
		return;
	}

	switch (r)
	{
	case 0:		/* MR1/MR2 */
		c->mr[c->mrptr] = v;
		if (c->mrptr == 0)
			c->mrptr = 1;
		break;
	case 1:		/* CSR, baud rate selects */
		c->csr = v;
		if (log_uvserial) {
			const int *tbl = (d->acr & 0x80) ? baudtable_set2 : baudtable_set1;
			write_log(_T("UVSER: port %d baud rx=%d tx=%d (csr %02x acr %02x)\n"),
				c->port, tbl[(v >> 4) & 15], tbl[v & 15], v, d->acr);
		}
		break;
	case 2:		/* CR */
		channel_command(c, v);
		break;
	case 3:		/* THR */
		backend_write(c, v);
		update_sr(c);
		update_isr(d);
		break;
	case 6:
		d->ctr = (d->ctr & 0x00ff) | (v << 8);
		break;
	case 7:
		d->ctr = (d->ctr & 0xff00) | v;
		break;
	default:
		break;
	}
}

static uae_u32 REGPARAM3 uvserial_bget(uaecptr) REGPARAM;
static uae_u32 REGPARAM3 uvserial_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM3 uvserial_lget(uaecptr) REGPARAM;
static void REGPARAM3 uvserial_bput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 uvserial_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 uvserial_lput(uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 uvserial_bget(uaecptr addr)
{
	addr &= BOARD_SIZE - 1;
	if (!(addr & 1) || addr >= 0x40)
		return 0xff;
	int index = (addr - 1) / 2;
	uae_u8 v = uvserial_reg_get(index >> 4, index & 15);
	if (log_uvserial > 2)
		write_log(_T("UVSER: get %02x (duart %d reg %d) = %02x PC=%08x\n"),
			addr, index >> 4, index & 15, v, M68K_GETPC);
	return v;
}

static void REGPARAM2 uvserial_bput(uaecptr addr, uae_u32 b)
{
	addr &= BOARD_SIZE - 1;
	b &= 0xff;
	if (!(addr & 1) || addr >= 0x40)
		return;
	int index = (addr - 1) / 2;
	if (log_uvserial > 2)
		write_log(_T("UVSER: put %02x (duart %d reg %d) = %02x PC=%08x\n"),
			addr, index >> 4, index & 15, b, M68K_GETPC);
	uvserial_reg_put(index >> 4, index & 15, (uae_u8)b);
}

static uae_u32 REGPARAM2 uvserial_wget(uaecptr addr)
{
	return (uvserial_bget(addr) << 8) | uvserial_bget(addr + 1);
}
static uae_u32 REGPARAM2 uvserial_lget(uaecptr addr)
{
	return (uvserial_wget(addr) << 16) | uvserial_wget(addr + 2);
}
static void REGPARAM2 uvserial_wput(uaecptr addr, uae_u32 w)
{
	uvserial_bput(addr, w >> 8);
	uvserial_bput(addr + 1, w);
}
static void REGPARAM2 uvserial_lput(uaecptr addr, uae_u32 l)
{
	uvserial_wput(addr, l >> 16);
	uvserial_wput(addr + 2, l);
}

static addrbank uvserial_bank = {
	uvserial_lget, uvserial_wget, uvserial_bget,
	uvserial_lput, uvserial_wput, uvserial_bput,
	default_xlate, default_check, NULL, _T("*"), _T("UV multiserial"),
	dummy_lgeti, dummy_wgeti,
	ABFLAG_IO, S_READ, S_WRITE
};

static void uvserial_hard_reset(int hardreset)
{
	for (int i = 0; i < 2; i++) {
		struct uvduart *d = &duart[i];
		memset(d, 0, sizeof(struct uvduart));
		for (int j = 0; j < 2; j++) {
			struct uvchannel *c = &d->ch[j];
			c->port = i * 2 + j;
			c->backend = (c->port == 2) ? BACKEND_LD : BACKEND_NONE;
			c->mrptr = 0;
			c->rxhead = c->rxtail = 0;
			c->rxen = false;
			c->txen = false;
			update_sr(c);
		}
	}
	poll_divider = 0;
}

static void uvserial_free(void)
{
	uvserial_active = false;
}

bool uvserial_init(struct autoconfig_info *aci)
{
	const struct expansionromtype *ert = get_device_expansion_rom(ROMTYPE_UVSERIAL);
	if (!ert)
		return false;

	aci->addrbank = &uvserial_bank;
	aci->autoconfigp = ert->autoconfig;
	aci->autoconfig_automatic = true;
	for (int i = 0; i < 16; i++)
		aci->autoconfig_bytes[i] = ert->autoconfig[i];

	if (!aci->doinit)
		return true;

	uvserial_hard_reset(1);
	uvserial_active = true;

	/* the LaserDisc player hangs off port 2 of the card, the same place the
	 * real Sneak Prevue control unit wires its round DIN connector */
	pioneerld_activate();

	device_add_hsync(uvserial_hsync);
	device_add_rethink(uvserial_rethink);
	device_add_reset(uvserial_hard_reset);
	device_add_exit(uvserial_free, NULL);

	write_log(_T("UVSER: United Video multiserial card configured\n"));
	return true;
}
