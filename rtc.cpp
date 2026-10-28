/*
* UAE - The Un*x Amiga Emulator
*
* RTC chips
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"

#include "rtc.h"

/*
 * Time register writes
 *
 * The clock runs on host time: time(0) + cs_rtc_adjust + delta, where delta
 * belongs to the chip and starts at 0. A guest write to a time digit
 * (registers 0x0-0xC) goes into a 13-digit shadow, filled from the current
 * time on the first write, and delta is recomputed from the whole shadow
 * after every digit. That makes the result independent of the order the
 * guest writes the digits in. While the clock is held or stopped (MSM6242B
 * HOLD or STOP, RF5C01A timer disabled) the shadow stays and reads return the
 * written digits, as on the chip, so a date can pass through invalid
 * combinations (31 Oct -> 31 Feb -> 27 Feb) without being normalised. When
 * the clock runs again, or after a write while it runs, the shadow goes.
 *
 * delta is runtime state: it is not saved to rtc_file or to savestates.
 */

static int rtc_hour12(int hour)
{
	hour %= 12;
	return hour ? hour : 12;
}

static int rtc_hour_from_digits(int tens, int units, bool h24, bool pm)
{
	int h = tens * 10 + units;
	if (!h24) {
		h %= 12; // 12 AM = 0
		if (pm)
			h += 12;
	}
	return h;
}

static int rtc_year_from_digits(int tens, int units)
{
	int y = tens * 10 + units; // years since 1900, or a two-digit year
	if (y < 78)
		y += 100;
	return y;
}

static time_t rtc_host_time(void)
{
	time_t t = time(0);
	t += currprefs.cs_rtc_adjust;
	return t;
}

// returns false if the digits do not make a time mktime() accepts
static bool rtc_set_delta(int *delta, struct tm *tm)
{
	tm->tm_isdst = -1;
	time_t t = mktime(tm);
	if (t == (time_t)-1)
		return false;
	*delta = (int)(t - rtc_host_time());
	return true;
}

static void rtc_log_set(const TCHAR *chip, int olddelta, int delta)
{
	if (olddelta == delta)
		return;
	time_t t = rtc_host_time() + delta;
	struct tm *ct = localtime(&t);
	if (!ct)
		return;
	write_log(_T("RTC %s: guest set clock to %04d-%02d-%02d %02d:%02d:%02d (offset %+d s from host%s)\n"),
		chip, ct->tm_year + 1900, ct->tm_mon + 1, ct->tm_mday, ct->tm_hour, ct->tm_min, ct->tm_sec,
		delta, currprefs.cs_rtc_adjust ? _T(" + chipset_rtc_adjust") : _T(""));
}

/* MSM6242B */

time_t rtc_msm_time(const struct rtc_msm_data *data)
{
	time_t t = rtc_host_time();
	if (data)
		t += data->delta;
	return t;
}

static bool msm_running(const struct rtc_msm_data *data)
{
	// D bit 0 = HOLD, F bit 1 = STOP
	return !(data->clock_control_d & 1) && !(data->clock_control_f & 2);
}

static uae_u8 msm_time_digit(const struct rtc_msm_data *data, int addr, const struct tm *ct)
{
	uae_u8 v = 0;
	int year = ct->tm_year;
	if (data->yearmode && year >= 100)
		year -= 100;

	switch (addr)
	{
		case 0x0: v = ct->tm_sec % 10; break;
		case 0x1: v = ct->tm_sec / 10; break;
		case 0x2: v = ct->tm_min % 10; break;
		case 0x3: v = ct->tm_min / 10; break;
		case 0x4:
			if (data->clock_control_f & 4)
				v = ct->tm_hour % 10; // 24h
			else
				v = rtc_hour12(ct->tm_hour) % 10; // 12h, 1-12
			break;
		case 0x5:
			if (data->clock_control_f & 4) {
				v = ct->tm_hour / 10; // 24h
			} else {
				v = rtc_hour12(ct->tm_hour) / 10; // 12h
				v |= ct->tm_hour >= 12 ? 4 : 0; // AM/PM bit
			}
			break;
		case 0x6: v = ct->tm_mday % 10; break;
		case 0x7: v = ct->tm_mday / 10; break;
		case 0x8: v = (ct->tm_mon + 1) % 10; break;
		case 0x9: v = (ct->tm_mon + 1) / 10; break;
		case 0xA: v = year % 10; break;
		case 0xB: v = (year / 10) & 0x0f;  break;
		case 0xC: v = ct->tm_wday; break;
	}
	return v;
}

