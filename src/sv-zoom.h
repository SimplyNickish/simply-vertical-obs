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

// Zoom on command. The member sets up a zoom for a piece on the website (where
// it grows to, how it moves, hold or press, coming back by itself); a key in
// OBS, a Stream Deck press or the remote hook then zooms it. Every piece is
// found by its name, never its place in the list, so reordering or switching
// layouts never changes which key zooms what.
//
// The state is shared by every vertical source and is safe from any thread:
// keys arrive on OBS's hotkey thread, the amounts are read while drawing.

#pragma once

#include "sv-layout.h"

#include <obs.h>

#include <string>
#include <vector>

/* The key a piece's zoom is found by: its name in small letters. */
std::string sv_zoom_key_for(const std::string &name);

/* The zoom set ups in a layout that has just arrived at one vertical source.
   Any thread. Each source keeps its own, so Main and Twitch on different layouts
   both have working zooms. Pieces in no source's layout go back home, and a new
   name gets its own OBS hotkey. */
void sv_zoom_note_layout(obs_source_t *owner, const SvLayout &layout);

/* A vertical source went away or stopped showing a layout. Any thread. */
void sv_zoom_forget(obs_source_t *owner);

/* How far a piece is zoomed right now: 0 at home, 1 fully zoomed, a little
   past 1 for a moment with Punch. Any thread, cheap. */
float sv_zoom_amount(const std::string &key);

/* A zoom key went down or up: Hold zooms while it is down, Press flips. */
void sv_zoom_key(const std::string &key, bool down);

/* Every zoom in the layouts on show, for the panel, the remote hook and the Stream Deck list. */
struct SvZoomInfo {
	std::string name;
	bool zoomed = false; /* in, or heading in */
	bool hold = false;   /* its key works as Hold */
};
std::vector<SvZoomInfo> sv_zoom_list();

enum class SvZoomDo { Out, In, Flip };
/* Zooms a piece in, out or the other way, whatever its key does. */
void sv_zoom_do(const std::string &key, SvZoomDo what);

/* Main thread only: registers a hotkey for every zoom seen so far, with its
   saved keys, and from then on for each new one. */
void sv_zoom_load();
/* Main thread only: saves the zoom hotkeys and removes them. */
void sv_zoom_shutdown();
