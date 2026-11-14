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
void sv_source_set_picks_listener(std::function<void()> listener);
