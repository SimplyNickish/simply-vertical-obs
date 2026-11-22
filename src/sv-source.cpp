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

// The "Simply Vertical" source: the whole finished vertical layout as one
// source. It reads the member's own game and camera sources straight from the
// graphics card (no second capture, no compression), cuts them to shape with
// masks made once per layout, and leaves everything outside the pieces
// see-through.

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <plugin-support.h>

#include "sv-source.h"
#include "sv-brand.h"
#include "sv-ease.h"
#include "sv-layout.h"
#include "sv-mask.h"
#include "sv-link.h"
#include "sv-store.h"
#include "sv-zoom.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define SV_ID "simply_vertical_source"
/* One piece of the layout on its own, for a horizontal scene (cut_to_piece). */
#define SV_PIECE_ID "simply_vertical_piece"
#define T(s) obs_module_text(s)

namespace {

constexpr uint32_t BLUR_W = 135, BLUR_H = 240;
constexpr float SOURCE_ASPECT = 16.0f / 9.0f;

/* Shared by every vertical source, set from the panel. */
std::atomic<int> g_switchMode{(int)SvSwitch::Glide};
std::atomic<int> g_switchMs{300};
std::atomic<int> g_switchFeel{SV_FEEL_SMOOTH};
std::atomic<int> g_switchDir{SV_DIR_LEFT};
std::atomic<bool> g_measuring{false};
std::atomic<float> g_costMs{-1.0f};
/* A layout switch the member asked for: its number and when, so each source takes it once. */
std::atomic<uint64_t> g_switchSeq{0};
std::atomic<uint64_t> g_switchAtNs{0};

/* One of the member's sources we read from: game or camera. */
struct SourceRef {
	std::string uuid;
	obs_weak_source_t *weak = nullptr;
	bool incShowing = false;
	bool incActive = false;
	uint64_t lastLookup = 0;
};

/* One piece's shape and frame as textures, made for a box of boxW wide. */
struct GpuLook {
	gs_texture_t *mask = nullptr;
	gs_texture_t *under = nullptr;
	gs_texture_t *over = nullptr;
	gs_texture_t *lamp = nullptr;
	gs_texture_t *pic = nullptr; /* brand kit pieces: the logo or channel name */
	std::vector<gs_texture_t *> pics; /* social rotator: one per link */
	SvBitmap underInfo, overInfo; /* sizes and offsets only, pixels dropped */
	uint32_t underColor = 0, overColor = 0;
	int lampW = 0, lampH = 0, lampX = 0, lampY = 0;
	float boxW = 1;
	float boxH = 0; /* 0 = scale both ways by the width */
};

/* A piece's look at its own size, and for a piece that zooms, made again at
   its zoomed size so its edges stay sharp when it is big. */
struct GpuArt {
	GpuLook base;
	GpuLook zoom;
	bool hasZoom = false;
};

/* A new layout handed from the settings thread to the drawing thread. */
struct Pending {
	bool clear = false; /* nothing to show any more: this PC was removed or unlinked */
	SvLayout layout;
	bool newArt = false;
	std::vector<SvPieceArt> art;
	std::vector<SvPieceArt> zoomArt; /* empty pictures for a piece that does not zoom */
	bool newBand = false;
	SvBitmap band;
	bool smooth = false; /* the member switched layout: change over smoothly */
};

/* How far on a piece is (0 gone, 1 on): moving from `from` to `to` since startNs,
   taking its show time for a whole way across. */
struct ShowState {
	float from = 1, to = 1;
	uint64_t startNs = 0;
};

/* A piece gone from the layout, still playing its hide, with the art it owns. */
struct Leaving {
	SvPiece piece;
	GpuArt art;
	ShowState st;
};

struct SvSource {
	obs_source_t *self = nullptr;

	/* Drawing thread only. */
	gs_effect_t *effect = nullptr;
	gs_texrender_t *gameTex = nullptr;
	gs_texrender_t *camTex = nullptr;
	gs_texrender_t *bgSmall = nullptr;
	gs_texrender_t *bgHalf = nullptr;
	gs_texrender_t *blurA = nullptr;
	gs_texrender_t *blurB = nullptr;
	gs_texrender_t *feedTex = nullptr;
	SvLayout layout;
	bool haveLayout = false;
	std::vector<GpuArt> art;
	gs_texture_t *band = nullptr;
	SvBitmap bandInfo;
	gs_texture_t *bandLamp = nullptr;
	int bandLampH = 0, bandLampY = 0;
	bool renderedThisFrame = false;
	bool gameReady = false, camReady = false;

	/* A smooth switch: the last layout, still live, fading or sliding out over
	   the new one. Its art is either its own (the shapes changed) or borrowed
	   from the new layout (only positions changed). Drawing thread only. */
	gs_texrender_t *transTex = nullptr;
	bool transActive = false;
	SvLayout oldLayout;
	std::vector<GpuArt> oldArt;
	bool oldOwnsArt = false;
	gs_texture_t *oldBand = nullptr;
	SvBitmap oldBandInfo;
	bool oldOwnsBand = false;
	uint64_t transStartNs = 0;
	int transMode = 0, transMs = 0, transFeel = 0, transDir = 0;
	/* Glide only: for each new piece, the old piece it moves from (-1 = none),
	   and which old pieces are moving, so neither layout also draws them. */
	std::vector<int> glideFrom;
	std::vector<char> newGlides, oldGlides;
	uint32_t gameW = 0, gameH = 0, camW = 0, camH = 0;

	/* Show and hide (SvShow): for each piece of the current layout, how far on it
	   is. Pieces gone from the layout that are still playing their hide keep their
	   own art here until it ends. `entering` and `oldLeaving` keep those pieces out
	   of a Glide. pieceTex fades one piece at a time. Drawing thread only. */
	std::vector<ShowState> shows;
	std::vector<Leaving> leaving;
	std::vector<char> entering, oldLeaving;
	gs_texrender_t *pieceTex = nullptr;

	/* Settings thread to drawing thread. */
	std::mutex mtx;
	std::unique_ptr<Pending> pending;
	SvLayout lastParsed; /* settings thread only: to tell when masks must be redrawn */
	bool lastParsedOk = false;
	float lastBandH = -1;
	std::string layoutPath;
	std::string lastJson; /* guarded by applyMtx: drawn again when a logo finishes loading */
	std::mutex applyMtx; /* layouts arrive from the settings window and from the account link */
	int linkListener = 0;
	uint64_t seenSwitch = 0; /* guarded by applyMtx */

	std::string wantGame, wantCam; /* guarded by mtx */

	/* A piece source (SV_PIECE_ID): draws only the piece with this name, cut out of
	   the layout by cut_to_piece. Set at create; pieceName guarded by applyMtx. */
	bool pieceOnly = false;
	std::string pieceName;
	int pieceGrow = -1; /* guarded by applyMtx, see cut_to_piece */

	/* A layer being dragged in the canvas panel: drawn at draftBox instead of its
	   saved place until the website has saved it. Guarded by mtx; the drawing
	   thread takes a copy once a frame into dragId and dragBox. */
	std::string draftId;
	SvRect draftBox;
	std::string dragId; /* drawing thread only */
	SvRect dragBox;

	/* A layout picked for this source alone (empty follows the panel), the panel's
	   layout kept for going back to it, and the picked layout last drawn. Guarded by pickMtx. */
	std::mutex pickMtx;
	std::string pickId, followJson, pickedJson;
	SourceRef game, cam;           /* tick thread only */

	std::atomic<bool> showing{false}, active{false};
	std::atomic<uint32_t> width{1080}, height{1920};

	/* Measuring. The test mode takes turns: 5 minutes drawing, 5 minutes not
	   drawing (sources still held, exactly as if the layout were switched
	   off), so one OBS session gives a fair on vs off comparison. */
	bool logStats = false;
	bool bench = false;
	float benchClock = 0;
	std::atomic<bool> benchDrawing{true};
	float statsTimer = 0;
	uint64_t statsFrames0 = 0, statsLagged0 = 0, statsVidTotal0 = 0, statsVidSkipped0 = 0;
	std::atomic<uint64_t> drawNs{0}, drawCount{0};

	/* Graphics card time, measured only while stats or test mode are on.
	   A small ring so each result is read a few frames later, when the card
	   has finished it, and never makes OBS wait. Drawing thread only. */
	static constexpr int GPU_RING = 6;
	struct GpuSlot {
		gs_timer_range_t *range = nullptr;
		gs_timer_t *feed = nullptr;   /* taking in the game and camera pictures */
		gs_timer_t *layout = nullptr; /* drawing our layout */
		bool used = false;
	} gpu[GPU_RING];
	int gpuNext = 0;
	bool gpuReady = false;
	double gpuFeedMs = 0, gpuLayoutMs = 0; /* sums since the last stats line */
	uint64_t gpuCount = 0;
	std::mutex gpuMtx; /* sums are read by the stats line on another thread */
};

/* ---------- small drawing helpers ---------- */

void quad(float x, float y, float w, float h)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
}

void set_vec4(gs_effect_t *e, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &v);
}

void set_colour(gs_effect_t *e, uint32_t argb, float alphaScale = 1.0f)
{
	set_vec4(e, "color", ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f,
		 ((argb >> 24) & 255) / 255.0f * alphaScale);
}

void set_tex(gs_effect_t *e, const char *name, gs_texture_t *t)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(e, name), t);
}

void run(gs_effect_t *e, const char *tech, float x, float y, float w, float h)
{
	while (gs_effect_loop(e, tech))
		quad(x, y, w, h);
}

/* Where a crop of the 16:9 game picture sits in the texture (0 to 1). A
   capture of another shape uses its 16:9 middle, like the website. */
void game_uv(gs_effect_t *e, const SvRect &c, uint32_t gw, uint32_t gh)
{
	float sw = (float)gw, sh = (float)gh, fw = sw, fh = sh, ox = 0, oy = 0;
	if (sw / sh > SOURCE_ASPECT) {
		fw = sh * SOURCE_ASPECT;
		ox = (sw - fw) / 2;
	} else {
		fh = sw / SOURCE_ASPECT;
		oy = (sh - fh) / 2;
	}
	set_vec4(e, "crop", (ox + c.x * fw) / sw, (oy + c.y * fh) / sh, c.w * fw / sw, c.h * fh / sh);
}

/* The camera fills its box, trimming the edges rather than squashing. */
void cam_uv(gs_effect_t *e, const SvRect &box, uint32_t cw, uint32_t ch)
{
	float k = std::max(box.w / cw, box.h / ch);
	float w = box.w / k, h = box.h / k;
	set_vec4(e, "crop", (cw - w) / 2 / cw, (ch - h) / 2 / ch, w / cw, h / ch);
}

gs_texture_t *upload(const SvBitmap &b)
{
	if (b.empty())
		return nullptr;
	const uint8_t *data = b.a.data();
	return gs_texture_create((uint32_t)b.w, (uint32_t)b.h, GS_R8, 1, &data, 0);
}

