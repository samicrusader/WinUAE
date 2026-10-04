/*
* UAE - The Un*x Amiga Emulator
*
* Pioneer LD-V8000 LaserDisc player emulation.
*
* The LD-V8000 talks the Level III line oriented ASCII protocol: the host
* sends [argument]MNEMONIC<CR>, several commands may share one line, and the
* player answers <text><CR>. Non-query commands answer with the completion
* message "R"; a reply beginning with 'E' is an error code (E00..E99).
*
* Written for the Sneak Prevue control unit, whose "vd" software drives the
* player over the second serial port of the United Video multiserial card.
*
* References, all from manuals.lddb.com:
*   LD-V8000 Level I & III User's Manual, TP 113 v2.1 (2/93)
*   PIB #151414  Listing of Level 3 Commands
*   PIB #151412  Error Messages
* Sneak Prevue "Installation and Operating Guide for LaserDisc System" names
* the LD-V8000 explicitly and cites Pioneer service manual ARP1758.
*
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "newcpu.h"
#include "devices.h"
#include "pioneerld.h"
#include "zfile.h"
#include <algorithm>
#ifdef VIDEOGRAB
#include "videograb.h"
#endif

int log_pioneerld = 1;

/* Player Active Mode Request codes, manual figure 4-O. */
#define LD_P_DOOR_OPEN   0
#define LD_P_PARK        1
#define LD_P_SETUP       2
#define LD_P_UNLOADING   3
#define LD_P_PLAY        4
#define LD_P_STILL       5
#define LD_P_PAUSE       6
#define LD_P_SEARCH      7
#define LD_P_SCAN        8
#define LD_P_MULTISPEED  9

#define CMDBUF_SIZE 128
#define TXBUF_SIZE 1024

/* The FRhmmssff argument is a base-30 encoding of an absolute disc frame
 * number, not a wall clock time: frame = ((h*3600 + mm*60 + ss) * 30) + ff.
 * Verified against Customer Data/DD9C060.dat, where the logged command
 * FR0443310SE resolves to frame 80200 = the "RushHour137" directory entry. */
#define LD_TC_BASE 30

/* Pioneer User's Code layout, manual section 4.7 command 38. 200 characters
 * of lead-in data: 120 of Disc Control Data, then 60 of Key Data (the
 * customer's own disc identifying information), then 20 of Control Data.
 * "vd" reads the disc label out of the Key Data field, which is why it looks
 * at the response buffer plus 120. */
#define LD_UC_TOTAL      200
#define LD_UC_KEY_OFFSET 120
#define LD_UC_KEY_LEN    60

/* Sneak Prevue's Sneak.ini [DISC] section defaults DefaultDiscID to "DD" and
 * IDLength to 5, and the data disk names its files DD<id>.dat / SO<id>.dat,
 * so the Key Data on a real Sneak disc reads "DD" followed by the 5 character
 * disc label. */
#define LD_UC_KEY_PREFIX "DD"

static bool ld_registered;
static int ld_mode;
static bool ld_disc_loaded;
static bool ld_spun_up;
static uae_s64 ld_frame;
/* Stop Marker: play stills automatically when this frame is reached,
 * and the marker clears itself. Manual command 18, page 4-20. */
static uae_s64 ld_mark;
static int ld_field;
static int ld_poslog;
static bool ld_mark_set;
/* last audio state handed to the grabber, -1 = never set */
static int ld_audio_ch = -1;
static int ld_audio_mute = -1;

/* Audio Control register, manual command 24. 0 off, 1 analogue Ch1, 2 analogue
 * Ch2, 3 analogue stereo, 4 off, 5/6/7 the digital tracks. A video capture only
 * carries the two analogue tracks on its left and right, so the digital
 * selections fall back to the matching analogue pair. */
static int ld_ad = 3;

/* Picture stops. The real disc carries stop codes in the vertical interval and
 * the player halts on them by itself - which is why Sneak sends no stop marker
 * for those segments and, without this, they play straight off the end. The
 * table is the disc's own segment boundaries, see mkstops.py. */
