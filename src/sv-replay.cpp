/*
Simply Vertical for OBS
Copyright (C) 2026 SimplyNickish

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/
#include "sv-replay.h"

#include "sv-brand.h"
#include "sv-canvas.h"
#include "sv-ease.h"
#include "sv-layout.h"
#include "sv-link.h"
#include "sv-output.h"
#include "sv-sound.h"
#include "sv-source.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <graphics/matrix4.h>

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

/* How a replay looks on the phone:
   - the live layout stays underneath the whole time, so the phone never goes black
     while the clip opens, and it is what shows again when the clip fades away,
   - the clip jumps to its last few seconds (the moment, not the whole Backtrack),
   - slanted bands in the layout's colour sweep across with a big REPLAY and uncover it
     (or it slides in, or fades in), it plays the run up at normal speed, then slows down
     on the moment Save clip was pressed, pushing in slowly on the game, then the bands
     sweep back the other way to the live layout,
   - a lit frame and a REPLAY tag in the layout's colour say it is not live, with a lit
     line under the tag that fills as it plays.

   How the slow motion works: OBS's media player cannot change speed while it plays, so
   there are two players of the same clip. The run up player plays at normal speed. The
   slow player waits underneath it, paused on the full picture where the slow part starts.
   A small filter on the run up player watches each picture go by and stops on exactly that
   picture, then the slow player takes over from the same picture, so nothing jumps. */
constexpr int CANVAS_W = 1080;
constexpr int CANVAS_H = 1920;
constexpr int IN_MS = 380;     /* the slide in */
constexpr int COVER_MS = 400;  /* the bands sweeping on until they cover the phone */
constexpr int REVEAL_MS = 460; /* and on off the other side, uncovering what is under them */
constexpr float BAND_DEG = 14.0f;  /* the bands' slant */
constexpr float BAND_H = 3000.0f;  /* long enough to cover the phone at that slant */
constexpr int OUT_MS = 320;    /* the fade back to live */
constexpr int HOLD_MS = 250;   /* the last picture stays this long before the fade */
constexpr int SETTLE_MS = 180; /* after the jump, for the clip's picture to catch up */
constexpr int READY_MS = 3000; /* show the clip anyway if it takes longer than this to open */
constexpr int TICK_MS = 16;
constexpr int VOICE_FADE_MS = 400; /* the run up's sound fades out this long before the slow part */
constexpr int64_t MIN_RUN_US = 400000; /* a run up shorter than this is not worth a second player */
constexpr const char *GATE_ID = "simply_vertical_replay_gate";
constexpr const char *WINDOW_ID = "simply_vertical_replay_window"; /* the member's Replay Window */
constexpr const char *LENS_ID = "simply_vertical_replay_lens";     /* the zoom on the game alone */
constexpr const char *LAYOUT_ID = "simply_vertical_source";
constexpr const char *PIECE_ID = "simply_vertical_piece";
constexpr uint32_t WINDOW_W = 1080, WINDOW_H = 1080; /* a new Replay Window's size */

enum class Phase { Off, Opening, In, Playing, Out };

/* ---------- the clip's full pictures, read from the MP4 itself ---------- */

/* When each full picture (keyframe) shows, in microseconds from the start of the clip.
   A media player can only jump to one of these, so the slow part starts on one. */
struct ClipTimes {
	std::vector<int64_t> keysUs;
	int64_t frameUs = 16667;
};

uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t rd64(const uint8_t *p)
{
	return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

struct Box {
	const uint8_t *p = nullptr;
	size_t n = 0;
	explicit operator bool() const { return p != nullptr; }
};

/* Calls `fn` with every box inside `in` of the given type until it returns false. */
template<class F> void each_child(Box in, const char *type, F fn)
{
	size_t i = 0;
	while (in.p && i + 8 <= in.n) {
		uint64_t size = rd32(in.p + i);
		size_t head = 8;
		if (size == 1) {
			if (i + 16 > in.n)
				return;
			size = rd64(in.p + i + 8);
			head = 16;
		} else if (size == 0) {
			size = in.n - i;
		}
		if (size < head || size > in.n - i)
			return;
		if (memcmp(in.p + i + 4, type, 4) == 0 && !fn(Box{in.p + i + head, (size_t)size - head}))
			return;
		i += (size_t)size;
	}
}

Box child(Box in, const char *type)
{
	Box found;
	each_child(in, type, [&](Box b) {
		found = b;
		return false;
	});
	return found;
}

/* The movie box, wherever it sits in the file (at the end for a Backtrack clip). */
bool read_moov(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = os_fopen(path.c_str(), "rb");
	if (!f)
		return false;
	os_fseeki64(f, 0, SEEK_END);
	const int64_t size = os_ftelli64(f);
	int64_t pos = 0;
	bool ok = false;
	while (pos + 8 <= size) {
		uint8_t head[16];
		if (os_fseeki64(f, pos, SEEK_SET) != 0 || fread(head, 1, 8, f) != 8)
			break;
		uint64_t box = rd32(head);
		int64_t headLen = 8;
		if (box == 1) {
			if (fread(head + 8, 1, 8, f) != 8)
				break;
			box = rd64(head + 8);
			headLen = 16;
		} else if (box == 0) {
			box = (uint64_t)(size - pos);
		}
		if ((int64_t)box < headLen || (int64_t)box > size - pos)
			break;
		if (memcmp(head + 4, "moov", 4) == 0) {
			const uint64_t body = box - (uint64_t)headLen;
			if (body > 64ull * 1024 * 1024)
				break;
			out.resize((size_t)body);
			ok = fread(out.data(), 1, out.size(), f) == out.size();
			break;
		}
		pos += (int64_t)box;
	}
	fclose(f);
	return ok;
}

uint32_t header_scale(Box b)
{
	if (!b || b.n < 1)
		return 0;
	if (b.p[0] == 1)
		return b.n >= 24 ? rd32(b.p + 20) : 0;
	return b.n >= 16 ? rd32(b.p + 12) : 0;
}

/* Reads the video track's full pictures: decode time (stts), which samples are full
   pictures (stss), the B-frame shift (ctts) and the edit list (elst). Show time is
   decode time plus the shift, minus where the edit list starts the clip. */
bool clip_times(const std::string &path, ClipTimes &t)
{
	std::vector<uint8_t> moov;
	if (!read_moov(path, moov))
		return false;
	const Box m{moov.data(), moov.size()};
	const uint32_t movieScale = header_scale(child(m, "mvhd"));
	bool ok = false;
	each_child(m, "trak", [&](Box trak) {
		const Box mdia = child(trak, "mdia");
		const Box hdlr = child(mdia, "hdlr");
		if (!hdlr || hdlr.n < 12 || memcmp(hdlr.p + 8, "vide", 4) != 0)
			return true;
		const uint32_t scale = header_scale(child(mdia, "mdhd"));
		const Box stbl = child(child(mdia, "minf"), "stbl");
		const Box stts = child(stbl, "stts");
		if (!scale || !stts || stts.n < 8)
			return false;

		std::vector<int64_t> dts;
		uint32_t n = rd32(stts.p + 4);
		if (8 + (size_t)n * 8 > stts.n)
			return false;
		int64_t at = 0;
		uint32_t first = 0;
		for (uint32_t i = 0; i < n; i++) {
			const uint32_t count = rd32(stts.p + 8 + i * 8), delta = rd32(stts.p + 12 + i * 8);
			if (!first && delta)
				first = delta;
			if (dts.size() + count > 4000000)
				return false;
			for (uint32_t c = 0; c < count; c++) {
				dts.push_back(at);
				at += delta;
			}
		}

		std::vector<int64_t> shift(dts.size(), 0);
		const Box ctts = child(stbl, "ctts");
		if (ctts && ctts.n >= 8) {
			n = rd32(ctts.p + 4);
			size_t s = 0;
			for (uint32_t i = 0; i < n && 16 + (size_t)i * 8 <= ctts.n; i++) {
				const uint32_t count = rd32(ctts.p + 8 + i * 8);
				const int32_t off = (int32_t)rd32(ctts.p + 12 + i * 8);
				for (uint32_t c = 0; c < count && s < shift.size(); c++)
					shift[s++] = off;
			}
		}

		int64_t start = 0, emptyUs = 0;
		const Box elst = child(child(trak, "edts"), "elst");
		if (elst && elst.n >= 8) {
			const bool v1 = elst.p[0] == 1;
			const size_t step = v1 ? 20 : 12;
			n = rd32(elst.p + 4);
			for (uint32_t i = 0; i < n; i++) {
				const size_t e = 8 + (size_t)i * step;
				if (e + step > elst.n)
					break;
				const int64_t segment = v1 ? (int64_t)rd64(elst.p + e) : (int64_t)rd32(elst.p + e);
				const int64_t media = v1 ? (int64_t)rd64(elst.p + e + 8) : (int64_t)(int32_t)rd32(elst.p + e + 4);
				if (media == -1) {
					if (movieScale)
						emptyUs += segment * 1000000 / movieScale;
					continue;
				}
				start = media;
				break;
			}
		}

		const auto show_us = [&](size_t i) { return (dts[i] + shift[i] - start) * 1000000 / scale + emptyUs; };
		const Box stss = child(stbl, "stss");
		if (stss && stss.n >= 8) {
			n = rd32(stss.p + 4);
			for (uint32_t i = 0; i < n && 12 + (size_t)i * 4 <= stss.n; i++) {
				const uint32_t sample = rd32(stss.p + 8 + i * 4);
				if (sample >= 1 && sample <= dts.size())
					t.keysUs.push_back(show_us(sample - 1));
			}
		} else {
			for (size_t i = 0; i < dts.size(); i++)
				t.keysUs.push_back(show_us(i));
		}
		std::sort(t.keysUs.begin(), t.keysUs.end());
		if (first)
			t.frameUs = (int64_t)first * 1000000 / scale;
		ok = !t.keysUs.empty();
		return false;
	});
	return ok;
}

/* ---------- the gate: stops the run up player on one exact picture ---------- */

/* Lives on the run up player. Before it is armed it does nothing. Once armed, the first
   picture through it is the full picture the run up started on, which ties OBS's clock
   to the clip's own times. From then on it knows where every picture sits in the clip,
   lets through the one where the slow part starts, and holds it there. */
struct Gate {
	obs_source_t *self = nullptr;
	std::atomic<bool> armed{false};
	std::atomic<bool> frozen{false};
	std::atomic<int64_t> fromNs{0}; /* clip time of the first picture after arming */
	std::atomic<int64_t> stopNs{0}; /* hold on the first picture at or after this clip time */
	std::atomic<int64_t> atNs{-1};  /* clip time of the last picture let through */
	std::atomic<bool> rearm{false}; /* the next picture through is fromNs again (after a jump or a pause) */
	bool anchored = false;          /* the video thread's own */
	int64_t offset = 0;
};

const char *gate_name(void *)
{
	return "Simply Vertical replay gate";
}

void *gate_create(obs_data_t *, obs_source_t *source)
{
	Gate *g = new Gate();
	g->self = source;
	return g;
}

void gate_destroy(void *data)
{
	delete static_cast<Gate *>(data);
}

obs_source_frame *gate_video(void *data, obs_source_frame *frame)
{
	Gate *g = static_cast<Gate *>(data);
	if (!g->armed.load(std::memory_order_acquire))
		return frame;
	if (g->rearm.exchange(false, std::memory_order_acq_rel))
		g->anchored = false;
	if (g->frozen.load(std::memory_order_relaxed)) {
		/* Dropped: the last picture stays on screen. */
		obs_source_release_frame(obs_filter_get_parent(g->self), frame);
		return nullptr;
	}
	if (!g->anchored) {
		g->offset = (int64_t)frame->timestamp - g->fromNs.load();
		g->anchored = true;
	}
	const int64_t at = (int64_t)frame->timestamp - g->offset;
	g->atNs.store(at);
	if (at >= g->stopNs.load())
		g->frozen.store(true);
	return frame;
}

Gate *gate_of(obs_source_t *filter)
{
	return filter ? static_cast<Gate *>(obs_obj_get_data(filter)) : nullptr;
}

/* ---------- one replay ---------- */

struct Player {
	obs_source_t *src = nullptr;
	obs_source_t *fade = nullptr; /* opacity filter, for the fade out */
	obs_sceneitem_t *item = nullptr;
	uint64_t heldAt = 0; /* when it was last put back on its picture */
};

/* All of these belong to OBS's main thread. */
Player g_run;                        /* the run up at normal speed, or the only player */
Player g_slow;                       /* the slow part, empty when one player plays it all */
obs_source_t *g_gate = nullptr;      /* on g_run, only when there are two players */
obs_source_t *g_frame = nullptr;     /* the lit frame and tag picture, or null when switched off */
obs_source_t *g_frameFade = nullptr; /* opacity filter on the frame */
obs_scene_t *g_scene = nullptr;      /* live layout underneath, the clip, the frame on top */
std::string g_canvas;           /* the canvas it covers */
uint64_t g_token = 0;           /* goes up on every start and stop, so a late "it ended" is ignored */
uint64_t g_heard = 0;           /* the token the players' "it ended" was connected with */
QTimer *g_guard = nullptr;      /* ends it in case the player never says it finished */
QTimer *g_tick = nullptr;       /* runs while opening, sliding in, waiting for the slow part, fading out */
Phase g_phase = Phase::Off;
uint64_t g_phaseAt = 0;         /* os_gettime_ns when the phase began */
uint64_t g_seekAt = 0;          /* when the jump to the last seconds was made, 0 before it */
bool g_swapped = false;         /* the slow player has taken over */
uint64_t g_frozenAt = 0;        /* when the run up stopped on the slow part's picture */
bool g_sound = false;
std::string g_path;             /* the clip being replayed */
std::string g_lastPlayed;       /* the last clip started, for Play previous and Play next clip */
std::deque<SvClip> g_row;       /* Play in a row: the clips still to come after this one */
int g_rowTotal = 0;             /* how many clips the row had, for "clip 2 of 3" */
uint64_t g_playOnSave = 0;      /* Save and replay was pressed at this time: play the clip it saves */
std::string g_noticeText;       /* the line the panel shows while it plays */

/* The controls while it plays (pause, back 5 s, restart, speed). */
bool g_paused = false;
uint64_t g_moves = 0;           /* goes up on every control, so a fade already on its way is called off */
Player *g_landing = nullptr;    /* the player a jump is waiting on, until it is on its new picture */
int64_t g_landUs = 0;
uint64_t g_landAt = 0;
uint64_t g_speedAt = 0;         /* when the hidden slow player last changed speed */
Player g_spare;                 /* a player at a new speed, waiting to take over from the one on screen */
Player *g_spareFor = nullptr;
int64_t g_spareUs = 0;
int g_sparePct = 100;
uint64_t g_spareAt = 0;
bool g_spareWait = false;       /* the player opening catches up with the one on screen before it takes over */
bool g_spareChasing = false;    /* and is playing underneath to catch up */
int g_spareKind = 0;            /* SPARE_SPEED, SPARE_PRESS (the pressed slow motion starts) or SPARE_BACK */
enum { SPARE_SPEED = 0, SPARE_PRESS = 1, SPARE_BACK = 2 };

/* "When I press": the replay plays at normal speed, and the Slow motion key slows it from
   just before the press for the length picked, then it goes back to normal speed. */
bool g_press = false;
int64_t g_pressFromUs = -1; /* where the pressed slow motion started, -1 at normal speed */
int64_t g_pressBackUs = -1; /* the full picture where it goes back to normal speed, -1 at the end */
int64_t g_pressWantUs = -1; /* where the slow motion asked for should end */
uint32_t g_runMixers = 0;   /* the replay's sound tracks, for the parts at normal speed */
constexpr int64_t PRESS_BACK_US = 1000000; /* the slow motion starts this far before the press */
constexpr int PRESS_LEAD_MS = 1200;        /* the normal speed player opens this long before it takes over */

/* What this replay plays, worked out once the clip is open. */
ClipTimes g_times;
bool g_timesOk = false;
int g_wantMs = 0;      /* how much of the end to play, 0 for all of it */
int g_after = 0;       /* seconds the clip kept going after Save clip */
int g_slowPct = 100;   /* the slow part's speed */
int g_slowSecs = 3;
int g_runPct = 100;    /* the run up player's speed */
int64_t g_fromUs = 0;  /* where the replay starts */
int64_t g_slowUs = -1; /* where the slow part starts, -1 with one player */
int64_t g_durUs = 0;
int64_t g_endUs = 0;   /* where the replay ends, the clip's end or a little after the press */

QRectF g_frameBox;              /* and round what */
uint32_t g_frameColour = 0;     /* the colour replay-frame.png was last drawn in */
QPointF g_frameTagAt;           /* and where its tag was */
bool g_frameRail = false;       /* and whether it has the line's rail */

/* The look: wipe, the line and the zoom. Cinematic is a whole look of its own (see cine_draw). */
enum { TRANS_WIPE = 0, TRANS_SLIDE = 1, TRANS_FADE = 2, TRANS_CINE = 3 };
int g_trans = TRANS_WIPE;
struct Band {
	obs_source_t *src = nullptr;
	obs_sceneitem_t *item = nullptr;
	float off = 0; /* from the middle of the wide band, along the way it travels */
	float half = 0; /* half its width across the phone */
};
std::vector<Band> g_bands;
obs_sceneitem_t *g_wordItem = nullptr; /* the big REPLAY riding on the wide band */
obs_source_t *g_word = nullptr;
bool g_covered = false;    /* the bands cover the whole phone right now */
bool g_startCovered = false; /* the next start comes in under bands already covering (a row) */
int g_wipeDir = -1;        /* -1 the bands travel left, 1 right */
uint64_t g_wipeAt = 0;
int g_inMs = IN_MS;
bool g_keepCover = false;  /* sv_replay_stop leaves the picture on the phone for the next clip */
obs_source_t *g_line = nullptr;      /* the lit line under the tag */
obs_source_t *g_lineFade = nullptr;
obs_sceneitem_t *g_lineItem = nullptr;
int g_lineW = 0;   /* the line's length in the picture */
int g_linePad = 0; /* the glow around it */
int g_lineShown = -1;
QTimer *g_fx = nullptr; /* moves the line and the zoom while it plays */
uint64_t g_fxLast = 0;
int64_t g_estUs = 0;    /* where the clip is, kept smooth between the player's own steps */
int g_zoomPct = 0;
QPointF g_focus(540, 960);
float g_zoomNow = 1.0f;
uint32_t g_wordColour = 0;
uint32_t g_lineColour = 0;
QRectF g_wipeBox(0, 0, CANVAS_W, CANVAS_H); /* what the bands sweep across: the phone or the window */

/* A Replay Window and the game zoom are drawn by our own sources on OBS's drawing
   thread, so what they show is kept here, under g_viewMtx. */
std::mutex g_viewMtx;
obs_source_t *g_winSrc = nullptr;   /* the Replay Window the replay plays in */
obs_source_t *g_winShow = nullptr;  /* the replay picture it shows */
QRectF g_winBox;                    /* the window's place on the phone */
obs_source_t *g_lensShow = nullptr; /* the player the game zoom magnifies */
QRectF g_lensRegion;                /* the part of the phone it magnifies */

/* Keep my facecam live, and the zoom that then pushes in on the game alone. Main thread. */
obs_source_t *g_camLive = nullptr;
obs_source_t *g_lens = nullptr;
obs_sceneitem_t *g_lensItem = nullptr;
QRectF g_lensBox;
bool g_inWindow = false;

bool on_main_thread()
{
	return qApp && QThread::currentThread() == qApp->thread();
}

double since_ms(uint64_t at)
{
	return at ? (double)(os_gettime_ns() - at) / 1e6 : 0.0;
}

void set_opacity(obs_source_t *filter, double a)
{
	if (!filter)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "opacity", std::clamp(a, 0.0, 1.0));
	obs_source_update(filter, d);
	obs_data_release(d);
}