gs_texture_t *upload_colour(const SvColorBitmap &b)
{
	if (b.empty())
		return nullptr;
	const uint8_t *data = b.rgba.data();
	return gs_texture_create((uint32_t)b.w, (uint32_t)b.h, GS_RGBA, 1, &data, 0);
}

SvBitmap shape_only(const SvBitmap &b)
{
	SvBitmap s;
	s.w = b.w;
	s.h = b.h;
	s.ox = b.ox;
	s.oy = b.oy;
	return s;
}

void free_look(GpuLook &g)
{
	gs_texture_destroy(g.mask);
	gs_texture_destroy(g.under);
	gs_texture_destroy(g.over);
	gs_texture_destroy(g.lamp);
	gs_texture_destroy(g.pic);
	for (gs_texture_t *t : g.pics)
		gs_texture_destroy(t);
	g = GpuLook();
}

void free_art_list(std::vector<GpuArt> &list)
{
	for (auto &a : list) {
		free_look(a.base);
		free_look(a.zoom);
	}
	list.clear();
}

GpuLook upload_look(const SvPieceArt &a, const SvRect &box)
{
	const float boxW = box.w;
	GpuLook g;
	g.mask = upload(a.mask);
	g.under = upload(a.under);
	g.over = upload(a.over);
	g.underInfo = shape_only(a.under);
	g.overInfo = shape_only(a.over);
	g.underColor = a.underColor;
	g.overColor = a.overColor;
	g.lamp = upload_colour(a.lamp);
	g.pic = upload_colour(a.pic);
	for (const SvColorBitmap &b : a.pics)
		if (gs_texture_t *t = upload_colour(b))
			g.pics.push_back(t);
	g.lampW = a.lamp.w;
	g.lampH = a.lamp.h;
	g.lampX = a.lamp.ox;
	g.lampY = a.lamp.oy;
	g.boxW = std::max(1.0f, boxW);
	g.boxH = std::max(1.0f, box.h);
	return g;
}

/* A piece at its zoomed size: the same shape, corners and frame, grown. */
SvPiece zoomed_piece(const SvPiece &p)
{
	SvPiece q = p;
	const float k = p.zoomBox.w / std::max(1.0f, p.box.w);
	q.box = p.zoomBox;
	q.radius = p.radius * k;
	for (auto &c : q.corners)
		c *= k;
	q.round = p.round * k;
	q.feather = p.feather * k;
	return q;
}

void free_art(SvSource *s)
{
	free_art_list(s->art);
}

/* ---------- show and hide ---------- */

float show_amount(const ShowState &st, float ms, uint64_t now)
{
	const float span = std::fabs(st.to - st.from);
	if (span < 1e-4f || ms <= 0)
		return st.to;
	const float t = std::clamp((float)((double)(now - st.startNs) / 1e6) / (ms * span), 0.0f, 1.0f);
	return st.from + (st.to - st.from) * t;
}

/* The same layer in two layouts: the same website id, or the same kind and name. */
bool same_layer(const SvPiece &a, const SvPiece &b);

void free_leaving(SvSource *s)
{
	for (Leaving &lv : s->leaving) {
		free_look(lv.art.base);
		free_look(lv.art.zoom);
	}
	s->leaving.clear();
}

/* Where a piece is and how see-through, part way through its show or hide. The
   same numbers as the website, so Try it there matches. */
void show_at(const SvPiece &p, const SvRect &box, float on, float W, float H, SvRect &out, float &alpha)
{
	const float t = std::clamp(on, 0.0f, 1.0f);
	out = box;
	alpha = 1.0f;
	switch (p.show) {
	case SvShow::Fade:
		alpha = sv_ease(SV_FEEL_SMOOTH, t);
		break;
	case SvShow::Slide: {
		const float pad = 40.0f; /* past the edge, so a frame's glow is off the phone too */
		const float k = std::pow(1.0f - t, 3.0f);
		float dx = 0, dy = 0;
		switch (p.showFrom) {
		case SvShowFrom::Right:
			dx = W - box.x + pad;
			break;
		case SvShowFrom::Top:
			dy = -(box.y + box.h + pad);
			break;
		case SvShowFrom::Bottom:
			dy = H - box.y + pad;
			break;
		default:
			dx = -(box.x + box.w + pad);
			break;
		}
		out.x += dx * k;
		out.y += dy * k;
		break;
	}
	case SvShow::Pop: {
		const float sc = std::max(0.01f, 0.4f + 0.6f * sv_ease(SV_FEEL_BOUNCY, t));
		out.w = box.w * sc;
		out.h = box.h * sc;
		out.x = box.x + (box.w - out.w) / 2;
		out.y = box.y + (box.h - out.h) / 2;
		alpha = std::min(1.0f, t * 2.0f);
		break;
	}
	default:
		alpha = t > 0 ? 1.0f : 0.0f;
		break;
	}
}

/* Ends a smooth switch, letting go of whatever the old layout owned. */
void end_transition(SvSource *s)
{
	if (s->oldOwnsArt)
		free_art_list(s->oldArt);
	s->oldArt.clear();
	if (s->oldOwnsBand)
		gs_texture_destroy(s->oldBand);
	s->oldBand = nullptr;
	s->oldBandInfo = SvBitmap();
	s->oldOwnsArt = s->oldOwnsBand = false;
	s->oldLayout = SvLayout();
	s->transActive = false;
	s->glideFrom.clear();
	s->newGlides.clear();
	s->oldGlides.clear();
}

/* How much two game cut-outs overlap, 0 (not at all) to 1 (the same). */
float crop_overlap(const SvRect &a, const SvRect &b)
{
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
	if (x1 <= x0 || y1 <= y0)
		return 0.0f;
	const float both = (x1 - x0) * (y1 - y0);
	const float either = a.w * a.h + b.w * b.h - both;
	return either > 0 ? both / either : 0.0f;
}

bool same_name(const std::string &a, const std::string &b)
{
	if (a.empty() || a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
			return false;
	return true;
}

bool same_layer(const SvPiece &a, const SvPiece &b)
{
	if (!a.id.empty() && a.id == b.id)
		return true;
	return a.src == b.src && same_name(a.name, b.name);
}

bool zooming(const SvPiece &p)
{
	return p.zoom && sv_zoom_amount(p.zoomKey) > 0.001f;
}

/* Pairs each new piece with the old piece it glides from: the same name first,
   then the camera with the camera, then game pieces showing the same part of
   the game. Anything left over fades. */
void match_glides(SvSource *s)
{
	const auto &o = s->oldLayout.pieces;
	const auto &n = s->layout.pieces;
	const size_t no = std::min(o.size(), s->oldArt.size());
	const size_t nn = std::min(n.size(), s->art.size());
	s->glideFrom.assign(n.size(), -1);
	s->newGlides.assign(n.size(), 0);
	s->oldGlides.assign(o.size(), 0);
	/* Pieces playing a show or hide of their own never glide. */
	auto open_new = [&](size_t j) {
		return !s->newGlides[j] && !zooming(n[j]) && !(j < s->entering.size() && s->entering[j]);
	};
	auto open_old = [&](size_t i) {
		return !s->oldGlides[i] && !zooming(o[i]) && !(i < s->oldLeaving.size() && s->oldLeaving[i]);
	};
	auto pair = [&](size_t j, size_t i) {
		s->glideFrom[j] = (int)i;
		s->newGlides[j] = 1;
		s->oldGlides[i] = 1;
	};
	for (size_t j = 0; j < nn; j++) {
		if (!open_new(j) || n[j].name.empty())
			continue;
		for (size_t i = 0; i < no; i++)
			if (open_old(i) && o[i].src == n[j].src && same_name(o[i].name, n[j].name)) {
				pair(j, i);
				break;
			}
	}
	for (size_t j = 0; j < nn; j++) {
		if (!open_new(j) || n[j].src != SvSrc::Cam)
			continue;
		for (size_t i = 0; i < no; i++)
			if (open_old(i) && o[i].src == SvSrc::Cam) {
				pair(j, i);
				break;
			}
	}
	for (size_t j = 0; j < nn; j++) {
		if (!open_new(j) || n[j].src != SvSrc::Game)
			continue;
		int best = -1;
		float bestOverlap = 0.25f;
		for (size_t i = 0; i < no; i++) {
			if (!open_old(i) || o[i].src != SvSrc::Game)
				continue;
			const float v = crop_overlap(o[i].crop, n[j].crop);
			if (v >= bestOverlap) {
				bestOverlap = v;
				best = (int)i;
			}
		}
		if (best >= 0)
			pair(j, (size_t)best);
	}
	const long moving = (long)std::count(s->newGlides.begin(), s->newGlides.end(), (char)1);
	obs_log(LOG_INFO, "Layout glide: %ld piece(s) move, %ld fade out, %ld fade in", moving,
		(long)o.size() - moving, (long)n.size() - moving);
}

/* ---------- the member's sources ---------- */

void ref_release_counts(SourceRef &r)
{
	obs_source_t *src = r.weak ? obs_weak_source_get_source(r.weak) : nullptr;
	if (src) {
		if (r.incShowing)
			obs_source_dec_showing(src);
		if (r.incActive)
			obs_source_dec_active(src);
		obs_source_release(src);
	}
	r.incShowing = r.incActive = false;
}

void ref_clear(SourceRef &r)
{
	ref_release_counts(r);
	obs_weak_source_release(r.weak);
	r.weak = nullptr;
	r.uuid.clear();
}

/* Keeps a source pointed at the wanted one, and keeps it showing and active
   while we are, so a capture card or game capture never goes to sleep under
   the vertical layout. Runs once per frame, costs a lookup only on change. */
void ref_sync(SvSource *s, SourceRef &r, const std::string &want)
{
	if (want != r.uuid) {
		ref_clear(r);
		r.uuid = want;
		r.lastLookup = 0;
	}
	if (r.uuid.empty())
		return;

	obs_source_t *src = r.weak ? obs_weak_source_get_source(r.weak) : nullptr;
	if (!src) {
		/* Gone or not found yet: look again at most every two seconds. */
		if (r.weak) {
			obs_weak_source_release(r.weak);
			r.weak = nullptr;
			r.incShowing = r.incActive = false;
		}
		const uint64_t now = os_gettime_ns();
		if (now - r.lastLookup < 2000000000ULL)
			return;
		r.lastLookup = now;
		src = obs_get_source_by_uuid(r.uuid.c_str());
		if (!src)
			return;
		if (src == s->self || std::strcmp(obs_source_get_id(src), SV_ID) == 0 ||
		    std::strcmp(obs_source_get_id(src), SV_PIECE_ID) == 0) {
			obs_source_release(src);
			return;
		}
		r.weak = obs_source_get_weak_source(src);
	}

	const bool wantShow = s->showing.load();
	const bool wantActive = s->active.load();
	if (wantShow != r.incShowing) {
		wantShow ? obs_source_inc_showing(src) : obs_source_dec_showing(src);
		r.incShowing = wantShow;
	}
	if (wantActive != r.incActive) {
		wantActive ? obs_source_inc_active(src) : obs_source_dec_active(src);
		r.incActive = wantActive;
	}
	obs_source_release(src);
}

/* Copies a source's current picture into a texture, once per frame. */
bool grab(SvSource *s, const SourceRef &r, gs_texrender_t *tr, uint32_t &w, uint32_t &h)
{
	obs_source_t *src = r.weak ? obs_weak_source_get_source(r.weak) : nullptr;
	if (!src)
		return false;
	w = obs_source_get_width(src);
	h = obs_source_get_height(src);
	bool ok = false;
	if (w && h) {
		gs_texrender_reset(tr);
		if (gs_texrender_begin(tr, w, h)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			obs_source_video_render(src);
			gs_texrender_end(tr);
			ok = true;
		}
	}
	obs_source_release(src);
	(void)s;
	return ok;
}

/* In test mode's "not drawing" turns the game and camera are still taken in
   each frame, as the member's normal scene would, so the comparison shows
   only what our layout adds. Drawn into a tiny texture so it costs nothing
   of ours. */
void feed_only(const SourceRef &r, gs_texrender_t *tr)
{
	obs_source_t *src = r.weak ? obs_weak_source_get_source(r.weak) : nullptr;
	if (!src)
		return;
	const uint32_t w = obs_source_get_width(src), h = obs_source_get_height(src);
	if (w && h) {
		gs_texrender_reset(tr);
		if (gs_texrender_begin(tr, 4, 4)) {
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			obs_source_video_render(src);
			gs_texrender_end(tr);
		}
	}
	obs_source_release(src);
}

/* ---------- graphics card timing (stats and test mode only) ---------- */

void gpu_free(SvSource *s)
{
	for (auto &g : s->gpu) {
		gs_timer_range_destroy(g.range);
		gs_timer_destroy(g.feed);
		gs_timer_destroy(g.layout);
		g = SvSource::GpuSlot();
	}
	s->gpuReady = false;
}

bool gpu_make(SvSource *s)
{
	if (s->gpuReady)
		return true;
	for (auto &g : s->gpu) {
		g.range = gs_timer_range_create();
		g.feed = gs_timer_create();
		g.layout = gs_timer_create();
		if (!g.range || !g.feed || !g.layout) {
			gpu_free(s);
			return false;
		}
	}
	s->gpuNext = 0;
	s->gpuReady = true;
	return true;
}

/* Reads the oldest slot (finished several frames ago) and adds it to the sums. */
void gpu_collect(SvSource *s, SvSource::GpuSlot &g)
{
	if (!g.used)
		return;
	g.used = false;
	bool disjoint = true;
	uint64_t freq = 0, feed = 0, layout = 0;
	if (!gs_timer_range_get_data(g.range, &disjoint, &freq) || disjoint || !freq)
		return;
	if (!gs_timer_get_data(g.feed, &feed) || !gs_timer_get_data(g.layout, &layout))
		return;
	const double feedMs = feed * 1000.0 / freq, layoutMs = layout * 1000.0 / freq;
	/* A steady reading for the panel: each frame moves it a little. */
	const float cost = (float)(feedMs + layoutMs);
	const float prev = g_costMs.load();
	g_costMs = prev < 0 ? cost : prev * 0.95f + cost * 0.05f;
	std::lock_guard<std::mutex> lock(s->gpuMtx);
	s->gpuFeedMs += feedMs;
	s->gpuLayoutMs += layoutMs;
	s->gpuCount++;
}

/* Draws into one of the small background textures. */
template<typename F> bool small_pass(gs_texrender_t *tr, uint32_t w, uint32_t h, F draw)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	draw();
	gs_texrender_end(tr);
	return true;
}