static uae_s64 *ld_stops;
static int ld_stops_num;
static bool ld_stops_loaded;
/* The player has a video frame memory (1MM sets Video Memory Mode at
 * boot). EM enables memory input, so the output follows live video; DM
 * disables it, so the output holds the frame already in memory while the
 * player goes off and plays somewhere else for its audio. That is how a
 * text page keeps a still background plate up over a music bed, and how
 * a seek is hidden (search mute). */
static bool ld_mem_hold;
static int ld_mem_pending;
static uae_s64 ld_stop_target;

static uae_u8 cmdbuf[CMDBUF_SIZE];
static int cmdlen;
static bool cmd_overflow;

static uae_u8 txbuf[TXBUF_SIZE];
static int txhead, txtail;

/* fallback disc label if the config does not name one */
static const char ld_disc_id_default[] = "9C060";

/* video frame that holds disc frame 0. Captures of a real disc usually start
 * inside the capture card's blue leader, so this is how far in the disc
 * actually begins. For "Sneak Prevue - 2-8-02.m4v" it is 14. */
static int ld_video_offset(void)
{
	return currprefs.genlock_ld_offset;
}

static void ld_get_disc_id(char *out, int outsize)
{
	int n = 0;
	for (; n < outsize - 1 && currprefs.genlock_ld_discid[n]; n++)
		out[n] = (char)currprefs.genlock_ld_discid[n];
	out[n] = 0;
	if (!n)
		strcpy(out, ld_disc_id_default);
}

/* NTSC CAV: 30000/1001 frames per second, so one frame is
 * 1e7 * 1001 / 30000 = 333666.7 units of 100ns. */
#define LD_TICKS_NUM 10000000LL
#define LD_TICKS_DEN 30000LL
#define LD_TICKS_1001 1001LL

static int ld_units;		/* 0 unknown, 1 frames, 2 100ns ticks */

#ifdef VIDEOGRAB
static uae_s64 ld_to_native(uae_s64 frame)
{
	if (!ld_units) {
		uae_s64 dur = getdurationvideograb();
		if (dur <= 0)
			return frame;
		/* a frame count for anything of a sane length stays well under
		 * this; a 100ns duration blows straight past it */
		ld_units = dur > 10000000LL ? 2 : 1;
		write_log(_T("PIONEERLD: grabber duration %lld, seeking in %hs\n"),
			dur, ld_units == 2 ? "100ns units" : "frames");
	}
	if (ld_units == 2)
		return frame * LD_TICKS_1001 * LD_TICKS_NUM / LD_TICKS_DEN;
	return frame;
}

static uae_s64 ld_from_native(uae_s64 native)
{
	if (ld_units == 2)
		return native * LD_TICKS_DEN / (LD_TICKS_1001 * LD_TICKS_NUM);
	return native;
}
#endif

static void tx_byte(uae_u8 b)
{
	int next = (txtail + 1) % TXBUF_SIZE;
	if (next == txhead) {
		write_log(_T("PIONEERLD: reply buffer overflow\n"));
		return;
	}
	txbuf[txtail] = b;
	txtail = next;
}

static void tx_raw(const char *s, int len)
{
	for (int i = 0; i < len; i++)
		tx_byte((uae_u8)s[i]);
	tx_byte(0x0d);
}

static void reply(const char *s)
{
	if (log_pioneerld)
		write_log(_T("PIONEERLD: -> \"%hs\"\n"), s);
	tx_raw(s, (int)strlen(s));
}

/* The Pioneer User's Code is fixed length and mostly padding, so log what it
 * carries rather than dumping 200 characters into the log. */
