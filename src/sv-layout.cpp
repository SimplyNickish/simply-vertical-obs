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

#include "sv-layout.h"
#include "sv-brand.h"
#include "sv-zoom.h"

#include <obs-module.h>
#include "plugin-support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

/* 24 layers plus the gameplay, which the website sends as a layer too. */
constexpr int MAX_PIECES = 25;
/* Every piece's box (and zoom box) added up, in pixels. Each piece keeps a few pictures of
   its own size, so this keeps a whole layout to a few hundred MB at most. A real layout uses
   a small part of it; a piece past it is left out rather than OBS running out of memory. */
constexpr double MAX_PIECE_PIXELS = 48.0 * 1000 * 1000;
constexpr int MAX_POINTS = 48;
constexpr size_t MAX_JSON = 256 * 1024;

float num(obs_data_t *d, const char *key, float lo, float hi, float fallback)
{
	if (!d || !obs_data_has_user_value(d, key))
		return fallback;
	double v = obs_data_get_double(d, key);
	if (!std::isfinite(v))
		return fallback;
	return (float)std::min<double>(hi, std::max<double>(lo, v));
}

uint32_t colour(obs_data_t *d, const char *key, uint32_t fallback)
{
	const char *s = d ? obs_data_get_string(d, key) : nullptr;
	if (!s || std::strlen(s) != 7 || s[0] != '#')
		return fallback;
	uint32_t rgb = 0;
	for (int i = 1; i < 7; i++) {
		char c = s[i];
		uint32_t v;
		if (c >= '0' && c <= '9')
			v = c - '0';
		else if (c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v = c - 'A' + 10;
		else
			return fallback;
		rgb = (rgb << 4) | v;
	}
	return 0xff000000u | rgb;
}

bool is(obs_data_t *d, const char *key, const char *value)
{
	const char *s = d ? obs_data_get_string(d, key) : nullptr;
	return s && std::strcmp(s, value) == 0;
}

/* A crop: fractions of the game picture, kept inside it and never empty. */
SvRect fraction_rect(obs_data_t *d)
{
	SvRect r;
	r.w = num(d, "w", 0.002f, 1, 1);
	r.h = num(d, "h", 0.002f, 1, 1);
	r.x = num(d, "x", 0, 1 - r.w, 0);
	r.y = num(d, "y", 0, 1 - r.h, 0);
	return r;
}

/* A box on the canvas: may hang off the edges a little, never absurd. */
SvRect canvas_rect(obs_data_t *d, int cw, int ch)
{
	SvRect r;
	r.w = num(d, "w", 1, (float)cw, 100);
	r.h = num(d, "h", 1, (float)ch, 100);
	r.x = num(d, "x", -r.w, (float)cw, 0);
	r.y = num(d, "y", -r.h, (float)ch, 0);
	return r;
}

/* A piece name: no control characters, at most 32 letters (the website's limit). */
std::string clean_name(const char *s)
{
	std::string out;
	int letters = 0;
	for (const char *c = s ? s : ""; *c; c++) {
		const unsigned char b = (unsigned char)*c;
		if (b < 0x20 || b == 0x7f)
			continue;
		/* A letter starts on any byte that does not carry on the last one,
		   so stopping there never cuts a letter in half. */
		if ((b & 0xc0) != 0x80 && letters++ == 32)
			break;
		out += (char)b;
	}
	return out;
}

/* A zoom key from the website: an OBS key name OBS knows, and the keys held
   with it, as "OBS_KEY_F8:1:0:1" (ctrl, shift, alt). Empty when it is not one. */
std::string clean_bind(obs_data_t *b)
{
	const char *key = obs_data_get_string(b, "key");
	const size_t n = key ? std::strlen(key) : 0;
	if (n < 9 || n > 28 || std::strncmp(key, "OBS_KEY_", 8) != 0)
		return std::string();
	for (size_t i = 8; i < n; i++)
		if (!std::isupper((unsigned char)key[i]) && !std::isdigit((unsigned char)key[i]))
			return std::string();
	if (obs_key_from_name(key) == OBS_KEY_NONE)
		return std::string();
	std::string out = key;
	out += obs_data_get_bool(b, "ctrl") ? ":1" : ":0";
	out += obs_data_get_bool(b, "shift") ? ":1" : ":0";
	out += obs_data_get_bool(b, "alt") ? ":1" : ":0";
	return out;
}

/* The channel name: one line of plain text, at most 60 letters. */
std::string clean_text(const char *s)
{
	std::string out;
	int letters = 0;
	for (const char *c = s ? s : ""; *c; c++) {
		const unsigned char b = (unsigned char)*c;
		if (b < 0x20 || b == 0x7f)
			continue;
		if ((b & 0xc0) != 0x80 && letters++ == 60)
			break;
		out += (char)b;
	}
	const size_t a = out.find_first_not_of(' '), z = out.find_last_not_of(' ');
	return a == std::string::npos ? std::string() : out.substr(a, z - a + 1);
}

/* A font family name: letters, digits, spaces and a few marks, nothing else. */
std::string clean_font(const char *s)
{
	std::string out;
	for (const char *c = s ? s : ""; *c && out.size() < 64; c++) {
		const unsigned char b = (unsigned char)*c;
		if (std::isalnum(b) || b == ' ' || b == '-' || b == '_' || b == '.' || b == 0x27)
			out += (char)b;
	}
	return out;
}

/* A layer id: letters, digits, '-' and '_', at most 64 (the website's own rule). */
std::string clean_id(const char *s)
{
	std::string out;
	for (const char *c = s ? s : ""; *c; c++) {
		const unsigned char b = (unsigned char)*c;
		if (!(std::isalnum(b) || b == '-' || b == '_') || out.size() >= 64)
			return std::string();
		out += (char)b;
	}
	return out;
}

} // namespace