/* ---------- layout loading (settings thread) ---------- */

/* This PC was removed or unlinked: stop showing the layout straight away. */
void clear_layout(SvSource *s)
{
	std::lock_guard<std::mutex> applyLock(s->applyMtx);
	sv_zoom_forget(s->self);
	s->lastJson.clear();
	s->lastParsedOk = false;
	s->lastBandH = -1;
	auto p = std::make_unique<Pending>();
	p->clear = true;
	std::lock_guard<std::mutex> lock(s->mtx);
	s->pending = std::move(p);
}

/* ---------- a piece on its own, for a horizontal scene ---------- */

constexpr float PIECE_PAD = 40.0f; /* room round the piece for its frame's glow */

std::string trimmed_lower(const std::string &in)
{
	size_t a = 0, b = in.size();
	while (a < b && std::isspace((unsigned char)in[a]))
		a++;
	while (b > a && std::isspace((unsigned char)in[b - 1]))
		b--;
	std::string out = in.substr(a, b - a);
	for (char &c : out)
		c = (char)std::tolower((unsigned char)c);
	return out;
}

bool same_piece_name(const std::string &a, const std::string &b)
{
	return !a.empty() && trimmed_lower(a) == trimmed_lower(b);
}

/* Cuts a layout down to the one piece a piece source shows: no background, no
   gameplay band, sized to the most room the piece takes (zoomed, when its zoom
   grows it). At rest it sits in the corner or edge it grows out of on the vertical,
   so a zoom moves the same way on both canvases and the same key works on both.
   A layout without that piece shows nothing and keeps the size it had. */
/* grow: the corner it grows from (0 top left, 1 top right, 2 bottom left,
   3 bottom right, 4 the middle), or -1 for the same as on the vertical. */
void cut_to_piece(SvLayout &l, const std::string &want, int grow, uint32_t keepW, uint32_t keepH)
{
	l.bg.mode = SvBgMode::None;
	l.main.show = false;
	const SvPiece *found = nullptr;
	for (const SvPiece &p : l.pieces)
		if (!want.empty() && (p.id == want || same_piece_name(p.name, want))) {
			found = &p;
			break;
		}
	if (!found) {
		l.pieces.clear();
		l.w = (int)std::max<uint32_t>(16, keepW);
		l.h = (int)std::max<uint32_t>(16, keepH);
		return;
	}
	SvPiece p = *found;
	const SvRect b = p.box;
	const bool zooms = p.zoom && p.zoomBox.w > 1 && p.zoomBox.h > 1;
	const SvRect z = zooms ? p.zoomBox : b;
	const bool grows = z.w * z.h >= b.w * b.h;
	const SvRect big = grows ? z : b;
	const SvRect small = grows ? b : z;
	/* The point that stays still while the piece zooms on the vertical, as a
	   fraction of the piece. Inside the piece it grows round that point; outside it
	   grows away from that side, so it is kept to the edge. */
	float u = 0.5f, v = 0.5f;
	const float kx = z.w / std::max(1.0f, b.w), ky = z.h / std::max(1.0f, b.h);
	if (std::fabs(1.0f - kx) > 0.01f)
		u = std::clamp(((z.x - b.x * kx) / (1.0f - kx) - b.x) / std::max(1.0f, b.w), 0.0f, 1.0f);
	if (std::fabs(1.0f - ky) > 0.01f)
		v = std::clamp(((z.y - b.y * ky) / (1.0f - ky) - b.y) / std::max(1.0f, b.h), 0.0f, 1.0f);
	if (grow >= 0 && grow <= 3) {
		u = (grow == 1 || grow == 3) ? 1.0f : 0.0f;
		v = grow >= 2 ? 1.0f : 0.0f;
	} else if (grow == 4) {
		u = v = 0.5f;
	}
	SvRect bigAt;
	bigAt.x = PIECE_PAD;
	bigAt.y = PIECE_PAD;
	bigAt.w = big.w;
	bigAt.h = big.h;
	SvRect smallAt;
	smallAt.x = PIECE_PAD + u * (big.w - small.w);
	smallAt.y = PIECE_PAD + v * (big.h - small.h);
	smallAt.w = small.w;
	smallAt.h = small.h;
	p.box = grows ? smallAt : bigAt;
	if (zooms)
		p.zoomBox = grows ? bigAt : smallAt;
	l.w = (int)std::ceil(big.w + 2 * PIECE_PAD);
	l.h = (int)std::ceil(big.h + 2 * PIECE_PAD);
	l.pieces.clear();
	l.pieces.push_back(std::move(p));
}

/* redraw: make every piece's art again even if no shape changed (a logo just finished loading). */
void apply_json(SvSource *s, const char *json, const char *from, bool redraw = false)
{
	std::lock_guard<std::mutex> applyLock(s->applyMtx);
	SvLayout l;
	std::string err;
	if (!sv_layout_parse(json, l, err)) {
		obs_log(LOG_WARNING, "layout from %s not used: %s", from, err.c_str());
		return;
	}
	s->lastJson = json;
	if (s->pieceOnly)
		cut_to_piece(l, s->pieceName, s->pieceGrow, s->width, s->height);
	auto p = std::make_unique<Pending>();
	if (redraw || !s->lastParsedOk || sv_layout_masks_differ(s->lastParsed, l)) {
		p->newArt = true;
		p->art.reserve(l.pieces.size());
		p->zoomArt.reserve(l.pieces.size());
		for (auto &piece : l.pieces) {
			p->art.push_back(sv_piece_art(piece));
			if (piece.src == SvSrc::Logo || piece.src == SvSrc::Name)
				p->art.back().pic = sv_brand_picture(piece);
			else if (piece.src == SvSrc::Social)
				p->art.back().pics = sv_brand_socials(piece);
			const float k = piece.zoomBox.w / std::max(1.0f, piece.box.w);
			/* Only worth making when it grows: shrinking looks fine from the normal art. */
			p->zoomArt.push_back(piece.zoom && k > 1.05f ? sv_piece_art(zoomed_piece(piece), k) : SvPieceArt());
		}
	}
	sv_zoom_note_layout(s->self, l);
	const bool band = l.main.show && l.main.h < l.h - 4;
	const float bandH = band ? std::round(l.main.h) : 0;
	if (bandH != s->lastBandH) {
		p->newBand = true;
		if (band)
			p->band = sv_band_shadow(l.w, (int)bandH);
		s->lastBandH = bandH;
	}
	p->layout = l;
	s->lastParsed = l;
	s->lastParsedOk = true;
	s->width = (uint32_t)l.w;
	s->height = (uint32_t)l.h;
	const uint64_t seq = g_switchSeq.load();
	if (seq != s->seenSwitch) {
		s->seenSwitch = seq;
		p->smooth = os_gettime_ns() - g_switchAtNs.load() < 1500000000ULL;
	}
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		if (s->pending && s->pending->smooth)
			p->smooth = true;
		if (s->pending && s->pending->newArt && !p->newArt) {
			/* A move arrived before the last reshape was drawn: keep the reshape. */
			p->newArt = true;
			p->art = std::move(s->pending->art);
			p->zoomArt = std::move(s->pending->zoomArt);
		}
		if (s->pending && s->pending->newBand && !p->newBand) {
			p->newBand = true;
			p->band = std::move(s->pending->band);
		}
		s->pending = std::move(p);
	}
	obs_log(LOG_INFO, "layout loaded from %s: %zu pieces", from, l.pieces.size());
}

