/*
 * UAE - The Un*x Amiga Emulator
 *
 * Genlock video source on FFmpeg (libavformat / libavcodec / libavfilter /
 * libswresample). Replaces the DirectShow backend (win32_videograb.cpp).
 *
 * Design (PrevueWork/shared/ffmpeg.md):
 *
 *  - Demuxing and decoding run on their own thread. The emulator thread
 *    only picks finished frames out of a short queue, so a slow open, a
 *    seek back to a keyframe or a network stall never holds up emulation.
 *
 *  - The picture is clocked by the emulated display, the way a genlock
 *    slaves the Amiga to its video: videograb_vsync() advances the play
 *    position by one field per emulated vsync. Pausing, warp and
 *    single-stepping the emulator move the video with it, and a position is
 *    always a whole source frame (getsetpositionvideograb() works in frames,
 *    which is what the LaserDisc models have always assumed).
 *
 *  - Seeks are frame accurate: seek back to the keyframe, decode forward,
 *    drop everything before the target.
 *
 *  - Frames come out already scaled to the genlock area (setvideograbtarget)
 *    through a libavfilter graph: [deinterlace] -> [user filter chain] ->
 *    scale (selectable algorithm, field-wise for interlaced material) ->
 *    bgr24. do_genlock() then copies 1:1 instead of point-sampling.
 *
 *  - Audio is resampled to 48 kHz stereo and fed into WinUAE's own mixer as
 *    one of its extra streams (audio_enable_stream), clocked by emulated
 *    time like every other sound source. The LaserDisc channel select
 *    (setchflagsvideograb) is applied there.
 *
 *  Sources: "Media file" (anything libavformat opens, stills included) and
 *  "Live stream" (a URL: no seeking, newest frame wins, latency bounded,
 *  reconnects on its own).
 */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef VIDEOGRAB_LIBAV

#include "options.h"
#include "custom.h"
#include "audio.h"
#include "videograb.h"
#include "videograb_libav.h"
#include "events.h"

#include <chrono>
#include <cmath>

static vg_state *vg;
static float vg_audio_clk;
static double vg_field_time(vg_state *s);

static std::string vg_utf8(const TCHAR *s)
{
	char *u = uutf8(s);
	std::string r = u ? u : "";
	xfree(u);
	return r;
}

static void vg_read_prefs(vg_state *s, const struct uae_prefs *p)
{
	s->opt_scaler = p->genlock_video_scaler >= 0 && p->genlock_video_scaler < vgl_num_scalers() ? p->genlock_video_scaler : 0;
	s->opt_deint = p->genlock_video_deinterlace >= 0 && p->genlock_video_deinterlace < vgl_num_deints() ? p->genlock_video_deinterlace : 0;
	s->opt_fields = p->genlock_video_fields;
	s->opt_filter = vg_utf8(p->genlock_video_filter);
}

static bool audio_state_videograb(int streamid, void *state)
{
	vg_state *s = vg;
	int out[2] = { 0, 0 };
	if (s && !s->paused && s->audio_hold > 0) {
		s->audio_hold--;
	} else if (s && !s->paused) {
		if (s->abuf_pos >= s->abuf_n) {
			std::lock_guard<std::mutex> l(s->am);
			s->abuf_t = s->ring_rt;
			size_t cap = s->ring.size() / 2;
			size_t n = s->ring_fill;
			if (n > sizeof s->abuf / sizeof s->abuf[0] / 2)
				n = sizeof s->abuf / sizeof s->abuf[0] / 2;
			for (size_t i = 0; i < n; i++) {
				s->abuf[i * 2 + 0] = s->ring[s->ring_r * 2 + 0];
				s->abuf[i * 2 + 1] = s->ring[s->ring_r * 2 + 1];
				s->ring_r = (s->ring_r + 1) % cap;
			}
			s->ring_fill -= n;
			if (s->ring_rt >= 0)
				s->ring_rt += (double)n / VG_AUDIO_RATE;
			s->abuf_n = (int)n;
			s->abuf_pos = 0;
			if (!n && s->has_audio)
				s->audio_underruns++;
		}
		if (s->abuf_pos < s->abuf_n) {
			int l = s->abuf[s->abuf_pos * 2 + 0];
			int r = s->abuf[s->abuf_pos * 2 + 1];
			s->abuf_pos++;
			/* the selected track goes to both outputs (LD channel select) */
			if (s->chflags == 1)
				r = l;
			else if (s->chflags == 2)
				l = r;
			if (s->chflags && !s->mute && s->sound_output) {
				int vol = s->volume * 32768 / 100;
				out[0] = l * vol / 32768;
				out[1] = r * vol / 32768;
			}
		}
	}
	/* Paced by the fields, not the sample clock: exactly 48000 / vblank_hz
	 * samples per vsync, the amount of media time videograb_vsync() moves
	 * the picture on. The two clocks can't drift apart then (they did, by
	 * ~0.6% on a 263-line NTSC display), at the price of a pitch error
	 * of the same size, which nobody hears. */
	unsigned int evt;
	if (s && s->cycles_per_field > 0)
		evt = (unsigned int)(s->cycles_per_field / (vg_field_time(s) * VG_AUDIO_RATE));
	else
		evt = (unsigned int)(vg_audio_clk * CYCLE_UNIT / VG_AUDIO_RATE);
	if (!evt)
		evt = CYCLE_UNIT;
	audio_state_stream_state(streamid, out, 2, evt);
	return true;
}