static void reply_user_code(void)
{
	char uc[LD_UC_TOTAL];
	char id[16];

	ld_get_disc_id(id, sizeof id);
	memset(uc, ' ', sizeof uc);

	int n = (int)strlen(LD_UC_KEY_PREFIX);
	memcpy(uc + LD_UC_KEY_OFFSET, LD_UC_KEY_PREFIX, n);
	int idlen = (int)strlen(id);
	if (n + idlen > LD_UC_KEY_LEN)
		idlen = LD_UC_KEY_LEN - n;
	memcpy(uc + LD_UC_KEY_OFFSET + n, id, idlen);

	if (log_pioneerld)
		write_log(_T("PIONEERLD: -> user code, key data \"%hs%hs\"\n"),
			LD_UC_KEY_PREFIX, id);
	tx_raw(uc, LD_UC_TOTAL);
}

/* One absolute CAV frame per line, '#' starts a comment. */
static void load_stops(void)
{
	if (ld_stops_loaded)
		return;
	ld_stops_loaded = true;
	if (!currprefs.genlock_ld_stops[0])
		return;
	struct zfile *z = zfile_fopen(currprefs.genlock_ld_stops, _T("rb"), ZFD_NORMAL);
	if (!z) {
		write_log(_T("PIONEERLD: cannot open stop table '%s'\n"),
			currprefs.genlock_ld_stops);
		return;
	}
	uae_s64 size = zfile_size(z);
	if (size > 0 && size < 4 * 1024 * 1024) {
		char *buf = xcalloc(char, (size_t)size + 1);
		if (buf && zfile_fread(buf, 1, (size_t)size, z) == (size_t)size) {
			int alloc = 256;
			ld_stops = xcalloc(uae_s64, alloc);
			for (char *p = buf; ld_stops && *p;) {
				while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
					p++;
				if (*p == '#') {
					while (*p && *p != '\n')
						p++;
					continue;
				}
				if (*p < '0' || *p > '9') {
					if (!*p)
						break;
					while (*p && *p != '\n')
						p++;
					continue;
				}
				uae_s64 v = 0;
				while (*p >= '0' && *p <= '9')
					v = v * 10 + (*p++ - '0');
				if (ld_stops_num >= alloc) {
					alloc *= 2;
					uae_s64 *t = xrealloc(uae_s64, ld_stops, alloc);
					if (!t)
						break;
					ld_stops = t;
				}
				ld_stops[ld_stops_num++] = v;
				while (*p && *p != '\n')
					p++;
			}
		}
		xfree(buf);
	}
	zfile_fclose(z);
	std::sort(ld_stops, ld_stops + ld_stops_num);
	write_log(_T("PIONEERLD: %d picture stops from '%s'\n"),
		ld_stops_num, currprefs.genlock_ld_stops);
}

/* The first stop strictly past this frame, or 0 if there is none. */
static uae_s64 next_stop(uae_s64 frame)
{
	load_stops();
	int lo = 0, hi = ld_stops_num;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (ld_stops[mid] <= frame)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < ld_stops_num ? ld_stops[lo] : 0;
}

static void seek_to(uae_s64 frame)
{
	if (frame < 0)
		frame = 0;
	ld_frame = frame;
	ld_field = 0;
	ld_stop_target = ld_mode == LD_P_PLAY ? next_stop(ld_frame) : 0;
#ifdef VIDEOGRAB
	if (isvideograb()) {
		uae_s64 vf = ld_frame + ld_video_offset();
		if (vf < 0)
			vf = 0;
		getsetpositionvideograb(ld_to_native(vf));
	}
#endif
}

static void set_mode(int mode)
{
	if (ld_mode == mode)
		return;
	ld_mode = mode;
	ld_stop_target = mode == LD_P_PLAY ? next_stop(ld_frame) : 0;
#ifdef VIDEOGRAB
	if (isvideograb())
		pausevideograb(mode == LD_P_PLAY ? 0 : 1);
#endif
	if (log_pioneerld)
		write_log(_T("PIONEERLD: mode P%02d frame %lld\n"), ld_mode, ld_frame);
}