void load_file(SvSource *s)
{
	/* A linked PC shows the account's layout; the file is only for testing without one. */
	if (s->layoutPath.empty() || sv_link_is_linked())
		return;
	char *text = os_quick_read_utf8_file(s->layoutPath.c_str());
	if (!text) {
		obs_log(LOG_WARNING, "could not read the layout file");
		return;
	}
	apply_json(s, text, "file");
	bfree(text);
}

/* Shows what this source should: the layout picked for it alone while that still exists,
   otherwise the panel's. Any thread. fromSettings: the pick itself just changed. */
void show_chosen(SvSource *s, bool fromSettings)
{
	std::string pick, follow, last;
	{
		std::lock_guard<std::mutex> lock(s->pickMtx);
		pick = s->pickId;
		follow = s->followJson;
		last = s->pickedJson;
	}
	std::string json;
	if (!pick.empty()) {
		for (const SvLayoutChoice &c : sv_link_presets())
			if (c.id == pick)
				json = c.drawList;
	}
	if (!json.empty()) {
		if (!fromSettings && json == last)
			return;
		{
			std::lock_guard<std::mutex> lock(s->pickMtx);
			s->pickedJson = json;
		}
		apply_json(s, json.c_str(), "the layout picked for this source");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s->pickMtx);
		s->pickedJson.clear();
	}
	/* Already following the panel: nothing to change. */
	if (!fromSettings && last.empty())
		return;
	if (!follow.empty())
		apply_json(s, follow.c_str(), "account");
	else if (fromSettings && !s->layoutPath.empty())
		load_file(s);
	else
		clear_layout(s);
}

/* ---------- every Simply Vertical source, so a logo that finishes loading is drawn in all of them ---------- */

std::mutex g_sourcesMtx;
std::vector<SvSource *> g_sources;

void redraw_for_logo()
{
	std::lock_guard<std::mutex> lock(g_sourcesMtx);
	for (SvSource *s : g_sources) {
		std::string json;
		{
			std::lock_guard<std::mutex> applyLock(s->applyMtx);
			json = s->lastJson;
		}
		if (!json.empty())
			apply_json(s, json.c_str(), "logo", true);
	}
}

/* ---------- source callbacks ---------- */

const char *sv_get_name(void *)
{
	return T("SimplyVertical");
}

const char *sv_piece_get_name(void *)
{
	return T("SimplyVerticalPiece");
}

/* The panel, told when a source's game or camera changes. */
std::mutex g_picksListenerMtx;
std::function<void()> g_picksListener;

void sv_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<SvSource *>(data);
	bool picksChanged;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		const std::string game = obs_data_get_string(settings, "game_source");
		const std::string cam = obs_data_get_string(settings, "cam_source");
		picksChanged = game != s->wantGame || cam != s->wantCam;
		s->wantGame = game;
		s->wantCam = cam;
	}
	if (picksChanged) {
		std::function<void()> tell;
		{
			std::lock_guard<std::mutex> lock(g_picksListenerMtx);
			tell = g_picksListener;
		}
		if (tell)
			tell();
	}
#ifdef SV_DEV_BUILD
	/* Testing options, only in a Dev build. A public build ignores any left in a saved scene. */
	s->logStats = obs_data_get_bool(settings, "log_stats");
	const bool bench = obs_data_get_bool(settings, "bench");
	if (bench != s->bench) {
		s->bench = bench;
		s->benchClock = 0;
		s->benchDrawing = true;
	}
	const std::string path = obs_data_get_string(settings, "layout_file");
	if (path != s->layoutPath) {
		s->layoutPath = path;
		load_file(s);
	}
#endif
	const std::string pick = obs_data_get_string(settings, "layout_pick");
	bool changed;
	{
		std::lock_guard<std::mutex> lock(s->pickMtx);
		changed = pick != s->pickId;
		s->pickId = pick;
	}
	if (changed)
		show_chosen(s, true);
	if (s->pieceOnly) {
		/* Another piece picked: cut the layout already here again. */
		const std::string name = obs_data_get_string(settings, "piece_name");
		const int grow = (int)obs_data_get_int(settings, "piece_grow");
		std::string json;
		{
			std::lock_guard<std::mutex> applyLock(s->applyMtx);
			if (name != s->pieceName || grow != s->pieceGrow) {
				s->pieceName = name;
				s->pieceGrow = grow;
				json = s->lastJson;
			}
		}
		if (!json.empty())
			apply_json(s, json.c_str(), "the piece picked for this source");
	}
}

void *create_source(obs_data_t *settings, obs_source_t *source, bool pieceOnly)
{
	auto *s = new SvSource();
	s->self = source;
	if (pieceOnly) {
		s->pieceOnly = true;
		s->pieceName = obs_data_get_string(settings, "piece_name");
		s->pieceGrow = (int)obs_data_get_int(settings, "piece_grow");
		s->width = 400;
		s->height = 400;
	}

	char *path = obs_module_file("simply-vertical.effect");
	obs_enter_graphics();
	if (path) {
		char *errors = nullptr;
		s->effect = gs_effect_create_from_file(path, &errors);
		if (!s->effect)
			obs_log(LOG_ERROR, "drawing effect failed to load: %s", errors ? errors : "unknown");
		bfree(errors);
	}
	s->gameTex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->camTex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->bgHalf = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->bgSmall = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->blurA = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->blurB = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->feedTex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->transTex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const SvColorBitmap bandLamp = sv_band_lamp(1080);
	s->bandLamp = upload_colour(bandLamp);
	s->bandLampH = bandLamp.h;
	s->bandLampY = bandLamp.oy;
	obs_leave_graphics();
	bfree(path);

	{
		std::lock_guard<std::mutex> lock(g_sourcesMtx);
		g_sources.push_back(s);
		sv_brand_on_logo(redraw_for_logo);
	}

	SvLinkListener l;
	l.onLayout = [s](const std::string &json) {
		bool picked;
		{
			std::lock_guard<std::mutex> lock(s->pickMtx);
			s->followJson = json;
			picked = !s->pickId.empty();
		}
		/* A source with its own layout keeps it, unless that layout was deleted. */
		if (picked)
			show_chosen(s, false);
		else if (json.empty())
			clear_layout(s);
		else
			apply_json(s, json.c_str(), "account");
	};
	/* The saved layouts may have changed on the website: a picked one is drawn again. */
	l.onStatus = [s]() {
		show_chosen(s, false);
		obs_source_update_properties(s->self);
	};
	std::string current;
	s->linkListener = sv_link_listen(std::move(l), &current);
	{
		std::lock_guard<std::mutex> lock(s->pickMtx);
		s->followJson = current;
	}
	if (!current.empty())
		apply_json(s, current.c_str(), "account");

	sv_update(s, settings);
	return s;
}

void *sv_create(obs_data_t *settings, obs_source_t *source)
{
	return create_source(settings, source, false);
}

void *sv_piece_create(obs_data_t *settings, obs_source_t *source)
{
	return create_source(settings, source, true);
}

void sv_destroy(void *data)
{
	auto *s = static_cast<SvSource *>(data);
	{
		std::lock_guard<std::mutex> lock(g_sourcesMtx);
		g_sources.erase(std::remove(g_sources.begin(), g_sources.end(), s), g_sources.end());
	}
	sv_link_unlisten(s->linkListener);
	sv_zoom_forget(s->self);
	ref_clear(s->game);
	ref_clear(s->cam);
	obs_enter_graphics();
	end_transition(s);
	free_art(s);
	free_leaving(s);
	gs_texrender_destroy(s->pieceTex);
	gs_texture_destroy(s->band);
	gs_texrender_destroy(s->transTex);
	gs_texture_destroy(s->bandLamp);
	gs_texrender_destroy(s->gameTex);
	gs_texrender_destroy(s->camTex);
	gs_texrender_destroy(s->bgHalf);
	gs_texrender_destroy(s->bgSmall);
	gs_texrender_destroy(s->blurA);
	gs_texrender_destroy(s->blurB);
	gs_texrender_destroy(s->feedTex);
	gpu_free(s);
	gs_effect_destroy(s->effect);
	obs_leave_graphics();
	delete s;
}

uint32_t sv_width(void *data)
{
	return static_cast<SvSource *>(data)->width;
}

uint32_t sv_height(void *data)
{
	return static_cast<SvSource *>(data)->height;
}

void sv_show(void *data)
{
	static_cast<SvSource *>(data)->showing = true;
}
void sv_hide(void *data)
{
	static_cast<SvSource *>(data)->showing = false;
}
void sv_activate(void *data)
{
	static_cast<SvSource *>(data)->active = true;
}
void sv_deactivate(void *data)
{
	static_cast<SvSource *>(data)->active = false;
}

void log_stats_line(SvSource *s, uint64_t frames, uint64_t lagged, uint64_t vidTotal, uint64_t vidSkipped)
{
	const uint64_t draws = s->drawCount.exchange(0);
	const uint64_t ns = s->drawNs.exchange(0);
	double feedMs = 0, layoutMs = 0;
	uint64_t gpuN = 0;
	{
		std::lock_guard<std::mutex> lock(s->gpuMtx);
		feedMs = s->gpuFeedMs;
		layoutMs = s->gpuLayoutMs;
		gpuN = s->gpuCount;
		s->gpuFeedMs = s->gpuLayoutMs = 0;
		s->gpuCount = 0;
	}
	obs_log(LOG_INFO,
		"stats: %s shown=%d avg frame %.3f ms | missed (rendering lag) %llu of %llu | skipped (encoding lag) %llu of %llu | our draw %.3f ms cpu x %llu | gpu feed %.4f ms layout %.4f ms x %llu",
		!s->bench ? "" : s->benchDrawing.load() ? "[DRAWING]" : "[NOT DRAWING]", s->showing.load() ? 1 : 0,
		obs_get_average_frame_time_ns() / 1e6, (unsigned long long)(lagged - s->statsLagged0),
		(unsigned long long)(frames - s->statsFrames0), (unsigned long long)(vidSkipped - s->statsVidSkipped0),
		(unsigned long long)(vidTotal - s->statsVidTotal0), draws ? ns / 1e6 / draws : 0.0,
		(unsigned long long)draws, gpuN ? feedMs / gpuN : 0.0, gpuN ? layoutMs / gpuN : 0.0,
		(unsigned long long)gpuN);
	s->statsTimer = 0;
	s->statsFrames0 = frames;
	s->statsLagged0 = lagged;
	s->statsVidTotal0 = vidTotal;
	s->statsVidSkipped0 = vidSkipped;
}

