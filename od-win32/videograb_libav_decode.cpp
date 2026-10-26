/*
 * FFmpeg genlock backend, decode half: demux, decode, filter, resample.
 * Compiled with the cdecl convention (see videograb_libav.h).
 */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef VIDEOGRAB_LIBAV

#include "videograb_libav.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}

#include <chrono>
#include <cmath>

/* indexes are the config/GUI values (cfgfile.cpp genlockscalers / genlockdeints) */
static const char *vg_scaler_flags[] = { "", "neighbor", "fast_bilinear", "bilinear", "bicubic", "lanczos", "spline", "area" };
static const char *vg_deint_filters[] = { "", "bwdif=mode=send_frame", "bwdif=mode=send_field", "yadif=mode=send_frame", "yadif=mode=send_field", "w3fdif", "estdif" };
#define VG_NUM_SCALERS ((int)(sizeof vg_scaler_flags / sizeof vg_scaler_flags[0]))
#define VG_NUM_DEINTS ((int)(sizeof vg_deint_filters / sizeof vg_deint_filters[0]))

static bool vg_open_streams(vg_state *s);

static void vg_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
	if (level > AV_LOG_WARNING)
		return;
	char line[1024];
	int prefix = 1;
	av_log_format_line(avcl, level, fmt, vl, line, sizeof line, &prefix);
	size_t l = strlen(line);
	while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
		line[--l] = 0;
	if (l)
		write_log(_T("libav: %hs\n"), line);
}

static const char *vg_err(int err, char *buf, size_t size)
{
	av_strerror(err, buf, size);
	return buf;
}

static int vg_interrupt_cb(void *opaque)
{
	vg_state *s = (vg_state*)opaque;
	return s->quit ? 1 : 0;
}

/* call with m held, or before the thread exists */
/* ------------------------------------------------------------------ */
/* audio ring, producer side */

static void vg_ring_flush(vg_state *s)
{
	std::lock_guard<std::mutex> l(s->am);
	s->ring_r = s->ring_w = s->ring_fill = 0;
	s->ring_rt = -1;
}

static size_t vg_ring_write(vg_state *s, const uae_s16 *src, size_t frames, double t, int gen)
{
	std::lock_guard<std::mutex> l(s->am);
	if (gen != s->ring_gen)
		return frames;	/* decoded before a seek the emulator has already asked for */
	size_t cap = s->ring.size() / 2;
	if (!s->ring_fill || s->ring_rt < 0)
		s->ring_rt = t;
	if (s->live && s->ring_fill + frames > VG_LIVE_LATENCY) {
		/* live: never let the delay build up, drop the oldest */
		size_t drop = s->ring_fill + frames - VG_LIVE_LATENCY;
		if (drop > s->ring_fill)
			drop = s->ring_fill;
		s->ring_r = (s->ring_r + drop) % cap;
		s->ring_fill -= drop;
		if (s->ring_rt >= 0)
			s->ring_rt += (double)drop / VG_AUDIO_RATE;
	}
	size_t n = cap - s->ring_fill;
	if (n > frames)
		n = frames;
	for (size_t i = 0; i < n; i++) {
		s->ring[s->ring_w * 2 + 0] = src[i * 2 + 0];
		s->ring[s->ring_w * 2 + 1] = src[i * 2 + 1];
		s->ring_w = (s->ring_w + 1) % cap;
	}
	s->ring_fill += n;
	return n;
}

static size_t vg_ring_fill(vg_state *s)
{
	std::lock_guard<std::mutex> l(s->am);
	return s->ring_fill;
}

/* Mixer callback, one stereo sample per call, emulated-time paced. */
/* ------------------------------------------------------------------ */
/* filter graph */

static void vg_free_graph(vg_state *s)
{
	avfilter_graph_free(&s->graph);
	s->bsrc = s->bsink = nullptr;
	s->graph_fmt = -1;
}