obs_source_t *add_fade(obs_source_t *to, double start)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "opacity", start);
	obs_source_t *f = obs_source_create_private("color_filter_v2", "Simply Vertical replay fade", d);
	obs_data_release(d);
	if (f)
		obs_source_filter_add(to, f);
	return f;
}

void set_clip_x(float x)
{
	vec2 pos;
	vec2_set(&pos, x, 0.0f);
	for (Player *p : {&g_slow, &g_run})
		if (p->item)
			obs_sceneitem_set_pos(p->item, &pos);
}

/* The tag's letters. */
QFont tag_font()
{
	QFont f("Archivo");
	f.setWeight(QFont::Black);
	f.setPixelSize(64);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 3);
	f.setStyleStrategy(QFont::PreferAntialias);
	return f;
}

qreal cap_height(const QFontMetricsF &fm)
{
	return fm.capHeight() > 0 ? fm.capHeight() : fm.ascent() * 0.72;
}

/* The tag's size: the play mark, a gap, then REPLAY. */
QSizeF tag_size()
{
	const QFontMetricsF fm(tag_font());
	const qreal capH = cap_height(fm);
	return QSizeF(capH * 0.62 * 1.78 + 18 + fm.horizontalAdvance(QString::fromUtf8(T("ReplayTag"))), capH);
}

/* How the replay looks for the layout on screen now. The colour: the facecam's frame,
   else the first framed layer, else Vertical blue (the website paints frames in the
   member's brand colour, so this follows their brand). The tag: the first spot from
   the top of every app's clear area, left side then right, that covers no layer, so
   it never sits on a face or a minimap. */
struct ReplayLook {
	uint32_t colour = 0xff4d7cff;
	QPointF tagAt{124, 304};
	QRectF box{0, 0, 1080, 1920}; /* where the replay shows: the whole phone, or the Replay Window */
	bool window = false;
};

/* In a Replay Window the frame and tag go round the window, the tag in its top left corner. */
ReplayLook replay_look(const QRectF *win = nullptr)
{
	ReplayLook look;
	SvLayout l;
	std::string err;
	const std::string json = sv_link_showing();
	if (json.empty() || !sv_layout_parse(json.c_str(), l, err)) {
		if (win) {
			look.box = *win;
			look.window = true;
			look.tagAt = QPointF(win->x() + 34, win->y() + 30);
		}
		return look;
	}
	bool found = false;
	for (const SvPiece &p : l.pieces)
		if (p.src == SvSrc::Cam) {
			look.colour = p.color | 0xff000000;
			found = true;
			break;
		}
	if (!found)
		for (const SvPiece &p : l.pieces)
			if (p.frame != SvFrame::None) {
				look.colour = p.color | 0xff000000;
				break;
			}
	if (win) {
		look.box = *win;
		look.window = true;
		look.tagAt = QPointF(win->x() + 34, win->y() + 30);
		return look;
	}

	/* Clear of TikTok, YouTube, Instagram and Twitch: x 116 to 964, y 300 to about 1180. */
	const QSizeF size = tag_size();
	const qreal pad = 18;
	const auto clear = [&](qreal x, qreal y) {
		const QRectF tag(x - pad, y - pad, size.width() + pad * 2, size.height() + pad * 2);
		for (const SvPiece &p : l.pieces)
			if (tag.intersects(QRectF(p.box.x, p.box.y, p.box.w, p.box.h)))
				return false;
		return true;
	};
	for (qreal y = 304; y + size.height() <= 1180; y += 8) {
		if (clear(124, y)) {
			look.tagAt = QPointF(124, y);
			return look;
		}
		if (clear(956 - size.width(), y)) {
			look.tagAt = QPointF(956 - size.width(), y);
			return look;
		}
	}
	return look; /* nowhere clear: top left, over whatever is there */
}

QColor mix_white(uint32_t argb, double accent)
{
	const auto ch = [&](int shift) { return (int)std::lround(((argb >> shift) & 255) * accent + 255 * (1 - accent)); };
	return QColor(ch(16), ch(8), ch(0));
}

/* Adds a soft glow of `shape`'s see-through part, in `colour`, under whatever `img` already has. */
void paint_glow(QImage &img, const QImage &shape, const QColor &colour, float sigma, double strength, int dy)
{
	const int w = shape.width(), h = shape.height();
	std::vector<float> a((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const QRgb *row = reinterpret_cast<const QRgb *>(shape.constScanLine(y));
		for (int x = 0; x < w; x++)
			a[(size_t)y * w + x] = qAlpha(row[x]) / 255.0f;
	}
	sv_brand_blur(a, w, h, sigma);
	QImage glow(w, h, QImage::Format_ARGB32_Premultiplied);
	for (int y = 0; y < h; y++) {
		QRgb *row = reinterpret_cast<QRgb *>(glow.scanLine(y));
		for (int x = 0; x < w; x++) {
			const int al = (int)std::lround(std::min(1.0f, a[(size_t)y * w + x] * (float)strength) * 255);
			row[x] = qPremultiply(qRgba(colour.red(), colour.green(), colour.blue(), al));
		}
	}
	QPainter p(&img);
	p.drawImage(0, dy, glow);
}

/* Where the line under the tag sits: as long as the tag, just under it. */
QRectF line_rect(const ReplayLook &look)
{
	const QSizeF size = tag_size();
	return QRectF(look.tagAt.x(), look.tagAt.y() + size.height() + 24, size.width(), 6);
}

std::string config_file(const char *name)
{
	char *p = obs_module_config_path(name);
	const std::string path = p ? p : "";
	bfree(p);
	return path;
}

/* The frame and tag, drawn once per colour into the plugin's settings folder for an
   image source to show. Plain lit text and lines, no plate behind the tag. */
std::string frame_picture(const ReplayLook &look, bool rail)
{
	const uint32_t argb = look.colour;
	const std::string path = config_file("replay-frame.png");
	if (path.empty())
		return path;
	if (g_frameColour == argb && g_frameTagAt == look.tagAt && g_frameRail == rail && g_frameBox == look.box &&
	    os_file_exists(path.c_str()))
		return path;

	const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
	QImage img(CANVAS_W, CANVAS_H, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);

	/* The frame: an inset rounded line lit in the colour, with its light spilling inwards and out. */
	QPainterPath edge;
	const qreal round = look.window ? std::min<qreal>(26, std::min(look.box.width(), look.box.height()) / 6) : 34;
	edge.addRoundedRect(look.box.adjusted(12, 12, -12, -12), round, round);
	{
		QImage shape(CANVAS_W, CANVAS_H, QImage::Format_ARGB32_Premultiplied);
		shape.fill(Qt::transparent);
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::Antialiasing);
		sp.setPen(QPen(Qt::white, 10));
		sp.drawPath(edge);
		sp.end();
		paint_glow(img, shape, accent, 12.0f, 0.9, 0);
	}
	{
		QPainter fp(&img);
		fp.setRenderHint(QPainter::Antialiasing);
		fp.setPen(QPen(mix_white(argb, 0.55), 3));
		fp.drawPath(edge);
	}

	/* The tag, in the clear spot replay_look() found: a play mark and REPLAY. */
	const QFont f = tag_font();
	const QString word = QString::fromUtf8(T("ReplayTag"));
	const QFontMetricsF fm(f);
	const qreal x0 = look.tagAt.x(), top = look.tagAt.y();
	const qreal capH = cap_height(fm);
	const qreal base = top + capH;
	QPainterPath mark; /* two play triangles, like a rewind played forwards */
	const qreal mh = capH, mw = capH * 0.62;
	for (int i = 0; i < 2; i++) {
		const qreal mx = x0 + i * (mw * 0.78);
		mark.moveTo(mx, top);
		mark.lineTo(mx + mw, top + mh / 2);
		mark.lineTo(mx, top + mh);
		mark.closeSubpath();
	}
	const qreal textX = x0 + mw * 1.78 + 18;

	QImage shape(CANVAS_W, CANVAS_H, QImage::Format_ARGB32_Premultiplied);
	shape.fill(Qt::transparent);
	{
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::Antialiasing);
		sp.setRenderHint(QPainter::TextAntialiasing);
		sp.fillPath(mark, Qt::white);
		sp.setFont(f);
		sp.setPen(Qt::white);
		sp.drawText(QPointF(textX, base), word);
	}
	/* The approved soft underglow (DESIGN.md text depth), never a halo: a dark drop
	   and a faint colour glow just under the letters. */
	paint_glow(img, shape, QColor(0, 0, 0), 8.0f, 0.4, 8);
	paint_glow(img, shape, accent, 6.0f, 0.3, 4);
	paint_glow(img, shape, accent, 3.0f, 0.6, 2);
	paint_glow(img, shape, QColor(0, 0, 0), 1.0f, 0.85, 2);
	{
		QPainter tp(&img);
		tp.setRenderHint(QPainter::Antialiasing);
		tp.setRenderHint(QPainter::TextAntialiasing);
		tp.fillPath(mark, mix_white(argb, 0.85));
		tp.setFont(f);
		tp.setPen(mix_white(argb, 0.22));
		tp.drawText(QPointF(textX, base), word);
		/* The rail the line fills along: the colour, faint. */
		if (rail) {
			QColor dim = accent;
			dim.setAlphaF(0.3f);
			const QRectF r = line_rect(look);
			tp.setPen(Qt::NoPen);
			tp.setBrush(dim);
			tp.drawRoundedRect(r, r.height() / 2, r.height() / 2);
		}
	}

	if (!img.save(QString::fromStdString(path), "PNG")) {
		obs_log(LOG_WARNING, "replay frame picture could not be saved");
		return "";
	}
	g_frameColour = argb;
	g_frameTagAt = look.tagAt;
	g_frameRail = rail;
	g_frameBox = look.box;
	return path;
}

/* The line itself, lit, with its glow round it. It is shown cut off at how far the
   replay has got. */
std::string line_picture(const ReplayLook &look)
{
	const uint32_t argb = look.colour;
	const std::string path = config_file("replay-line.png");
	const QRectF r = line_rect(look);
	const int pad = 20;
	g_lineW = (int)std::ceil(r.width());
	g_linePad = pad;
	if (path.empty())
		return path;
	if (g_lineColour == argb && os_file_exists(path.c_str())) {
		QImage was(QString::fromStdString(path));
		if (was.width() == g_lineW + pad * 2)
			return path;
	}
	const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
	QImage img(g_lineW + pad * 2, (int)r.height() + pad * 2, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	const QRectF bar(pad, pad, g_lineW, r.height());
	QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
	shape.fill(Qt::transparent);
	{
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::Antialiasing);
		sp.setPen(Qt::NoPen);
		sp.setBrush(Qt::white);
		sp.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
	}
	paint_glow(img, shape, accent, 7.0f, 0.9, 0);
	{
		QPainter lp(&img);
		lp.setRenderHint(QPainter::Antialiasing);
		lp.setPen(Qt::NoPen);
		lp.setBrush(mix_white(argb, 0.5));
		lp.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
	}
	if (!img.save(QString::fromStdString(path), "PNG"))
		return "";
	g_lineColour = argb;
	return path;
}

/* The big REPLAY the wipe carries across: the tag's play mark and word, large and light
   on the colour, with a soft dark drop under it. */
std::string word_picture(uint32_t argb)
{
	const std::string path = config_file("replay-word.png");
	if (path.empty())
		return path;
	if (g_wordColour == argb && os_file_exists(path.c_str()))
		return path;
	QFont f("Archivo");
	f.setWeight(QFont::Black);
	f.setPixelSize(150);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 6);
	f.setStyleStrategy(QFont::PreferAntialias);
	const QFontMetricsF fm(f);
	const QString word = QString::fromUtf8(T("ReplayTag"));
	const qreal capH = cap_height(fm);
	const qreal mw = capH * 0.62, gap = 36, pad = 40;
	const qreal w = mw * 1.78 + gap + fm.horizontalAdvance(word);
	QImage img((int)std::ceil(w + pad * 2), (int)std::ceil(capH + pad * 2), QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	const qreal top = pad, x0 = pad;
	QPainterPath mark;
	for (int i = 0; i < 2; i++) {
		const qreal mx = x0 + i * (mw * 0.78);
		mark.moveTo(mx, top);
		mark.lineTo(mx + mw, top + capH / 2);
		mark.lineTo(mx, top + capH);
		mark.closeSubpath();
	}
	const qreal textX = x0 + mw * 1.78 + gap;
	QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
	shape.fill(Qt::transparent);
	{
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::Antialiasing);
		sp.setRenderHint(QPainter::TextAntialiasing);
		sp.fillPath(mark, Qt::white);
		sp.setFont(f);
		sp.setPen(Qt::white);
		sp.drawText(QPointF(textX, top + capH), word);
	}
	paint_glow(img, shape, QColor(0, 0, 0), 10.0f, 0.45, 10);
	paint_glow(img, shape, QColor(0, 0, 0), 2.0f, 0.6, 3);
	{
		QPainter tp(&img);
		tp.setRenderHint(QPainter::Antialiasing);
		tp.setRenderHint(QPainter::TextAntialiasing);
		const QColor ink = mix_white(argb, 0.08);
		tp.fillPath(mark, ink);
		tp.setFont(f);
		tp.setPen(ink);
		tp.drawText(QPointF(textX, top + capH), word);
	}
	if (!img.save(QString::fromStdString(path), "PNG"))
		return "";
	g_wordColour = argb;
	return path;
}

/* OBS colour sources take their colour as 0xAABBGGRR. */
uint32_t abgr(const QColor &c)
{
	return 0xff000000u | ((uint32_t)c.blue() << 16) | ((uint32_t)c.green() << 8) | (uint32_t)c.red();
}

/* The bands: two thin ones leading, the wide one that covers the phone, a thin one behind. */
void add_bands(obs_scene_t *scene, uint32_t argb)
{
	const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
	const QColor dark(accent.red() * 55 / 100, accent.green() * 55 / 100, accent.blue() * 55 / 100);
	const QColor light = mix_white(argb, 0.45);
	const float across = 1.0f / std::cos(BAND_DEG * 3.14159265f / 180.0f);
	struct Spec {
		float w;
		QColor c;
		int place; /* -2, -1 leading, 0 the wide one, 1 behind */
	};
	const Spec specs[] = {{1600, accent, 0}, {110, dark, -1}, {56, light, -2}, {90, dark, 1}};
	const float mainHalf = 1600 * across / 2;
	float lead = -mainHalf, trail = mainHalf;
	g_bands.clear();
	for (const Spec &s : specs) {
		Band b;
		b.half = s.w * across / 2;
		if (s.place == 0)
			b.off = 0;
		else if (s.place < 0) {
			const float gap = s.place == -1 ? 44.0f : 30.0f;
			b.off = lead - gap - b.half;
			lead = b.off - b.half;
		} else {
			b.off = trail + 44.0f + b.half;
			trail = b.off + b.half;
		}
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "color", abgr(s.c));
		obs_data_set_int(d, "width", (int)s.w);
		obs_data_set_int(d, "height", (int)BAND_H);
		b.src = obs_source_create_private("color_source_v3", "Simply Vertical replay band", d);
		obs_data_release(d);
		if (!b.src)
			continue;
		b.item = obs_scene_add(scene, b.src);
		obs_sceneitem_set_alignment(b.item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_rot(b.item, BAND_DEG);
		obs_sceneitem_set_visible(b.item, false);
		g_bands.push_back(b);
	}
	const std::string pic = word_picture(argb);
	if (!pic.empty()) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "file", pic.c_str());
		g_word = obs_source_create_private("image_source", "Simply Vertical replay word", d);
		obs_data_release(d);
		if (g_word) {
			g_wordItem = obs_scene_add(scene, g_word);
			obs_sceneitem_set_alignment(g_wordItem, OBS_ALIGN_CENTER);
			obs_sceneitem_set_visible(g_wordItem, false);
			const uint32_t ww = obs_source_get_width(g_word);
			const float fit = ww ? std::min(1.0f, (float)g_wipeBox.width() * 0.86f / (float)ww) : 1.0f;
			if (fit < 1.0f) {
				vec2 sc;
				vec2_set(&sc, fit, fit);
				obs_sceneitem_set_scale(g_wordItem, &sc);
			}
		}
	}
}