void log_stats(SvSource *s, float seconds)
{
	video_t *video = obs_get_video();
	const uint64_t frames = obs_get_total_frames();
	const uint64_t lagged = obs_get_lagged_frames();
	const uint64_t vidTotal = video ? video_output_get_total_frames(video) : 0;
	const uint64_t vidSkipped = video ? video_output_get_skipped_frames(video) : 0;
	if (s->bench) {
		s->benchClock += seconds;
		const bool drawing = ((int)(s->benchClock / 300.0f)) % 2 == 0;
		if (drawing != s->benchDrawing.load()) {
			/* Report the phase that just ended, then start the next one clean. */
			log_stats_line(s, frames, lagged, vidTotal, vidSkipped);
			s->benchDrawing = drawing;
			obs_log(LOG_INFO, "test mode: now %s", drawing ? "DRAWING" : "NOT DRAWING");
			return;
		}
	}
	if (!s->logStats && !s->bench) {
		s->statsTimer = 0;
		s->statsFrames0 = frames;
		s->statsLagged0 = lagged;
		s->statsVidTotal0 = vidTotal;
		s->statsVidSkipped0 = vidSkipped;
		return;
	}
	s->statsTimer += seconds;
	if (s->statsTimer < 10.0f)
		return;
	log_stats_line(s, frames, lagged, vidTotal, vidSkipped);
}

void sv_tick(void *data, float seconds)
{
	auto *s = static_cast<SvSource *>(data);
	std::string wantGame, wantCam;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		wantGame = s->wantGame;
		wantCam = s->wantCam;
	}
	ref_sync(s, s->game, wantGame);
	ref_sync(s, s->cam, wantCam);
	s->renderedThisFrame = false;
	log_stats(s, seconds);
}

/* Hide game HUD (sv-source.h), and when a vertical last drew a layer it would hide. */
std::atomic<bool> g_hudHidden{false};
std::atomic<uint64_t> g_hudSeenNs{0};

/* A small game layer: cut from less than a quarter of the game picture. Bigger
   game layers are the member's gameplay and always stay. */
bool is_hud(const SvPiece &p)
{
	/* The gameplay (sent as a layer with the id "main") is never HUD, however far it is zoomed in. */
	return p.src == SvSrc::Game && p.id != "main" && p.crop.w * p.crop.h < 0.25f;
}

/* Takes a waiting layout and turns its masks into textures. Drawing thread. */
void take_pending(SvSource *s)
{
	std::unique_ptr<Pending> p;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		p = std::move(s->pending);
		if (s->dragId != s->draftId)
			s->dragId = s->draftId;
		s->dragBox = s->draftBox;
	}
	if (!p)
		return;
	/* A layout arriving mid-switch ends the switch first. */
	end_transition(s);
	if (p->clear) {
		free_leaving(s);
		s->shows.clear();
		free_art(s);
		gs_texture_destroy(s->band);
		s->band = nullptr;
		s->bandInfo = SvBitmap();
		s->layout = SvLayout();
		s->haveLayout = false;
		return;
	}
	const int mode = g_switchMode.load();
	const int ms = g_switchMs.load();
	const bool smooth = p->smooth && s->haveLayout && mode != (int)SvSwitch::Cut && ms > 0 && s->transTex;

	/* Show and hide. A layer in both layouts keeps how far on it is. A new one
	   with a motion of its own starts off and comes on; an old one with a motion
	   of its own takes its art and plays its hide. Only when the art is made
	   again, since otherwise the pieces stay the same. */
	const uint64_t now = os_gettime_ns();
	const auto &np = p->layout.pieces;
	std::vector<ShowState> nextShows(np.size());
	/* A game HUD layer arriving while Hide game HUD is on starts hidden, not playing its hide. */
	for (size_t j = 0; j < np.size(); j++)
		if (is_hud(np[j]) && g_hudHidden.load())
			nextShows[j] = ShowState{0.0f, 0.0f, now};
	s->entering.assign(np.size(), 0);
	s->oldLeaving.assign(s->layout.pieces.size(), 0);
	if (s->haveLayout) {
		const auto &op = s->layout.pieces;
		std::vector<char> used(op.size(), 0);
		for (size_t j = 0; j < np.size(); j++) {
			size_t i = 0;
			while (i < op.size() && (used[i] || !same_layer(op[i], np[j])))
				i++;
			if (i < op.size()) {
				used[i] = 1;
				if (i < s->shows.size())
					nextShows[j] = s->shows[i];
			} else if (p->newArt && np[j].show != SvShow::None && nextShows[j].to > 0.0f) {
				nextShows[j] = ShowState{0.0f, 1.0f, now};
				s->entering[j] = 1;
			}
		}
		for (size_t i = 0; p->newArt && i < op.size() && i < s->art.size(); i++) {
			if (used[i] || op[i].show == SvShow::None)
				continue;
			const float on = i < s->shows.size() ? show_amount(s->shows[i], op[i].showMs, now) : 1.0f;
			if (on <= 0.0005f)
				continue;
			Leaving lv;
			lv.piece = op[i];
			lv.art = s->art[i];
			lv.st = ShowState{on, 0.0f, now};
			s->art[i] = GpuArt(); /* the leaving piece owns it now */
			s->leaving.push_back(std::move(lv));
			s->oldLeaving[i] = 1;
		}
	}

	if (smooth) {
		/* Keep the current layout to switch away from. */
		s->oldLayout = s->layout;
		s->oldArt = s->art;
		s->oldOwnsArt = p->newArt;
		s->oldBand = s->band;
		s->oldBandInfo = s->bandInfo;
		s->oldOwnsBand = p->newBand;
		s->transActive = true;
		s->transStartNs = os_gettime_ns();
		s->transMode = mode;
		s->transMs = ms;
		s->transFeel = g_switchFeel.load();
		s->transDir = g_switchDir.load();
		if (p->newArt)
			s->art.clear(); /* now owned by the old layout */
		if (p->newBand)
			s->band = nullptr;
	}
	s->layout = std::move(p->layout);
	s->shows = std::move(nextShows);
	s->haveLayout = true;
	if (p->newArt) {
		free_art(s);
		const auto &pieces = s->layout.pieces;
		for (size_t i = 0; i < p->art.size() && i < pieces.size(); i++) {
			GpuArt g;
			g.base = upload_look(p->art[i], pieces[i].box);
			if (i < p->zoomArt.size() && !p->zoomArt[i].mask.empty()) {
				g.zoom = upload_look(p->zoomArt[i], pieces[i].zoomBox);
				g.hasZoom = true;
			}
			s->art.push_back(std::move(g));
		}
	}
	if (p->newBand) {
		gs_texture_destroy(s->band);
		s->band = upload(p->band);
		s->bandInfo = shape_only(p->band);
	}
	if (s->transActive && s->transMode == (int)SvSwitch::Glide)
		match_glides(s);
}

/* One layout to draw: the current one, or the one a smooth switch leaves. */
struct View {
	const SvLayout &layout;
	const std::vector<GpuArt> &art;
	gs_texture_t *band;
	const SvBitmap &bandInfo;
};

View current_view(SvSource *s)
{
	return View{s->layout, s->art, s->band, s->bandInfo};
}

View old_view(SvSource *s)
{
	return View{s->oldLayout, s->oldArt, s->oldBand, s->oldBandInfo};
}

void draw_background(SvSource *s, const View &v)
{
	gs_effect_t *e = s->effect;
	const SvLayout &l = v.layout;
	/* The background's own box (the whole canvas unless the website moved it), turned and
	   mirrored around its middle like any layer. */
	const SvRect &b = l.bg.box;
	const float W = b.w, H = b.h;
	const bool turned = l.bg.rot != 0.0f || l.bg.flipH || l.bg.flipV;
	enum gs_cull_mode cull = GS_NEITHER;
	if (turned) {
		const float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
		gs_matrix_push();
		gs_matrix_translate3f(cx, cy, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(l.bg.rot));
		gs_matrix_scale3f(l.bg.flipH ? -1.0f : 1.0f, l.bg.flipV ? -1.0f : 1.0f, 1.0f);
		gs_matrix_translate3f(-cx, -cy, 0.0f);
		cull = gs_get_cull_mode();
		gs_set_cull_mode(GS_NEITHER);
	}

	if (l.bg.mode == SvBgMode::Color) {
		set_colour(e, l.bg.color);
		run(e, "Solid", b.x, b.y, W, H);
	} else if (l.bg.mode == SvBgMode::Blur && s->gameReady) {
		gs_texture_t *game = gs_texrender_get_texture(s->gameTex);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		/* Shrink in two steps so the small picture does not shimmer. */
		small_pass(s->bgHalf, BLUR_W * 2, BLUR_H * 2, [&] {
			set_tex(e, "source", game);
			game_uv(e, l.bg.crop, s->gameW, s->gameH);
			run(e, "Crop", 0, 0, BLUR_W * 2, BLUR_H * 2);
		});
		small_pass(s->bgSmall, BLUR_W, BLUR_H, [&] {
			set_tex(e, "source", gs_texrender_get_texture(s->bgHalf));
			set_vec4(e, "crop", 0, 0, 1, 1);
			run(e, "Crop", 0, 0, BLUR_W, BLUR_H);
		});
		const float sigma = std::min(24.0f / 3.0f, std::max(1.0f, l.bg.blur * BLUR_W / W));
		small_pass(s->blurA, BLUR_W, BLUR_H, [&] {
			set_tex(e, "image", gs_texrender_get_texture(s->bgSmall));
			set_vec4(e, "misc", 1.0f / BLUR_W, 0, sigma, 0);
			run(e, "Blur", 0, 0, BLUR_W, BLUR_H);
		});
		small_pass(s->blurB, BLUR_W, BLUR_H, [&] {
			set_tex(e, "image", gs_texrender_get_texture(s->blurA));
			set_vec4(e, "misc", 0, 1.0f / BLUR_H, sigma, 0);
			run(e, "Blur", 0, 0, BLUR_W, BLUR_H);
		});
		gs_blend_state_pop();

		const float over = H * 0.08f;
		set_tex(e, "image", gs_texrender_get_texture(s->blurB));
		set_vec4(e, "misc", l.bg.dim, over * 0.5625f, over, 0);
		set_vec4(e, "misc2", W, H, 0, 0);
		run(e, "Light", b.x, b.y, W, H);
	}
	if (turned) {
		gs_set_cull_mode(cull);
		gs_matrix_pop();
	}

	if (v.band) {
		set_tex(e, "image", v.band);
		set_colour(e, 0xff000000);
		run(e, "Tint", (float)v.bandInfo.ox, l.main.y + v.bandInfo.oy, (float)v.bandInfo.w,
		    (float)v.bandInfo.h);
	}
}

