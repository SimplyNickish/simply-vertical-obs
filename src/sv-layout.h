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

// The draw list: the finished layout, already worked out by the website, as
// plain boxes, crops and outlines. The plugin only draws what it is given and
// never needs to know how a layout was made. Everything that arrives here is
// treated as untrusted and checked before use.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SvRect {
	float x = 0, y = 0, w = 0, h = 0;
};

enum class SvSrc { Game, Cam, Logo, Name, Social };
enum class SvMask { Rect, Circle, Poly };
enum class SvFrame { None, Glow, Border, Depth };
enum class SvBgMode { None, Blur, Color };
enum class SvZoomMotion { Smooth, Snap, Punch };

struct SvPiece {
	SvSrc src = SvSrc::Game;
	/* Game pieces: the cut-out as fractions of the 16:9 game picture. */
	SvRect crop;
	/* Where it sits on the canvas, in canvas pixels. */
	SvRect box;
	SvMask mask = SvMask::Rect;
	float radius = 0;
	/* Outline points as fractions of the box (SvMask::Poly only). */
	std::vector<std::pair<float, float>> pts;
	SvFrame frame = SvFrame::None;
	uint32_t color = 0xff4d7cff; /* 0xAARRGGBB */
	bool lit = false;
	/* The member's name for it, which names its zoom key. */
	std::string name;
	/* Brand kit pieces (sv-brand.h). Logo: where its picture is. Name: the line of
	   text, the font family wanted and the letters' colour (Social uses the font
	   and colour too). */
	std::string url;
	std::string text;
	std::string font;
	uint32_t textColor = 0xffffffff;
	/* Social rotator: each link as its platform ("twitch", "youtube", "kick",
	   "tiktok", "x" or "website") and the words shown, and how long each shows. */
	std::vector<std::pair<std::string, std::string>> socials;
	float secs = 5;
	/* Zoom on command (sv-zoom.h): off unless the website switched it on. */
	bool zoom = false;
	SvRect zoomBox;     /* where it grows to, canvas pixels */
	SvZoomMotion zoomMotion = SvZoomMotion::Smooth;
	float zoomMs = 350; /* how long the move takes */
	bool zoomHold = false; /* zoomed only while the key is held */
	float zoomBack = 0;    /* seconds before it comes back by itself, 0 = never */
	std::string zoomKey;   /* sv_zoom_key_for(name) */
	/* The key set with "Set key" on the website, as "OBS_KEY_F8:ctrl:shift:alt"
	   with 1 or 0 for each, or empty to leave the key to OBS Settings, Hotkeys. */
	std::string zoomBind;
};

struct SvMain {
	bool show = false;
	SvRect crop;
	float y = 0, h = 0;
};

struct SvBackground {
	SvBgMode mode = SvBgMode::None;
	uint32_t color = 0xff000000;
	float blur = 40;
	float dim = 0.35f;
	SvRect crop;
	bool litEdges = false;
};

struct SvLayout {
	int w = 1080, h = 1920;
	SvBackground bg;
	SvMain main;
	std::vector<SvPiece> pieces;
};

/* Reads a draw list. Returns false (and leaves out untouched) when it is not usable. */
bool sv_layout_parse(const char *json, SvLayout &out, std::string &err);

/* True when two layouts give different piece masks, so the masks must be redrawn. */
bool sv_layout_masks_differ(const SvLayout &a, const SvLayout &b);