/* The host sends a 7 digit argument: h mm ss ff, base 30. */
static uae_s64 timecode_to_frame(const char *digits, int len)
{
	if (len != 7)
		return -1;
	int h = digits[0] - '0';
	int mm = (digits[1] - '0') * 10 + (digits[2] - '0');
	int ss = (digits[3] - '0') * 10 + (digits[4] - '0');
	int ff = (digits[5] - '0') * 10 + (digits[6] - '0');
	return ((uae_s64)h * 3600 + mm * 60 + ss) * LD_TC_BASE + ff;
}

static void pioneerld_vsync(void)
{
	/* A genlock only keys once there is something to key to. custom_reset()
	 * clears genlock on a cold boot so the unit comes up showing the Amiga's
	 * own screen in its own colours, and the player turns it back on when the
	 * disc spins up - which is the moment a real unit has video on the loop
	 * through. BPLCON0 ERSY is no good as the trigger here: the Sneak boot
	 * sets it seconds before the player exists. */
	if (ld_spun_up && !currprefs.genlock) {
		changed_prefs.genlock = currprefs.genlock = 1;
		if (log_pioneerld)
			write_log(_T("PIONEERLD: disc running, enabling Genlock\n"));
	}
#ifdef VIDEOGRAB
	/* Anything outside the player may put the graph back into Run: the big
	 * one is devices_unpause(), which does an unconditional pausevideograb(0)
	 * every time the emulator resumes or the GUI closes, but specialmonitors
	 * also pauses on its own when the player is inactive. Nothing restores
	 * the state the player asked for, so a Still quietly becomes playback.
	 * Re-assert it here; pausevideograb() is a no-op when it already
	 * matches. */
#ifdef VIDEOGRAB
	if (ld_mem_pending > 0 && isvideograb()) {
		ld_mem_pending--;
		if (ld_mem_pending == 0) {
			setfreezevideograb(1);
			if (log_pioneerld)
				write_log(_T("PIONEERLD: memory input disabled, holding frame %lld\n"),
					ld_frame);
		}
	}
#endif

	if (isvideograb()) {
		bool want_paused = (ld_mode != LD_P_PLAY);
		if (getpausevideograb() != want_paused)
			pausevideograb(want_paused ? 1 : 0);

		/* Nothing ever claimed the grabber's audio for this player, so it sat
		 * on the chflags=0 path that initvideograb() leaves behind. Take it:
		 * a LaserDisc carries two independent analogue tracks on left and
		 * right, and the host picks between them with AD. */
		int want_ch = ld_ad;
		int want_mute = currprefs.genlock_audio_mute ? 1 : 0;
		if (ld_audio_ch != want_ch || ld_audio_mute != want_mute) {
			ld_audio_ch = want_ch;
			ld_audio_mute = want_mute;
			setchflagsvideograb(want_ch, want_mute != 0);
			if (log_pioneerld)
				write_log(_T("PIONEERLD: audio channels %d, mute %d\n"),
					want_ch, want_mute);
		}
	}
#endif
	if (ld_mode != LD_P_PLAY)
		return;
	/* CAV: 29.97 frames/s, one disc frame per two NTSC fields. */
	ld_field ^= 1;
	if (!ld_field)
		ld_frame++;
#ifdef VIDEOGRAB
	if (log_pioneerld && isvideograb() && ++ld_poslog >= 600) {
		ld_poslog = 0;
		write_log(_T("PIONEERLD: frame %lld, grabber frame %lld\n"),
			ld_frame,
			ld_from_native(getsetpositionvideograb(-1)) - ld_video_offset());
	}
#endif
	/* "When the stop marker address is reached in Play or Multi-Speed Mode,
	 * Still Mode occurs and the stop marker is cleared." No completion
	 * status is returned when the marker is reached. */
	/* Whichever comes first wins. The marker must not merely GATE the picture
	 * stop: vd arms markers it then abandons by moving on early, and a marker
	 * that is pending-but-never-reached stays armed, which used to disable
	 * picture stops for the rest of the session. On the disc the stop code is
	 * in the vertical interval and fires whatever the host has armed. */
	if (ld_mark_set && ld_frame >= ld_mark) {
		ld_mark_set = false;
		if (log_pioneerld)
			write_log(_T("PIONEERLD: stop marker %lld reached\n"), ld_mark);
		set_mode(LD_P_STILL);
	} else if (ld_stop_target && ld_frame >= ld_stop_target) {
		/* No marker for this segment, so on the real disc it ended on a
		 * picture stop pressed into the vertical interval. A capture has no
		 * VBI, so stop at the segment boundary the disc directory gives us.
		 * A marker always wins: Sneak only omits one when it is relying on
		 * the disc to stop the player. */
		if (log_pioneerld)
			write_log(_T("PIONEERLD: picture stop %lld reached\n"), ld_stop_target);
		ld_stop_target = 0;
		set_mode(LD_P_STILL);
	}
}