/* One piece in a box. Its art was made for a box look.boxW wide, and is
   scaled with it, so a zooming piece's frame grows as it does. */
/* Game and camera pieces wait for their picture; brand kit pieces bring their own.
   Small game layers stay out while Hide game HUD is on, unless hudHides is false
   (the current layout, where Hide game HUD plays each layer's own hide instead). */
bool piece_ready(const SvSource *s, const SvPiece &p, bool hudHides = true)
{
	if (is_hud(p)) {
		g_hudSeenNs.store(os_gettime_ns());
		if (hudHides && g_hudHidden.load())
			return false;
	}
	return p.src == SvSrc::Game ? s->gameReady : p.src == SvSrc::Cam ? s->camReady : true;
}

void draw_piece_plain(SvSource *s, const SvPiece &p, const SvRect &box, const GpuLook &a);

/* One layer at `box`, turned and mirrored around its middle when it has a rotation
   or a flip (the website's OBS-style transform), the frame and outline with it. */
void draw_piece(SvSource *s, const SvPiece &p, const SvRect &box, const GpuLook &a)
{
	if (p.rot == 0.0f && !p.flipH && !p.flipV) {
		draw_piece_plain(s, p, box, a);
		return;
	}
	const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
	gs_matrix_push();
	gs_matrix_translate3f(cx, cy, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(p.rot));
	gs_matrix_scale3f(p.flipH ? -1.0f : 1.0f, p.flipV ? -1.0f : 1.0f, 1.0f);
	gs_matrix_translate3f(-cx, -cy, 0.0f);
	/* A mirrored quad faces away: draw both sides while it is flipped. */
	const enum gs_cull_mode cull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);
	draw_piece_plain(s, p, box, a);
	gs_set_cull_mode(cull);
	gs_matrix_pop();
}

void draw_piece_plain(SvSource *s, const SvPiece &p, const SvRect &box, const GpuLook &a)
{
	gs_effect_t *e = s->effect;
	const bool isGame = p.src == SvSrc::Game;
	const bool isSocial = p.src == SvSrc::Social;
	const bool isBrand = p.src == SvSrc::Logo || p.src == SvSrc::Name || isSocial;
	/* A logo that has not loaded (or could not) is left out completely, frame and all. */
	if (isBrand && (isSocial ? a.pics.empty() : !a.pic))
		return;
	/* A rotator shows one link at a time, on the same clock for every copy on screen. */
	size_t link = 0;
	float linkAlpha = 1.0f;
	if (isSocial)
		sv_brand_social_at(a.pics.size(), p.secs, os_gettime_ns(), link, linkAlpha);
	/* Across and down apart, so a piece changing shape mid-glide keeps its frame on its edges. */
	const float k = box.w / a.boxW;
	const float ky = a.boxH > 0 ? box.h / a.boxH : k;
	if (a.under) {
		set_tex(e, "image", a.under);
		set_colour(e, a.underColor);
		run(e, "Tint", box.x + a.underInfo.ox * k, box.y + a.underInfo.oy * ky, a.underInfo.w * k,
		    a.underInfo.h * ky);
	}
	if (a.mask) {
		set_tex(e, "image", a.mask);
		if (isSocial) {
			set_tex(e, "source", a.pics[link]);
			set_vec4(e, "crop", 0, 0, 1, 1);
			set_vec4(e, "misc", linkAlpha, 0, 0, 0);
		} else if (isBrand) {
			set_tex(e, "source", a.pic);
			set_vec4(e, "crop", 0, 0, 1, 1);
		} else if (isGame) {
			set_tex(e, "source", gs_texrender_get_texture(s->gameTex));
			game_uv(e, p.crop, s->gameW, s->gameH);
		} else {
			set_tex(e, "source", gs_texrender_get_texture(s->camTex));
			cam_uv(e, box, s->camW, s->camH);
		}
		run(e, isSocial ? "MaskedFade" : "Masked", box.x, box.y, box.w, box.h);
	}
	if (a.over) {
		set_tex(e, "image", a.over);
		set_colour(e, a.overColor);
		run(e, "Tint", box.x + a.overInfo.ox * k, box.y + a.overInfo.oy * ky, a.overInfo.w * k,
		    a.overInfo.h * ky);
	}
	if (a.lamp) {
		set_tex(e, "image", a.lamp);
		run(e, "Stamp", box.x + a.lampX * k, box.y + a.lampY * ky, a.lampW * k, a.lampH * ky);
	}
}

/* A piece drawn part way through its show or hide: moved, and faded through
   pieceTex when it is see-through. Fully on, it is drawn as usual. */
void draw_piece_shown(SvSource *s, const SvPiece &p, const SvRect &box, const GpuLook &a, float on, float W, float H)
{
	if (on >= 0.9995f || p.show == SvShow::None) {
		draw_piece(s, p, box, a);
		return;
	}
	SvRect b;
	float alpha;
	show_at(p, box, on, W, H, b, alpha);
	if (alpha <= 0.001f)
		return;
	if (alpha >= 0.999f) {
		draw_piece(s, p, b, a);
		return;
	}
	if (!s->pieceTex)
		s->pieceTex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(s->pieceTex);
	if (!s->pieceTex || !gs_texrender_begin(s->pieceTex, (uint32_t)W, (uint32_t)H)) {
		draw_piece(s, p, b, a);
		return;
	}
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, W, 0.0f, H, -100.0f, 100.0f);
	gs_blend_state_push();
	/* Colour comes out premultiplied, ready to fade, as for a layout switch. */
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	draw_piece(s, p, b, a);
	gs_blend_state_pop();
	gs_texrender_end(s->pieceTex);

	gs_effect_t *e = s->effect;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	set_tex(e, "image", gs_texrender_get_texture(s->pieceTex));
	set_vec4(e, "misc", alpha, 0, 0, 0);
	run(e, "Fade", 0, 0, W, H);
	gs_blend_state_pop();
}

/* How far on piece i of the current layout is now. Hide game HUD turning on or
   off starts that piece's own hide or show (at once for None). */
float current_on(SvSource *s, size_t i, uint64_t now)
{
	if (i >= s->shows.size() || i >= s->layout.pieces.size())
		return 1.0f;
	const SvPiece &p = s->layout.pieces[i];
	ShowState &st = s->shows[i];
	const float want = is_hud(p) && g_hudHidden.load() ? 0.0f : 1.0f;
	if (want != st.to) {
		if (p.show == SvShow::None)
			st = ShowState{want, want, now};
		else
			st = ShowState{show_amount(st, p.showMs, now), want, now};
	}
	return show_amount(st, p.showMs, now);
}

/* True while a layout switch draws the old picture over the new one, so pieces
   on the move are drawn after it, on top, or the old picture would hide them. */
bool moving_on_top(const SvSource *s)
{
	return s->transActive && s->transMode != (int)SvSwitch::Slide;
}

/* skip: pieces a glide is drawing itself, left out here. */
void draw_layout(SvSource *s, const View &v, const std::vector<char> *skip = nullptr)
{
	gs_effect_t *e = s->effect;
	const SvLayout &l = v.layout;
	const SvMain &m = l.main;
	const bool fullPhone = m.show && m.y <= 0 && m.y + m.h >= l.h;

	/* The background is drawn at its place in the layer order (l.bg.at), below. */
	(void)fullPhone;
	bool bgDrawn = false;

	/* The gameplay, drawn at its place in the layer order (m.at pieces behind it). */
	bool mainDrawn = false;
	auto drawMain = [&]() {
		if (mainDrawn)
			return;
		mainDrawn = true;
		if (!m.show || !s->gameReady)
			return;
		set_tex(e, "source", gs_texrender_get_texture(s->gameTex));
		game_uv(e, m.crop, s->gameW, s->gameH);
		run(e, "Crop", 0, m.y, (float)l.w, m.h);
		if (l.bg.litEdges && m.h < l.h - 4 && s->bandLamp) {
			set_tex(e, "image", s->bandLamp);
			for (float y : {m.y - 1, m.y + m.h - 1})
				run(e, "Stamp", 0, y + s->bandLampY, (float)l.w, (float)s->bandLampH);
		}
	};

	/* Pieces in their own order with the gameplay among them, then any that are
	   zooming in front of everything. */
	struct Front {
		size_t i;
		float amount;
	};
	std::vector<Front> front;
	/* A layer being dragged in the canvas panel is drawn where the mouse has it. */
	const bool current = &l == &s->layout;
	const bool dragging = !s->dragId.empty() && current;
	const uint64_t now = os_gettime_ns();
	for (size_t i = 0; i < l.pieces.size() && i < v.art.size(); i++) {
		if (i == l.bg.at && !bgDrawn) {
			draw_background(s, v);
			bgDrawn = true;
		}
		if (i == m.at)
			drawMain();
		const SvPiece &p = l.pieces[i];
		if (!piece_ready(s, p, !current))
			continue;
		/* An old piece playing its hide gave its art away and is drawn by draw_moving. */
		if (!current && i < s->oldLeaving.size() && s->oldLeaving[i])
			continue;
		const float on = current ? current_on(s, i, now) : 1.0f;
		if (on <= 0.0005f)
			continue;
		if (skip && i < skip->size() && (*skip)[i])
			continue;
		if (dragging && p.id == s->dragId) {
			draw_piece(s, p, s->dragBox, v.art[i].base);
			continue;
		}
		const float amount = p.zoom ? sv_zoom_amount(p.zoomKey) : 0.0f;
		if (amount > 0.001f)
			front.push_back({i, amount});
		else if (on < 0.9995f && moving_on_top(s))
			continue; /* drawn on top by draw_moving */
		else
			draw_piece_shown(s, p, p.box, v.art[i].base, on, (float)l.w, (float)l.h);
	}
	if (!bgDrawn)
		draw_background(s, v);
	drawMain();
	for (const Front &f : front) {
		const SvPiece &p = l.pieces[f.i];
		const GpuArt &a = v.art[f.i];
		const float t = f.amount;
		SvRect box;
		box.x = p.box.x + (p.zoomBox.x - p.box.x) * t;
		box.y = p.box.y + (p.zoomBox.y - p.box.y) * t;
		box.w = std::max(1.0f, p.box.w + (p.zoomBox.w - p.box.w) * t);
		box.h = std::max(1.0f, p.box.h + (p.zoomBox.h - p.box.h) * t);
		/* The art made nearest this size: past halfway (in scale), the zoomed one. */
		const bool big = a.hasZoom && box.w * box.w > a.base.boxW * a.zoom.boxW;
		draw_piece(s, p, box, big ? a.zoom : a.base);
	}
}

SvRect lerp_rect(const SvRect &a, const SvRect &b, float k)
{
	SvRect r;
	r.x = a.x + (b.x - a.x) * k;
	r.y = a.y + (b.y - a.y) * k;
	r.w = std::max(1e-4f, a.w + (b.w - a.w) * k);
	r.h = std::max(1e-4f, a.h + (b.h - a.h) * k);
	return r;
}

