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

// Everything the member can make Simply Vertical do. The panel, the hotkeys and
// scene linking all come through here, and so will automatic switching and
// automatic clips when they are added. Call these on OBS's main thread.

#pragma once

#include <string>

/* Registers the hotkeys and restores their saved keys. */
void sv_actions_load();
/* Saves the hotkeys and removes them. */
void sv_actions_shutdown();

/* Starts or stops one stream, the recording, Backtrack or the virtual camera. */
void sv_action_toggle(const std::string &id);
void sv_action_go_live_all();
void sv_action_stop_all();
/* Stops everything if anything is live, otherwise starts everything. */
void sv_action_toggle_all();
void sv_action_save_backtrack();

/* An empty id goes back to following the website. */
void sv_action_pick_layout(const std::string &id);
/* Steps through the layouts of one canvas, the main one when the canvas is empty. */
void sv_action_step_layout(int direction, const std::string &canvas = "");
/* An extra canvas's own layout; an empty id follows the main canvas's layout.
   On the main canvas this is sv_action_pick_layout. */
void sv_action_pick_canvas_layout(const std::string &canvas, const std::string &id);

/* The platforms showing one canvas (the main one when empty): whether any is live,
   and starting or stopping just those. */
bool sv_action_canvas_live(const std::string &canvas);
void sv_action_set_canvas_live(const std::string &canvas, bool on);
void sv_action_toggle_canvas(const std::string &canvas);

/* Hold changes on or off: while on, the website's moves wait. */
void sv_action_set_hold(bool on);
void sv_action_toggle_hold();

/* Applies the vertical scene and layout linked to the current OBS scene, if any. */
void sv_action_follow_scene();
/* Puts the next or previous vertical scene on one canvas's picture, the main one when empty. */
void sv_action_step_vscene(int direction, const std::string &canvas = "");