uae_u8 get_clock_msm(struct rtc_msm_data *data, int addr, struct tm *ct)
{
	switch (addr)
	{
		case 0xD: return data->clock_control_d;
		case 0xE: return data->clock_control_e;
		case 0xF: return data->clock_control_f;
	}
	if (addr < 0 || addr > 0xC)
		return 0;
	if (data->shadow_valid)
		return data->shadow[addr];
	if (!ct) {
		time_t t = rtc_msm_time(data);
		ct = localtime (&t);
		if (!ct)
			return 0;
	}
	return msm_time_digit(data, addr, ct);
}

static void msm_commit(struct rtc_msm_data *data)
{
	const uae_u8 *s = data->shadow;
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	tm.tm_sec = s[1] * 10 + s[0];
	tm.tm_min = s[3] * 10 + s[2];
	tm.tm_hour = rtc_hour_from_digits(s[5] & 3, s[4], (data->clock_control_f & 4) != 0, (s[5] & 4) != 0);
	tm.tm_mday = s[7] * 10 + s[6];
	tm.tm_mon = s[9] * 10 + s[8] - 1;
	tm.tm_year = rtc_year_from_digits(s[11], s[10]);
	rtc_set_delta(&data->delta, &tm);
}

static void msm_put_time(struct rtc_msm_data *data, int addr, uae_u8 v)
{
	if (!data->shadow_valid) {
		time_t t = rtc_msm_time(data);
		struct tm *ct = localtime(&t);
		if (!ct)
			return;
		for (int i = 0; i < 13; i++)
			data->shadow[i] = msm_time_digit(data, i, ct);
		data->shadow_valid = true;
		data->shadow_delta = data->delta;
	}
	data->shadow[addr] = v & 0x0f;
	msm_commit(data);
}

bool put_clock_msm(struct rtc_msm_data *data, int addr, uae_u8 v)
{
	switch (addr)
	{
		case 0xD:
			if (data->clock_control_d != (v & (1 | 8))) {
				data->clock_control_d = v & (1 | 8);
				data->delayed_write = -1;
			}
		break;
		case 0xE:
			if (data->clock_control_e != v) {
				data->clock_control_e = v;
				data->delayed_write = -1;
			}
			break;
		case 0xF:
			if (data->clock_control_f != v) {
				data->clock_control_f = v;
				data->delayed_write = -1;
			}
		break;
		default:
			if (addr >= 0 && addr <= 0xC)
				msm_put_time(data, addr, v);
			break;
	}
	if (data->shadow_valid && msm_running(data)) {
		data->shadow_valid = false;
		rtc_log_set(_T("MSM6242B"), data->shadow_delta, data->delta);
	}
	return false;
}

/* RF5C01A */

time_t rtc_ricoh_time(const struct rtc_ricoh_data *data)
{
	time_t t = rtc_host_time();
	if (data)
		t += data->delta;
	return t;
}

static bool ricoh_running(const struct rtc_ricoh_data *data)
{
	// D bit 3 = timer enable
	return (data->clock_control_d & 8) != 0;
}

static uae_u8 ricoh_time_digit(const struct rtc_ricoh_data *data, int addr, const struct tm *ct)
{
	uae_u8 v = 0;
	switch (addr)
	{
		case 0x0: v = ct->tm_sec % 10; break;
		case 0x1: v = ct->tm_sec / 10; break;
		case 0x2: v = ct->tm_min % 10; break;
		case 0x3: v = ct->tm_min / 10; break;
		case 0x4:
			if (data->rtc_alarm[10] & 1)
				v = ct->tm_hour % 10; // 24h
			else
				v = rtc_hour12(ct->tm_hour) % 10; // 12h, 1-12
			break;
		case 0x5:
			if (data->rtc_alarm[10] & 1)
				v = ct->tm_hour / 10; // 24h
			else
				v = (rtc_hour12(ct->tm_hour) / 10) | (ct->tm_hour >= 12 ? 2 : 0); // 12h
		break;
		case 0x6: v = ct->tm_wday; break;
		case 0x7: v = ct->tm_mday % 10; break;
		case 0x8: v = ct->tm_mday / 10; break;
		case 0x9: v = (ct->tm_mon + 1) % 10; break;
		case 0xA: v = (ct->tm_mon + 1) / 10; break;
		case 0xB: v = (ct->tm_year % 100) % 10; break;
		case 0xC: v = (ct->tm_year % 100) / 10; break;
	}
	return v;
}