/* Glide: each paired piece moves and resizes from its old spot to its new one,
   easing in and out. Its frame and shape are the old ones for the first half
   and the new ones after, swapped while it is moving fastest. */
void draw_glides(SvSource *s, float t)
{
	const float k = sv_ease(s->transFeel, t);
	const float kc = std::min(k, 1.0f); /* the crop never goes past the new one, only the box bounces */
	const auto &o = s->oldLayout.pieces;
	const auto &n = s->layout.pieces;
	for (size_t j = 0; j < s->glideFrom.size() && j < n.size() && j < s->art.size(); j++) {
		const int i = s->glideFrom[j];
		if (i < 0 || (size_t)i >= o.size() || (size_t)i >= s->oldArt.size())
			continue;
		const SvPiece &a = o[(size_t)i];
		const SvPiece &b = n[j];
		if (!piece_ready(s, b))
			continue;
		const bool late = k >= 0.5f;
		SvPiece q = late ? b : a;
		q.crop = lerp_rect(a.crop, b.crop, kc);
		SvRect box = lerp_rect(a.box, b.box, k);
		box.w = std::max(1.0f, box.w);
		box.h = std::max(1.0f, box.h);
		draw_piece(s, q, box, late ? s->art[j].base : s->oldArt[(size_t)i].base);
	}
}

/* Pieces on the move drawn last, on top: new ones still coming on while a layout
   switch draws the old picture over the new (see moving_on_top), and old ones
   playing their hide, which ride with the old picture (dx, dy) in a Slide.
   Old ones that have finished are let go here. */
void draw_moving(SvSource *s, float dx, float dy)
{
	const uint64_t now = os_gettime_ns();
	const float W = (float)s->layout.w, H = (float)s->layout.h;
	if (moving_on_top(s)) {
		const auto &n = s->layout.pieces;
		const bool glide = s->transMode == (int)SvSwitch::Glide;
		for (size_t i = 0; i < n.size() && i < s->art.size(); i++) {
			const SvPiece &p = n[i];
			if (glide && i < s->newGlides.size() && s->newGlides[i])
				continue;
			if (p.zoom && sv_zoom_amount(p.zoomKey) > 0.001f)
				continue;
			if (!piece_ready(s, p, false))
				continue;
			const float on = current_on(s, i, now);
			if (on > 0.0005f && on < 0.9995f)
				draw_piece_shown(s, p, p.box, s->art[i].base, on, W, H);
		}
	}
	for (size_t k = 0; k < s->leaving.size();) {
		Leaving &lv = s->leaving[k];
		const float on = show_amount(lv.st, lv.piece.showMs, now);
		if (on <= 0.0005f) {
			free_look(lv.art.base);
			free_look(lv.art.zoom);
			s->leaving.erase(s->leaving.begin() + (std::ptrdiff_t)k);
			continue;
		}
		if (piece_ready(s, lv.piece)) {
			gs_matrix_push();
			gs_matrix_translate3f(dx, dy, 0.0f);
			draw_piece_shown(s, lv.piece, lv.piece.box, lv.art.base, on, W, H);
			gs_matrix_pop();
		}
		k++;
	}
}

void sv_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<SvSource *>(data);
	if (!s->effect)
		return;
	const uint64_t t0 = os_gettime_ns();

	take_pending(s);
	if (!s->haveLayout)
		return;

	const bool firstThisFrame = !s->renderedThisFrame;
	s->renderedThisFrame = true;
	const bool drawing = !s->bench || s->benchDrawing;

	/* Graphics card timing, only while stats, test mode or the panel's cost line want it. */
	const bool timing = s->logStats || s->bench || g_measuring.load();
	SvSource::GpuSlot *slot = nullptr;
	if (firstThisFrame && timing && gpu_make(s)) {
		slot = &s->gpu[s->gpuNext];
		s->gpuNext = (s->gpuNext + 1) % SvSource::GPU_RING;
		gpu_collect(s, *slot);
		gs_timer_range_begin(slot->range);
	} else if (!timing && s->gpuReady) {
		gpu_free(s);
	}

	if (!drawing) {
		if (firstThisFrame) {
			if (slot)
				gs_timer_begin(slot->feed);
			feed_only(s->game, s->feedTex);
			feed_only(s->cam, s->feedTex);
			if (slot) {
				gs_timer_end(slot->feed);
				gs_timer_begin(slot->layout);
				gs_timer_end(slot->layout);
				gs_timer_range_end(slot->range);
				slot->used = true;
			}
		}
		return;
	}

	/* Our colours and blending work in the same space as a web page, so the
	   picture matches the website. */
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	if (firstThisFrame) {
		if (slot)
			gs_timer_begin(slot->feed);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		/* Copy only the pictures this layout uses: a layout with no camera
		   piece never copies the camera. */
		const SvLayout &l = s->layout;
		bool useGame = l.main.show || l.bg.mode == SvBgMode::Blur, useCam = false;
		for (const auto &p : l.pieces) {
			useGame = useGame || p.src == SvSrc::Game;
			useCam = useCam || p.src == SvSrc::Cam;
		}
		if (s->transActive) {
			const SvLayout &o = s->oldLayout;
			useGame = useGame || o.main.show || o.bg.mode == SvBgMode::Blur;
			for (const auto &p : o.pieces) {
				useGame = useGame || p.src == SvSrc::Game;
				useCam = useCam || p.src == SvSrc::Cam;
			}
		}
		s->gameReady = useGame && grab(s, s->game, s->gameTex, s->gameW, s->gameH);
		s->camReady = useCam && grab(s, s->cam, s->camTex, s->camW, s->camH);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(false);
		if (slot) {
			gs_timer_end(slot->feed);
			gs_timer_begin(slot->layout);
		}

		/* The layout being switched away from, drawn once a frame while it is still live. */
		if (s->transActive) {
			const uint64_t gone = os_gettime_ns() - s->transStartNs;
			if (gone >= (uint64_t)s->transMs * 1000000ULL) {
				end_transition(s);
			} else {
				const SvLayout &o = s->oldLayout;
				gs_texrender_reset(s->transTex);
				if (gs_texrender_begin(s->transTex, (uint32_t)o.w, (uint32_t)o.h)) {
					struct vec4 clear;
					vec4_zero(&clear);
					gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
					gs_ortho(0.0f, (float)o.w, 0.0f, (float)o.h, -100.0f, 100.0f);
					gs_blend_state_push();
					/* Colour comes out premultiplied, ready to fade. */
					gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
								   GS_BLEND_INVSRCALPHA);
					draw_layout(s, old_view(s),
						    s->transMode == (int)SvSwitch::Glide ? &s->oldGlides : nullptr);
					gs_blend_state_pop();
					gs_texrender_end(s->transTex);
				} else {
					end_transition(s);
				}
			}
		}
	}

	if (s->transActive) {
		const float W = (float)s->layout.w;
		const float H = (float)s->layout.h;
		const float t = std::clamp((float)((os_gettime_ns() - s->transStartNs) / 1e6) / (float)s->transMs, 0.0f, 1.0f);
		gs_effect_t *e = s->effect;
		if (s->transMode == (int)SvSwitch::Slide) {
			/* The whole picture slides the way the member picked: the old one out, the new one
			   in from the other side. It never goes past the end, or an edge would show. */
			const float k = std::min(sv_ease(s->transFeel, t), 1.0f);
			float dx = 0, dy = 0; /* where the old picture ends up */
			switch (s->transDir) {
			case SV_DIR_RIGHT:
				dx = W;
				break;
			case SV_DIR_UP:
				dy = -H;
				break;
			case SV_DIR_DOWN:
				dy = H;
				break;
			default:
				dx = -W;
				break;
			}
			gs_matrix_push();
			gs_matrix_translate3f(-dx * (1.0f - k), -dy * (1.0f - k), 0.0f);
			draw_layout(s, current_view(s));
			gs_matrix_pop();
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			set_tex(e, "image", gs_texrender_get_texture(s->transTex));
			set_vec4(e, "misc", 1.0f, 0, 0, 0);
			run(e, "Fade", dx * k, dy * k, (float)s->oldLayout.w, (float)s->oldLayout.h);
			gs_blend_state_pop();
			draw_moving(s, dx * k, dy * k);
		} else {
			/* Fade, and Glide: the old picture fades over the new one. A glide
			   then draws the pieces both layouts share, on the move, on top. */
			const bool glide = s->transMode == (int)SvSwitch::Glide;
			draw_layout(s, current_view(s), glide ? &s->newGlides : nullptr);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			set_tex(e, "image", gs_texrender_get_texture(s->transTex));
			set_vec4(e, "misc", 1.0f - std::clamp(sv_ease(s->transFeel, t), 0.0f, 1.0f), 0, 0, 0);
			run(e, "Fade", 0, 0, (float)s->oldLayout.w, (float)s->oldLayout.h);
			gs_blend_state_pop();
			if (glide)
				draw_glides(s, t);
			draw_moving(s, 0, 0);
		}
	} else {
		draw_layout(s, current_view(s));
		draw_moving(s, 0, 0);
	}

	if (slot) {
		gs_timer_end(slot->layout);
		gs_timer_range_end(slot->range);
		slot->used = true;
	}

	gs_enable_framebuffer_srgb(prevSrgb);
	s->drawNs += os_gettime_ns() - t0;
	s->drawCount++;
}

/* ---------- settings window ---------- */

struct ListCtx {
	obs_property_t *list;
	obs_source_t *self;
};

bool add_source(void *data, obs_source_t *src)
{
	auto *c = static_cast<ListCtx *>(data);
	if (src == c->self || std::strcmp(obs_source_get_id(src), SV_ID) == 0 ||
	    std::strcmp(obs_source_get_id(src), SV_PIECE_ID) == 0)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
		return true;
	obs_property_list_add_string(c->list, obs_source_get_name(src), obs_source_get_uuid(src));
	return true;
}

#ifdef SV_DEV_BUILD
bool reload_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = static_cast<SvSource *>(data);
	load_file(s);
	return false;
}
#endif

bool link_clicked(obs_properties_t *, obs_property_t *, void *)
{
	sv_link_request();
	return false;
}

bool unlink_clicked(obs_properties_t *, obs_property_t *, void *)
{
	sv_link_unlink();
	return false;
}

/* Every piece name in the panel's layout and the saved layouts, each once, for a
   piece source to pick from. */
