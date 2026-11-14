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

// Looks at what is already in OBS so the member does not have to: the likely
// game picture, the likely camera, and any other vertical plugin installed.

#pragma once

#include "sv-store.h"

#include <obs.h>

#include <string>
#include <vector>

struct SvFound {
	std::string uuid;
	std::string name;
	int gameScore = 0; /* 0 means it does not look like a game picture */
	int camScore = 0;  /* 0 means it does not look like a camera */
};

/* Every video source in OBS, best guess work included. */
std::vector<SvFound> sv_detect_sources();

/* The best game and camera out of a list, either may be left empty. */
SvPicks sv_detect_best(const std::vector<SvFound> &found);

struct SvOtherApp {
	std::string name;     /* "" when no other vertical plugin is installed */
	std::string canvas;   /* its vertical canvas by uuid, "" when none found */
};

SvOtherApp sv_detect_other_app();

/* Adds Simply Vertical to the other plugin's vertical canvas, filling it.
   False means it could not (the member gets the by-hand steps instead). */
bool sv_detect_add_to_canvas(const std::string &canvasUuid, const SvPicks &picks);

/* Leaves one Simply Vertical in each scene of another app's vertical, removing extra
   copies (SE.Live can put back its saved copy after ours went in). Never touches the
   main canvas or ours. */
void sv_detect_remove_doubles();

/* Hands the picks to every Simply Vertical source already in the member's scenes. */
void sv_detect_apply_picks(const SvPicks &picks);

/* The game and camera of the first Simply Vertical source using something other
   than `stored`, for when a source was changed in its own Properties. False when
   every source matches. */
bool sv_detect_source_picks(const SvPicks &stored, SvPicks &out);