/* Where the bands are: 0 all off the side they come from, 0.5 the phone covered, 1 all off
   the other side. dir -1 travels left, 1 right. */
void place_bands(float p, int dir)
{
	if (g_bands.empty())
		return;
	const float bx = (float)g_wipeBox.x(), bw = (float)g_wipeBox.width();
	const float cy = (float)g_wipeBox.center().y();
	const float slant = (float)g_wipeBox.height() / 2.0f * std::tan(BAND_DEG * 3.14159265f / 180.0f);
	float front = 0, back = 0;
	for (const Band &b : g_bands) {
		front = std::max(front, -b.off + b.half);
		back = std::max(back, b.off + b.half);
	}
	const float start = bx + bw + slant + 20 + front;
	const float end = bx - slant - 20 - back;
	const float mid = bx + bw / 2.0f;
	const float s = p <= 0.5f ? start + (mid - start) * (p * 2) : mid + (end - mid) * ((p - 0.5f) * 2);
	const bool on = p > 0.0f && p < 1.0f;
	for (const Band &b : g_bands) {
		float x = s + b.off;
		if (dir > 0)
			x = 2.0f * mid - x;
		vec2 pos;
		vec2_set(&pos, x, cy);
		obs_sceneitem_set_pos(b.item, &pos);
		obs_sceneitem_set_visible(b.item, on);
	}
	if (g_wordItem) {
		vec2 pos;
		vec2_set(&pos, dir > 0 ? 2.0f * mid - s : s, cy);
		obs_sceneitem_set_pos(g_wordItem, &pos);
		obs_sceneitem_set_visible(g_wordItem, on);
	}
}

/* The bands come on fast and settle on the cover; they leave slowly first, so REPLAY can be read. */
float cover_k(double t)
{
	const float k = std::clamp((float)(t / COVER_MS), 0.0f, 1.0f);
	return 1.0f - std::pow(1.0f - k, 3.0f);
}

float reveal_k(double t)
{
	const float k = std::clamp((float)(t / REVEAL_MS), 0.0f, 1.0f);
	return k * k * k;
}

void frame_opacity(double a)
{
	set_opacity(g_frameFade, a);
	set_opacity(g_lineFade, a);
}

void clip_opacity(double a)
{
	set_opacity(g_run.fade, a);
	set_opacity(g_slow.fade, a);
}

void show_clip_items(bool on)
{
	for (Player *p : {&g_run, &g_slow})
		if (p->item)
			obs_sceneitem_set_visible(p->item, on);
	if (g_lensItem)
		obs_sceneitem_set_visible(g_lensItem, on && g_zoomNow > 1.0005f);
}

/* ---------- Cinematic, taken from Simply Ops' Cinematic opener ----------
   The way in: the picture closes like a camera shutter (two black halves meet in the
   middle), a line of light cuts across where they meet and blooms, then the shutter opens
   on the replay. The pop up: INSTANT settles in from wide between two thin rules, REPLAY
   lands big over a lens flare, and a thin lit frame with corner brackets goes round the
   game. As the slow motion starts a streak of light races across the game. The way out: the pop up fades, the shutter closes and opens on live.
   Every piece is a picture drawn once per colour, then only moved, scaled and faded,
   with Ops' own timings and curves. */
constexpr int CINE_CLOSE_MS = 490;                     /* the halves closing */
constexpr int CINE_LINE_FROM = 385, CINE_LINE_TO = 665; /* the line of light growing across */
constexpr int CINE_BLOOM_FROM = 455, CINE_BLOOM_TO = 770;
constexpr int CINE_SHUT_MS = 770;  /* closed and at its brightest: the shutter can open */
constexpr int CINE_OPEN_MS = 770;  /* the halves opening */
constexpr int CINE_OUT_LEAD = 140; /* on the way out, the pop up starts fading this long before the shutter */
constexpr int CINE_POP_OUT_MS = 210;
constexpr int CINE_STREAK_MS = 900;
constexpr int CINE_WHOLE_MOMENT_MS = 1100; /* with the whole replay slow, the moment comes once the pop up is in */
constexpr qreal CINE_CARD_W = 720, CINE_CARD_H = 200;

struct CineItem {
	obs_source_t *src = nullptr;
	obs_source_t *fade = nullptr;
	obs_sceneitem_t *item = nullptr;
	QPointF at;        /* its middle on the phone */
	float base = 1.0f; /* its size here (smaller in a Replay Window) */
	bool on = false;
	double shown = -1, sx = -1, sy = -1, dx = 1e9, dy = 1e9; /* what was last set, so nothing is sent twice */
};

struct Cine {
	CineItem top, bottom, topEdge, bottomEdge, line, bloom; /* the shutter */
	CineItem frame, kick, rules, flare, word; /* the pop up */
	CineItem streak, haze;                         /* the slow motion moment */
	float halfH = 0;    /* each shutter half's height */
	float boxW = CANVAS_W;
	float wordDrop = 0; /* how far REPLAY settles from */
	uint64_t shutAt = 0;   /* when the shutter began to close, 0 before */
	uint64_t openAt = 0;   /* when it began to open, 0 before */
	uint64_t popAt = 0;    /* when the pop up began */
	uint64_t momentAt = 0; /* when the slow motion moment begins (can be ahead), 0 for none */
	uint64_t outAt = 0;    /* when the pop up began to fade out */
	bool hid = false;      /* on the way out, the clip has gone from under the shutter */
	double progShown = -1;
};
Cine g_cine;
std::map<std::string, std::string> g_cineDrawn; /* each Cinematic picture and what it was last drawn for */

/* A CSS cubic-bezier curve, the same ones Ops' Cinematic uses. */
double bez(double x1, double y1, double x2, double y2, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	if (t <= 0.0 || t >= 1.0)
		return t;
	const double cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
	const double cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
	double u = t;
	for (int i = 0; i < 10; i++) {
		const double x = ((ax * u + bx) * u + cx) * u - t;
		const double dx = (3 * ax * u + 2 * bx) * u + cx;
		if (std::fabs(x) < 1e-6 || std::fabs(dx) < 1e-9)
			break;
		u = std::clamp(u - x / dx, 0.0, 1.0);
	}
	return ((ay * u + by) * u + cy) * u;
}

double ease_css(double k)
{
	return bez(0.25, 0.1, 0.25, 1.0, k);
}

double ease_settle(double k)
{
	return bez(0.16, 1.0, 0.3, 1.0, k);
}

/* 0 before a, 1 after b, in between in a straight line. */
double ramp(double t, double a, double b)
{
	return std::clamp((t - a) / (b - a), 0.0, 1.0);
}

/* Milliseconds since `at`, below 0 while `at` is still ahead. */
double cine_ms(uint64_t at)
{
	return at ? ((double)os_gettime_ns() - (double)at) / 1e6 : -1e9;
}

void cine_put(CineItem &c, double opacity, double sx, double sy, double dx = 0, double dy = 0)
{
	if (!c.item)
		return;
	const bool on = opacity > 0.002;
	if (on != c.on) {
		obs_sceneitem_set_visible(c.item, on);
		c.on = on;
	}
	if (!on)
		return;
	if (c.fade && std::fabs(opacity - c.shown) > 0.002) {
		set_opacity(c.fade, opacity);
		c.shown = opacity;
	}
	if (std::fabs(sx - c.sx) > 0.0005 || std::fabs(sy - c.sy) > 0.0005) {
		vec2 sc;
		vec2_set(&sc, (float)(c.base * sx), (float)(c.base * sy));
		obs_sceneitem_set_scale(c.item, &sc);
		c.sx = sx;
		c.sy = sy;
	}
	if (std::fabs(dx - c.dx) > 0.05 || std::fabs(dy - c.dy) > 0.05) {
		vec2 pos;
		vec2_set(&pos, (float)(c.at.x() + dx), (float)(c.at.y() + dy));
		obs_sceneitem_set_pos(c.item, &pos);
		c.dx = dx;
		c.dy = dy;
	}
}

void cine_release(CineItem &c)
{
	obs_source_release(c.fade);
	obs_source_release(c.src);
	c = CineItem{};
}

/* One of the look's pictures, drawn into the plugin's settings folder, again only when
   what it shows (`key`) has changed. */
