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

// The member's vertical: a 1080 x 1920 OBS canvas beside the main one, holding
// ordinary OBS scenes. Every new scene starts with the Simply Vertical layout
// as its bottom layer, and the member adds whatever they like on top, exactly
// as in a normal OBS scene. OBS itself saves the canvas and its scenes inside
// the scene collection, so they come back after a restart and travel with the
// collection's backups. The main canvas and main stream are never touched.

#pragma once

#include "sv-store.h"

#include <obs.h>

#include <functional>
#include <string>
#include <vector>

enum SvCanvasUser : unsigned {
	SV_CANVAS_PREVIEW = 1, /* the panel's picture is on screen */
	SV_CANVAS_OUTPUT = 2,  /* going live or recording */
};

/* The name OBS shows for our canvas, and the name we look for on load. */
inline constexpr const char *SV_CANVAS_NAME = "Simply Vertical";
/* Extra canvases the member makes (one per platform, say) are named with this
   in front of the member's own name, so OBS saves them in the collection and we
   find them again on load. */
inline constexpr const char *SV_CANVAS_PREFIX = "Simply Vertical: ";
/* The most canvases a member can have, the main vertical one included. */
inline constexpr int SV_CANVAS_MAX = 4;

/* True for our main vertical canvas and for every extra one the member made. */
bool sv_canvas_is_ours(const char *obsName);

/* The layout source in each extra canvas the collection has, running or not.
   Only for comparing: the sources are not held. Any thread. */
std::vector<obs_source_t *> sv_canvas_extra_layout_sources();

/* Every function below that takes a canvas id works on the main vertical canvas
   when the id is empty, so callers that only know one canvas keep working. */

/* Finds this collection's vertical canvas or makes it, with one scene if it
   has none. Safe to call when already running: it just updates the picks.
   Main thread only. */
bool sv_canvas_start(const SvPicks &picks);
/* Stops drawing and lets go of the canvas. The member's scenes stay saved in
   their collection; nothing is ever deleted here. */
void sv_canvas_stop();
bool sv_canvas_running();

/* Points every Simply Vertical layout source at the chosen game and camera. */
void sv_canvas_set_picks(const SvPicks &picks);
void sv_canvas_set_user(SvCanvasUser user, bool using_it, const std::string &canvas = "");
/* The canvases that going live or recording draw from right now. Every other
   canvas stops drawing for outputs (it may still draw for the panel). An empty
   id stands for the main canvas. */
void sv_canvas_set_output_canvases(const std::vector<std::string> &canvases);

/* ---------- the canvases, main thread only ---------- */

struct SvVCanvas {
	std::string uuid;
	std::string name; /* the member's name for it, without our prefix */
	bool main = false;
};

/* The main canvas first, then the extra ones in the order they were made. */
std::vector<SvVCanvas> sv_canvases();
/* The id of the main canvas, "" when not running. */
std::string sv_canvas_main_id();
/* True when this id is one of ours (an empty id is the main canvas). */
bool sv_canvas_exists(const std::string &canvas);
/* A new canvas named by the member, starting as a copy of another one (every
   scene and layer, the same sources shared). Returns its id, or "" when the
   name is empty or taken or there are already SV_CANVAS_MAX canvases. */
std::string sv_canvas_add(const std::string &name, const std::string &copyFrom = "");
/* The main canvas keeps its name. False when the name is empty or taken. */
bool sv_canvas_rename(const std::string &canvas, const std::string &name);
/* Never the main canvas and never one that is live or recording. Its scenes
   go with it; the sources inside them stay, since other scenes share them. */
bool sv_canvas_remove(const std::string &canvas);
/* A name no canvas uses yet: "Twitch", "Twitch 2" and so on. */
std::string sv_canvas_free_canvas_name(const std::string &base);

/* The canvas the panel shows and edits, "" for the main one. Switching moves the
   panel's picture to it. Falls back to "" when that canvas has gone. */
std::string sv_canvas_view();
void sv_canvas_set_view(const std::string &canvas);

/* Each extra canvas has its own copy of the layout source, so it can show another
   saved layout. "" follows the main canvas's layout. The main canvas's layout is
   picked the usual way and these do nothing for it. */
std::string sv_canvas_layout_pick(const std::string &canvas);
void sv_canvas_set_layout_pick(const std::string &canvas, const std::string &id);

/* ---------- the vertical scenes, main thread only ---------- */

struct SvVScene {
	std::string uuid;
	std::string name;
};

std::vector<SvVScene> sv_canvas_scenes(const std::string &canvas = "");
std::string sv_canvas_current(const std::string &canvas = "");
/* Scene ids are unique across canvases, so these find the scene's own canvas. */
void sv_canvas_switch(const std::string &uuid);
/* A new scene with the layout as its bottom layer. Returns its uuid, or "". */
std::string sv_canvas_add_scene(const std::string &name, const std::string &canvas = "");
/* A copy of a scene with all of its layers, on the scene's own canvas. Returns
   its uuid, or "". */
std::string sv_canvas_duplicate_scene(const std::string &uuid, const std::string &name);
/* False when the name is empty or another vertical scene already has it. */
bool sv_canvas_rename_scene(const std::string &uuid, const std::string &name);
/* Never removes the last scene. */
bool sv_canvas_remove_scene(const std::string &uuid);
/* A name no vertical scene uses yet: "Vertical 1", "Vertical 2" and so on. */
std::string sv_canvas_free_name(const std::string &base, const std::string &canvas = "");

/* The scene on the picture right now, with a reference the caller releases.
   Safe from any thread. */
obs_scene_t *sv_canvas_scene_ref(const std::string &canvas = "");

/* Adds the Simply Vertical layout to a scene as a full-picture layer, reusing
   the collection's layout source. Returns the new item, not referenced. */
obs_sceneitem_t *sv_canvas_add_layout_item(obs_scene_t *scene, const std::string &canvas = "");

/* Called on the main thread whenever the scene list or the current scene changes. */
void sv_canvas_set_listener(std::function<void()> listener);
/* A second one, for the hotkeys, called at the same moments. It may be called from
   any thread, so it should only queue its work. */
void sv_canvas_set_watcher(std::function<void()> watcher);

/* ---------- the picture ---------- */

/* The picture stream outputs feed from, null when not running. */
video_t *sv_canvas_video(const std::string &canvas = "");

/* Draws the finished picture into the current render target. Graphics thread. */
void sv_canvas_render(const std::string &canvas = "");