void videograb_update_sound(float clk)
{
	vg_audio_clk = clk;
}

/* ------------------------------------------------------------------ */
/* emulator-side API (videograb.h) */

static void vg_close(vg_state *s)
{
	vgl_stop(s);
	if (s->streamid > 0)
		audio_enable_stream(false, s->streamid, 0, NULL, NULL);
	s->streamid = 0;
	for (vg_frame *f : s->queue)
		delete f;
	for (vg_frame *f : s->pool)
		delete f;
	delete s->cur;
	s->queue.clear();
	s->pool.clear();
	s->cur = nullptr;
}

void uninitvideograb(void)
{
	if (!vg)
		return;
	write_log(_T("VIDEOGRAB: closing '%hs'\n"), vg->url.c_str());
	vg_state *s = vg;
	vg = nullptr;
	vg_close(s);
	delete s;
}

bool initvideograb(const TCHAR *filename)
{
	uninitvideograb();
	write_log(_T("initvideograb '%s'\n"), filename ? filename : _T("<null>"));
	if (!filename || !filename[0])
		return false;	/* capture devices are gone; a live source is a URL */

	vgl_global_init();

	vg_state *s = new vg_state;
	s->url = vg_utf8(filename);
	s->live = currprefs.genlock_image == 5;
	s->ld = currprefs.genlock_image >= 6;
	s->ring.resize(VG_AUDIO_RING * 2);
	vg_read_prefs(s, &currprefs);
	/* a plain video plays its sound; LaserDisc players come up parked and
	 * pick their tracks over the serial line */
	s->chflags = currprefs.genlock_image >= 6 ? 0 : 3;
	s->volume = 100 - currprefs.sound_volume_genlock;
	s->mute = false;

	if (!vgl_alloc(s)) {
		vg_close(s);
		delete s;
		return false;
	}
	if (!s->live) {
		/* files open here, synchronously: the LD models want the duration now */
		if (!vgl_open_input(s)) {
			vg_close(s);
			delete s;
			return false;
		}
		s->opened = true;
	}
	if (s->live || s->has_audio)
		s->streamid = audio_enable_stream(true, -1, 2, audio_state_videograb, NULL);
	vg = s;
	vgl_start(s);
	return true;
}

bool isvideograb(void)
{
	return vg != nullptr;
}

static void vg_request_seek(vg_state *s, double t)
{
	{
		std::lock_guard<std::mutex> l(s->m);
		s->seek_req = t;
		s->gen++;
		vg_clear_queue_locked(s);	/* nothing decoded before the seek may show after it */
		std::lock_guard<std::mutex> la(s->am);
		s->ring_r = s->ring_w = s->ring_fill = 0;
		s->ring_rt = -1;
		s->ring_gen = s->gen;		/* and no sound from before it either */
	}
	s->cv.notify_all();
	s->abuf_n = s->abuf_pos = 0;
	s->abuf_t = -1;
	s->audio_hold = 0;
}

/* The sound follows the picture, as it would off a genlocked source: a
 * decoder that fell behind (underrun) or a stall leaves the audio late, so
 * drop what's late or wait out what's early, once it is off by more than a
 * couple of fields. */