std::string cine_picture(const char *name, const std::string &key, QSize size, const std::function<void(QImage &)> &paint)
{
	const std::string path = config_file(name);
	if (path.empty())
		return path;
	auto it = g_cineDrawn.find(name);
	if (it != g_cineDrawn.end() && it->second == key && os_file_exists(path.c_str()))
		return path;
	QImage img(size, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	paint(img);
	if (!img.save(QString::fromStdString(path), "PNG")) {
		obs_log(LOG_WARNING, "replay look picture %s could not be saved", name);
		return "";
	}
	g_cineDrawn[name] = key;
	return path;
}

std::string key_of(uint32_t argb, const std::string &more = "")
{
	return std::to_string(argb) + "|" + more;
}

/* A word, its letters' middle in the middle of the picture. Small words get a faint glow
   in the colour; REPLAY gets the approved soft depth (a dark drop and a tight colour glow). */
std::string cine_text(const char *name, const QString &text, const QFont &f, const QColor &ink, uint32_t argb, bool title)
{
	const QFontMetricsF fm(f);
	const qreal spacing = f.letterSpacing();
	const qreal w = fm.horizontalAdvance(text) - spacing, capH = cap_height(fm), pad = title ? 40 : 24;
	const QSize size((int)std::ceil(w + pad * 2), (int)std::ceil(capH + pad * 2));
	const std::string key = key_of(argb, text.toStdString() + "|" + std::to_string(f.pixelSize()) + "|" +
						     std::to_string(ink.rgba()));
	return cine_picture(name, key, size, [&](QImage &img) {
		const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
		QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
		shape.fill(Qt::transparent);
		{
			QPainter sp(&shape);
			sp.setRenderHint(QPainter::Antialiasing);
			sp.setRenderHint(QPainter::TextAntialiasing);
			sp.setFont(f);
			sp.setPen(Qt::white);
			sp.drawText(QPointF(pad, pad + capH), text);
		}
		if (title) {
			paint_glow(img, shape, QColor(0, 0, 0), 4.0f, 0.75, 3);
			paint_glow(img, shape, accent, 5.0f, 0.55, 0);
		} else {
			paint_glow(img, shape, accent, 4.0f, 0.6, 0);
		}
		QPainter tp(&img);
		tp.setRenderHint(QPainter::Antialiasing);
		tp.setRenderHint(QPainter::TextAntialiasing);
		tp.setFont(f);
		tp.setPen(ink);
		tp.drawText(QPointF(pad, pad + capH), text);
	});
}

/* A thin lit bar across `w`, brightest in the middle, with its glow. */
std::string cine_bar(const char *name, uint32_t argb, int w, int h, int core, const std::vector<std::pair<qreal, QColor>> &stops,
		     float glowSigma, double glowStrength, bool whiteGlow)
{
	std::string key = key_of(argb, std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(core));
	return cine_picture(name, key, QSize(w, h), [&](QImage &img) {
		const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
		QLinearGradient g(0, 0, w, 0);
		for (const auto &s : stops)
			g.setColorAt(s.first, s.second);
		const QRectF bar(0, (h - core) / 2.0, w, core);
		QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
		shape.fill(Qt::transparent);
		{
			QPainter sp(&shape);
			sp.fillRect(bar, g);
		}
		if (whiteGlow)
			paint_glow(img, shape, QColor(255, 255, 255), glowSigma * 0.4f, glowStrength, 0);
		paint_glow(img, shape, accent, glowSigma, glowStrength, 0);
		QPainter p(&img);
		p.fillRect(bar, g);
	});
}

/* A soft round light: white in the middle, the colour, then nothing. `wide` stretches it sideways. */
std::string cine_glow(const char *name, uint32_t argb, int w, int h, bool whiteCore)
{
	return cine_picture(name, key_of(argb, std::to_string(w) + "x" + std::to_string(h)), QSize(w, h), [&](QImage &img) {
		const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.translate(w / 2.0, h / 2.0);
		p.scale((qreal)w / h, 1.0);
		QRadialGradient g(QPointF(0, 0), h / 2.0);
		QColor a = accent, none = accent;
		none.setAlpha(0);
		if (whiteCore) {
			a.setAlphaF(0.7f);
			g.setColorAt(0.0, QColor(255, 255, 255));
			g.setColorAt(0.18, a);
			g.setColorAt(0.62, none);
		} else {
			a.setAlphaF(0.8f);
			g.setColorAt(0.0, a);
			g.setColorAt(0.7, none);
		}
		g.setColorAt(1.0, none);
		p.setPen(Qt::NoPen);
		p.setBrush(g);
		p.drawEllipse(QPointF(0, 0), h / 2.0, h / 2.0);
	});
}

/* The frame round the game: a hairline in the colour with a dark ring and a soft glow round
   it, corner brackets, and the faint rail the progress line fills along (railY from the
   frame's top, below 0 for none). The frame sits PAD in from the picture's edge. */
constexpr int CINE_FRAME_PAD = 80;
std::string cine_frame(uint32_t argb, QSizeF fs, qreal railY)
{
	const int pad = CINE_FRAME_PAD;
	const QSize size((int)std::ceil(fs.width()) + pad * 2, (int)std::ceil(fs.height()) + pad * 2);
	const std::string key = key_of(argb, std::to_string(size.width()) + "x" + std::to_string(size.height()) + "|" +
						     std::to_string((int)std::lround(railY)));
	return cine_picture("replay-cine-frame.png", key, size, [&](QImage &img) {
		const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
		const QRectF r(pad, pad, fs.width(), fs.height());
		{
			QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
			shape.fill(Qt::transparent);
			QPainter sp(&shape);
			sp.setRenderHint(QPainter::Antialiasing);
			sp.setPen(QPen(Qt::white, 10));
			sp.drawRect(r);
			sp.end();
			paint_glow(img, shape, accent, 22.0f, 0.5, 0);
		}
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(QColor(0, 0, 0, 140), 8));
		p.drawRect(r.adjusted(-5, -5, 5, 5));
		p.setPen(QPen(mix_white(argb, 0.6), 2));
		p.drawRect(r);
		/* The brackets, centred on each corner. */
		const qreal arm = 40, off = 20;
		QPainterPath br;
		const QPointF cs[] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
		for (int i = 0; i < 4; i++) {
			const qreal sx = (i == 1 || i == 2) ? 1 : -1, sy = (i >= 2) ? 1 : -1;
			const QPointF c = cs[i] + QPointF(sx * off, sy * off);
			br.moveTo(c + QPointF(-sx * arm, 0));
			br.lineTo(c);
			br.lineTo(c + QPointF(0, -sy * arm));
		}
		p.setPen(QPen(mix_white(argb, 0.65), 3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
		p.drawPath(br);
		if (railY >= 0) {
			QColor dim = accent;
			dim.setAlphaF(0.24f);
			p.setPen(Qt::NoPen);
			p.setBrush(dim);
			p.drawRect(QRectF(r.x(), r.y() + railY, r.width(), 4));
		}
	});
}

/* The progress line itself, lit, cut off at how far the replay has got (see fx_tick). */
std::string cine_progress(uint32_t argb, int w, int pad)
{
	return cine_picture("replay-cine-progress.png", key_of(argb, std::to_string(w)), QSize(w + pad * 2, 4 + pad * 2),
			    [&](QImage &img) {
				    const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
				    const QRectF bar(pad, pad, w, 4);
				    QLinearGradient g(bar.left(), 0, bar.right(), 0);
				    g.setColorAt(0, accent);
				    g.setColorAt(1, QColor(255, 255, 255));
				    QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
				    shape.fill(Qt::transparent);
				    {
					    QPainter sp(&shape);
					    sp.fillRect(bar, Qt::white);
				    }
				    paint_glow(img, shape, accent, 6.0f, 1.0, 0);
				    QPainter p(&img);
				    p.fillRect(bar, g);
			    });
}

/* The two rules either side of INSTANT, each fading out to a small diamond. */
std::string cine_rules(uint32_t argb)
{
	const int inner = 162, len = 162, pad = 14;
	const int w = (inner + len) * 2 + pad * 2, h = 28;
	return cine_picture("replay-cine-rules.png", key_of(argb), QSize(w, h), [&](QImage &img) {
		const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
		const qreal cx = w / 2.0, cy = h / 2.0;
		QImage shape(img.size(), QImage::Format_ARGB32_Premultiplied);
		shape.fill(Qt::transparent);
		const auto draw = [&](QPainter &p, bool lit) {
			p.setRenderHint(QPainter::Antialiasing);
			for (int side : {-1, 1}) {
				const qreal a = cx + side * inner, b = cx + side * (inner + len);
				QLinearGradient g(a, 0, b, 0);
				QColor near = lit ? mix_white(argb, 0.7) : QColor(255, 255, 255), far = near;
				far.setAlpha(0);
				g.setColorAt(0, near);
				g.setColorAt(1, far);
				p.fillRect(QRectF(std::min(a, b), cy - 1, len, 2), g);
				QPolygonF d;
				d << QPointF(b, cy - 6) << QPointF(b + 6, cy) << QPointF(b, cy + 6) << QPointF(b - 6, cy);
				p.setPen(Qt::NoPen);
				p.setBrush(lit ? mix_white(argb, 0.55) : QColor(255, 255, 255));
				p.drawPolygon(d);
			}
		};
		{
			QPainter sp(&shape);
			draw(sp, false);
		}
		paint_glow(img, shape, accent, 2.5f, 0.6, 0);
		QPainter p(&img);
		draw(p, true);
	});
}

/* Where the title goes: centred, in the first spot down from the top of every app's clear
   area that covers no layer, then one that covers only the game, then the top of that area.
   In a Replay Window, across the top of the window, smaller if the window is narrow. */
void cine_card(const ReplayLook &look, qreal &cx, qreal &top, float &scale)
{
	if (look.window) {
		scale = (float)std::min<qreal>(1.0, look.box.width() * 0.86 / CINE_CARD_W);
		cx = look.box.center().x();
		top = look.box.y() + 26 * scale;
		return;
	}
	scale = 1.0f;
	cx = CANVAS_W / 2.0;
	top = 300;
	SvLayout l;
	std::string err;
	const std::string json = sv_link_showing();
	if (json.empty() || !sv_layout_parse(json.c_str(), l, err))
		return;
	for (int pass = 0; pass < 2; pass++)
		for (qreal y = 300; y + CINE_CARD_H <= 1180; y += 8) {
			const QRectF card(cx - CINE_CARD_W / 2, y, CINE_CARD_W, CINE_CARD_H);
			bool clear = true;
			for (const SvPiece &p : l.pieces) {
				if (pass == 1 && p.src == SvSrc::Game)
					continue;
				if (card.intersects(QRectF(p.box.x, p.box.y, p.box.w, p.box.h))) {
					clear = false;
					break;
				}
			}
			if (clear) {
				top = y;
				return;
			}
		}
}

/* The frame goes round the layout's biggest game layer, just outside it; in a Replay Window
   round the window; with no game layer, round the phone. */
QRectF cine_frame_rect(const ReplayLook &look)
{
	if (look.window)
		return look.box.adjusted(10, 10, -10, -10);
	QRectF fr(27, 27, CANVAS_W - 54, CANVAS_H - 54);
	SvLayout l;
	std::string err;
	const std::string json = sv_link_showing();
	if (json.empty() || !sv_layout_parse(json.c_str(), l, err))
		return fr;
	const SvPiece *game = nullptr;
	for (const SvPiece &p : l.pieces)
		if (p.src == SvSrc::Game && (!game || p.box.w * p.box.h > game->box.w * game->box.h))
			game = &p;
	if (!game || game->box.w * game->box.h < CANVAS_W * CANVAS_H * 0.12)
		return fr;
	const QRectF g = QRectF(game->box.x, game->box.y, game->box.w, game->box.h).adjusted(-15, -15, 15, 15);
	return g.intersected(fr);
}

CineItem cine_add(obs_scene_t *scene, const std::string &pic, QPointF at, float base, bool screen)
{
	CineItem c;
	if (pic.empty())
		return c;
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "file", pic.c_str());
	c.src = obs_source_create_private("image_source", "Simply Vertical replay look", d);
	obs_data_release(d);
	if (!c.src)
		return c;
	c.item = obs_scene_add(scene, c.src);
	obs_sceneitem_set_alignment(c.item, OBS_ALIGN_CENTER);
	if (screen)
		obs_sceneitem_set_blending_mode(c.item, OBS_BLEND_SCREEN);
	obs_sceneitem_set_visible(c.item, false);
	c.fade = add_fade(c.src, 1.0);
	c.at = at;
	c.base = base;
	return c;
}

CineItem cine_half(obs_scene_t *scene, float w, float h, QPointF at)
{
	CineItem c;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "color", 0xff000000);
	obs_data_set_int(d, "width", (int)std::ceil(w));
	obs_data_set_int(d, "height", (int)std::ceil(h));
	c.src = obs_source_create_private("color_source_v3", "Simply Vertical replay shutter", d);
	obs_data_release(d);
	if (!c.src)
		return c;
	c.item = obs_scene_add(scene, c.src);
	obs_sceneitem_set_alignment(c.item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_visible(c.item, false);
	c.at = at;
	return c;
}

/* Puts every piece of the look where it is right now, from the times in g_cine. */
void cine_draw()
{
	Cine &c = g_cine;

	/* The shutter, the line of light and the bloom. */
	double close = 0, lineOp = 0, lineSx = 0, bloomOp = 0, bloomSc = 0.3;
	if (c.shutAt) {
		const double w = cine_ms(c.shutAt);
		close = bez(0.7, 0, 0.84, 0, ramp(w, 0, CINE_CLOSE_MS));
		const double lk = ease_css(ramp(w, CINE_LINE_FROM, CINE_LINE_TO));
		lineOp = lk;
		lineSx = lk;
		const double bk = ease_css(ramp(w, CINE_BLOOM_FROM, CINE_BLOOM_TO));
		bloomOp = bk;
		bloomSc = 0.3 + 0.7 * bk;
		if (c.openAt) {
			const double t = cine_ms(c.openAt);
			close = 1.0 - bez(0.76, 0, 0.24, 1, ramp(t, 0, CINE_OPEN_MS));
			lineSx = 1.0 + 0.25 * ramp(t, -105, 630);
			lineOp = 1.0 - ease_css(ramp(t, 70, 630));
			const double bo = ease_css(ramp(t, 0, 700));
			bloomOp = 1.0 - bo;
			bloomSc = 1.0 + 0.7 * bo;
		}
	}
	const double shift = (1.0 - close) * (c.halfH + 6);
	cine_put(c.top, close > 0.0005 ? 1.0 : 0.0, 1, 1, 0, -shift);
	cine_put(c.bottom, close > 0.0005 ? 1.0 : 0.0, 1, 1, 0, shift);
	/* The halves' lit edges, so they read over dark parts of the picture; the line takes over as they meet. */
	const double edge = close > 0.0005 ? 0.9 * (1.0 - lineOp) : 0.0;
	cine_put(c.topEdge, edge, 1, 1, 0, -shift);
	cine_put(c.bottomEdge, edge, 1, 1, 0, shift);
	cine_put(c.line, lineOp, lineSx, 1);
	cine_put(c.bloom, bloomOp, bloomSc, bloomSc);

	/* The pop up. */
	if (c.popAt) {
		const double t = cine_ms(c.popAt);
		const double fade = c.outAt ? 1.0 - ramp(cine_ms(c.outAt), 0, CINE_POP_OUT_MS) : 1.0;
		const double fk = ease_settle(ramp(t, 490, 910));
		cine_put(c.frame, fk * fade, 1.035 - 0.035 * fk, 1.035 - 0.035 * fk);
		const double kk = ease_settle(ramp(t, 560, 875));
		cine_put(c.kick, kk * fade, 1.3 - 0.3 * kk, 1);
		const double rk = ease_settle(ramp(t, 630, 980));
		cine_put(c.rules, rk * fade, rk, 1);
		double flOp, flSx;
		if (t < 805) {
			const double k = ease_css(ramp(t, 630, 805));
			flOp = k;
			flSx = 1.35 * k;
		} else {
			const double k = ease_css(ramp(t, 805, 1085));
			flOp = 1.0 - 0.15 * k;
			flSx = 1.35 - 0.35 * k;
		}
		const double drift = std::clamp((t - 1085) / 2900.0, 0.0, 1.0) * 54.0 * c.flare.base;
		cine_put(c.flare, flOp * fade, flSx, 1, drift, 0);
		const double wk = ease_settle(ramp(t, 630, 1015));
		cine_put(c.word, ramp(t, 630, 805) * fade, 1.1 - 0.1 * wk, 1.1 - 0.1 * wk, 0, c.wordDrop * (1.0 - wk));
		const double prog = ramp(t, 560, 840) * fade;
		if (g_lineFade && std::fabs(prog - c.progShown) > 0.002) {
			set_opacity(g_lineFade, prog);
			c.progShown = prog;
		}
	}

	/* The slow motion moment: a streak of light across the game, with a soft haze behind it. */
	if (c.momentAt) {
		const double m = cine_ms(c.momentAt);
		const double sk = bez(0.3, 0, 0.2, 1, ramp(m, 0, CINE_STREAK_MS));
		const double so = m < 0 ? 0.0 : (m < 90 ? m / 90.0 : 1.0 - ease_css(ramp(m, CINE_STREAK_MS * 0.55, CINE_STREAK_MS)));
		const double span = 1.15 * c.boxW;
		cine_put(c.streak, so, 0.6 + 0.4 * sk, 1, -span + 2 * span * sk, 0);
		const double ho = m < 320 ? ease_css(ramp(m, 40, 320)) : 1.0 - ease_css(ramp(m, 320, 1200));
		cine_put(c.haze, ho, 0.3 + 1.2 * ease_css(ramp(m, 40, 1200)), 1);
	}
}

/* Builds the look into the replay's scene, on top of the clip. `prog` adds the progress line. */
void cine_build(obs_scene_t *scene, const ReplayLook &look, bool popup, bool prog)
{
	Cine &c = g_cine;
	c = Cine{};
	const uint32_t argb = look.colour;
	const QColor accent((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
	const QRectF box = look.box;
	const float k = (float)(box.width() / CANVAS_W); /* the full width pieces, in a Replay Window */
	c.boxW = (float)box.width();
	const QRectF fr = cine_frame_rect(look);

	if (popup) {
		/* The frame, with the progress line's rail under it, or just inside its bottom when
		   there is no room below. */
		qreal railY = -1;
		if (prog) {
			railY = fr.height() + 34;
			if (fr.bottom() + 38 > box.bottom() - 12)
				railY = fr.height() - 26;
		}
		c.frame = cine_add(scene, cine_frame(argb, fr.size(), railY), fr.center(), 1.0f, false);
		if (prog) {
			const int pad = 16;
			const std::string pic = cine_progress(argb, (int)std::lround(fr.width()), pad);
			if (!pic.empty()) {
				obs_data_t *ls = obs_data_create();
				obs_data_set_string(ls, "file", pic.c_str());
				g_line = obs_source_create_private("image_source", "Simply Vertical replay line", ls);
				obs_data_release(ls);
				if (g_line) {
					g_lineW = (int)std::lround(fr.width());
					g_linePad = pad;
					g_lineItem = obs_scene_add(scene, g_line);
					vec2 pos;
					vec2_set(&pos, (float)fr.x() - pad, (float)(fr.y() + railY) - pad);
					obs_sceneitem_set_pos(g_lineItem, &pos);
					obs_sceneitem_crop crop = {};
					crop.right = g_lineW + pad * 2 - 1;
					obs_sceneitem_set_crop(g_lineItem, &crop);
					g_lineFade = add_fade(g_line, 0.0);
				}
			}
		}
	}

	/* The moment's streak and haze, across the middle of the game, under the title. */
	const QPointF mid(box.center().x(), fr.center().y());
	c.haze = cine_add(scene, cine_glow("replay-cine-haze.png", argb, 1512, 420, false), mid, k, true);
	{
		/* A comet: a long tail in the colour, a white hot head, racing left to right. */
		QColor tail = accent, none = accent;
		tail.setAlphaF(0.85f);
		none.setAlpha(0);
		c.streak = cine_add(scene,
				    cine_bar("replay-cine-streak.png", argb, 1400, 140, 8,
					     {{0.0, none}, {0.55, tail}, {0.86, mix_white(argb, 0.4)}, {0.93, Qt::white}, {1.0, none}},
					     18.0f, 1.6, true),
				    mid, k, true);
	}

	if (popup) {
		qreal cx, top;
		float s;
		cine_card(look, cx, top, s);
		/* A title over the top of the game starts inside the frame, never on its line. */
		if (!look.window && top < fr.y() + 24 && top + CINE_CARD_H > fr.y() - 24)
			top = fr.y() + 24;
		QFont kf("Archivo");
		kf.setWeight(QFont::Bold);
		kf.setPixelSize(36);
		kf.setLetterSpacing(QFont::AbsoluteSpacing, 15);
		kf.setStyleStrategy(QFont::PreferAntialias);
		c.rules = cine_add(scene, cine_rules(argb), QPointF(cx, top + 18 * s), s, false);
		c.kick = cine_add(scene, cine_text("replay-cine-kick.png", QString::fromUtf8(T("ReplayKicker")), kf, mix_white(argb, 0.5), argb, false),
				  QPointF(cx, top + 18 * s), s, false);
		QColor none = accent;
		none.setAlpha(0);
		c.flare = cine_add(scene,
				   cine_bar("replay-cine-flare.png", argb, 1080, 60, 4,
					    {{0.0, none}, {0.04, none}, {0.34, accent}, {0.5, Qt::white}, {0.66, accent}, {0.96, none}, {1.0, none}},
					    9.0f, 1.2, false),
				   QPointF(box.center().x(), top + 123 * s), k, true);
		QFont wf("Archivo");
		wf.setWeight(QFont::Black);
		wf.setPixelSize(140);
		wf.setLetterSpacing(QFont::AbsoluteSpacing, 4);
		wf.setStyleStrategy(QFont::PreferAntialias);
		c.word = cine_add(scene, cine_text("replay-cine-word.png", QString::fromUtf8(T("ReplayTag")), wf, Qt::white, argb, true),
				  QPointF(cx, top + 124 * s), s, false);
		c.wordDrop = 22 * s;
	}

	/* The shutter, the line of light and the bloom, on top of it all. */
	c.halfH = (float)box.height() / 2.0f + 6.0f;
	const float w = (float)box.width() + 8.0f;
	c.top = cine_half(scene, w, c.halfH, QPointF(box.center().x(), box.y() - 4 + c.halfH / 2.0));
	c.bottom = cine_half(scene, w, c.halfH, QPointF(box.center().x(), box.bottom() + 4 - c.halfH / 2.0));
	QColor none = accent;
	none.setAlpha(0);
	const std::string edgePic = cine_bar("replay-cine-edge.png", argb, 1080, 48, 3,
					     {{0.0, none}, {0.2, accent}, {0.5, mix_white(argb, 0.5)}, {0.8, accent}, {1.0, none}}, 7.0f,
					     1.0, false);
	c.topEdge = cine_add(scene, edgePic, QPointF(box.center().x(), box.y() - 4 + c.halfH - 2), k, false);
	c.bottomEdge = cine_add(scene, edgePic, QPointF(box.center().x(), box.bottom() + 4 - c.halfH + 2), k, false);
	c.line = cine_add(scene,
			  cine_bar("replay-cine-line.png", argb, 1080, 90, 6,
				   {{0.0, none}, {0.25, accent}, {0.5, Qt::white}, {0.75, accent}, {1.0, none}}, 12.0f, 1.4, false),
			  box.center(), k, false);
	c.bloom = cine_add(scene, cine_glow("replay-cine-bloom.png", argb, 864, 864, true), box.center(), k, true);
}

void cine_release_all()
{
	Cine &c = g_cine;
	for (CineItem *i : {&c.top, &c.bottom, &c.topEdge, &c.bottomEdge, &c.line, &c.bloom, &c.frame, &c.kick, &c.rules, &c.flare, &c.word,
			    &c.streak, &c.haze})
		cine_release(*i);
	c = Cine{};
}

obs_source_t *shown_player();

/* The game zoom magnifies whichever player is on screen. */
void lens_follow()
{
	if (!g_lens)
		return;
	obs_source_t *want = shown_player();
	obs_source_t *old = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_viewMtx);
		if (g_lensShow == want)
			return;
		old = g_lensShow;
		g_lensShow = want ? obs_source_get_ref(want) : nullptr;
	}
	obs_source_release(old);
}

/* The push in: the clip grows round the middle of the game, never uncovering an edge. */
void apply_zoom(float z)
{
	if (g_lens) {
		/* Keep my facecam live: only the game grows, inside its own box, so the
		   recorded facecam never peeks out round the live one. */
		const QRectF b = g_lensBox;
		const QPointF f = g_focus;
		const QRectF r(f.x() - (f.x() - b.x()) / z, f.y() - (f.y() - b.y()) / z, b.width() / z, b.height() / z);
		{
			std::lock_guard<std::mutex> lock(g_viewMtx);
			g_lensRegion = r;
		}
		lens_follow();
		const bool on = z > 1.0005f && g_run.item && (obs_sceneitem_visible(g_run.item) ||
							       (g_slow.item && obs_sceneitem_visible(g_slow.item)));
		obs_sceneitem_set_visible(g_lensItem, on);
		g_zoomNow = z;
		return;
	}
	for (Player *p : {&g_run, &g_slow, &g_spare}) {
		if (!p->item)
			continue;
		vec2 box, pos;
		vec2_set(&box, CANVAS_W * z, CANVAS_H * z);
		vec2_set(&pos, (float)g_focus.x() * (1.0f - z), (float)g_focus.y() * (1.0f - z));
		obs_sceneitem_set_bounds(p->item, &box);
		obs_sceneitem_set_pos(p->item, &pos);
	}
	g_zoomNow = z;
}

/* The middle of the layout's biggest game layer, which is where the moment is. */
QPointF game_focus()
{
	SvLayout l;
	std::string err;
	const std::string json = sv_link_showing();
	if (json.empty() || !sv_layout_parse(json.c_str(), l, err))
		return QPointF(CANVAS_W / 2.0, CANVAS_H / 2.0);
	float best = 0;
	QPointF at(CANVAS_W / 2.0, CANVAS_H / 2.0);
	for (const SvPiece &p : l.pieces)
		if (p.src == SvSrc::Game && p.box.w * p.box.h > best) {
			best = p.box.w * p.box.h;
			at = QPointF(std::clamp(p.box.x + p.box.w / 2.0, 0.0, (double)CANVAS_W),
				     std::clamp(p.box.y + p.box.h / 2.0, 0.0, (double)CANVAS_H));
		}
	return at;
}

obs_source_t *make_player(const std::string &path, int speed, const char *name)
{
	obs_data_t *st = obs_data_create();
	obs_data_set_bool(st, "is_local_file", true);
	obs_data_set_string(st, "local_file", path.c_str());
	obs_data_set_bool(st, "looping", false);
	obs_data_set_bool(st, "restart_on_activate", false);
	obs_data_set_bool(st, "close_when_inactive", false);
	obs_data_set_bool(st, "clear_on_media_end", false); /* holds the last picture for the fade */
	obs_data_set_bool(st, "hw_decode", true);
	obs_data_set_int(st, "speed_percent", std::clamp(speed, 10, 200));
	obs_source_t *media = obs_source_create_private("ffmpeg_source", name, st);
	obs_data_release(st);
	return media;
}

/* A seek lands on the full picture at or before the time asked for. Asking a hair past the
   picture we want makes sure it lands on that one and not the one before. */
int64_t seek_ms_for(int64_t keyUs)
{
	return (keyUs + 5000) / 1000;
}

/* The full picture at or before `us`, or `us` itself when the clip's pictures are unknown. */
int64_t key_at_or_before(int64_t us)
{
	if (!g_timesOk)
		return us;
	int64_t best = g_times.keysUs.front();
	for (int64_t k : g_times.keysUs)
		if (k <= us)
			best = k;
	return best;
}

/* True when a player that should wait on its picture is not there. OBS's media player
   switches itself back to playing when it finishes opening, which undoes a pause sent
   before that, and the source still says "paused". So its clock is checked as well. */
bool off_picture(obs_source_t *src, int64_t atUs)
{
	if (!src)
		return false;
	const obs_media_state st = obs_source_media_get_state(src);
	if (st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_ENDED)
		return true;
	return std::llabs(obs_source_media_get_time(src) - atUs / 1000) > 250;
}

/* Puts a player that wandered back on its picture, paused. A jump takes a moment to land,
   so a player is not moved again straight after. Returns true when it moved it. */
/* Pauses a player and jumps it to a picture. */
void park(Player &p, int64_t atUs)
{
	if (!p.src)
		return;
	p.heldAt = os_gettime_ns();
	if (obs_source_media_get_state(p.src) == OBS_MEDIA_STATE_ENDED)
		obs_source_media_restart(p.src); /* an ended player ignores pause and jumps */
	obs_source_media_play_pause(p.src, true);
	obs_source_media_set_time(p.src, seek_ms_for(atUs));
}

bool hold_player(Player &p, int64_t atUs)
{
	if (!p.src || !off_picture(p.src, atUs))
		return false;
	if (p.heldAt && (os_gettime_ns() - p.heldAt) / 1000000 < 400)
		return false;
	park(p, atUs);
	return true;
}

/* Works out where the replay starts and where it slows down, now that the clip is open. */
void plan(int64_t durMs)
{
	g_durUs = durMs * 1000;
	/* The press sits `after` seconds from the end. When the moment slows down, the replay
	   ends as many seconds after the press as the slow part starts before it, so a clip
	   that kept going long after the press never plays all of that in slow motion. */
	const int64_t pressUs = std::max<int64_t>(0, g_durUs - (int64_t)g_after * 1000000);
	g_endUs = g_durUs;
	if (g_slow.src)
		g_endUs = std::min(g_durUs, pressUs + (int64_t)g_slowSecs * 1000000);
	int64_t startUs = 0;
	if (g_wantMs > 0 && g_endUs > (int64_t)g_wantMs * 1000 + 500000)
		startUs = g_endUs - (int64_t)g_wantMs * 1000;
	g_fromUs = key_at_or_before(startUs + 1000);
	g_slowUs = -1;
	g_runPct = g_slow.src || g_press ? 100 : g_slowPct;

	if (g_slow.src && g_gate && g_timesOk) {
		/* The slow part starts a few seconds before the press, on the nearest full picture
		   that leaves a run up and some slow part. */
		const int64_t wantUs = pressUs - (int64_t)g_slowSecs * 1000000;
		int64_t best = -1;
		for (int64_t k : g_times.keysUs) {
			if (k < g_fromUs + MIN_RUN_US || k > g_endUs - 500000)
				continue;
			if (best < 0 || std::llabs(k - wantUs) < std::llabs(best - wantUs))
				best = k;
		}
		if (best >= 0)
			g_slowUs = best;
	}
	if (g_slow.src && g_slowUs < 0) {
		/* No room for a run up (or no slow motion filter): the slow player plays it all
		   and the normal speed one stays hidden. */
		std::swap(g_run, g_slow);
		g_runPct = g_slowPct;
		obs_log(LOG_INFO, "replay: no room for a run up, so all of it plays slow");
	}

	obs_source_media_play_pause(g_run.src, true);
	obs_source_media_set_time(g_run.src, seek_ms_for(g_fromUs));
	if (g_slowUs >= 0 && g_slow.src) {
		obs_source_media_play_pause(g_slow.src, true);
		obs_source_media_set_time(g_slow.src, seek_ms_for(g_slowUs));
	}
	obs_log(LOG_INFO, "replay plan: clip %lld ms, press %lld ms, from %lld ms to %lld ms, slow %s at %d%% (pictures %s)",
		(long long)durMs, (long long)(pressUs / 1000), (long long)(g_fromUs / 1000), (long long)(g_endUs / 1000),
		g_slowUs >= 0 ? (std::to_string(g_slowUs / 1000) + " ms").c_str() : (g_runPct < 100 ? "all" : "none"),
		g_slowUs >= 0 ? g_slowPct : g_runPct, g_timesOk ? "read" : "unknown");
}

/* How long the replay takes to play from a point in the clip to its end, in ms of real time. */
int left_ms_from(int64_t atUs)
{
	const int64_t endUs = g_endUs > 0 ? g_endUs : 1000000;
	atUs = std::clamp<int64_t>(atUs, 0, endUs);
	if (g_slowUs < 0) {
		const int pct = std::clamp(g_runPct, 10, 200);
		/* Pressed slow motion goes back to normal speed on its picture. */
		if (g_press && pct < 100 && g_pressBackUs > atUs && g_pressBackUs < endUs)
			return (int)((g_pressBackUs - atUs) / 1000 * 100 / pct + (endUs - g_pressBackUs) / 1000);
		return (int)((endUs - atUs) / 1000 * 100 / pct);
	}
	if (atUs < g_slowUs)
		return (int)((g_slowUs - atUs) / 1000 + (endUs - g_slowUs) / 1000 * 100 / std::clamp(g_slowPct, 10, 200));
	return (int)((endUs - atUs) / 1000 * 100 / std::clamp(g_slowPct, 10, 200));
}

int play_left_ms()
{
	return left_ms_from(g_fromUs);
}

/* A replay that ends before the clip does fades out while still moving, exactly when it
   gets there. One that plays to the end waits for the player to say so, with a little
   spare in case it never does. */
void begin_out(uint64_t token);

bool ends_early()
{
	return g_endUs > 0 && g_endUs < g_durUs - 100000;
}

void arm_end(int ms)
{
	const uint64_t token = g_token;
	g_guard->disconnect();
	QObject::connect(g_guard, &QTimer::timeout, [token] { begin_out(token); });
	g_guard->start(std::max(200, ms) + (ends_early() ? 0 : 3000));
}

/* The slow player takes over on the picture the run up stopped on. */
void swap_to_slow()
{
	if (g_swapped || !g_slow.src)
		return;
	g_swapped = true;
	if (g_trans == TRANS_CINE && g_scene)
		g_cine.momentAt = os_gettime_ns();
	obs_source_media_play_pause(g_slow.src, false);
	if (g_run.item)
		obs_sceneitem_set_visible(g_run.item, false);
	obs_source_media_play_pause(g_run.src, true);
	arm_end((int)((g_endUs - g_slowUs) / 1000 * 100 / std::clamp(g_slowPct, 10, 200)));
	if (const Gate *g = gate_of(g_gate))
		obs_log(LOG_INFO, "replay slowed at %lld ms (wanted %lld ms)", (long long)(g->atNs.load() / 1000000),
			(long long)(g_slowUs / 1000));
}

void spare_tick();
void land_tick();
void continue_row();
void hand_over();
void start_fx();
void clear_display(const std::string &canvas);

void tick()
{
	if (!g_scene || !g_run.src)
		return;
	const double t = since_ms(g_phaseAt);
	switch (g_phase) {
	case Phase::Opening: {
		/* Wait for the clip to open, jump to its last seconds, let the picture catch up. */
		const int64_t dur = obs_source_media_get_duration(g_run.src);
		if (!g_seekAt && dur > 0) {
			plan(dur);
			g_seekAt = os_gettime_ns();
		}
		/* Both players wait on their pictures until the replay starts. One that opened late
		   and started by itself is held again, and the pictures get time to settle. */
		if (g_seekAt) {
			bool held = hold_player(g_run, g_fromUs);
			held = hold_player(g_slow, g_slowUs >= 0 ? g_slowUs : g_fromUs) || held;
			if (held) {
				g_seekAt = os_gettime_ns();
				obs_log(LOG_INFO, "replay: a player started before its turn, held it");
			}
		}
		/* The wipe: the bands come on while the clip opens, and cover the phone until it has. */
		if (g_trans == TRANS_WIPE && !g_covered) {
			const double w = since_ms(g_wipeAt);
			place_bands(0.5f * cover_k(w), g_wipeDir);
			if (w >= COVER_MS) {
				g_covered = true;
				set_clip_x(0);
			}
		}
		/* Cinematic: the shutter closes while the clip opens. */
		if (g_trans == TRANS_CINE) {
			if (!g_covered && cine_ms(g_cine.shutAt) >= CINE_SHUT_MS) {
				g_covered = true;
				set_clip_x(0);
			}
			cine_draw();
		}
		const bool ready = g_seekAt && since_ms(g_seekAt) >= SETTLE_MS && obs_source_get_width(g_run.src) > 0 &&
				   (g_slowUs < 0 || obs_source_get_width(g_slow.src) > 0);
		if ((ready || t >= READY_MS) && ((g_trans != TRANS_WIPE && g_trans != TRANS_CINE) || g_covered)) {
			if (!g_seekAt) /* never opened in time: play whatever it has */
				plan(0);
			/* One player does it all: the second one is not needed. */
			if (g_slowUs < 0 && g_slow.item) {
				obs_sceneitem_set_visible(g_slow.item, false);
				obs_source_media_play_pause(g_slow.src, true);
			}
			if (g_slowUs >= 0) {
				if (Gate *g = gate_of(g_gate)) {
					g->fromNs.store(g_fromUs * 1000);
					g->stopNs.store(g_slowUs * 1000 - g_times.frameUs * 500);
					g->armed.store(true, std::memory_order_release);
				}
			}
			obs_source_media_play_pause(g_run.src, false);
			g_phase = Phase::In;
			g_phaseAt = os_gettime_ns();
			g_inMs = g_trans == TRANS_WIPE ? REVEAL_MS : IN_MS;
			if (g_trans == TRANS_WIPE) {
				set_clip_x(0);
				frame_opacity(1.0); /* under the bands, uncovered with the clip */
			} else if (g_trans == TRANS_FADE) {
				set_clip_x(0);
			} else if (g_trans == TRANS_CINE) {
				set_clip_x(0);
				g_inMs = CINE_OPEN_MS;
				g_cine.openAt = g_cine.popAt = g_phaseAt;
				/* With the whole replay slow, the streak comes once the pop up is in. */
				if (g_slowUs < 0 && g_runPct < 100)
					g_cine.momentAt = g_phaseAt + (uint64_t)CINE_WHOLE_MOMENT_MS * 1000000ULL;
				cine_draw();
			}
		}
		break;
	}
	case Phase::In: {
		const double k = std::clamp(t / g_inMs, 0.0, 1.0);
		if (g_trans == TRANS_CINE) {
			cine_draw();
		} else if (g_trans == TRANS_WIPE) {
			place_bands(0.5f + 0.5f * reveal_k(t), g_wipeDir);
		} else if (g_trans == TRANS_FADE) {
			const double a = sv_ease(SV_FEEL_SMOOTH, (float)k);
			clip_opacity(a);
			frame_opacity(a);
		} else {
			set_clip_x((float)CANVAS_W * (1.0f - sv_ease(SV_FEEL_SNAPPY, (float)k)));
			frame_opacity(std::clamp((t - IN_MS * 0.35) / (IN_MS * 0.65), 0.0, 1.0));
		}
		if (t >= g_inMs) {
			if (g_trans == TRANS_WIPE)
				place_bands(1.0f, g_wipeDir);
			set_clip_x(0);
			clip_opacity(1.0);
			if (g_trans != TRANS_CINE)
				frame_opacity(1.0);
			g_phase = Phase::Playing;
			start_fx();
			/* With one player nothing moves while it plays. With two, the tick keeps
			   watching for the moment the slow player takes over. */
			if (g_slowUs < 0)
				g_tick->stop();
			/* Coming on took this much of the replay already. */
			arm_end(play_left_ms() - g_inMs);
		}
		break;
	}
	case Phase::Playing: {
		if (g_spare.src) {
			spare_tick();
			break;
		}
		if (g_landing) {
			land_tick();
			break;
		}
		if (g_swapped || g_slowUs < 0) {
			g_tick->stop();
			break;
		}
		/* The slow player keeps waiting on its picture until it takes over. */
		if (hold_player(g_slow, g_slowUs))
			obs_log(LOG_INFO, "replay: the slow player started before its turn, held it");
		if (g_paused)
			break;
		const Gate *g = gate_of(g_gate);
		if (!g)
			break;
		/* The run up's sound fades out just before the slow part, which plays silent. */
		if (g_sound) {
			const int64_t at = g->atNs.load();
			if (at >= 0) {
				const double left = (double)(g_slowUs * 1000 - at) / 1e6;
				obs_source_set_volume(g_run.src, (float)std::clamp(left / VOICE_FADE_MS, 0.0, 1.0));
			}
		}
		if (g->frozen.load()) {
			if (!g_frozenAt)
				g_frozenAt = os_gettime_ns();
			/* The run up holds its last picture while the slow player lands on the same
			   one, if it had wandered. */
			const double wait = g_speedAt && since_ms(g_speedAt) < 3000 ? 1500 : 600;
			if (off_picture(g_slow.src, g_slowUs) && since_ms(g_frozenAt) < wait)
				break;
			swap_to_slow();
			g_tick->stop();
		}
		break;
	}
	case Phase::Out: {
		if (g_trans == TRANS_CINE) {
			/* The pop up fades, the shutter closes over the clip; in a row the next clip
			   starts under it, otherwise it opens on the live layout. */
			const double s = t - CINE_OUT_LEAD;
			if (!g_cine.hid && s >= CINE_CLOSE_MS) {
				g_cine.hid = true;
				show_clip_items(false);
				frame_opacity(0.0);
			}
			if (!g_covered && s >= CINE_SHUT_MS) {
				g_covered = true;
				if (!g_row.empty()) {
					hand_over();
					return;
				}
				g_cine.openAt = os_gettime_ns();
			}
			cine_draw();
			if (g_covered && cine_ms(g_cine.openAt) >= CINE_OPEN_MS) {
				sv_replay_stop();
				continue_row();
			}
			break;
		}
		if (g_trans == TRANS_WIPE) {
			/* The bands come back the other way; once they cover the phone the next clip of a
			   row takes over under them, or the live layout is uncovered. */
			if (!g_covered) {
				place_bands(0.5f * cover_k(t), g_wipeDir);
				if (t >= COVER_MS) {
					g_covered = true;
					if (!g_row.empty()) {
						hand_over();
						return;
					}
					show_clip_items(false);
					frame_opacity(0.0);
				}
			} else {
				place_bands(0.5f + 0.5f * reveal_k(t - COVER_MS), g_wipeDir);
				if (t >= COVER_MS + REVEAL_MS) {
					sv_replay_stop();
					continue_row();
				}
			}
			break;
		}
		const double a = 1.0 - sv_ease(SV_FEEL_SMOOTH, (float)(t / OUT_MS));
		clip_opacity(a);
		frame_opacity(a);
		if (t >= OUT_MS) {
			sv_replay_stop();
			continue_row();
		}
		break;
	}
	default:
		g_tick->stop();
		break;
	}
}

void ensure_timers()
{
	if (!g_tick) {
		g_tick = new QTimer();
		g_tick->setInterval(TICK_MS);
		g_tick->setTimerType(Qt::PreciseTimer);
		QObject::connect(g_tick, &QTimer::timeout, [] { tick(); });
	}
	if (!g_guard) {
		g_guard = new QTimer();
		g_guard->setSingleShot(true);
	}
}

/* Fades back to the live layout, then ends. Safe to call twice. */
void begin_out(uint64_t token)
{
	if (token != g_token || !g_scene || g_phase == Phase::Out)
		return;
	if (g_phase == Phase::Opening) { /* never shown: just end */
		sv_replay_stop();
		continue_row();
		return;
	}
	if (g_phase == Phase::In && g_trans != TRANS_WIPE)
		set_clip_x(0);
	g_phase = Phase::Out;
	g_phaseAt = os_gettime_ns();
	if (g_trans == TRANS_CINE) {
		if (g_phaseAt > 0 && !g_cine.popAt)
			g_cine.popAt = g_phaseAt;
		g_cine.outAt = g_phaseAt;
		g_cine.shutAt = g_phaseAt + (uint64_t)CINE_OUT_LEAD * 1000000ULL;
		g_cine.openAt = 0;
		g_cine.hid = false;
	}
	/* The bands leave the way they came. */
	g_covered = false;
	g_wipeDir = 1;
	g_guard->stop();
	g_tick->start();
}

/* The player on screen right now. A player that is not on screen never ends the replay. */
obs_source_t *shown_player()
{
	return g_slowUs >= 0 && g_swapped ? g_slow.src : g_run.src;
}

/* The last picture holds a moment, then fades to the live layout. */
void hold_then_out(uint64_t token)
{
	const uint64_t moves = g_moves;
	QTimer::singleShot(HOLD_MS, qApp, [token, moves] {
		if (moves == g_moves) /* a control since then (back, restart) keeps it playing */
			begin_out(token);
	});
}

/* The player showing the end of the clip got there. */
void on_last_ended(void *param, calldata_t *cd)
{
	const uint64_t token = (uint64_t)(uintptr_t)param;
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!qApp)
		return;
	QMetaObject::invokeMethod(
		qApp,
		[token, src] {
			if (token != g_token)
				return;
			if (src != shown_player()) {
				obs_log(LOG_INFO, "replay: a hidden player reached the end, ignored");
				return;
			}
			hold_then_out(token);
		},
		Qt::QueuedConnection);
}