uae_u8 get_clock_ricoh(struct rtc_ricoh_data *data, int addr, struct tm *ct)
{
	uae_u8 v = 0;
	int bank = data->clock_control_d & 3;

	/* memory access */
	if (bank >= 2 && addr < 0x0d)
		return (data->rtc_memory[addr] >> ((bank == 2) ? 0 : 4)) & 0x0f;
	/* alarm */
	if (bank == 1 && addr < 0x0d) {
		v = data->rtc_alarm[addr];
#if CLOCK_DEBUG
		write_log (_T("CLOCK ALARM R %X: %X\n"), addr, v);
#endif
		return v;
	}
	switch (addr)
	{
		case 0xD: return data->clock_control_d;
		/* E and F = write-only, reads as zero */
		case 0xE: return 0;
		case 0xF: return 0;
	}
	if (addr < 0 || addr > 0xC)
		return 0;
	if (data->shadow_valid)
		return data->shadow[addr];
	if (!ct) {
		time_t t = rtc_ricoh_time(data);
		ct = localtime (&t);
		if (!ct)
			return 0;
	}
	return ricoh_time_digit(data, addr, ct);
}

static void ricoh_commit(struct rtc_ricoh_data *data)
{
	const uae_u8 *s = data->shadow;
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	tm.tm_sec = s[1] * 10 + s[0];
	tm.tm_min = s[3] * 10 + s[2];
	tm.tm_hour = rtc_hour_from_digits(s[5] & 3, s[4], (data->rtc_alarm[10] & 1) != 0, (s[5] & 2) != 0);
	tm.tm_mday = s[8] * 10 + s[7];
	tm.tm_mon = s[10] * 10 + s[9] - 1;
	tm.tm_year = rtc_year_from_digits(s[12], s[11]);
	rtc_set_delta(&data->delta, &tm);
}

static void ricoh_put_time(struct rtc_ricoh_data *data, int addr, uae_u8 v)
{
	if (!data->shadow_valid) {
		time_t t = rtc_ricoh_time(data);
		struct tm *ct = localtime(&t);
		if (!ct)
			return;
		for (int i = 0; i < 13; i++)
			data->shadow[i] = ricoh_time_digit(data, i, ct);
		data->shadow_valid = true;
		data->shadow_delta = data->delta;
	}
	data->shadow[addr] = v & 0x0f;
	ricoh_commit(data);
}

void put_clock_ricoh(struct rtc_ricoh_data *data, int addr, uae_u8 v)
{
	int bank = data->clock_control_d & 3;
	/* memory access */
	if (bank >= 2 && addr < 0x0d) {
		uae_u8 ov = data->rtc_memory[addr];
		data->rtc_memory[addr] &= ((bank == 2) ? 0xf0 : 0x0f);
		data->rtc_memory[addr] |= v << ((bank == 2) ? 0 : 4);
		if (data->rtc_memory[addr] != ov)
			data->delayed_write = -1;
		return;
	}
	/* alarm */
	if (bank == 1 && addr < 0x0d) {
#if CLOCK_DEBUG
		write_log (_T("CLOCK ALARM W %X: %X\n"), addr, value);
#endif
		uae_u8 ov = data->rtc_alarm[addr];
		data->rtc_alarm[addr] = v;
		data->rtc_alarm[0] = data->rtc_alarm[1] = data->rtc_alarm[9] = data->rtc_alarm[12] = 0;
		data->rtc_alarm[3] &= ~0x8;
		data->rtc_alarm[5] &= ~0xc;
		data->rtc_alarm[6] &= ~0x8;
		data->rtc_alarm[8] &= ~0xc;
		data->rtc_alarm[10] &= ~0xe;
		data->rtc_alarm[11] &= ~0xc;
		if (data->rtc_alarm[addr] != ov)
			data->delayed_write = -1;
		return;
	}
#if CLOCK_DEBUG
	write_log (_T("CLOCK W %X: %X\n"), addr, value);
#endif
	switch (addr)
	{
		case 0xD:
			if (data->clock_control_d != v) {
				data->clock_control_d = v;
				data->delayed_write = -1;
			}
			break;
		case 0xE:
			if (data->clock_control_e != v) {
				data->clock_control_e = v;
				data->delayed_write = -1;
			}
			break;
		case 0xF:
			if (data->clock_control_f != v) {
				data->clock_control_f = v;
				data->delayed_write = -1;
			}
			break;
		default:
			if (addr >= 0 && addr <= 0xC)
				ricoh_put_time(data, addr, v);
			break;
	}
	if (data->shadow_valid && ricoh_running(data)) {
		data->shadow_valid = false;
		rtc_log_set(_T("RF5C01A"), data->shadow_delta, data->delta);
	}
}
