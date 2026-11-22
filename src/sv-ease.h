/*
Simply Vertical for OBS
Copyright (C) 2026 SimplyNickish

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// How a layout switch moves over its length. Shared by the canvas and the
// moving phones in Settings, so what the member picks is what they get.

#pragma once

#include <algorithm>
#include <cmath>

enum { SV_FEEL_SMOOTH = 0, SV_FEEL_SNAPPY = 1, SV_FEEL_BOUNCY = 2 };
/* Which way the picture moves on a Slide: the new layout comes in from the other side. */
enum { SV_DIR_LEFT = 0, SV_DIR_RIGHT = 1, SV_DIR_UP = 2, SV_DIR_DOWN = 3 };

/* t from 0 to 1 in, how far along the move is out. Bouncy goes a little past 1 and settles back. */
inline float sv_ease(int feel, float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	switch (feel) {
	case SV_FEEL_SNAPPY:
		return 1.0f - std::pow(1.0f - t, 5.0f);
	case SV_FEEL_BOUNCY: {
		const float c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	default:
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
	}
}