/* The run up player reached the end of the clip. With one player that is the end. With
   two it should never happen (the gate stops it first), so the slow player takes over
   rather than leaving a still picture. */
void on_run_ended(void *param, calldata_t *cd)
{
	const uint64_t token = (uint64_t)(uintptr_t)param;
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!qApp)
		return;
	QMetaObject::invokeMethod(
		qApp,
		[token, src] {
			if (token != g_token)
				return;
			if (src != shown_player()) {
				obs_log(LOG_INFO, "replay: a hidden player reached the end, ignored");
				return;
			}
			if (g_slowUs < 0)
				hold_then_out(token);
			else
				swap_to_slow();
		},
		Qt::QueuedConnection);
}

void connect_ends(uint64_t token)
{
	signal_handler_connect(obs_source_get_signal_handler(g_run.src), "media_ended", on_run_ended,
			       (void *)(uintptr_t)token);
	if (g_slow.src)
		signal_handler_connect(obs_source_get_signal_handler(g_slow.src), "media_ended", on_last_ended,
				       (void *)(uintptr_t)token);
}

/* The players may have swapped roles, so both ways round are let go. */
void disconnect_ends(uint64_t token)
{
	for (Player *p : {&g_run, &g_slow}) {
		if (!p->src)
			continue;
		signal_handler_t *sh = obs_source_get_signal_handler(p->src);
		signal_handler_disconnect(sh, "media_ended", on_run_ended, (void *)(uintptr_t)token);
		signal_handler_disconnect(sh, "media_ended", on_last_ended, (void *)(uintptr_t)token);
	}
}

