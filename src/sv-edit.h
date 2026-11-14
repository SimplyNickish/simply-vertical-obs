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

// Editing the vertical scene on the panel's picture, the way OBS's own preview
// works: click to select, drag to move, drag a handle to resize, hold Alt on a
// handle to crop, drag the top dot to rotate, snapping to the edges, the centre
// and other layers. Hold Ctrl to move without snapping, Shift to resize freely.
// The handle maths follows OBS Studio's OBSBasicPreview (GPL-2.0-or-later).
//
// Positions are in canvas pixels (0..1080, 0..1920). `scale` is how many
// canvas pixels one screen pixel covers, so handles and snapping feel the same
// size whatever size the panel is.

#pragma once

#include <obs.h>
#include <graphics/matrix4.h>

#include <mutex>
#include <string>
#include <vector>

struct SvEditMods {
	bool alt = false;
	bool shift = false;
	bool ctrl = false;
};

enum class SvCursor { Arrow, Move, SizeH, SizeV, SizeFDiag, SizeBDiag, Rotate, Rotating };

class SvEdit {
public:
	~SvEdit();

	/* Main thread. Returns the cursor to show. */
	SvCursor press(obs_scene_t *scene, const vec2 &pos, float scale, const SvEditMods &mods);
	SvCursor drag(obs_scene_t *scene, const vec2 &pos, const SvEditMods &mods);
	/* True when the scene changed, so the caller can keep an undo step. */
	bool release(obs_scene_t *scene, const vec2 &pos, const SvEditMods &mods);
	SvCursor hover(obs_scene_t *scene, const vec2 &pos, float scale);
	void leave();
	bool dragging() const { return down; }

	/* The topmost unlocked layer with a picture under a point, or null. */
	static obs_sceneitem_t *item_at(obs_scene_t *scene, const vec2 &pos);
	static void select_only(obs_scene_t *scene, obs_sceneitem_t *item);
	/* The selected layers, top first, each referenced: release them. */
	static std::vector<obs_sceneitem_t *> selected(obs_scene_t *scene);
	/* Arrow keys: moves the selection by whole canvas pixels. */
	static void nudge(obs_scene_t *scene, float dx, float dy);

	/* Undo for layer positions and sizes. Call remember() before a change that
	   does not come from dragging (menu, arrow keys); dragging keeps its own. */
	void remember(obs_scene_t *scene);
	bool undo();
	void forget() { undoSteps.clear(); }

	/* Graphics thread: draws the boxes, handles, crop edges and hover outline
	   over the picture. The current projection must be canvas pixels. */
	void draw(obs_scene_t *scene, float pixel);

private:
	enum Handle : uint32_t {
		None = 0,
		Left = 1 << 0,
		Right = 1 << 1,
		Top = 1 << 2,
		Bottom = 1 << 3,
		Rot = 1 << 4,
	};

	void find_handle(obs_scene_t *scene, const vec2 &pos, float scale);
	void stretch(const vec2 &pos, const SvEditMods &mods);
	void crop(const vec2 &pos);
	void rotate(const vec2 &pos, const SvEditMods &mods);
	void move_items(obs_scene_t *scene, const vec2 &pos, const SvEditMods &mods);
	vec3 snap_offset(const vec3 &tl, const vec3 &br) const;
	void snap_stretch(vec3 &tl, vec3 &br) const;
	void clamp_aspect(vec3 &tl, vec3 &br, vec2 &size, const vec2 &base) const;
	vec3 stretch_pos(const vec3 &tl, const vec3 &br) const;
	void set_hover(obs_sceneitem_t *item);
	static SvCursor cursor_for(obs_sceneitem_t *item, uint32_t flags);

	bool down = false;
	bool moved = false;
	bool overSelected = false;
	bool cropping = false;
	float scale = 1.0f;
	vec2 startPos = {};
	vec2 lastMove = {};

	obs_sceneitem_t *handleItem = nullptr; /* referenced */
	uint32_t handle = None;
	vec2 itemSize = {};
	vec2 cropSize = {};
	obs_sceneitem_crop startCrop = {};
	matrix4 itemToScreen = {};
	matrix4 screenToItem = {};
	float rotateAngle = 0.0f;
	vec2 rotatePoint = {};
	vec2 offsetPoint = {};

	std::mutex hoverMtx;
	obs_sceneitem_t *hovered = nullptr; /* referenced */

	std::string before;                  /* the layers' places when the drag began */
	std::vector<std::string> undoSteps; /* newest last */
};
