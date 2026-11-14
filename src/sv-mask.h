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

// Piece shapes as small greyscale pictures, made once when the layout changes
// (never per frame): the cut-out itself, its shadow or glow, and its border.
// The graphics card then just stamps them.

#pragma once

#include "sv-layout.h"

#include <cstdint>
#include <vector>

struct SvBitmap {
	int w = 0, h = 0;
	/* Offset of the bitmap's top left from the piece box's top left, canvas pixels. */
	int ox = 0, oy = 0;
	std::vector<uint8_t> a;
	bool empty() const { return w <= 0 || h <= 0; }
};

/* A small full colour picture, four bytes (red, green, blue, see-through) per pixel. */
struct SvColorBitmap {
	int w = 0, h = 0;
	int ox = 0, oy = 0;
	std::vector<uint8_t> rgba;
	bool empty() const { return w <= 0 || h <= 0; }
};

struct SvPieceArt {
	SvBitmap mask;  /* the cut-out, same size as the box */
	SvBitmap under; /* shadow or glow, drawn before the piece */
	SvBitmap over;  /* border or lit rim, drawn after the piece */
	SvColorBitmap lamp; /* the lamp line along the top of a lit, square-sided piece */
	SvColorBitmap pic;  /* brand kit pieces: the logo or channel name itself, box sized */
	std::vector<SvColorBitmap> pics; /* social rotator: each link, box sized, shown in turn */
	uint32_t underColor = 0xff000000;
	uint32_t overColor = 0xff4d7cff;
};

/* k: how much bigger than its normal size the piece is drawn (zoomed art), so frames grow with it. */
SvPieceArt sv_piece_art(const SvPiece &p, float k = 1.0f);

/* The soft shadow around the full-width gameplay band, w x h is the band. */
SvBitmap sv_band_shadow(int w, int h);

/* The light line on the band's top and bottom edges. oy is relative to the line's y. */
SvColorBitmap sv_band_lamp(int w);