obs_sceneitem_t *add_clip_item(obs_scene_t *scene, obs_source_t *media)
{
	obs_sceneitem_t *item = obs_scene_add(scene, media);
	if (item) {
		vec2 box;
		vec2_set(&box, (float)CANVAS_W, (float)CANVAS_H);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds(item, &box);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	}
	return item;
}

void release_player(Player &p)
{
	if (p.fade && p.src) {
		obs_source_filter_remove(p.src, p.fade);
		obs_source_release(p.fade);
	}
	obs_source_release(p.src);
	p = Player{};
}


/* ---------- the controls while it plays ---------- */

/* The speed the Speed key shows and changes: the slow part's, or the whole replay's
   when one player plays it all. */
int &speed_now()
{
	return g_slowUs >= 0 ? g_slowPct : g_runPct;
}

int next_speed(int pct)
{
	return pct > 75 ? 75 : pct > 50 ? 50 : pct > 25 ? 25 : 100;
}

/* Where in the clip the picture on screen is. */
int64_t clip_now_us()
{
	if (g_landing)
		return g_landUs;
	if (g_slowUs >= 0 && !g_swapped)
		if (const Gate *g = gate_of(g_gate)) {
			const int64_t at = g->atNs.load();
			if (at >= 0)
				return at / 1000;
		}
	return obs_source_media_get_time(shown_player()) * 1000;
}

bool can_steer()
{
	return g_scene && g_phase == Phase::Playing && !g_spare.src;
}

/* Jumps the replay to a full picture at or before `targetUs`. With two players, a jump
   before the slow part plays the run up again and slows down again on the moment. */
bool open_spare(int pct, int64_t at, bool wait, int kind);

void jump_to(int64_t targetUs)
{
	g_moves++;
	g_paused = false;
	targetUs = std::max(g_fromUs, key_at_or_before(std::max<int64_t>(0, targetUs)));
	/* Pressed slow motion: a jump to before it started plays on at normal speed. */
	if (g_press && g_slowUs < 0 && g_runPct < 100 && targetUs < g_pressFromUs) {
		open_spare(100, targetUs, false, SPARE_SPEED);
		return;
	}
	const bool runUp = g_slowUs >= 0 && (targetUs < g_slowUs || !g_swapped);
	if (g_slowUs >= 0 && !g_swapped)
		targetUs = std::max(g_fromUs, std::min(targetUs, key_at_or_before(g_slowUs - 1)));
	if (runUp) {
		if (Gate *g = gate_of(g_gate))
			g->armed.store(false, std::memory_order_release);
		if (g_swapped) {
			g_swapped = false;
			if (g_run.item)
				obs_sceneitem_set_visible(g_run.item, true);
		}
		g_frozenAt = 0;
		park(g_slow, g_slowUs);
		park(g_run, targetUs);
		g_landing = &g_run;
	} else {
		Player &p = g_slowUs >= 0 ? g_slow : g_run;
		park(p, targetUs);
		g_landing = &p;
	}
	g_landUs = targetUs;
	g_landAt = os_gettime_ns();
	g_guard->stop();
	g_tick->start();
}

/* The jumped player is on its new picture: it plays on from there. */
void land()
{
	Player &p = *g_landing;
	const int64_t at = g_landUs;
	const bool runUp = g_landing == &g_run && g_slowUs >= 0;
	g_landing = nullptr;
	if (runUp) {
		if (Gate *g = gate_of(g_gate)) {
			g->fromNs.store(at * 1000);
			g->stopNs.store(g_slowUs * 1000 - g_times.frameUs * 500);
			g->atNs.store(-1);
			g->frozen.store(false);
			g->rearm.store(true);
			g->armed.store(true, std::memory_order_release);
		}
		if (g_sound)
			obs_source_set_volume(g_run.src,
					      (float)std::clamp((double)(g_slowUs - at) / 1000.0 / VOICE_FADE_MS, 0.0, 1.0));
	}
	obs_source_media_play_pause(p.src, false);
	arm_end(left_ms_from(at));
	obs_log(LOG_INFO, "replay: jumped to %lld ms", (long long)(at / 1000));
	if (!runUp)
		g_tick->stop();
}

void land_tick()
{
	Player &p = *g_landing;
	if (hold_player(p, g_landUs))
		g_landAt = os_gettime_ns();
	if (g_landing == &g_run && g_slowUs >= 0)
		hold_player(g_slow, g_slowUs);
	const double t = since_ms(g_landAt);
	if (t < SETTLE_MS || (off_picture(p.src, g_landUs) && t < 1000))
		return;
	land();
}

void drop_spare()
{
	if (g_spare.item)
		obs_sceneitem_remove(g_spare.item);
	release_player(g_spare);
	g_spareFor = nullptr;
	g_spareWait = g_spareChasing = false;
}

/* The player at the new speed is on its picture: it takes over from the one on screen. */
void commit_spare()
{
	Player &old = *g_spareFor;
	const bool slowRole = &old == &g_slow;
	void *param = (void *)(uintptr_t)g_heard;
	signal_handler_t *was = obs_source_get_signal_handler(old.src);
	signal_handler_disconnect(was, "media_ended", on_run_ended, param);
	signal_handler_disconnect(was, "media_ended", on_last_ended, param);
	signal_handler_connect(obs_source_get_signal_handler(g_spare.src), "media_ended",
			       slowRole ? on_last_ended : on_run_ended, param);
	obs_source_media_play_pause(g_spare.src, false);
	if (old.item)
		obs_sceneitem_remove(old.item);
	release_player(old);
	old = g_spare;
	g_spare = Player{};
	g_spareFor = nullptr;
	g_spareWait = false;
	g_spareChasing = false;
	(g_slowUs >= 0 ? g_slowPct : g_runPct) = g_sparePct;
	g_paused = false;
	if (g_press && g_slowUs < 0) {
		if (g_sparePct >= 100) {
			if (g_sound) {
				obs_source_set_audio_mixers(g_run.src, g_runMixers);
				obs_source_set_muted(g_run.src, false);
			}
			g_pressFromUs = g_pressBackUs = -1;
		} else if (g_spareKind == SPARE_PRESS) {
			/* Slow for the length picked after the press. Too near the end: slow to the end. */
			g_pressFromUs = g_spareUs;
			g_pressBackUs = g_pressWantUs < g_endUs - 500000 ? g_pressWantUs : -1;
			if (g_trans == TRANS_CINE && g_scene)
				g_cine.momentAt = os_gettime_ns();
		}
	}
	arm_end(left_ms_from(g_spareUs));
	obs_log(LOG_INFO, "replay: now at %d%% from %lld ms%s", g_sparePct, (long long)(g_spareUs / 1000),
		g_press && g_pressBackUs >= 0 && g_sparePct < 100
			? (", back to normal speed at " + std::to_string(g_pressBackUs / 1000) + " ms").c_str()
			: "");
	if (g_slowUs < 0 || g_swapped)
		g_tick->stop();
}

void spare_tick()
{
	const double t = since_ms(g_spareAt);
	const bool open = obs_source_media_get_duration(g_spare.src) > 0 && obs_source_get_width(g_spare.src) > 0;
	if (!open) {
		if (t > READY_MS) {
			obs_log(LOG_WARNING, "replay: the player at the new speed never opened");
			drop_spare();
			if (g_slowUs < 0 || g_swapped)
				g_tick->stop();
		}
		return;
	}
	if (!g_spare.heldAt) {
		park(g_spare, g_spareUs);
		return;
	}
	/* Back to normal speed: the player underneath plays until it reaches the picture on
	   screen, then takes over there (or once the slow one has ended or it takes too long). */
	if (g_spareChasing) {
		const int64_t mine = obs_source_media_get_time(g_spare.src) * 1000;
		if (mine + g_times.frameUs / 2 < clip_now_us() && t < PRESS_LEAD_MS + 5000 &&
		    obs_source_media_get_state(shown_player()) == OBS_MEDIA_STATE_PLAYING)
			return;
		g_spareUs = mine;
		commit_spare();
		return;
	}
	hold_player(g_spare, g_spareUs);
	const double held = since_ms(g_spare.heldAt);
	if (held < SETTLE_MS || (off_picture(g_spare.src, g_spareUs) && held < 1000))
		return;
	if (g_spareWait) {
		g_spareChasing = true;
		obs_source_media_play_pause(g_spare.src, false);
		return;
	}
	commit_spare();
}

void pause_toggle()
{
	if (!can_steer() || g_landing)
		return;
	obs_source_t *src = shown_player();
	if (!g_paused) {
		if (obs_source_media_get_state(src) == OBS_MEDIA_STATE_ENDED)
			return; /* already finishing */
		g_moves++;
		g_guard->stop();
		obs_source_media_play_pause(src, true);
		g_paused = true;
		obs_log(LOG_INFO, "replay: paused");
		return;
	}
	g_paused = false;
	/* A paused player's picture times start again from the clock when it plays on, so
	   the gate learns them again from the picture after the one on screen. */
	if (g_slowUs >= 0 && !g_swapped)
		if (Gate *g = gate_of(g_gate)) {
			const int64_t at = g->atNs.load();
			if (at >= 0 && !g->frozen.load()) {
				g->fromNs.store(at + g_times.frameUs * 1000);
				g->rearm.store(true);
			}
		}
	obs_source_media_play_pause(src, false);
	arm_end(left_ms_from(clip_now_us()));
	obs_log(LOG_INFO, "replay: playing again");
}

void speed_next()
{
	if (!can_steer() || g_landing)
		return;
	if (g_press && g_slowUs < 0) {
		/* Pressed slow motion: the key picks the speed of the slow motion, so it never
		   goes to full speed. At normal speed that is the next press's; while slow, now. */
		const int pct = g_slowPct > 75 ? 75 : g_slowPct > 50 ? 50 : g_slowPct > 25 ? 25 : 75;
		g_moves++;
		g_slowPct = pct;
		if (g_runPct >= 100) {
			obs_log(LOG_INFO, "replay: the next slow motion will play at %d%%", pct);
			return;
		}
		const int64_t at = std::max(g_pressFromUs >= 0 ? g_pressFromUs : g_fromUs, key_at_or_before(clip_now_us()));
		open_spare(pct, at, false, SPARE_SPEED);
		return;
	}
	const int pct = next_speed(speed_now());
	g_moves++;
	if (g_slowUs >= 0 && !g_swapped) {
		/* The slow part has not started: its hidden player changes speed, and the tick
		   puts it back on its picture once it has opened again. */
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "speed_percent", pct);
		obs_source_update(g_slow.src, d);
		obs_data_release(d);
		g_slow.heldAt = 0;
		g_slowPct = pct;
		g_speedAt = os_gettime_ns();
		if (!g_paused)
			arm_end(left_ms_from(clip_now_us()));
		obs_log(LOG_INFO, "replay: the slow part will play at %d%%", pct);
		return;
	}
	/* A player cannot change speed while it is on screen without a blank picture, so a
	   new one at the new speed opens underneath, lands on the full picture at or before
	   the one showing, and takes over. */
	int64_t at = key_at_or_before(clip_now_us());
	at = std::max(at, g_slowUs >= 0 ? g_slowUs : g_fromUs);
	open_spare(pct, at, false, SPARE_SPEED);
}

/* Opens a player at `pct` underneath the one on screen, parked on `at`. The tick lands it
   and hands over: at once, or with `wait` once the one on screen reaches `at`. */
bool open_spare(int pct, int64_t at, bool wait, int kind)
{
	Player &old = g_slowUs >= 0 ? g_slow : g_run;
	obs_source_t *src = make_player(g_path, pct, "Simply Vertical replay speed");
	if (!src || !old.src) {
		obs_source_release(src);
		return false;
	}
	if (g_press && g_slowUs < 0) {
		/* Pressed slow motion plays silent, like the slow part of the moment; the normal
		   speed parts have the replay's sound. */
		const bool loud = g_sound && pct >= 100 && !wait; /* one catching up gets its sound when it takes over */
		obs_source_set_audio_mixers(src, loud ? g_runMixers : 0u);
		obs_source_set_muted(src, !loud);
		obs_source_set_volume(src, 1.0f);
	} else {
		obs_source_set_audio_mixers(src, obs_source_get_audio_mixers(old.src));
		obs_source_set_muted(src, obs_source_muted(old.src));
		obs_source_set_volume(src, obs_source_get_volume(old.src));
	}
	obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_NONE);
	g_spare = Player{};
	g_spare.src = src;
	g_spare.item = add_clip_item(g_scene, src);
	if (g_spare.item && old.item) {
		vec2 pos;
		vec2_set(&pos, 0.0f, 0.0f);
		obs_sceneitem_set_pos(g_spare.item, &pos);
		obs_sceneitem_set_order_position(g_spare.item, obs_sceneitem_get_order_position(old.item));
	}
	g_spare.fade = add_fade(src, 1.0);
	if (g_zoomPct > 0)
		apply_zoom(g_zoomNow); /* the new player starts as pushed in as the one on screen */
	g_spareFor = &old;
	g_spareUs = at;
	g_sparePct = pct;
	g_spareAt = os_gettime_ns();
	g_spareWait = wait;
	g_spareChasing = false;
	g_spareKind = kind;
	g_tick->start();
	obs_log(LOG_INFO, "replay: opening a player at %d%% for %lld ms", pct, (long long)(at / 1000));
	return true;
}

/* How far the slowed clip moves while the normal speed player opens. */
int64_t press_lead_us()
{
	return (int64_t)PRESS_LEAD_MS * 1000 * std::clamp(g_runPct, 10, 100) / 100;
}

/* Pressed slow motion at its end: a normal speed player opens on the full picture before
   the one showing and plays underneath, faster, until it catches up with the slow one and
   takes over on the same picture. A player can only jump to full pictures, so this is how
   it goes back to normal speed anywhere, not just on one. */
void press_watch()
{
	if (!g_press || g_slowUs >= 0 || g_runPct >= 100 || g_pressBackUs < 0)
		return;
	if (g_phase != Phase::Playing || g_spare.src || g_landing || g_paused)
		return;
	const int64_t now = clip_now_us();
	if (now < g_pressBackUs - press_lead_us())
		return;
	open_spare(100, std::max(g_pressFromUs, key_at_or_before(now)), true, SPARE_BACK);
}

/* The Slow motion key: at normal speed it slows from about a second before the press.
   While slow, it goes back to normal speed now. */
void slow_press()
{
	if (!g_press || g_slowUs >= 0 || !g_scene || g_phase != Phase::Playing || g_landing || g_spare.src)
		return;
	const int64_t now = clip_now_us();
	if (g_runPct < 100) {
		g_pressBackUs = now;
		obs_log(LOG_INFO, "replay: slow motion pressed again, back to normal speed at %lld ms", (long long)(now / 1000));
		press_watch();
		return;
	}
	if (now >= g_endUs - 700000)
		return; /* too near the end to slow anything */
	g_moves++;
	g_pressWantUs = now + (int64_t)g_slowSecs * 1000000;
	const int64_t at = std::max(g_fromUs, key_at_or_before(std::max<int64_t>(0, now - PRESS_BACK_US)));
	if (open_spare(g_slowPct, at, false, SPARE_PRESS))
		obs_log(LOG_INFO, "replay: slow motion pressed at %lld ms", (long long)(now / 1000));
}

/* ---------- more than one clip ---------- */

bool same_file(std::string a, std::string b)
{
	for (std::string *s : {&a, &b})
		for (char &c : *s)
			if (c == '\\')
				c = '/';
	return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0;
}

/* The replay before ended by itself: the next clip of a row plays. One that cannot be
   played is skipped. */
void continue_row()
{
	if (g_row.empty())
		return;
	QTimer::singleShot(0, qApp, [] {
		while (!g_row.empty() && !g_scene) {
			const SvClip next = g_row.front();
			g_row.pop_front();
			std::string problem;
			if (sv_replay_play_clip(next, problem))
				return;
			obs_log(LOG_WARNING, "replay: a clip in the row could not be played, skipped it");
		}
		g_rowTotal = 0;
	});
}