static void pioneerld_hard_reset(int hardreset)
{
	ld_mode = LD_P_PARK;
	ld_spun_up = false;
	ld_frame = 0;
	ld_mark_set = false;
	ld_stop_target = 0;
	ld_mem_hold = false;
	ld_mem_pending = 0;
#ifdef VIDEOGRAB
	if (isvideograb())
		setfreezevideograb(0);
#endif
	ld_ad = 3;
	ld_audio_ch = -1;
	ld_audio_mute = -1;
	cmdlen = 0;
	cmd_overflow = false;
	txhead = txtail = 0;
	ld_disc_loaded = true;
#ifdef VIDEOGRAB
	if (isvideograb())
		pausevideograb(1);
#endif
}

void pioneerld_reset(void)
{
	pioneerld_hard_reset(1);
}

void pioneerld_activate(void)
{
	if (ld_registered)
		return;
	ld_registered = true;
	ld_disc_loaded = true;
	ld_spun_up = false;
	ld_mark_set = false;
	ld_mode = LD_P_PARK;
	device_add_vsync_pre(pioneerld_vsync);
	device_add_reset(pioneerld_hard_reset);
#ifdef VIDEOGRAB
	if (isvideograb())
		pausevideograb(1);
#endif
	write_log(_T("PIONEERLD: LD-V8000 emulation active\n"));
}

/* Once the disc has been started the player owns the genlock video path, even
 * while it is parked on a still, so specialmonitors stops free running it. */
bool pioneerld_active(void)
{
	return ld_spun_up;
}

bool pioneerld_video_enabled(void)
{
	return ld_mode == LD_P_PLAY || ld_mode == LD_P_STILL;
}

/* ------------------------------------------------------------ dispatch --- */

struct ld_result {
	bool quiet;			/* a query already answered, do not add "R" */
	const char *error;	/* non NULL: stop the line and report this */
};