std::vector<std::string> piece_names(SvSource *s)
{
	std::vector<std::string> names;
	auto add = [&names](const std::string &json) {
		SvLayout l;
		std::string err;
		if (json.empty() || !sv_layout_parse(json.c_str(), l, err))
			return;
		for (const SvPiece &p : l.pieces) {
			if (p.name.empty())
				continue;
			bool seen = false;
			for (const std::string &n : names)
				seen = seen || same_piece_name(n, p.name);
			if (!seen)
				names.push_back(p.name);
		}
	};
	if (s) {
		std::string follow;
		{
			std::lock_guard<std::mutex> lock(s->pickMtx);
			follow = s->followJson;
		}
		add(follow);
	}
	for (const SvLayoutChoice &c : sv_link_presets())
		add(c.drawList);
	return names;
}

obs_properties_t *build_properties(SvSource *s, bool pieceOnly)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *account = obs_properties_create();
	obs_properties_add_text(account, "link_status", sv_link_status().c_str(), OBS_TEXT_INFO);
	if (sv_link_is_linked())
		obs_properties_add_button(account, "unlink", T("UnlinkButton"), unlink_clicked);
	else
		obs_properties_add_button(account, "link", T("LinkButton"), link_clicked);
	obs_properties_add_group(props, "account", T("Account"), OBS_GROUP_NORMAL, account);

	obs_property_t *game = obs_properties_add_list(props, "game_source", T("GameSource"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(game, T("NoSource"), "");
	ListCtx gc{game, s ? s->self : nullptr};
	obs_enum_sources(add_source, &gc);

	obs_property_t *cam = obs_properties_add_list(props, "cam_source", T("CamSource"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cam, T("NoSource"), "");
	ListCtx cc{cam, s ? s->self : nullptr};
	obs_enum_sources(add_source, &cc);

	/* Each copy of the source can keep a layout of its own, so a vertical scene made in
	   another app (Be Right Back, Just Chatting) always shows the layout made for it. */
	obs_property_t *pick = obs_properties_add_list(props, "layout_pick", T("SourceLayoutPick"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(pick, T("SourceLayoutFollow"), "");
	for (const SvLayoutChoice &c : sv_link_presets())
		obs_property_list_add_string(pick, c.name.c_str(), c.id.c_str());
	obs_property_set_long_description(pick, pieceOnly ? T("SourcePieceLayoutTip") : T("SourceLayoutPickTip"));

	if (pieceOnly) {
		obs_property_t *piece = obs_properties_add_list(props, "piece_name", T("SourcePiecePick"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(piece, T("SourcePieceNone"), "");
		std::string current;
		if (s) {
			std::lock_guard<std::mutex> lock(s->applyMtx);
			current = s->pieceName;
		}
		bool listed = current.empty();
		for (const std::string &n : piece_names(s)) {
			obs_property_list_add_string(piece, n.c_str(), n.c_str());
			listed = listed || same_piece_name(n, current);
		}
		/* A piece no saved layout has any more stays picked, in case it comes back. */
		if (!listed)
			obs_property_list_add_string(piece, current.c_str(), current.c_str());
		obs_property_set_long_description(piece, T("SourcePiecePickTip"));
		obs_property_t *grow = obs_properties_add_list(props, "piece_grow", T("SourcePieceGrow"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(grow, T("SourcePieceGrowSame"), -1);
		obs_property_list_add_int(grow, T("SourcePieceGrowTopLeft"), 0);
		obs_property_list_add_int(grow, T("SourcePieceGrowTopRight"), 1);
		obs_property_list_add_int(grow, T("SourcePieceGrowBottomLeft"), 2);
		obs_property_list_add_int(grow, T("SourcePieceGrowBottomRight"), 3);
		obs_property_list_add_int(grow, T("SourcePieceGrowMiddle"), 4);
		obs_property_set_long_description(grow, T("SourcePieceGrowTip"));
		obs_properties_add_text(props, "piece_help", T("SourcePieceHelp"), OBS_TEXT_INFO);
	}

#ifdef SV_DEV_BUILD
	obs_properties_add_path(props, "layout_file", T("LayoutFile"), OBS_PATH_FILE, "Layout (*.json)", nullptr);
	obs_properties_add_button(props, "reload", T("Reload"), reload_clicked);
	obs_properties_add_bool(props, "log_stats", T("LogStats"));
	obs_properties_add_bool(props, "bench", T("Bench"));
#endif
	return props;
}

obs_properties_t *sv_properties(void *data)
{
	return build_properties(static_cast<SvSource *>(data), false);
}

obs_properties_t *sv_piece_properties(void *data)
{
	return build_properties(static_cast<SvSource *>(data), true);
}

void sv_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "log_stats", false);
	obs_data_set_default_string(settings, "layout_pick", "");
	obs_data_set_default_int(settings, "piece_grow", -1);
	/* A source added by hand starts with the game and camera already chosen. */
	const SvPicks picks = sv_store_current_picks();
	obs_data_set_default_string(settings, "game_source", picks.game.c_str());
	obs_data_set_default_string(settings, "cam_source", picks.cam.c_str());
}

} // namespace

void sv_source_set_switch(SvSwitch mode, int ms)
{
	ms = std::clamp(ms, 100, 2000);
	g_switchMode = (int)mode;
	g_switchMs = ms;
	sv_store_set_int("switch_mode", (int)mode);
	sv_store_set_int("switch_ms", ms);
}

SvSwitch sv_source_switch_mode()
{
	return (SvSwitch)g_switchMode.load();
}

int sv_source_switch_ms()
{
	return g_switchMs.load();
}

void sv_source_set_switch_style(int feel, int dir)
{
	g_switchFeel = std::clamp(feel, 0, 2);
	g_switchDir = std::clamp(dir, 0, 3);
	sv_store_set_int("switch_feel", g_switchFeel.load());
	sv_store_set_int("switch_dir", g_switchDir.load());
}

int sv_source_switch_feel()
{
	return g_switchFeel.load();
}

int sv_source_switch_dir()
{
	return g_switchDir.load();
}

void sv_source_arm_switch()
{
	g_switchAtNs = os_gettime_ns();
	g_switchSeq++;
}

void sv_source_set_measuring(bool on)
{
	g_measuring = on;
}

float sv_source_cost_ms()
{
	return g_costMs.load();
}

void sv_source_set_hud_hidden(bool on)
{
	if (g_hudHidden.exchange(on) != on)
		obs_log(LOG_INFO, "game HUD %s", on ? "hidden" : "showing");
}

bool sv_source_hud_hidden()
{
	return g_hudHidden.load();
}

bool sv_source_has_hud()
{
	const uint64_t seen = g_hudSeenNs.load();
	return seen != 0 && os_gettime_ns() - seen < 2000000000ULL;
}

void sv_source_set_picks_listener(std::function<void()> listener)
{
	std::lock_guard<std::mutex> lock(g_picksListenerMtx);
	g_picksListener = std::move(listener);
}

namespace {

SvSource *layout_source(obs_source_t *src)
{
	if (!src || std::strcmp(obs_source_get_id(src), SV_ID) != 0)
		return nullptr;
	return static_cast<SvSource *>(obs_obj_get_data(src));
}

} // namespace

bool sv_source_is_layout(obs_source_t *src)
{
	return layout_source(src) != nullptr;
}

bool sv_piece_inset(const SvPiece &p, float &dx, float &dy)
{
	SvLayout l;
	l.pieces.push_back(p);
	cut_to_piece(l, p.id.empty() ? p.name : p.id, -1, 0, 0);
	if (l.pieces.empty())
		return false;
	dx = l.pieces[0].box.x;
	dy = l.pieces[0].box.y;
	return true;
}

bool sv_source_pieces(obs_source_t *src, std::vector<SvPieceInfo> &out)
{
	out.clear();
	SvSource *s = layout_source(src);
	if (!s)
		return false;
	/* Asked as the pointer moves: never waits for a layout still being made. */
	std::unique_lock<std::mutex> lock(s->applyMtx, std::try_to_lock);
	if (!lock.owns_lock() || !s->lastParsedOk)
		return true;
	const bool hudHidden = g_hudHidden.load();
	/* A layer still being dragged, or waiting for its save, is where it is drawn. */
	std::string draftId;
	SvRect draftBox;
	{
		std::lock_guard<std::mutex> drawLock(s->mtx);
		draftId = s->draftId;
		draftBox = s->draftBox;
	}
	const SvLayout &l = s->lastParsed;
	SvRect band;
	band.x = 0;
	band.y = l.main.y;
	band.w = (float)l.w;
	band.h = l.main.h;
	for (size_t i = 0; i < l.pieces.size(); i++) {
		const SvPiece &p = l.pieces[i];
		if (p.id.empty() || (hudHidden && is_hud(p)))
			continue;
		SvPieceInfo info{p.id, p.name, p.id == draftId ? draftBox : p.box, p.locked};
		if (l.main.show && i < l.main.at)
			info.cover = band;
		out.push_back(info);
	}
	return true;
}

void sv_source_set_draft(obs_source_t *src, const std::string &id, const SvRect &box)
{
	SvSource *s = layout_source(src);
	if (!s)
		return;
	std::lock_guard<std::mutex> lock(s->mtx);
	s->draftId = id;
	s->draftBox = box;
}

std::string sv_source_move_target(obs_source_t *src)
{
	SvSource *s = layout_source(src);
	if (!s)
		return std::string();
	std::string pick;
	{
		std::lock_guard<std::mutex> lock(s->pickMtx);
		pick = s->pickId;
	}
	if (!pick.empty())
		for (const SvLayoutChoice &c : sv_link_presets())
			if (c.id == pick)
				return pick;
	return sv_link_active_preset();
}

void sv_source_load_settings()
{
	g_switchMode = std::clamp(sv_store_int("switch_mode", (int)SvSwitch::Glide), 0, 3);
	g_switchMs = std::clamp(sv_store_int("switch_ms", 300), 100, 2000);
	g_switchFeel = std::clamp(sv_store_int("switch_feel", SV_FEEL_SMOOTH), 0, 2);
	g_switchDir = std::clamp(sv_store_int("switch_dir", SV_DIR_LEFT), 0, 3);
}

void sv_register_source()
{
	struct obs_source_info info = {};
	info.id = SV_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = sv_get_name;
	info.create = sv_create;
	info.destroy = sv_destroy;
	info.update = sv_update;
	info.get_defaults = sv_defaults;
	info.get_properties = sv_properties;
	info.get_width = sv_width;
	info.get_height = sv_height;
	info.show = sv_show;
	info.hide = sv_hide;
	info.activate = sv_activate;
	info.deactivate = sv_deactivate;
	info.video_tick = sv_tick;
	info.video_render = sv_render;
	info.icon_type = OBS_ICON_TYPE_CUSTOM;
	obs_register_source(&info);

	/* One piece on its own, for a horizontal scene. The same drawing, so its shape,
	   frame, show and hide, and zoom key all match the vertical. */
	struct obs_source_info piece = info;
	piece.id = SV_PIECE_ID;
	piece.get_name = sv_piece_get_name;
	piece.create = sv_piece_create;
	piece.get_properties = sv_piece_properties;
	obs_register_source(&piece);
}