/* A row with the wipe: the bands cover the phone, so the next clip starts under them and
   they carry on off the far side, one wipe between clips instead of two. */
void hand_over()
{
	const SvClip next = g_row.front();
	g_row.pop_front();
	const std::string canvas = g_canvas;
	g_keepCover = true; /* the covered picture stays on the phone until the next one replaces it */
	sv_replay_stop();
	g_keepCover = false;
	g_startCovered = true;
	std::string problem;
	const bool ok = sv_replay_play_clip(next, problem);
	g_startCovered = false;
	if (!ok) {
		clear_display(canvas);
		obs_log(LOG_WARNING, "replay: a clip in the row could not be played, skipped it");
		continue_row();
	}
}

/* Where the clip is in real time: follows the clock between the player's own steps (at 25%
   a new picture comes only every few frames), and goes back to the player after a jump. */
void fx_tick()
{
	if (!g_scene || g_phase == Phase::Off || g_phase == Phase::Opening) {
		g_fx->stop();
		return;
	}
	const double dt = g_fxLast ? since_ms(g_fxLast) : 0.0;
	g_fxLast = os_gettime_ns();
	const bool moving = !g_paused && !g_landing && (!g_spare.src || g_spareChasing);
	if (moving) {
		const int pct = g_slowUs >= 0 ? (g_swapped ? g_slowPct : 100) : g_runPct;
		g_estUs += (int64_t)(dt * 1000.0 * pct / 100.0);
		if (g_slowUs >= 0 && !g_swapped)
			g_estUs = std::min(g_estUs, g_slowUs);
	}
	const int64_t real = clip_now_us();
	if (real >= 0 && std::llabs(g_estUs - real) > 300000)
		g_estUs = real;
	g_estUs = std::clamp(g_estUs, g_fromUs, std::max(g_fromUs, g_endUs));

	lens_follow();
	if (g_lineItem && g_lineW > 0) {
		/* With pressed slow motion nobody knows ahead how long it plays, so the line follows the clip. */
		const int total = std::max(1, play_left_ms());
		const double p = g_press ? std::clamp((double)(g_estUs - g_fromUs) / (double)std::max<int64_t>(1, g_endUs - g_fromUs), 0.0, 1.0)
					 : std::clamp(1.0 - (double)left_ms_from(g_estUs) / total, 0.0, 1.0);
		const int shown = (int)std::lround(p * g_lineW);
		if (shown != g_lineShown) {
			g_lineShown = shown;
			obs_sceneitem_crop crop = {};
			crop.right = g_linePad + (g_lineW - shown);
			if (shown == 0)
				crop.right = g_lineW + g_linePad * 2 - 1;
			obs_sceneitem_set_crop(g_lineItem, &crop);
		}
	}

	/* The push in, only on the slow part: in slowly until 70% through, then back out. */
	if (g_zoomPct > 0 && g_phase == Phase::Playing) {
		int64_t from = g_slowUs >= 0 ? g_slowUs : (g_runPct < 100 ? g_fromUs : -1);
		int64_t to = g_endUs;
		if (g_press) {
			from = g_runPct < 100 ? g_pressFromUs : -1;
			to = g_pressBackUs >= 0 ? g_pressBackUs : g_endUs;
		}
		float z = 1.0f;
		if (from >= 0 && to > from) {
			const float u = std::clamp((float)(g_estUs - from) / (float)(to - from), 0.0f, 1.0f);
			const auto smooth = [](float k) { return k * k * (3.0f - 2.0f * k); };
			const float shape = u < 0.7f ? smooth(u / 0.7f) : 1.0f - smooth((u - 0.7f) / 0.3f);
			z = 1.0f + g_zoomPct / 100.0f * shape;
		}
		/* Pressed slow motion can end early: the push in eases back out instead of jumping. */
		if (g_press)
			z = g_zoomNow + (z - g_zoomNow) * (float)std::clamp(dt / 160.0, 0.0, 1.0);
		if (std::fabs(z - g_zoomNow) > 0.0002f)
			apply_zoom(z);
	}
	/* Cinematic's pop up settling and the streak, while it plays (the tick does the way out). */
	if (g_trans == TRANS_CINE && g_phase == Phase::Playing)
		cine_draw();
	press_watch();
}

void start_fx()
{
	if (!g_lineItem && g_zoomPct <= 0 && g_trans != TRANS_CINE && !g_press)
		return;
	if (!g_fx) {
		g_fx = new QTimer();
		g_fx->setInterval(TICK_MS);
		g_fx->setTimerType(Qt::PreciseTimer);
		QObject::connect(g_fx, &QTimer::timeout, [] { fx_tick(); });
	}
	g_fxLast = 0;
	g_estUs = g_fromUs;
	g_lineShown = -1;
	g_fx->start();
	fx_tick();
}

/* Main thread. The Replay Window `win` shows `show` (both null: nothing). */
void set_window_show(obs_source_t *win, obs_source_t *show, const QRectF &box)
{
	obs_source_t *oldWin = nullptr, *oldShow = nullptr;
	if (win && show)
		obs_source_add_active_child(win, show); /* its players play while the window is on screen */
	{
		std::lock_guard<std::mutex> lock(g_viewMtx);
		oldWin = g_winSrc;
		oldShow = g_winShow;
		g_winSrc = win && show ? obs_source_get_ref(win) : nullptr;
		g_winShow = win && show ? obs_source_get_ref(show) : nullptr;
		g_winBox = box;
	}
	if (oldWin && oldShow)
		obs_source_remove_active_child(oldWin, oldShow);
	obs_source_release(oldShow);
	obs_source_release(oldWin);
}

/* Takes the replay off the phone, whichever way it was showing. */
void clear_display(const std::string &canvas)
{
	sv_canvas_set_cover(canvas, nullptr);
	set_window_show(nullptr, nullptr, QRectF());
}

/* The topmost Replay Window showing in the vertical scene that is up now, and its
   place on the phone. Looks inside groups too. Main thread. */
struct WindowHit {
	obs_source_t *src = nullptr; /* not held */
	QRectF box;
	const matrix4 *outer = nullptr;
};

QRectF item_rect(obs_sceneitem_t *item, const matrix4 *outer)
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (const auto &c : {std::pair<float, float>{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
		vec3 v;
		vec3_set(&v, c.first, c.second, 0.0f);
		vec3_transform(&v, &v, &m);
		if (outer)
			vec3_transform(&v, &v, outer);
		x0 = std::min(x0, v.x);
		y0 = std::min(y0, v.y);
		x1 = std::max(x1, v.x);
		y1 = std::max(y1, v.y);
	}
	return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

bool scan_windows(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *hit = static_cast<WindowHit *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	if (obs_sceneitem_is_group(item)) {
		matrix4 g;
		obs_sceneitem_get_draw_transform(item, &g);
		WindowHit inner;
		inner.outer = &g;
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), scan_windows, &inner);
		if (inner.src) {
			hit->src = inner.src;
			hit->box = inner.box;
		}
		return true;
	}
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && std::strcmp(obs_source_get_id(src), WINDOW_ID) == 0) {
		const QRectF r = item_rect(item, hit->outer);
		/* A window with no real place (a damaged scene file) is skipped, never read as the whole phone. */
		if (std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height()) &&
		    r.width() > 0 && r.height() > 0 && r.width() < 1e6 && r.height() < 1e6) {
			hit->src = src; /* later layers are higher up, so the last one wins */
			hit->box = r;
		}
	}
	return true;
}

/* win comes back held. */
bool find_window(const std::string &canvas, obs_source_t *&win, QRectF &box)
{
	win = nullptr;
	obs_scene_t *scene = sv_canvas_scene_ref(canvas);
	if (!scene)
		return false;
	WindowHit hit;
	obs_scene_enum_items(scene, scan_windows, &hit);
	if (hit.src) {
		const QRectF on = hit.box.intersected(QRectF(0, 0, CANVAS_W, CANVAS_H));
		if (on.width() >= 32 && on.height() >= 32) {
			win = obs_source_get_ref(hit.src);
			box = on;
		}
	}
	obs_scene_release(scene);
	return win != nullptr;
}

/* The vertical's layout source in the scene that is up now, held, for its game and camera picks. */
bool scan_layout(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && std::strcmp(obs_source_get_id(src), LAYOUT_ID) == 0) {
		*static_cast<obs_source_t **>(param) = obs_source_get_ref(src);
		return false;
	}
	return true;
}

obs_source_t *layout_source_of(const std::string &canvas)
{
	obs_source_t *found = nullptr;
	if (obs_scene_t *scene = sv_canvas_scene_ref(canvas)) {
		obs_scene_enum_items(scene, scan_layout, &found);
		obs_scene_release(scene);
	}
	return found;
}

/* Runs `f` on OBS's main thread instead when called from another one. */
template<class F> bool sent_to_main(F f)
{
	if (on_main_thread())
		return false;
	if (qApp)
		QMetaObject::invokeMethod(qApp, f, Qt::QueuedConnection);
	return true;
}

/* Draws `region` of `src` (in src's own pixels) into a texture tw x th, then that
   texture at outW x outH. Drawing thread. */
void draw_region(gs_texrender_t *&tex, obs_source_t *src, const QRectF &region, uint32_t outW, uint32_t outH,
		 uint32_t tw, uint32_t th)
{
	if (tw < 2 || th < 2 || region.width() < 1 || region.height() < 1)
		return;
	if (!tex)
		tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!tex)
		return;
	gs_texrender_reset(tex);
	if (!gs_texrender_begin(tex, tw, th))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho((float)region.left(), (float)region.right(), (float)region.top(), (float)region.bottom(), -100.0f,
		 100.0f);
	gs_blend_state_push();
	/* Colour comes out premultiplied, ready to lay over what is under the window. */
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(src);
	gs_blend_state_pop();
	gs_texrender_end(tex);

	gs_texture_t *t = gs_texrender_get_texture(tex);
	gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!t || !e)
		return;
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), t);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(t, 0, outW, outH);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb);
}

struct Drawn {
	obs_source_t *self = nullptr;
	gs_texrender_t *tex = nullptr;
	uint32_t w = WINDOW_W, h = WINDOW_H;
};

void drawn_destroy(void *data)
{
	auto *d = static_cast<Drawn *>(data);
	obs_enter_graphics();
	gs_texrender_destroy(d->tex);
	obs_leave_graphics();
	delete d;
}

/* The Replay Window: shows nothing until a replay plays, then the replay, showing the
   same part of the clip the window covers on the phone. */
const char *window_name(void *)
{
	return T("ReplayWindowName");
}

void *window_create(obs_data_t *, obs_source_t *source)
{
	auto *d = new Drawn();
	d->self = source;
	return d;
}

uint32_t drawn_width(void *data)
{
	return static_cast<Drawn *>(data)->w;
}

uint32_t drawn_height(void *data)
{
	return static_cast<Drawn *>(data)->h;
}

void window_render(void *data, gs_effect_t *)
{
	auto *d = static_cast<Drawn *>(data);
	obs_source_t *show = nullptr;
	QRectF box;
	{
		std::lock_guard<std::mutex> lock(g_viewMtx);
		if (g_winShow && g_winSrc == d->self) {
			show = obs_source_get_ref(g_winShow);
			box = g_winBox;
		}
	}
	if (!show)
		return;
	draw_region(d->tex, show, box, d->w, d->h, (uint32_t)std::lround(box.width()),
		    (uint32_t)std::lround(box.height()));
	obs_source_release(show);
}

/* The game zoom with Keep my facecam live: the game's box of the clip, pushed in. */
const char *lens_name(void *)
{
	return "Simply Vertical replay zoom";
}

void *lens_create(obs_data_t *settings, obs_source_t *source)
{
	auto *d = new Drawn();
	d->self = source;
	d->w = (uint32_t)std::max<long long>(2, obs_data_get_int(settings, "w"));
	d->h = (uint32_t)std::max<long long>(2, obs_data_get_int(settings, "h"));
	return d;
}

void lens_render(void *data, gs_effect_t *)
{
	auto *d = static_cast<Drawn *>(data);
	obs_source_t *show = nullptr;
	QRectF r;
	{
		std::lock_guard<std::mutex> lock(g_viewMtx);
		if (g_lensShow) {
			show = obs_source_get_ref(g_lensShow);
			r = g_lensRegion;
		}
	}
	if (!show)
		return;
	const uint32_t sw = obs_source_get_width(show), sh = obs_source_get_height(show);
	if (sw && sh) {
		/* The player fills the phone the way its scene layer does: scaled to fit, centred. */
		const double k = std::min((double)CANVAS_W / sw, (double)CANVAS_H / sh);
		const double ox = (CANVAS_W - sw * k) / 2, oy = (CANVAS_H - sh * k) / 2;
		const QRectF src((r.x() - ox) / k, (r.y() - oy) / k, r.width() / k, r.height() / k);
		draw_region(d->tex, show, src, d->w, d->h, d->w, d->h);
	}
	obs_source_release(show);
}

} // namespace

void sv_replay_register()
{
	{
		obs_source_info info = {};
		info.id = WINDOW_ID;
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
		info.icon_type = OBS_ICON_TYPE_MEDIA;
		info.get_name = window_name;
		info.create = window_create;
		info.destroy = drawn_destroy;
		info.get_width = drawn_width;
		info.get_height = drawn_height;
		info.video_render = window_render;
		obs_register_source(&info);
	}
	{
		obs_source_info info = {};
		info.id = LENS_ID;
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
		info.get_name = lens_name;
		info.create = lens_create;
		info.destroy = drawn_destroy;
		info.get_width = drawn_width;
		info.get_height = drawn_height;
		info.video_render = lens_render;
		obs_register_source(&info);
	}

	obs_source_info info = {};
	info.id = GATE_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	/* Never offered in OBS's filter menu. */
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC | OBS_SOURCE_CAP_DISABLED;
	info.get_name = gate_name;
	info.create = gate_create;
	info.destroy = gate_destroy;
	info.filter_video = gate_video;
	obs_register_source(&info);
}

bool sv_replay_playing()
{
	return g_scene != nullptr;
}

bool sv_replay_play(std::string &problem)
{
	if (g_scene)
		return true;
	g_row.clear();
	g_rowTotal = 0;
	return sv_replay_play_clip(sv_output_last_clip(), problem);
}