static void vg_audio_resync(vg_state *s)
{
	if (!s->has_audio || s->live)
		return;
	double at;
	if (s->abuf_pos < s->abuf_n && s->abuf_t >= 0) {
		at = s->abuf_t + (double)s->abuf_pos / VG_AUDIO_RATE;
	} else {
		std::lock_guard<std::mutex> l(s->am);
		if (!s->ring_fill || s->ring_rt < 0)
			return;
		at = s->ring_rt;
	}
	at -= (double)s->audio_hold / VG_AUDIO_RATE;
	double diff = s->clock - at;
	if (fabs(diff) < 0.040)
		return;
	if (fabs(diff) > 2.0) {
		/* not drift: a discontinuity the seek path didn't catch; start over */
		write_log(_T("VIDEOGRAB: audio %+.1f s off the picture at %.2f, dropping the buffer\n"), -diff, s->clock);
		std::lock_guard<std::mutex> l(s->am);
		s->ring_r = s->ring_w = s->ring_fill = 0;
		s->ring_rt = -1;
		s->abuf_n = s->abuf_pos = 0;
		s->audio_hold = 0;
		return;
	}
	s->audio_resyncs++;
	if (s->audio_resyncs <= 20 || !(s->audio_resyncs % 100))
		write_log(_T("VIDEOGRAB: audio %+.1f ms off the picture at %.2f, resync %d (%d underruns)\n"),
			-diff * 1000, s->clock, s->audio_resyncs, s->audio_underruns);
	if (diff < 0) {
		/* early: play silence until the picture catches up */
		s->audio_hold += (int)(-diff * VG_AUDIO_RATE);
		return;
	}
	/* late: drop samples, first from the local buffer, then the ring */
	int drop = (int)(diff * VG_AUDIO_RATE);
	int h = s->audio_hold < drop ? s->audio_hold : drop;
	s->audio_hold -= h;
	drop -= h;
	int b = s->abuf_n - s->abuf_pos;
	if (b > drop)
		b = drop;
	s->abuf_pos += b;
	drop -= b;
	if (drop > 0) {
		std::lock_guard<std::mutex> l(s->am);
		size_t cap = s->ring.size() / 2;
		size_t d = (size_t)drop < s->ring_fill ? (size_t)drop : s->ring_fill;
		s->ring_r = (s->ring_r + d) % cap;
		s->ring_fill -= d;
		if (s->ring_rt >= 0)
			s->ring_rt += (double)d / VG_AUDIO_RATE;
	}
}

/* Media time per emulated field. A LaserDisc is genlocked: one disc frame
 * per two fields whatever the Amiga's exact field rate, which is how the
 * player models count (arcadia.cpp); a plain media file
 * plays at its own speed against the emulated field rate. */
static double vg_field_time(vg_state *s)
{
	if (s->ld)
		return 0.5 / s->fps;
	return 1.0 / (vblank_hz > 1 ? vblank_hz : 60.0f);
}

/* Called once per emulated field. */
void videograb_vsync(void)
{
	vg_state *s = vg;
	if (!s)
		return;
	uae_s64 now = (uae_s64)get_cycles();
	if (s->last_vsync_cycles >= 0 && now > s->last_vsync_cycles) {
		double d = (double)(now - s->last_vsync_cycles);
		/* interlace alternates long and short fields; smooth over a few */
		s->cycles_per_field = s->cycles_per_field > 0 ? s->cycles_per_field * 0.875 + d * 0.125 : d;
	}
	s->last_vsync_cycles = now;
	if (s->paused || s->live || s->still)
		return;
	s->clock += vg_field_time(s);
	if (s->duration_frames > 1) {
		double end = (double)s->duration_frames / s->fps;
		if (s->clock >= end) {
			if (currprefs.genlock_image < 6) {
				/* a plain media file loops, as the DirectShow one did */
				s->clock = 0;
				vg_request_seek(s, 0);
			} else {
				/* a disc stops at its end, on its last frame */
				s->clock = end;
			}
			return;
		}
	}
	/* fell far behind (warp, a stall): jump instead of decoding through */
	double newest;
	bool eof;
	{
		std::lock_guard<std::mutex> l(s->m);
		newest = s->newest_t;
		eof = s->eof;
	}
	if (!eof && newest >= 0 && s->clock - newest > 1.5 && s->clock - s->last_catchup > 1.0) {
		write_log(_T("VIDEOGRAB: %.2fs behind at %.2f, seeking ahead\n"), s->clock - newest, s->clock);
		s->last_catchup = s->clock;
		vg_request_seek(s, s->clock);
		return;
	}
	vg_audio_resync(s);
}

bool getvideograb(long **buffer, int *width, int *height)
{
	vg_state *s = vg;
	if (!s)
		return false;
	/* the newest frame at or before the play position */
	double eps = 0.25 / s->fps;
	bool took = false;
	{
		std::lock_guard<std::mutex> l(s->m);
		while (!s->queue.empty()) {
			vg_frame *f = s->queue.front();
			if (f->gen != s->gen) {
				s->queue.pop_front();
				vg_put_frame_locked(s, f);
				continue;
			}
			if (!s->live && !s->still && f->t > s->clock + eps)
				break;
			s->queue.pop_front();
			vg_put_frame_locked(s, s->cur);
			s->cur = f;
			took = true;
		}
	}
	if (took)
		s->cv.notify_all();
	if (!s->cur)
		return false;
	vg_frame *f = s->cur;
	*buffer = (long*)f->pix.data();
	*width = f->w;
	*height = f->h;
	return true;
}

