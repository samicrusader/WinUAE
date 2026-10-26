/*
 * Private interface between the two halves of the FFmpeg genlock backend.
 *
 * Win32 WinUAE is built with __fastcall as the default convention (/Gr), and
 * FFmpeg's headers don't name one, so nothing that includes them may be
 * compiled that way. videograb_libav_decode.cpp holds everything that talks
 * to FFmpeg and is compiled cdecl; videograb_libav.cpp holds the emulator
 * side (videograb.h, the clock, the mixer callback) at the default. The
 * entry points between them say __cdecl explicitly so both agree.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_M_IX86)
#define VGL_CALL __cdecl
#else
#define VGL_CALL
#endif

struct AVFormatContext;
struct AVCodecContext;
struct AVFilterGraph;
struct AVFilterContext;
struct SwrContext;
struct AVFrame;
struct AVPacket;

#define VG_AUDIO_RATE 48000
#define VG_VIDEO_QUEUE 6			/* decoded frames waiting ahead of the clock */
#define VG_AUDIO_RING (VG_AUDIO_RATE * 4)	/* stereo frames */
#define VG_AUDIO_LOW (VG_AUDIO_RATE / 2)	/* keep reading packets until this much is buffered */
#define VG_LIVE_LATENCY (VG_AUDIO_RATE / 2)	/* live: drop audio beyond this */
#define VG_MAX_PACKETS 2048			/* queued compressed video packets */

struct vg_frame
{
	double t = 0;		/* seconds from the start of the source */
	int gen = 0;		/* seek generation it was decoded in */
	int w = 0, h = 0;
	bool prescaled = false;	/* scaled to the target the emulator asked for */
	std::vector<uae_u8> pix;	/* BGR24, bottom-up, rows of w * 3 bytes */
};

struct vg_state
{
	/* --- decode thread (and init, before the thread starts) --- */
	struct AVFormatContext *fmt = nullptr;
	struct AVCodecContext *vdec = nullptr, *adec = nullptr;
	int vsi = -1, asi = -1;
	struct AVFilterGraph *graph = nullptr;
	struct AVFilterContext *bsrc = nullptr, *bsink = nullptr;
	struct SwrContext *swr = nullptr;
	struct AVFrame *frame = nullptr, *ffrm = nullptr;
	struct AVPacket *pkt = nullptr;
	std::deque<struct AVPacket*> vpackets;
	int graph_w = 0, graph_h = 0, graph_fmt = -1;	/* input the graph was built for */
	int graph_csp = -1, graph_range = -1;
	int graph_cfg = -1;				/* cfg_serial it was built with */
	bool graph_prescaled = false;
	double start_sec = 0;
	double discard_before = -1;			/* drop decoded frames before this time (seek) */
	double audio_discard_before = -1;
	std::atomic<bool> eof{false};
	bool drained = false;
	int read_errors = 0;
	double last_t = -1;

	/* --- fixed after open --- */
	std::string url;
	bool live = false, still = false;
	bool ld = false;				/* a LaserDisc mode: one disc frame per two fields */
	double fps = 30000.0 / 1001.0;
	uae_s64 duration_frames = 0;
	bool has_audio = false;

	/* --- shared, under m --- */
	std::mutex m;
	std::condition_variable cv;
	std::thread thr;
	std::atomic<bool> quit{false};
	bool opened = false;
	int gen = 0;					/* bumped by every seek request */
	double seek_req = -1;				/* seconds, -1 none */
	int target_w = 0, target_h = 0, target_aspect = 0;
	int cfg_serial = 0;				/* bumped when target or filter options change */
	int opt_scaler = 0, opt_deint = 0, opt_fields = 0;
	std::string opt_filter;
	std::deque<vg_frame*> queue;
	std::vector<vg_frame*> pool;
	double newest_t = -1;				/* newest frame queued in the current gen */

	/* --- audio ring, under am (lock order: m before am) --- */
	std::mutex am;
	std::vector<uae_s16> ring;
	size_t ring_r = 0, ring_w = 0, ring_fill = 0;
	double ring_rt = -1;				/* media time of the sample at ring_r */
	int ring_gen = 0;				/* seek generation the ring holds; stale writes are dropped */
	int swr_in_rate = 0, swr_in_fmt = -1, swr_in_ch = 0;	/* what the resampler was built for */

	/* --- emulator thread only --- */
	double clock = 0;				/* play position, seconds */
	bool paused = false;
	vg_frame *cur = nullptr;			/* frame on screen */
	int chflags = 3;
	bool mute = false, sound_output = true;
	int volume = 100;				/* 0..100 */
	int streamid = 0;
	uae_s16 abuf[512];
	int abuf_n = 0, abuf_pos = 0;
	double abuf_t = -1;				/* media time of abuf[0] */
	int audio_hold = 0;				/* samples of silence to play before consuming */
	int audio_resyncs = 0, audio_underruns = 0;
	uae_s64 last_vsync_cycles = -1;
	double cycles_per_field = 0;			/* emulated cycles (CYCLE_UNIT scaled) per vsync, smoothed */
	double last_catchup = -100;
	int cand_w = 0, cand_h = 0, cand_aspect = 0, cand_count = 0;	/* target change waiting to settle */
};

/* decode side (videograb_libav_decode.cpp) */
void VGL_CALL vgl_global_init(void);
bool VGL_CALL vgl_alloc(vg_state *s);
bool VGL_CALL vgl_open_input(vg_state *s);	/* files: called before the thread starts */
void VGL_CALL vgl_start(vg_state *s);
void VGL_CALL vgl_stop(vg_state *s);		/* quit, join, free everything FFmpeg */
int VGL_CALL vgl_num_scalers(void);
int VGL_CALL vgl_num_deints(void);

/* shared helpers; m held */
static inline void vg_put_frame_locked(vg_state *s, vg_frame *f)
{
	if (f)
		s->pool.push_back(f);
}

static inline void vg_clear_queue_locked(vg_state *s)
{
	for (vg_frame *f : s->queue)
		vg_put_frame_locked(s, f);
	s->queue.clear();
	s->newest_t = -1;
}