static bool vg_build_graph_str(vg_state *s, const AVFrame *f, const std::string &chain, int *ow, int *oh, AVRational *osar)
{
	char args[512];
	char err[128];
	AVStream *st = s->fmt->streams[s->vsi];
	AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
	if (!sar.num)
		sar = av_make_q(1, 1);

	vg_free_graph(s);
	s->graph = avfilter_graph_alloc();
	if (!s->graph)
		return false;
	/* the frame's colourspace and range, so the scaler converts with the right
	 * matrix (BT.601 SD vs BT.709 HD) and libavfilter doesn't complain */
	snprintf(args, sizeof args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d",
		f->width, f->height, f->format, st->time_base.num, st->time_base.den, sar.num, sar.den,
		(int)f->colorspace, (int)f->color_range);
	AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
	if (fr.num > 0 && fr.den > 0) {
		char frs[64];
		snprintf(frs, sizeof frs, ":frame_rate=%d/%d", fr.num, fr.den);
		strncat(args, frs, sizeof args - strlen(args) - 1);
	}
	int r = AVERROR(ENOMEM);
	s->bsrc = avfilter_graph_alloc_filter(s->graph, avfilter_get_by_name("buffer"), "in");
	if (s->bsrc) {
		/* chroma siting has no option string; without it the source warns on every frame */
		AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
		if (par) {
			par->chroma_location = f->chroma_location;
			av_buffersrc_parameters_set(s->bsrc, par);
			av_free(par);
		}
		r = avfilter_init_str(s->bsrc, args);
	}
	if (r < 0) {
		write_log(_T("VIDEOGRAB: buffer source '%hs': %hs\n"), args, vg_err(r, err, sizeof err));
		vg_free_graph(s);
		return false;
	}
	s->bsink = avfilter_graph_alloc_filter(s->graph, avfilter_get_by_name("buffersink"), "out");
	if (!s->bsink) {
		vg_free_graph(s);
		return false;
	}
	enum AVPixelFormat pf = AV_PIX_FMT_BGR24;
	av_opt_set_array(s->bsink, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_PIXEL_FMT, &pf);
	r = avfilter_init_dict(s->bsink, nullptr);
	if (r < 0) {
		write_log(_T("VIDEOGRAB: buffersink: %hs\n"), vg_err(r, err, sizeof err));
		vg_free_graph(s);
		return false;
	}

	std::string desc = chain.empty() ? std::string("null") : chain;
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = s->bsrc;
	outputs->pad_idx = 0;
	outputs->next = nullptr;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = s->bsink;
	inputs->pad_idx = 0;
	inputs->next = nullptr;
	r = avfilter_graph_parse_ptr(s->graph, desc.c_str(), &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (r < 0) {
		write_log(_T("VIDEOGRAB: filter graph '%hs': %hs\n"), desc.c_str(), vg_err(r, err, sizeof err));
		vg_free_graph(s);
		return false;
	}
	r = avfilter_graph_config(s->graph, nullptr);
	if (r < 0) {
		write_log(_T("VIDEOGRAB: filter graph config '%hs': %hs\n"), desc.c_str(), vg_err(r, err, sizeof err));
		vg_free_graph(s);
		return false;
	}
	if (ow)
		*ow = av_buffersink_get_w(s->bsink);
	if (oh)
		*oh = av_buffersink_get_h(s->bsink);
	if (osar) {
		*osar = av_buffersink_get_sample_aspect_ratio(s->bsink);
		if (!osar->num)
			*osar = av_make_q(1, 1);
	}
	s->graph_w = f->width;
	s->graph_h = f->height;
	s->graph_fmt = f->format;
	s->graph_csp = (int)f->colorspace;
	s->graph_range = (int)f->color_range;
	return true;
}

/* [deinterlace], [user chain], scale to the target, bgr24 */
static bool vg_build_graph(vg_state *s, const AVFrame *f)
{
	int scaler, deint, fields, tw, th, aspect, serial;
	std::string user;
	{
		std::lock_guard<std::mutex> l(s->m);
		scaler = s->opt_scaler;
		deint = s->opt_deint;
		fields = s->opt_fields;
		user = s->opt_filter;
		tw = s->target_w;
		th = s->target_h;
		aspect = s->target_aspect;
		serial = s->cfg_serial;
	}
	std::string pre;
	if (deint > 0 && deint < VG_NUM_DEINTS)
		pre = vg_deint_filters[deint];
	if (!user.empty())
		pre = pre.empty() ? user : pre + "," + user;

	bool ok = false;
	s->graph_prescaled = false;
	if (scaler > 0 && tw >= 16 && th >= 16) {
		/* build without the scale first to learn what the user's chain
		 * produces (a crop changes it), then fit that to the target */
		int pw = 0, ph = 0;
		AVRational psar = { 1, 1 };
		if (vg_build_graph_str(s, f, pre, &pw, &ph, &psar) && pw > 0 && ph > 0) {
			int w = tw, h = th;
			if (aspect) {
				/* the area's pixels are taken as square, as do_genlock does */
				double dar = (double)pw * psar.num / psar.den / ph;
				if ((double)tw / th > dar)
					w = (int)(th * dar + 0.5);
				else
					h = (int)(tw / dar + 0.5);
				if (w < 2)
					w = 2;
				if (h < 2)
					h = 2;
			}
			/* interl: -1 from the frame flags, 1 always field-wise, 0 never.
			 * After a deinterlacer the picture is progressive. */
			int interl = deint > 0 ? 0 : (fields == 1 ? 1 : (fields == 2 ? 0 : -1));
			char sc[256];
			snprintf(sc, sizeof sc, "scale=w=%d:h=%d:flags=%s+accurate_rnd+full_chroma_int:interl=%d,setsar=1",
				w, h, vg_scaler_flags[scaler], interl);
			std::string chain = pre.empty() ? std::string(sc) : pre + "," + sc;
			ok = vg_build_graph_str(s, f, chain, nullptr, nullptr, nullptr);
			if (ok) {
				s->graph_prescaled = true;
				write_log(_T("VIDEOGRAB: %dx%d -> %dx%d, target %dx%d%hs: %hs\n"),
					f->width, f->height, w, h, tw, th, aspect ? " keep aspect" : "", chain.c_str());
			}
		}
	}
	if (!ok) {
		ok = vg_build_graph_str(s, f, pre, nullptr, nullptr, nullptr);
		if (ok)
			write_log(_T("VIDEOGRAB: %dx%d unscaled: %hs\n"), f->width, f->height, pre.empty() ? "no filters" : pre.c_str());
	}
	if (!ok && !pre.empty()) {
		write_log(_T("VIDEOGRAB: filters failed, falling back to none\n"));
		ok = vg_build_graph_str(s, f, "", nullptr, nullptr, nullptr);
	}
	s->graph_cfg = serial;
	return ok;
}

/* ------------------------------------------------------------------ */
/* decode thread */

static vg_frame *vg_get_frame(vg_state *s)
{
	std::lock_guard<std::mutex> l(s->m);
	if (!s->pool.empty()) {
		vg_frame *f = s->pool.back();
		s->pool.pop_back();
		return f;
	}
	return new vg_frame;
}

static double vg_ts_sec(vg_state *s, int64_t ts, AVRational tb)
{
	if (ts == AV_NOPTS_VALUE)
		return -1;
	return ts * av_q2d(tb) - s->start_sec;
}

static bool vg_stale_locked(vg_state *s, int gen)
{
	return s->quit || s->seek_req >= 0 || s->gen != gen;
}

/* Wait for room in the frame queue. False: drop the frame (seek or quit). */
static bool vg_wait_queue(vg_state *s, int gen)
{
	std::unique_lock<std::mutex> l(s->m);
	while (!vg_stale_locked(s, gen) && !s->live && (int)s->queue.size() >= VG_VIDEO_QUEUE)
		s->cv.wait_for(l, std::chrono::milliseconds(5));
	return !vg_stale_locked(s, gen);
}

static void vg_emit_filtered(vg_state *s, int gen)
{
	AVRational tb = av_buffersink_get_time_base(s->bsink);
	for (;;) {
		if (av_buffersink_get_frame(s->bsink, s->ffrm) < 0)
			break;
		double t = vg_ts_sec(s, s->ffrm->pts, tb);
		if (t < 0)
			t = s->last_t >= 0 ? s->last_t + 1.0 / s->fps : 0;
		s->last_t = t;
		if (s->discard_before >= 0 && t < s->discard_before - 0.25 / s->fps) {
			av_frame_unref(s->ffrm);
			continue;
		}
		if (!vg_wait_queue(s, gen)) {
			av_frame_unref(s->ffrm);
			continue;
		}
		vg_frame *vf = vg_get_frame(s);
		int w = s->ffrm->width, h = s->ffrm->height;
		vf->t = t;
		vf->gen = gen;
		vf->w = w;
		vf->h = h;
		vf->prescaled = s->graph_prescaled;
		vf->pix.resize((size_t)w * h * 3);
		/* do_genlock reads bottom-up BGR24, rows of exactly w * 3 */
		for (int y = 0; y < h; y++)
			memcpy(&vf->pix[(size_t)(h - 1 - y) * w * 3], s->ffrm->data[0] + (size_t)y * s->ffrm->linesize[0], (size_t)w * 3);
		av_frame_unref(s->ffrm);
		s->discard_before = -1;
		std::lock_guard<std::mutex> l(s->m);
		if (vg_stale_locked(s, gen)) {
			vg_put_frame_locked(s, vf);
			continue;
		}
		if (s->live) {
			/* newest frame wins */
			while (s->queue.size() >= 2) {
				vg_put_frame_locked(s, s->queue.front());
				s->queue.pop_front();
			}
		}
		s->queue.push_back(vf);
		s->newest_t = t;
	}
}

static void vg_decode_video(vg_state *s, AVPacket *p, int gen)
{
	int r = avcodec_send_packet(s->vdec, p);
	if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
		return;
	AVStream *st = s->fmt->streams[s->vsi];
	for (;;) {
		r = avcodec_receive_frame(s->vdec, s->frame);
		if (r == AVERROR_EOF) {
			if (s->bsrc) {
				if (av_buffersrc_add_frame_flags(s->bsrc, nullptr, 0) >= 0)
					vg_emit_filtered(s, gen);
			}
			s->drained = true;
			return;
		}
		if (r < 0)
			return;
		s->frame->pts = s->frame->best_effort_timestamp;
		double t = vg_ts_sec(s, s->frame->pts, st->time_base);
		/* a seek lands on the keyframe before the target: decode, but only
		 * filter the last few frames before it (deinterlacer history) */
		if (s->discard_before >= 0 && t >= 0 && t < s->discard_before - 3.0 / s->fps) {
			av_frame_unref(s->frame);
			continue;
		}
		int serial;
		{
			std::lock_guard<std::mutex> l(s->m);
			serial = s->cfg_serial;
		}
		if (!s->graph || s->frame->width != s->graph_w || s->frame->height != s->graph_h ||
			s->frame->format != s->graph_fmt || (int)s->frame->colorspace != s->graph_csp ||
			(int)s->frame->color_range != s->graph_range || serial != s->graph_cfg) {
			if (!vg_build_graph(s, s->frame)) {
				av_frame_unref(s->frame);
				continue;
			}
		}
		r = av_buffersrc_add_frame_flags(s->bsrc, s->frame, 0);
		av_frame_unref(s->frame);
		if (r < 0)
			continue;
		vg_emit_filtered(s, gen);
	}
}

static void vg_queue_audio(vg_state *s, const uae_s16 *src, int n, double t, int gen)
{
	if (n <= 0)
		return;
	if (s->audio_discard_before >= 0 && t >= 0) {
		double end = t + (double)n / VG_AUDIO_RATE;
		if (end <= s->audio_discard_before)
			return;
		if (t < s->audio_discard_before) {
			int skip = (int)((s->audio_discard_before - t) * VG_AUDIO_RATE);
			src += skip * 2;
			n -= skip;
			t += (double)skip / VG_AUDIO_RATE;
		}
		s->audio_discard_before = -1;
	}
	size_t left = n;
	while (left > 0) {
		size_t w = vg_ring_write(s, src, left, t, gen);
		left -= w;
		src += w * 2;
		if (t >= 0)
			t += (double)w / VG_AUDIO_RATE;
		if (!left)
			break;
		/* ring full: the emulator is paused or far behind */
		std::unique_lock<std::mutex> l(s->m);
		if (s->quit || s->seek_req >= 0)
			return;
		s->cv.wait_for(l, std::chrono::milliseconds(10));
	}
}

static void vg_decode_audio(vg_state *s, AVPacket *p, int gen)
{
	if (avcodec_send_packet(s->adec, p) < 0)
		return;
	static std::vector<uae_s16> out;
	AVStream *st = s->fmt->streams[s->asi];
	while (avcodec_receive_frame(s->adec, s->frame) >= 0) {
		if (s->swr && (s->frame->sample_rate != s->swr_in_rate || s->frame->format != s->swr_in_fmt ||
			s->frame->ch_layout.nb_channels != s->swr_in_ch)) {
			write_log(_T("VIDEOGRAB: audio input changed to %d Hz, %d channels\n"), s->frame->sample_rate, s->frame->ch_layout.nb_channels);
			swr_free(&s->swr);
		}
		if (!s->swr) {
			s->swr_in_rate = s->frame->sample_rate;
			s->swr_in_fmt = s->frame->format;
			s->swr_in_ch = s->frame->ch_layout.nb_channels;
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			if (swr_alloc_set_opts2(&s->swr, &stereo, AV_SAMPLE_FMT_S16, VG_AUDIO_RATE,
				&s->frame->ch_layout, (enum AVSampleFormat)s->frame->format, s->frame->sample_rate, 0, nullptr) < 0 ||
				swr_init(s->swr) < 0) {
				write_log(_T("VIDEOGRAB: audio resampler init failed\n"));
				swr_free(&s->swr);
				av_frame_unref(s->frame);
				continue;
			}
		}
		double t = vg_ts_sec(s, s->frame->best_effort_timestamp, st->time_base);
		int cap = swr_get_out_samples(s->swr, s->frame->nb_samples);
		if (cap <= 0) {
			av_frame_unref(s->frame);
			continue;
		}
		if (out.size() < (size_t)cap * 2)
			out.resize((size_t)cap * 2);
		uae_u8 *o = (uae_u8*)out.data();
		int n = swr_convert(s->swr, &o, cap, (const uae_u8**)s->frame->extended_data, s->frame->nb_samples);
		av_frame_unref(s->frame);
		vg_queue_audio(s, out.data(), n, t, gen);
	}
}

static void vg_do_seek(vg_state *s, double t)
{
	char err[128];
	for (AVPacket *p : s->vpackets)
		av_packet_free(&p);
	s->vpackets.clear();
	vg_ring_flush(s);
	if (s->vdec)
		avcodec_flush_buffers(s->vdec);
	if (s->adec)
		avcodec_flush_buffers(s->adec);
	swr_free(&s->swr);
	vg_free_graph(s);	/* deinterlacer history belongs to the old position */
	s->eof = s->drained = false;
	s->last_t = -1;
	if (s->still)
		t = 0;		/* a still is decoded again and held */
	AVStream *st = s->fmt->streams[s->vsi];
	int64_t ts = (int64_t)floor((t + s->start_sec) / av_q2d(st->time_base) + 0.5);
	int r = avformat_seek_file(s->fmt, s->vsi, INT64_MIN, ts, ts, 0);
	if (r < 0)
		r = av_seek_frame(s->fmt, s->vsi, ts, AVSEEK_FLAG_BACKWARD);
	if (r < 0)
		write_log(_T("VIDEOGRAB: seek to %.3f failed: %hs\n"), t, vg_err(r, err, sizeof err));
	s->discard_before = t;
	s->audio_discard_before = t;
}

static void vg_close_input(vg_state *s)
{
	for (AVPacket *p : s->vpackets)
		av_packet_free(&p);
	s->vpackets.clear();
	vg_free_graph(s);
	swr_free(&s->swr);
	avcodec_free_context(&s->vdec);
	avcodec_free_context(&s->adec);
	avformat_close_input(&s->fmt);
	s->vsi = s->asi = -1;
	s->eof = s->drained = false;
}

static bool vg_open_input(vg_state *s)
{
	char err[128];
	AVDictionary *o = nullptr;
	if (s->live) {
		av_dict_set(&o, "rw_timeout", "10000000", 0);
		av_dict_set(&o, "reconnect", "1", 0);
		av_dict_set(&o, "reconnect_streamed", "1", 0);
	}
	s->fmt = avformat_alloc_context();
	if (!s->fmt)
		return false;
	s->fmt->interrupt_callback.callback = vg_interrupt_cb;
	s->fmt->interrupt_callback.opaque = s;
	int r = avformat_open_input(&s->fmt, s->url.c_str(), nullptr, &o);
	av_dict_free(&o);
	if (r < 0) {
		write_log(_T("VIDEOGRAB: cannot open '%hs': %hs\n"), s->url.c_str(), vg_err(r, err, sizeof err));
		return false;
	}
	if (!vg_open_streams(s)) {
		vg_close_input(s);
		return false;
	}
	return true;
}

/* sleep that a quit cuts short */
static bool vg_sleep(vg_state *s, int ms)
{
	std::unique_lock<std::mutex> l(s->m);
	s->cv.wait_for(l, std::chrono::milliseconds(ms), [s] { return s->quit.load(); });
	return !s->quit;
}

static void vg_thread(vg_state *s)
{
	char err[128];
	int gen = 0;
	for (;;) {
		if (!s->fmt) {
			/* live sources (re)open here, so a slow server never stalls emulation */
			if (!vg_open_input(s)) {
				if (!vg_sleep(s, 3000))
					break;
				continue;
			}
			std::lock_guard<std::mutex> l(s->m);
			s->opened = true;
		}

		double seek = -1;
		{
			std::lock_guard<std::mutex> l(s->m);
			if (s->quit)
				break;
			if (s->seek_req >= 0) {
				seek = s->seek_req;
				s->seek_req = -1;
			}
			gen = s->gen;
		}
		if (seek >= 0) {
			vg_do_seek(s, seek);
			continue;
		}

		/* decode queued video packets while there is room */
		while (!s->vpackets.empty()) {
			{
				std::lock_guard<std::mutex> l(s->m);
				if (vg_stale_locked(s, gen) || (!s->live && (int)s->queue.size() >= VG_VIDEO_QUEUE))
					break;
			}
			AVPacket *p = s->vpackets.front();
			s->vpackets.pop_front();
			vg_decode_video(s, p, gen);
			av_packet_free(&p);
		}

		bool want_read;
		{
			std::lock_guard<std::mutex> l(s->m);
			bool video_full = (int)s->queue.size() >= VG_VIDEO_QUEUE || s->vpackets.size() >= VG_MAX_PACKETS;
			bool audio_low = s->has_audio && vg_ring_fill(s) < VG_AUDIO_LOW;
			want_read = !s->eof && (s->live || !video_full || (audio_low && s->vpackets.size() < VG_MAX_PACKETS));
		}
		if (!want_read) {
			if (s->eof && s->vpackets.empty() && !s->drained) {
				vg_decode_video(s, nullptr, gen);	/* drain the decoder at end of file */
				continue;
			}
			std::unique_lock<std::mutex> l(s->m);
			if (s->quit)
				break;
			if (s->seek_req < 0)
				s->cv.wait_for(l, std::chrono::milliseconds(s->eof ? 50 : 5));
			continue;
		}

		int r = av_read_frame(s->fmt, s->pkt);
		if (r < 0) {
			if (s->quit)
				break;
			if (s->live) {
				/* a live source that ends or errors out gets reopened */
				write_log(_T("VIDEOGRAB: stream '%hs' lost: %hs, reconnecting\n"), s->url.c_str(), vg_err(r, err, sizeof err));
				vg_close_input(s);
				if (!vg_sleep(s, 1000))
					break;
				continue;
			}
			if (r == AVERROR(EAGAIN)) {
				vg_sleep(s, 5);
				continue;
			}
			if (!s->eof)
				write_log(_T("VIDEOGRAB: end of '%hs' (%hs)\n"), s->url.c_str(), vg_err(r, err, sizeof err));
			s->eof = true;
			continue;
		}
		if (s->pkt->stream_index == s->vsi) {
			AVPacket *c = av_packet_clone(s->pkt);
			if (c)
				s->vpackets.push_back(c);
		} else if (s->pkt->stream_index == s->asi && s->adec) {
			vg_decode_audio(s, s->pkt, gen);
		}
		av_packet_unref(s->pkt);
	}
}

static AVCodecContext *vg_open_decoder(vg_state *s, int si)
{
	char err[128];
	AVStream *st = s->fmt->streams[si];
	const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
	/* the native AV1 decoder only drives hardware; dav1d is the software one */
	if (st->codecpar->codec_id == AV_CODEC_ID_AV1) {
		const AVCodec *d = avcodec_find_decoder_by_name("libdav1d");
		if (d)
			c = d;
	}
	if (!c) {
		write_log(_T("VIDEOGRAB: no decoder for stream %d (%hs)\n"), si, avcodec_get_name(st->codecpar->codec_id));
		return nullptr;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 0;
	int r = avcodec_open2(ctx, c, nullptr);
	if (r < 0) {
		write_log(_T("VIDEOGRAB: cannot open decoder %hs: %hs\n"), c->name, vg_err(r, err, sizeof err));
		avcodec_free_context(&ctx);
		return nullptr;
	}
	write_log(_T("VIDEOGRAB: stream %d %hs decoder '%hs'\n"), si,
		st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", c->name);
	return ctx;
}

static bool vg_open_streams(vg_state *s)
{
	char err[128];
	int r = avformat_find_stream_info(s->fmt, nullptr);
	if (r < 0)
		write_log(_T("VIDEOGRAB: stream info: %hs\n"), vg_err(r, err, sizeof err));
	s->vsi = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (s->vsi < 0) {
		write_log(_T("VIDEOGRAB: '%hs' has no video\n"), s->url.c_str());
		return false;
	}
	s->vdec = vg_open_decoder(s, s->vsi);
	if (!s->vdec)
		return false;
	s->asi = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, s->vsi, nullptr, 0);
	if (s->asi >= 0)
		s->adec = vg_open_decoder(s, s->asi);
	s->has_audio = s->adec != nullptr;

	AVStream *st = s->fmt->streams[s->vsi];
	AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
	if (fr.num > 0 && fr.den > 0)
		s->fps = av_q2d(fr);
	if (s->fps < 1 || s->fps > 1000)
		s->fps = 30000.0 / 1001.0;
	s->start_sec = st->start_time != AV_NOPTS_VALUE ? st->start_time * av_q2d(st->time_base) :
		(s->fmt->start_time != AV_NOPTS_VALUE ? (double)s->fmt->start_time / AV_TIME_BASE : 0);
	double dur = -1;
	if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
		dur = st->duration * av_q2d(st->time_base);
	else if (s->fmt->duration != AV_NOPTS_VALUE && s->fmt->duration > 0)
		dur = (double)s->fmt->duration / AV_TIME_BASE;
	s->duration_frames = dur > 0 ? (uae_s64)(dur * s->fps + 0.5) : (st->nb_frames > 0 ? st->nb_frames : 0);
	/* a single picture (png, jpeg, jxl...) is decoded once and held */
	const char *dn = s->fmt->iformat->name;
	bool image_demuxer = !strcmp(dn, "image2") || strstr(dn, "_pipe") != nullptr;
	s->still = !s->live && (st->nb_frames == 1 || (image_demuxer && s->duration_frames <= 1));
	AVRational sar = st->codecpar->sample_aspect_ratio.num ? st->codecpar->sample_aspect_ratio : av_make_q(1, 1);
	write_log(_T("VIDEOGRAB: '%hs' %hs, %dx%d sar %d/%d, %.3f fps, %lld frames%hs%hs%hs\n"),
		s->url.c_str(), dn, st->codecpar->width, st->codecpar->height, sar.num, sar.den,
		s->fps, s->duration_frames, s->has_audio ? ", audio" : "", s->still ? ", still" : "", s->live ? ", live" : "");
	return true;
}

/* ------------------------------------------------------------------ */
/* entry points */

int VGL_CALL vgl_num_scalers(void)
{
	return VG_NUM_SCALERS;
}

int VGL_CALL vgl_num_deints(void)
{
	return VG_NUM_DEINTS;
}

void VGL_CALL vgl_global_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	av_log_set_callback(vg_log_cb);
	avformat_network_init();
	write_log(_T("VIDEOGRAB: libavformat %hs, libavcodec %hs, libavfilter %hs\n"),
		AV_STRINGIFY(LIBAVFORMAT_VERSION), AV_STRINGIFY(LIBAVCODEC_VERSION), AV_STRINGIFY(LIBAVFILTER_VERSION));
}

bool VGL_CALL vgl_alloc(vg_state *s)
{
	s->frame = av_frame_alloc();
	s->ffrm = av_frame_alloc();
	s->pkt = av_packet_alloc();
	return s->frame && s->ffrm && s->pkt;
}

bool VGL_CALL vgl_open_input(vg_state *s)
{
	return vg_open_input(s);
}

void VGL_CALL vgl_start(vg_state *s)
{
	s->thr = std::thread(vg_thread, s);
}

void VGL_CALL vgl_stop(vg_state *s)
{
	{
		std::lock_guard<std::mutex> l(s->m);
		s->quit = true;
	}
	s->cv.notify_all();
	if (s->thr.joinable())
		s->thr.join();
	vg_close_input(s);
	av_frame_free(&s->frame);
	av_frame_free(&s->ffrm);
	av_packet_free(&s->pkt);
}

#endif /* VIDEOGRAB_LIBAV */