bool getvideograbprescaled(void)
{
	vg_state *s = vg;
	if (!s)
		return false;
	const vg_frame *f = s->cur;
	if (!f || !f->prescaled)
		return false;
	/* scaled for an older area (a mode change): let do_genlock resample it */
	std::lock_guard<std::mutex> l(s->m);
	return f->w <= s->target_w + 8 && f->h <= s->target_h + 8 &&
		(abs(f->w - s->target_w) <= 8 || abs(f->h - s->target_h) <= 8);
}

void setvideograbtarget(int width, int height, bool keep_aspect)
{
	vg_state *s = vg;
	if (!s)
		return;
	int aspect = keep_aspect ? 1 : 0;
	std::lock_guard<std::mutex> l(s->m);
	if (s->target_w == width && s->target_h == height && s->target_aspect == aspect) {
		s->cand_count = 0;
		return;
	}
	/* An interlaced display alternates the area height by a line or two
	 * field to field (482/484), which rebuilt the scaler every frame. A
	 * small change has to hold for a second before it counts; a real mode
	 * change (or the first request) goes through at once. */
	bool minor_change = s->target_w && abs(width - s->target_w) <= 8 && abs(height - s->target_h) <= 8 && aspect == s->target_aspect;
	if (minor_change) {
		if (width != s->cand_w || height != s->cand_h || aspect != s->cand_aspect) {
			s->cand_w = width;
			s->cand_h = height;
			s->cand_aspect = aspect;
			s->cand_count = 0;
		}
		if (++s->cand_count < 60)
			return;
	}
	s->cand_count = 0;
	s->target_w = width;
	s->target_h = height;
	s->target_aspect = aspect;
	s->cfg_serial++;
}

void pausevideograb(int pause)
{
	vg_state *s = vg;
	if (!s)
		return;
	if (pause < 0)
		pause = s->paused ? 0 : 1;
	s->paused = pause > 0;
}

bool getpausevideograb(void)
{
	return vg && vg->paused;
}

uae_s64 getsetpositionvideograb(uae_s64 framepos)
{
	vg_state *s = vg;
	if (!s)
		return 0;
	if (framepos < 0)
		return (uae_s64)floor(s->clock * s->fps + 1e-6);
	if (s->live)
		return 0;
	double t = framepos / s->fps;
	s->clock = t;
	vg_request_seek(s, t);
	return framepos;
}

uae_s64 getdurationvideograb(void)
{
	return vg ? vg->duration_frames : 0;
}

void setvolumevideograb(int volume)
{
	if (!vg)
		return;
	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;
	vg->volume = volume;
}

void setmastervolumevideograb(int volume, bool mute)
{
	/* the mixer's own master volume already applies */
}

void setsoundoutputvideograb(bool enabled)
{
	if (vg)
		vg->sound_output = enabled;
}

void setchflagsvideograb(int chflags, bool mute)
{
	if (!vg)
		return;
	vg->chflags = chflags;
	vg->mute = mute;
}

void isvideograb_status(void)
{
	vg_state *s = vg;
	if (!s)
		return;
	if (currprefs.sound_volume_genlock != changed_prefs.sound_volume_genlock) {
		currprefs.sound_volume_genlock = changed_prefs.sound_volume_genlock;
		setvolumevideograb(100 - currprefs.sound_volume_genlock);
	}
	if (currprefs.genlock_video_scaler != changed_prefs.genlock_video_scaler ||
		currprefs.genlock_video_deinterlace != changed_prefs.genlock_video_deinterlace ||
		currprefs.genlock_video_fields != changed_prefs.genlock_video_fields ||
		_tcscmp(currprefs.genlock_video_filter, changed_prefs.genlock_video_filter)) {
		currprefs.genlock_video_scaler = changed_prefs.genlock_video_scaler;
		currprefs.genlock_video_deinterlace = changed_prefs.genlock_video_deinterlace;
		currprefs.genlock_video_fields = changed_prefs.genlock_video_fields;
		_tcscpy(currprefs.genlock_video_filter, changed_prefs.genlock_video_filter);
		std::lock_guard<std::mutex> l(s->m);
		vg_read_prefs(s, &currprefs);
		s->cfg_serial++;
	}
}

#endif /* VIDEOGRAB_LIBAV */