static void do_command(const char *mn, const char *arg, int arglen,
	struct ld_result *res)
{
	if (!strcmp(mn, "?P")) {
		char b[8];
		sprintf(b, "P%02d", ld_mode);
		reply(b);
		res->quiet = true;
		return;
	}
	if (!strcmp(mn, "?D")) {
		/* C1 disc loading, C2 CAV/CLV, C3 size, C4 side, C5 chapter code.
		 * A Sneak disc is a 12 inch CAV side 1 pressing with chapter codes. */
		reply(ld_disc_loaded ? "10001" : "0XXXX");
		res->quiet = true;
		return;
	}
	if (!strcmp(mn, "?U")) {
		if (!ld_disc_loaded) {
			res->error = "E11";
			return;
		}
		reply_user_code();
		res->quiet = true;
		return;
	}
	if (!strcmp(mn, "?F")) {
		char b[16];
		sprintf(b, "%05d", (int)(ld_frame % 100000));
		reply(b);
		res->quiet = true;
		return;
	}
	if (!strcmp(mn, "?X")) {
		reply("P150601");
		res->quiet = true;
		return;
	}
	if (!strcmp(mn, "SA")) {
		/* Start: spin the disc up and leave park. The manual recommends ?U be
		 * issued straight after SA, which only works on a spinning disc. */
		if (!ld_disc_loaded) {
			res->error = "E11";
			return;
		}
		ld_spun_up = true;
		seek_to(0);
		set_mode(LD_P_SETUP);
		return;
	}
	if (!strcmp(mn, "SE")) {
		uae_s64 f = -1;
		if (arglen == 7)
			f = timecode_to_frame(arg, arglen);
		else if (arglen > 0) {
			f = 0;
			for (int i = 0; i < arglen; i++)
				f = f * 10 + (arg[i] - '0');
		}
		if (arglen <= 0) {
			res->error = "E06";
			return;
		}
		if (f < 0) {
			res->error = "E12";
			return;
		}
		ld_spun_up = true;
		seek_to(f);
		set_mode(LD_P_STILL);
		return;
	}
	if (!strcmp(mn, "SM")) {
		/* Stop Marker. The completion status is returned when the marker is
		 * set, not when it is reached. Sneak uses this to end each segment of
		 * the play script, so without it playback runs straight off the end. */
		if (arglen <= 0) {
			res->error = "E06";
			return;
		}
		uae_s64 f = (arglen == 7) ? timecode_to_frame(arg, arglen) : -1;
		if (f < 0) {
			f = 0;
			for (int i = 0; i < arglen; i++)
				f = f * 10 + (arg[i] - '0');
		}
		ld_mark = f;
		ld_mark_set = true;
		if (log_pioneerld)
			write_log(_T("PIONEERLD: stop marker set to %lld\n"), ld_mark);
		return;
	}
	if (!strcmp(mn, "EM")) {
		/* Enable Memory Input: the output follows the disc again. */
		ld_mem_pending = 0;
		if (ld_mem_hold) {
			ld_mem_hold = false;
#ifdef VIDEOGRAB
			if (isvideograb())
				setfreezevideograb(0);
#endif
			if (log_pioneerld)
				write_log(_T("PIONEERLD: memory input enabled, live video\n"));
		}
		return;
	}
	if (!strcmp(mn, "DM")) {
		/* Disable Memory Input: the output holds what is in memory. Take the
		 * snapshot a few vsyncs later so a seek issued just before has landed
		 * and we hold the frame that was actually asked for. */
		if (!ld_mem_hold) {
			ld_mem_hold = true;
			ld_mem_pending = 4;
		}
		return;
	}
	if (!strcmp(mn, "CL")) {
		/* "If a CLEAR or a REJECT command is sent before the marker is
		 * reached, it is cleared." */
		ld_mark_set = false;
		return;
	}
	if (!strcmp(mn, "PL")) {
		if (!ld_disc_loaded) {
			res->error = "E11";
			return;
		}
		if (arglen > 0) {
			uae_s64 f = (arglen == 7) ? timecode_to_frame(arg, arglen) : -1;
			if (f >= 0)
				seek_to(f);
		}
		ld_spun_up = true;
		set_mode(LD_P_PLAY);
		return;
	}
	if (!strcmp(mn, "ST")) {
		set_mode(LD_P_STILL);
		return;
	}
	if (!strcmp(mn, "PA")) {
		set_mode(LD_P_PAUSE);
		return;
	}
	if (!strcmp(mn, "RJ")) {
		ld_mark_set = false;
		ld_spun_up = false;
		set_mode(LD_P_PARK);
		return;
	}
	if (!strcmp(mn, "OP")) {
		ld_spun_up = false;
		ld_disc_loaded = false;
		set_mode(LD_P_DOOR_OPEN);
		return;
	}
	if (!strcmp(mn, "CO")) {
		ld_disc_loaded = true;
		set_mode(LD_P_PARK);
		return;
	}
	if (!strcmp(mn, "AD")) {
		/* Audio Control, manual command 24 and figure 4-I. Sneak drives this
		 * itself - "2AD" and "0EM1AD" are all over the play script - so the
		 * audio switcher is the player's, not ours. A capture carries the two
		 * analogue tracks as its left and right, so 5/6/7 (the digital
		 * selections) fall back to the matching analogue pair. */
		int v = 3;
		if (arglen > 0) {
			v = 0;
			for (int i = 0; i < arglen; i++)
				v = v * 10 + (arg[i] - '0');
		}
		switch (v) {
		case 0: case 4: ld_ad = 0; break;	/* off */
		case 1: case 5: ld_ad = 1; break;	/* Ch1 only, to both speakers */
		case 2: case 6: ld_ad = 2; break;	/* Ch2 only */
		case 3: case 7: ld_ad = 3; break;	/* stereo */
		default:
			res->error = "E06";
			return;
		}
		if (log_pioneerld)
			write_log(_T("PIONEERLD: audio control %d -> channels %d\n"), v, ld_ad);
		return;
	}