bool sv_replay_play_clip(const SvClip &clip, std::string &problem)
{
	if (g_scene)
		return true;
	if (clip.path.empty() || !os_file_exists(clip.path.c_str())) {
		problem = T("ReplayNone");
		return false;
	}
	if (!sv_canvas_running()) {
		problem = T("ReplayNoVertical");
		return false;
	}
	const SvOutSettings set = sv_output_settings();
	std::string canvas = set.clip_canvas;
	if (!canvas.empty() && !sv_canvas_exists(canvas))
		canvas.clear();

	const int slowPct = std::clamp(set.replay_slow, 10, 100);
	const bool press = slowPct < 100 && set.replay_slow_press;
	const bool wantTwo = slowPct < 100 && !set.replay_slow_whole && !press;
	g_times = ClipTimes{};
	g_timesOk = clip_times(clip.path, g_times);
	const bool two = wantTwo && g_timesOk;

	/* With slow motion on the moment, the run up player plays at normal speed, and so does
	   the replay that slows when the Slow motion key is pressed. Otherwise
	   the one player plays at the speed picked. */
	obs_source_t *run = make_player(clip.path, two || press ? 100 : slowPct, "Simply Vertical replay");
	obs_source_t *slow = two ? make_player(clip.path, slowPct, "Simply Vertical replay slow") : nullptr;
	if (!run || (two && !slow)) {
		obs_source_release(run);
		obs_source_release(slow);
		problem = T("ReplayFailed");
		return false;
	}

	/* Its sound goes only on the vertical's own track. Without one, the main stream
	   would hear it too, so it plays silent. The slow part is always silent: slowed
	   sound only drones. */
	const SvSoundDesk desk = sv_sound_desk();
	const bool sound = set.replay_sound && desk.own && desk.track >= 1 && desk.track <= 6;
	obs_source_set_audio_mixers(run, sound ? (1u << (desk.track - 1)) : 0u);
	obs_source_set_muted(run, !sound);
	obs_source_set_monitoring_type(run, OBS_MONITORING_TYPE_NONE);
	if (slow) {
		obs_source_set_audio_mixers(slow, 0u);
		obs_source_set_muted(slow, true);
		obs_source_set_monitoring_type(slow, OBS_MONITORING_TYPE_NONE);
	}

	obs_scene_t *scene = obs_scene_create_private("Simply Vertical replay");

	/* A Replay Window in the scene: the replay plays inside it, and everything else on
	   the phone stays live. None: the whole phone, as before. */
	obs_source_t *win = nullptr;
	QRectF winBox;
	const bool inWindow = find_window(canvas, win, winBox);
	const bool camLive = !inWindow && set.replay_cam_live;

	/* 1. The live layout, so there is never a black phone while the clip opens. A window
	   has the live scene round and under it already. */
	if (!inWindow)
		if (obs_scene_t *live = sv_canvas_scene_ref(canvas)) {
			obs_scene_add(scene, obs_scene_get_source(live));
			obs_scene_release(live);
		}

	/* 2. The clip, waiting just off the right edge until it has its picture. The slow
	   player sits under the run up player, paused on the picture where it takes over. */
	g_slow = Player{};
	if (slow) {
		g_slow.src = slow;
		g_slow.item = add_clip_item(scene, slow);
		g_slow.fade = add_fade(slow, 1.0);
	}
	g_run = Player{};
	g_run.src = run;
	g_run.item = add_clip_item(scene, run);
	g_run.fade = add_fade(run, 1.0);
	g_gate = nullptr;
	if (two) {
		g_gate = obs_source_create_private(GATE_ID, "Simply Vertical replay gate", nullptr);
		if (g_gate)
			obs_source_filter_add(run, g_gate);
		else
			obs_log(LOG_WARNING, "replay: the slow motion filter could not be made");
	}

	/* Keep my facecam live: the layout's facecam, live, goes on top. The zoom then pushes in
	   on the game's own box, which only works for a plain square cornered game box clear of
	   the facecam; otherwise there is no zoom, rather than a second face peeking out. */
	int zoomPct = std::clamp(set.replay_zoom, 0, 25);
	SvPiece camPiece;
	bool haveCam = false;
	QRectF gameBox;
	g_lens = nullptr;
	g_lensItem = nullptr;
	g_camLive = nullptr;
	if (camLive) {
		SvLayout l;
		std::string err;
		const std::string json = sv_link_showing();
		const SvPiece *game = nullptr;
		if (!json.empty() && sv_layout_parse(json.c_str(), l, err))
			for (const SvPiece &p : l.pieces) {
				if (p.src == SvSrc::Cam && !haveCam) {
					camPiece = p;
					haveCam = true;
				}
				if (p.src == SvSrc::Game && (!game || p.box.w * p.box.h > game->box.w * game->box.h))
					game = &p;
			}
		if (game)
			gameBox = QRectF(game->box.x, game->box.y, game->box.w, game->box.h);
		if (haveCam && zoomPct > 0) {
			bool plain = game && game->mask == SvMask::Rect && game->feather <= 0.0f && std::fabs(game->rot) < 0.01f;
			if (plain)
				for (float c : game->corners)
					plain = plain && c <= 0.5f;
			plain = plain && game->radius <= 0.5f;
			const SvRect &c = camPiece.box;
			const bool apart = game && (c.x + c.w <= game->box.x + 2 || game->box.x + game->box.w <= c.x + 2 ||
						    c.y + c.h <= game->box.y + 2 || game->box.y + game->box.h <= c.y + 2);
			if (plain && apart) {
				obs_data_t *ls = obs_data_create();
				obs_data_set_int(ls, "w", (int)std::lround(game->box.w));
				obs_data_set_int(ls, "h", (int)std::lround(game->box.h));
				g_lens = obs_source_create_private(LENS_ID, "Simply Vertical replay zoom", ls);
				obs_data_release(ls);
				if (g_lens) {
					g_lensItem = obs_scene_add(scene, g_lens);
					vec2 pos;
					vec2_set(&pos, game->box.x, game->box.y);
					obs_sceneitem_set_pos(g_lensItem, &pos);
					obs_sceneitem_set_visible(g_lensItem, false);
					g_lensBox = QRectF(game->box.x, game->box.y, game->box.w, game->box.h);
				}
			}
			if (!g_lens) {
				zoomPct = 0;
				obs_log(LOG_INFO, "replay: no zoom with the live facecam on this layout");
			}
		}
	}

	/* 3. The lit frame and REPLAY tag, and the line under the tag. */
	ReplayLook look = replay_look(inWindow ? &winBox : nullptr);
	/* The live facecam goes on top, so a tag in its spot would be hidden: it moves to the
	   top left of the game instead, the way it sits in a Replay Window. */
	if (haveCam) {
		const QRectF cam(camPiece.box.x, camPiece.box.y, camPiece.box.w, camPiece.box.h);
		if (QRectF(look.tagAt, tag_size()).adjusted(-18, -18, 18, 18).intersects(cam)) {
			const QRectF b = gameBox.isEmpty() ? QRectF(0, cam.bottom(), CANVAS_W, CANVAS_H - cam.bottom()) : gameBox;
			look.tagAt = QPointF(std::max<qreal>(b.x() + 34, 124), b.y() + 30);
		}
	}
	g_wipeBox = look.box;
	const int trans = std::clamp(set.replay_transition, 0, 3);
	obs_source_t *frame = nullptr, *frameFade = nullptr;
	if (set.replay_tag && trans != TRANS_CINE) {
		const std::string pic = frame_picture(look, set.replay_line);
		if (!pic.empty()) {
			obs_data_t *fs = obs_data_create();
			obs_data_set_string(fs, "file", pic.c_str());
			frame = obs_source_create_private("image_source", "Simply Vertical replay frame", fs);
			obs_data_release(fs);
			if (frame) {
				obs_scene_add(scene, frame);
				frameFade = add_fade(frame, 0.0);
			}
		}
	}
	g_line = nullptr;
	g_lineFade = nullptr;
	g_lineItem = nullptr;
	if (frame && set.replay_line) {
		const std::string pic = line_picture(look);
		if (!pic.empty()) {
			obs_data_t *ls = obs_data_create();
			obs_data_set_string(ls, "file", pic.c_str());
			g_line = obs_source_create_private("image_source", "Simply Vertical replay line", ls);
			obs_data_release(ls);
			if (g_line) {
				g_lineItem = obs_scene_add(scene, g_line);
				const QRectF r = line_rect(look);
				vec2 pos;
				vec2_set(&pos, (float)r.x() - g_linePad, (float)r.y() - g_linePad);
				obs_sceneitem_set_pos(g_lineItem, &pos);
				obs_sceneitem_crop crop = {};
				crop.right = g_lineW + g_linePad * 2 - 1;
				obs_sceneitem_set_crop(g_lineItem, &crop);
				g_lineFade = add_fade(g_line, 0.0);
			}
		}
	}
	/* Cinematic draws its own frame, line, title and shutter (see cine_build). */
	if (trans == TRANS_CINE)
		cine_build(scene, look, set.replay_tag, set.replay_tag && set.replay_line);

	/* 4. The wipe's bands and the big REPLAY, on top of everything. */
	g_bands.clear();
	g_word = nullptr;
	g_wordItem = nullptr;
	if (trans == TRANS_WIPE)
		add_bands(scene, look.colour);
	if (trans == TRANS_FADE) {
		set_opacity(g_run.fade, 0.0);
		set_opacity(g_slow.fade, 0.0);
	}

	/* 5. The live facecam, on top of everything, so it never blinks. */
	if (haveCam) {
		obs_data_t *st = obs_data_create();
		if (obs_source_t *lay = layout_source_of(canvas)) {
			obs_data_t *ls = obs_source_get_settings(lay);
			obs_data_apply(st, ls);
			obs_data_release(ls);
			obs_source_release(lay);
		}
		obs_data_set_string(st, "piece_name", camPiece.id.empty() ? camPiece.name.c_str() : camPiece.id.c_str());
		obs_data_set_int(st, "piece_grow", -1);
		float dx = 0, dy = 0;
		if (sv_piece_inset(camPiece, dx, dy))
			g_camLive = obs_source_create_private(PIECE_ID, "Simply Vertical replay facecam", st);
		obs_data_release(st);
		if (g_camLive) {
			obs_sceneitem_t *item = obs_scene_add(scene, g_camLive);
			vec2 pos;
			vec2_set(&pos, camPiece.box.x - dx, camPiece.box.y - dy);
			obs_sceneitem_set_pos(item, &pos);
		}
	}

	g_frame = frame;
	g_frameFade = frameFade;
	g_scene = scene;
	g_canvas = canvas;
	g_wantMs = set.replay_seconds > 0 ? set.replay_seconds * 1000 : 0;
	g_after = clip.after;
	g_slowPct = slowPct;
	g_slowSecs = std::clamp(set.replay_slow_seconds, 1, 30);
	g_runPct = two || press ? 100 : slowPct;
	g_press = press;
	g_pressFromUs = g_pressBackUs = g_pressWantUs = -1;
	g_runMixers = sound ? (1u << (desk.track - 1)) : 0u;
	g_spareWait = g_spareChasing = false;
	g_fromUs = 0;
	g_slowUs = -1;
	g_durUs = (int64_t)std::max(1, clip.seconds) * 1000000;
	g_endUs = g_durUs;
	g_seekAt = 0;
	g_swapped = false;
	g_frozenAt = 0;
	g_sound = sound;
	g_path = clip.path;
	g_paused = false;
	g_landing = nullptr;
	g_speedAt = 0;
	g_spare = Player{};
	g_spareFor = nullptr;
	g_trans = trans;
	g_inMs = trans == TRANS_WIPE ? REVEAL_MS : IN_MS;
	g_zoomPct = zoomPct;
	g_focus = inWindow ? winBox.center() : (g_lens ? g_lensBox.center() : game_focus());
	g_inWindow = inWindow;
	g_zoomNow = 1.0f;
	g_covered = (trans == TRANS_WIPE || trans == TRANS_CINE) && g_startCovered;
	g_wipeDir = g_startCovered ? 1 : -1;
	g_wipeAt = os_gettime_ns();
	set_clip_x(g_covered ? 0.0f : (float)CANVAS_W);
	if (g_covered && trans == TRANS_WIPE)
		place_bands(0.5f, g_wipeDir);
	if (trans == TRANS_CINE) {
		/* In a row the shutter is already shut over the last clip: this one opens from it. */
		const uint64_t now = os_gettime_ns();
		g_cine.shutAt = g_startCovered ? now - (uint64_t)CINE_SHUT_MS * 1000000ULL : now;
		cine_draw();
	}
	const uint64_t token = ++g_token;
	g_heard = token;
	connect_ends(token);
	if (inWindow) {
		sv_canvas_set_cover(canvas, nullptr);
		set_window_show(win, obs_scene_get_source(scene), winBox);
	} else {
		set_window_show(nullptr, nullptr, QRectF());
		sv_canvas_set_cover(canvas, obs_scene_get_source(scene));
	}
	obs_source_release(win);

	ensure_timers();
	g_phase = Phase::Opening;
	g_phaseAt = os_gettime_ns();
	g_tick->start();
	/* Until it is on screen, the guard only covers a clip that never opens. */
	g_guard->disconnect();
	QObject::connect(g_guard, &QTimer::timeout, [token] {
		if (token == g_token) {
			sv_replay_stop();
			continue_row();
		}
	});
	g_guard->start(READY_MS + COVER_MS + REVEAL_MS + (int)((int64_t)std::max(1, clip.seconds) * 1000 * 100 / slowPct) + 4000);

	obs_log(LOG_INFO,
		"replay started (%d s clip, %d s after the press, last %d s, slow %d%% %s, sound %s, tag %s, %s, facecam %s)",
		clip.seconds, clip.after, set.replay_seconds, slowPct,
		slowPct >= 100 ? "off" : (two ? "on the moment" : press ? "on a press" : "whole clip"), sound ? "on" : "off", set.replay_tag ? "on" : "off",
		inWindow ? "in a window" : "whole phone", g_camLive ? "live" : "from the clip");
	g_lastPlayed = clip.path;
	const int done = g_rowTotal - (int)g_row.size();
	g_noticeText = g_rowTotal > 1 ? QString::fromUtf8(T("ReplayRowPlaying")).arg(done).arg(g_rowTotal).toStdString()
				      : std::string(T("ReplayPlaying"));
	sv_output_set_notice(g_noticeText, false);
	return true;
}

void sv_replay_stop()
{
	if (!g_scene)
		return;
	disconnect_ends(g_heard);
	g_token++;
	g_phase = Phase::Off;
	if (g_guard)
		g_guard->stop();
	if (g_tick)
		g_tick->stop();
	if (g_fx)
		g_fx->stop();
	if (!g_keepCover)
		clear_display(g_canvas);
	{
		obs_source_t *lensShow = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_viewMtx);
			lensShow = g_lensShow;
			g_lensShow = nullptr;
		}
		obs_source_release(lensShow);
	}
	obs_source_release(g_lens);
	g_lens = nullptr;
	g_lensItem = nullptr;
	obs_source_release(g_camLive);
	g_camLive = nullptr;
	g_inWindow = false;
	if (g_gate) {
		if (obs_source_t *parent = obs_filter_get_parent(g_gate))
			obs_source_filter_remove(parent, g_gate);
		obs_source_release(g_gate);
		g_gate = nullptr;
	}
	release_player(g_run);
	release_player(g_slow);
	release_player(g_spare);
	g_spareFor = nullptr;
	g_spareWait = false;
	g_spareChasing = false;
	g_press = false;
	g_pressFromUs = g_pressBackUs = -1;
	g_landing = nullptr;
	g_paused = false;
	g_path.clear();
	if (g_frameFade) {
		obs_source_filter_remove(g_frame, g_frameFade);
		obs_source_release(g_frameFade);
	}
	if (g_lineFade) {
		obs_source_filter_remove(g_line, g_lineFade);
		obs_source_release(g_lineFade);
	}
	obs_source_release(g_line);
	g_line = nullptr;
	g_lineFade = nullptr;
	g_lineItem = nullptr;
	for (Band &b : g_bands)
		obs_source_release(b.src);
	g_bands.clear();
	obs_source_release(g_word);
	g_word = nullptr;
	g_wordItem = nullptr;
	cine_release_all();
	g_covered = false;
	obs_scene_release(g_scene);
	obs_source_release(g_frame);
	g_scene = nullptr;
	g_frame = nullptr;
	g_frameFade = nullptr;
	g_canvas.clear();
	obs_log(LOG_INFO, "replay ended");
	if (sv_output_notice().text == g_noticeText)
		sv_output_set_notice("", false);
	else
		sv_output_set_notice(sv_output_notice().text, sv_output_notice().problem); /* the panel looks again */
}

void sv_replay_finish()
{
	/* Stopped on purpose: a row stops with it. */
	g_row.clear();
	g_rowTotal = 0;
	if (g_scene)
		begin_out(g_token);
}

void sv_replay_toggle()
{
	if (!on_main_thread()) {
		if (qApp)
			QMetaObject::invokeMethod(qApp, [] { sv_replay_toggle(); }, Qt::QueuedConnection);
		return;
	}
	if (g_scene) {
		/* Pressed again while it plays: fade back to live rather than cut. */
		sv_replay_finish();
		return;
	}
	std::string problem;
	if (!sv_replay_play(problem))
		sv_output_set_notice(problem, true);
}

void sv_replay_pause_toggle()
{
	if (!sent_to_main([] { sv_replay_pause_toggle(); }))
		pause_toggle();
}

void sv_replay_back()
{
	if (sent_to_main([] { sv_replay_back(); }))
		return;
	if (can_steer())
		jump_to(clip_now_us() - 5000000);
}

void sv_replay_restart()
{
	if (sent_to_main([] { sv_replay_restart(); }))
		return;
	if (can_steer())
		jump_to(g_fromUs);
}

void sv_replay_speed_next()
{
	if (!sent_to_main([] { sv_replay_speed_next(); }))
		speed_next();
}

bool sv_replay_paused()
{
	return g_scene && g_paused;
}

int sv_replay_speed()
{
	if (!g_scene)
		return 0;
	/* Pressed slow motion at normal speed: the speed the next press slows to. */
	if (g_press && g_slowUs < 0 && g_runPct >= 100)
		return g_slowPct;
	return speed_now();
}

void sv_replay_slow_press()
{
	if (!sent_to_main([] { sv_replay_slow_press(); }))
		slow_press();
}

bool sv_replay_press_mode()
{
	return g_scene && g_press && g_slowUs < 0;
}

bool sv_replay_press_slowed()
{
	return sv_replay_press_mode() && (g_runPct < 100 || (g_spare.src && g_spareKind == SPARE_PRESS));
}

void sv_replay_play_row(const std::vector<SvClip> &clips)
{
	if (sent_to_main([clips] { sv_replay_play_row(clips); }))
		return;
	if (clips.empty())
		return;
	if (g_scene)
		sv_replay_stop();
	g_row.assign(clips.begin(), clips.end());
	g_rowTotal = (int)clips.size();
	obs_log(LOG_INFO, "replay: playing %d clips in a row", g_rowTotal);
	continue_row();
}

void sv_replay_save_and_play()
{
	if (sent_to_main([] { sv_replay_save_and_play(); }))
		return;
	if (!sv_output_save_backtrack()) {
		sv_output_set_notice(T("BacktrackIsOff"), true);
		return;
	}
	g_playOnSave = os_gettime_ns();
	sv_output_set_notice(T("ReplaySaving"), false);
}

void sv_replay_clip_saved()
{
	if (sent_to_main([] { sv_replay_clip_saved(); }))
		return;
	const SvOutSettings set = sv_output_settings();
	/* Saving waits for the seconds after Save clip, then writes the file. */
	const bool asked = g_playOnSave && since_ms(g_playOnSave) < (set.clip_after + 60) * 1000.0;
	g_playOnSave = 0;
	if (!asked && !set.replay_auto)
		return;
	const SvClip clip = sv_output_last_clip();
	if (g_scene) {
		/* One is playing already: this one plays straight after it. */
		g_row.push_back(clip);
		g_rowTotal = std::max(g_rowTotal, 1) + 1;
		return;
	}
	g_row.clear();
	g_rowTotal = 0;
	std::string problem;
	if (!sv_replay_play_clip(clip, problem))
		sv_output_set_notice(problem, true);
}

void sv_replay_step_clip(int older)
{
	if (sent_to_main([older] { sv_replay_step_clip(older); }))
		return;
	const std::vector<SvClipInfo> list = sv_output_recent_clips(10);
	if (list.empty()) {
		sv_output_set_notice(T("ReplayNone"), true);
		return;
	}
	int at = -1;
	for (size_t i = 0; i < list.size(); i++)
		if (same_file(list[i].clip.path, g_lastPlayed))
			at = (int)i;
	const int want = at < 0 ? 0 : at + (older > 0 ? 1 : -1);
	if (want < 0 || want >= (int)list.size()) {
		sv_output_set_notice(older > 0 ? T("ReplayNoOlder") : T("ReplayNoNewer"), false);
		return;
	}
	g_row.clear();
	g_rowTotal = 0;
	if (g_scene)
		sv_replay_stop(); /* straight to the other clip */
	std::string problem;
	if (!sv_replay_play_clip(list[(size_t)want].clip, problem))
		sv_output_set_notice(problem, true);
}

int sv_replay_row_left()
{
	return (int)g_row.size();
}

std::string sv_replay_last_played()
{
	return g_lastPlayed;
}