bool sv_layout_parse(const char *json, SvLayout &out, std::string &err)
{
	if (!json || !*json) {
		err = "empty";
		return false;
	}
	if (std::strlen(json) > MAX_JSON) {
		err = "too big";
		return false;
	}
	obs_data_t *root = obs_data_create_from_json(json);
	if (!root) {
		err = "not JSON";
		return false;
	}
	if (obs_data_get_int(root, "v") != 1) {
		obs_data_release(root);
		err = "unknown version";
		return false;
	}

	SvLayout l;
	l.w = (int)num(root, "w", 64, 4096, 1080);
	l.h = (int)num(root, "h", 64, 4096, 1920);

	/* The background's place in the list's order, counted in the list's own layers. */
	long long bgAt = 0;
	obs_data_t *bg = obs_data_get_obj(root, "bg");
	if (bg) {
		l.bg.mode = is(bg, "mode", "blur") ? SvBgMode::Blur : is(bg, "mode", "color") ? SvBgMode::Color : SvBgMode::None;
		l.bg.color = colour(bg, "color", 0xff000000);
		l.bg.blur = num(bg, "blur", 0, 200, 40);
		l.bg.dim = num(bg, "dim", 0, 0.95f, 0.35f);
		l.bg.litEdges = obs_data_get_bool(bg, "litEdges");
		obs_data_t *crop = obs_data_get_obj(bg, "crop");
		l.bg.crop = fraction_rect(crop);
		obs_data_release(crop);
		obs_data_t *box = obs_data_get_obj(bg, "box");
		if (box) {
			l.bg.hasBox = true;
			l.bg.box = canvas_rect(box, l.w, l.h);
			obs_data_release(box);
		}
		l.bg.rot = num(bg, "rot", -360, 360, 0);
		l.bg.flipH = obs_data_get_bool(bg, "flipH");
		l.bg.flipV = obs_data_get_bool(bg, "flipV");
		bgAt = obs_data_get_int(bg, "at");
		obs_data_release(bg);
	}

	/* The gameplay's place in the list's order (an older website sends none: 0, all in front). */
	long long mainAt = 0;
	obs_data_t *main = obs_data_get_obj(root, "main");
	if (main) {
		mainAt = obs_data_get_int(main, "at");
		l.main.show = obs_data_get_bool(main, "show");
		obs_data_t *crop = obs_data_get_obj(main, "crop");
		l.main.crop = fraction_rect(crop);
		obs_data_release(crop);
		l.main.h = num(main, "h", 1, (float)l.h, (float)l.h);
		l.main.y = num(main, "y", -l.main.h, (float)l.h, 0);
		obs_data_release(main);
	}

	obs_data_array_t *pieces = obs_data_get_array(root, "pieces");
	size_t count = pieces ? obs_data_array_count(pieces) : 0;
	double pixels = 0;
	/* Counted in the pieces kept, so a piece left out below never moves the gameplay. */
	bool mainPlaced = false;
	bool bgPlaced = false;
	for (size_t i = 0; i < count && (int)l.pieces.size() < MAX_PIECES; i++) {
		if ((long long)i == mainAt) {
			l.main.at = l.pieces.size();
			mainPlaced = true;
		}
		if ((long long)i == bgAt) {
			l.bg.at = l.pieces.size();
			bgPlaced = true;
		}
		obs_data_t *pd = obs_data_array_item(pieces, i);
		SvPiece p;
		/* A kind of piece this plugin does not know yet is left out on its own,
		   so an older plugin still draws the rest of the layout. */
		const char *src = obs_data_get_string(pd, "src");
		if (!src || !*src || std::strcmp(src, "game") == 0)
			p.src = SvSrc::Game;
		else if (std::strcmp(src, "cam") == 0)
			p.src = SvSrc::Cam;
		else if (std::strcmp(src, "logo") == 0)
			p.src = SvSrc::Logo;
		else if (std::strcmp(src, "name") == 0)
			p.src = SvSrc::Name;
		else if (std::strcmp(src, "socials") == 0)
			p.src = SvSrc::Social;
		else {
			obs_data_release(pd);
			continue;
		}
		if (p.src == SvSrc::Logo) {
			p.url = obs_data_get_string(pd, "url");
			if (!sv_brand_logo_url_ok(p.url)) {
				obs_data_release(pd);
				continue;
			}
		} else if (p.src == SvSrc::Name) {
			p.text = clean_text(obs_data_get_string(pd, "text"));
			p.font = clean_font(obs_data_get_string(pd, "font"));
			p.textColor = colour(pd, "textColor", 0xffffffff);
			if (p.text.empty()) {
				obs_data_release(pd);
				continue;
			}
		} else if (p.src == SvSrc::Social) {
			static const char *const ICONS[] = {"twitch", "youtube", "kick", "tiktok", "x", "website"};
			obs_data_array_t *items = obs_data_get_array(pd, "items");
			const size_t n = items ? obs_data_array_count(items) : 0;
			for (size_t k = 0; k < n && p.socials.size() < 6; k++) {
				obs_data_t *it = obs_data_array_item(items, k);
				const char *icon = obs_data_get_string(it, "icon");
				std::string text = clean_text(obs_data_get_string(it, "text"));
				for (const char *known : ICONS)
					if (icon && std::strcmp(icon, known) == 0 && !text.empty()) {
						p.socials.emplace_back(known, std::move(text));
						break;
					}
				obs_data_release(it);
			}
			obs_data_array_release(items);
			p.font = clean_font(obs_data_get_string(pd, "font"));
			p.textColor = colour(pd, "textColor", 0xffffffff);
			p.secs = num(pd, "secs", 2, 15, 5);
			if (p.socials.empty()) {
				obs_data_release(pd);
				continue;
			}
		}
		const bool brand = p.src == SvSrc::Logo || p.src == SvSrc::Name || p.src == SvSrc::Social;
		obs_data_t *crop = obs_data_get_obj(pd, "crop");
		p.crop = fraction_rect(crop);
		obs_data_release(crop);
		obs_data_t *box = obs_data_get_obj(pd, "box");
		p.box = canvas_rect(box, l.w, l.h);
		obs_data_release(box);
		p.radius = num(pd, "radius", 0, 200, 0);
		p.corners = {{p.radius, p.radius, p.radius, p.radius}};
		p.round = num(pd, "round", 0, 200, 0);
		p.feather = brand ? 0.0f : num(pd, "feather", 0, 40, 0);
		if (obs_data_t *c = obs_data_get_obj(pd, "corners")) {
			p.corners = {{num(c, "tl", 0, 200, p.radius), num(c, "tr", 0, 200, p.radius),
				      num(c, "br", 0, 200, p.radius), num(c, "bl", 0, 200, p.radius)}};
			obs_data_release(c);
		}
		p.mask = brand			     ? SvMask::Rect
			 : is(pd, "mask", "circle") ? SvMask::Circle
			 : is(pd, "mask", "poly")   ? SvMask::Poly
						    : SvMask::Rect;
		if (p.mask == SvMask::Poly) {
			obs_data_array_t *pts = obs_data_get_array(pd, "pts");
			size_t n = pts ? obs_data_array_count(pts) : 0;
			for (size_t k = 0; k < n && (int)p.pts.size() < MAX_POINTS; k++) {
				obs_data_t *pt = obs_data_array_item(pts, k);
				p.pts.emplace_back(num(pt, "x", 0, 1, 0), num(pt, "y", 0, 1, 0));
				obs_data_release(pt);
			}
			obs_data_array_release(pts);
			if (p.pts.size() < 3) {
				p.mask = SvMask::Rect;
				p.pts.clear();
			}
		}
		p.frame = is(pd, "frame", "depth")    ? SvFrame::Depth
			  : is(pd, "frame", "glow")   ? SvFrame::Glow
			  : is(pd, "frame", "border") ? SvFrame::Border
						      : SvFrame::None;
		p.color = colour(pd, "color", 0xff4d7cff);
		p.lit = obs_data_get_bool(pd, "lit");
		p.name = clean_name(obs_data_get_string(pd, "name"));
		p.id = clean_id(obs_data_get_string(pd, "id"));
		p.locked = obs_data_get_bool(pd, "locked");
		p.rot = num(pd, "rot", -360, 360, 0);
		p.flipH = obs_data_get_bool(pd, "flipH");
		p.flipV = obs_data_get_bool(pd, "flipV");
		obs_data_t *zoom = brand ? nullptr : obs_data_get_obj(pd, "zoom");
		if (zoom) {
			obs_data_t *zb = obs_data_get_obj(zoom, "box");
			if (zb) {
				p.zoom = true;
				p.zoomBox = canvas_rect(zb, l.w, l.h);
				obs_data_release(zb);
			}
			p.zoomMotion = is(zoom, "motion", "snap")    ? SvZoomMotion::Snap
				       : is(zoom, "motion", "punch") ? SvZoomMotion::Punch
								     : SvZoomMotion::Smooth;
			p.zoomMs = num(zoom, "ms", 0, 3000, 350);
			p.zoomHold = obs_data_get_bool(zoom, "hold");
			p.zoomBack = num(zoom, "back", 0, 600, 0);
			p.zoomKey = sv_zoom_key_for(p.name);
			obs_data_t *bind = obs_data_get_obj(zoom, "bind");
			if (bind) {
				p.zoomBind = clean_bind(bind);
				obs_data_release(bind);
			}
			obs_data_release(zoom);
		}
		obs_data_t *show = obs_data_get_obj(pd, "show");
		if (show) {
			p.show = is(show, "style", "fade")    ? SvShow::Fade
				 : is(show, "style", "slide") ? SvShow::Slide
				 : is(show, "style", "pop")   ? SvShow::Pop
							      : SvShow::None;
			p.showFrom = is(show, "from", "right")	? SvShowFrom::Right
				     : is(show, "from", "top")	? SvShowFrom::Top
				     : is(show, "from", "bottom") ? SvShowFrom::Bottom
								  : SvShowFrom::Left;
			p.showMs = num(show, "ms", 120, 1200, 350);
			obs_data_release(show);
		}
		const double area = (double)p.box.w * p.box.h + (p.zoom ? (double)p.zoomBox.w * p.zoomBox.h : 0.0);
		if (pixels + area > MAX_PIECE_PIXELS) {
			obs_log(LOG_WARNING, "layout: piece %zu left out, the layout is too big to draw", i + 1);
			obs_data_release(pd);
			continue;
		}
		pixels += area;
		l.pieces.push_back(std::move(p));
		obs_data_release(pd);
	}
	obs_data_array_release(pieces);
	obs_data_release(root);
	if (!mainPlaced)
		l.main.at = mainAt <= 0 ? 0 : l.pieces.size();
	if (!bgPlaced)
		l.bg.at = bgAt <= 0 ? 0 : l.pieces.size();
	if (!l.bg.hasBox) {
		l.bg.box.x = 0;
		l.bg.box.y = 0;
		l.bg.box.w = (float)l.w;
		l.bg.box.h = (float)l.h;
	}

	out = std::move(l);
	return true;
}

static bool same_mask(const SvPiece &a, const SvPiece &b)
{
	return a.src == b.src && a.url == b.url && a.text == b.text && a.font == b.font && a.socials == b.socials &&
	       a.textColor == b.textColor && a.mask == b.mask && a.frame == b.frame && a.color == b.color && a.lit == b.lit &&
	       std::lround(a.box.w) == std::lround(b.box.w) && std::lround(a.box.h) == std::lround(b.box.h) &&
	       a.radius == b.radius && a.corners == b.corners && a.round == b.round && a.feather == b.feather && a.pts == b.pts && a.zoom == b.zoom &&
	       (!a.zoom || (std::lround(a.zoomBox.w) == std::lround(b.zoomBox.w) &&
			    std::lround(a.zoomBox.h) == std::lround(b.zoomBox.h)));
}

bool sv_layout_masks_differ(const SvLayout &a, const SvLayout &b)
{
	if (a.pieces.size() != b.pieces.size())
		return true;
	for (size_t i = 0; i < a.pieces.size(); i++)
		if (!same_mask(a.pieces[i], b.pieces[i]))
			return true;
	return false;
}