	/* Accepted and ignored: the setup strings Sneak sends are
	 *   *H 0DS CL      program halt, display off, clear
	 *   1MM 0EM 0DM    video memory mode, enable/disable memory input
	 * none of which change anything we model. FR selects frame addressing,
	 * which is the only addressing mode we support anyway. */
	if (!strcmp(mn, "CS") || !strcmp(mn, "FR") ||
		!strcmp(mn, "TM") || !strcmp(mn, "CH") || !strcmp(mn, "DS") ||
		!strcmp(mn, "MM") || !strcmp(mn, "VM") ||
		!strcmp(mn, "RM") || !strcmp(mn, "IM") ||
		!strcmp(mn, "AS") || !strcmp(mn, "VD") ||
		!strcmp(mn, "KL") || !strcmp(mn, "BP") || !strcmp(mn, "CM") ||
		!strcmp(mn, "LO") || !strcmp(mn, "SP") ||
		!strcmp(mn, "*H") || !strcmp(mn, "*S") || !strcmp(mn, "*R")) {
		return;
	}

	write_log(_T("PIONEERLD: unhandled mnemonic \"%hs\"\n"), mn);
	res->error = "E04";
}

/* A Level III line may carry several [argument]MNEMONIC pairs back to back,
 * and the completion message is sent once the whole line has executed. */
static void execute(void)
{
	const char *c = (const char*)cmdbuf;
	int len = cmdlen;
	int i = 0;
	struct ld_result res;

	if (log_pioneerld)
		write_log(_T("PIONEERLD: <- \"%hs\"\n"), c);

	res.quiet = false;
	res.error = NULL;

	while (i < len) {
		char mn[3];
		const char *arg;
		int arglen;

		if (c[i] == ' ' || c[i] == '\t') {
			i++;
			continue;
		}
		arg = c + i;
		while (i < len && c[i] >= '0' && c[i] <= '9')
			i++;
		arglen = (int)(c + i - arg);
		if (i >= len) {
			/* digits with no mnemonic after them */
			if (arglen > 0)
				res.error = "E04";
			break;
		}
		mn[0] = c[i++];
		if (i >= len) {
			res.error = "E04";
			break;
		}
		mn[1] = c[i++];
		mn[2] = 0;
		if (mn[0] >= 'a' && mn[0] <= 'z')
			mn[0] -= 32;
		if (mn[1] >= 'a' && mn[1] <= 'z')
			mn[1] -= 32;

		do_command(mn, arg, arglen, &res);
		if (res.error)
			break;
	}

	if (res.error)
		reply(res.error);
	else if (!res.quiet)
		reply("R");
}

void pioneerld_put(uae_u8 b)
{
	pioneerld_activate();

	if (b == 0x0d) {
		if (cmd_overflow) {
			cmd_overflow = false;
			cmdlen = 0;
			reply("E00");
			return;
		}
		cmdbuf[cmdlen] = 0;
		if (cmdlen)
			execute();
		cmdlen = 0;
		return;
	}
	if (b == 0x0a || b == 0x00)
		return;
	if (cmdlen >= CMDBUF_SIZE - 1) {
		cmd_overflow = true;
		return;
	}
	cmdbuf[cmdlen++] = b;
}

int pioneerld_get(void)
{
	if (txhead == txtail)
		return -1;
	uae_u8 v = txbuf[txhead];
	txhead = (txhead + 1) % TXBUF_SIZE;
	return v;
}
