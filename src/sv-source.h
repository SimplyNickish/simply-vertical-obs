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

// What the panel can ask of the layout source: how a layout change looks, and
// what the layout costs to draw.

#pragma once

/* Glide: pieces found in both layouts move and resize to their new spots, the
   rest fade. */
enum class SvSwitch { Cut = 0, Fade = 1, Slide = 2, Glide = 3 };

void sv_register_source();
/* Reads the saved switch choice. Call once the settings store is loaded. */
void sv_source_load_settings();

/* How the picture changes when the layout does. Saved on this PC. */
void sv_source_set_switch(SvSwitch mode, int ms);
SvSwitch sv_source_switch_mode();
int sv_source_switch_ms();
/* How the move feels (SV_FEEL_*) and which way a Slide goes (SV_DIR_*), see sv-ease.h. */
void sv_source_set_switch_style(int feel, int dir);
int sv_source_switch_feel();
int sv_source_switch_dir();

/* Called just before the member switches layout (panel, hotkey, scene), so only
   that change is smooth. Moves made on the website always land at once. */
void sv_source_arm_switch();

/* Turns the graphics card timing on while the panel shows the cost line. */
void sv_source_set_measuring(bool on);
/* What the layout costs the graphics card each frame, in milliseconds, or below 0 before the first reading. */
float sv_source_cost_ms();

/* Called, on any thread, when a source's game or camera changes, so the panel can
   show what the source really uses after a change in its own Properties. */
#include <functional>
/* Hide game HUD: while on, every vertical leaves out its small game layers (the
   minimap, killfeed and the like, any game layer cut from less than a quarter of
   the game picture). Gameplay, camera and brand kit pieces stay. Not saved, so
   OBS always starts with them showing. */
void sv_source_set_hud_hidden(bool on);
bool sv_source_hud_hidden();
/* True when a vertical drawn in the last two seconds has game layers that Hide game HUD hides. */
bool sv_source_has_hud();

void sv_source_set_picks_listener(std::function<void()> listener);

/* ---------- moving single layers in the canvas panel ---------- */

#include <string>
#include <vector>
#include "sv-layout.h"

typedef struct obs_source obs_source_t;

/* One layer of a layout source, in canvas pixels on its 1080 x 1920 phone. */
struct SvPieceInfo {
	std::string id;
	std::string name;
	SvRect box;
	bool locked = false;
	/* The gameplay's box when this layer sits behind it (w 0 when in front): clicks there are the gameplay's. */
	SvRect cover;
};

bool sv_source_is_layout(obs_source_t *src);
/* The layers the source shows now, bottom first. Layers with no id (an old
   website) and those Hide game HUD hides are left out. False when src is not a layout source. */
bool sv_source_pieces(obs_source_t *src, std::vector<SvPieceInfo> &out);
/* A piece source showing piece p draws it this far in from its own top left corner,
   so placing the source at p.box minus this puts it exactly on the piece. */
bool sv_piece_inset(const SvPiece &p, float &dx, float &dy);
/* Draws one layer at `box` instead of its saved place while it is dragged. An empty id stops. */
void sv_source_set_draft(obs_source_t *src, const std::string &id, const SvRect &box);
/* Where a move of one of this source's layers is saved: the saved layout picked
   for this source alone, else the one picked in the panel, else empty (the website's own). */
std::string sv_source_move_target(obs_source_t *src);
