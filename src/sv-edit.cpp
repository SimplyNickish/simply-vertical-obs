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

// The handle, stretch, crop, rotate and snap maths below follows OBS Studio's
// frontend/widgets/OBSBasicPreview.cpp (GPL-2.0-or-later, the OBS Project), so
// a layer behaves on our picture exactly as it does in OBS's own preview.

#include "sv-edit.h"
#include "sv-link.h"
#include "sv-output.h"
#include "sv-source.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/math-defs.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace {

constexpr float CANVAS_W = 1080.0f;
constexpr float CANVAS_H = 1920.0f;
constexpr float HANDLE_PX = 7.0f;      /* handle square size on screen */
constexpr float HANDLE_HIT_PX = 9.0f;  /* how close counts as on a handle */
constexpr float ROT_GAP_PX = 22.0f;    /* the rotate dot sits this far above the top */
constexpr float SNAP_PX = 10.0f;       /* snapping distance on screen */
constexpr size_t UNDO_STEPS = 50;

const uint32_t COLOR_SELECT = 0xFFFF0000;  /* OBS's red selection box */
const uint32_t COLOR_HOVER = 0xFFFF8080;   /* lighter red outline under the pointer */
const uint32_t COLOR_CROP = 0xFF00FF00;    /* green edges where a layer is cropped */
const uint32_t COLOR_HANDLE = 0xFFFF0000;
const uint32_t COLOR_PIECE = 0xFF4D7CFF;  /* a layer inside the vertical layout: blue, apart from OBS's red */
const uint32_t COLOR_PIECE_LOCKED = 0xFFFFB020; /* locked on the website: amber, and no handle */
const uint32_t COLOR_WINDOW = 0xFF34D399;  /* a Replay Window: green dashes, so it can be found when empty */
constexpr float PIECE_MIN = 24.0f;        /* the smallest a layer can be resized to, layout pixels */

vec3 transformed(float x, float y, const matrix4 &m)
{
	vec3 r;
	vec3_set(&r, x, y, 0.0f);
	vec3_transform(&r, &r, &m);
	return r;
}

void rotate_pos(vec2 *pos, float rot)
{
	const float c = cosf(rot), s = sinf(rot);
	const vec2 old = *pos;
	pos->x = c * old.x - s * old.y;
	pos->y = s * old.x + c * old.y;
}

bool has_video(obs_sceneitem_t *item)
{
	return (obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO) != 0;
}

vec2 item_size(obs_sceneitem_t *item)
{
	vec2 size;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		vec2 sc;
		obs_sceneitem_get_scale(item, &sc);
		obs_sceneitem_get_crop(item, &crop);
		size.x = fmaxf(float((int)obs_source_get_width(source) - crop.left - crop.right), 0.0f);
		size.y = fmaxf(float((int)obs_source_get_height(source) - crop.top - crop.bottom), 0.0f);
		vec2_mul(&size, &size, &sc);
	}
	return size;
}

/* The rotate dot's place for a layer. */
vec3 rot_handle_pos(obs_sceneitem_t *item, const matrix4 &box, float gap)
{
	vec2 sc;
	obs_sceneitem_get_scale(item, &sc);
	const bool none = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE;
	const bool invertx = sc.x < 0.0f && none;
	const bool inverty = sc.y < 0.0f && none;
	const float angle = atan2f(invertx ? -box.x.y : box.x.y, invertx ? -box.x.x : box.x.x);
	vec2 off;
	vec2_set(&off, 0.0f, gap);
	rotate_pos(&off, angle);
	vec3 p = transformed(0.5f, inverty ? 1.0f : 0.0f, box);
	p.x -= off.x;
	p.y -= off.y;
	return p;
}

void bounds_of(const matrix4 &box, vec3 &tl, vec3 &br)
{
	const vec3 t[4] = {transformed(0, 0, box), transformed(1, 0, box), transformed(0, 1, box),
			   transformed(1, 1, box)};
	tl = br = t[0];
	for (const vec3 &v : t) {
		vec3_min(&tl, &tl, &v);
		vec3_max(&br, &br, &v);
	}
}

/* ---------- drawing ----------
   Like OBS's own preview, everything is drawn with gs_draw_quadf under a
   matrix, never with the immediate-mode vertex calls: those leave their vertex
   buffer bound, and OBS's own drawing that runs next then crashes on it. */

/* A bar of `thick` from p1 to p2. */
void line(const vec3 &p1, const vec3 &p2, float thick)
{
	const float dx = p2.x - p1.x, dy = p2.y - p1.y;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.0001f)
		return;
	gs_matrix_push();
	gs_matrix_translate3f(p1.x, p1.y, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, atan2f(dy, dx));
	gs_matrix_translate3f(-thick * 0.5f, -thick * 0.5f, 0.0f);
	gs_draw_quadf(nullptr, 0, len + thick, thick);
	gs_matrix_pop();
}

void square(const vec3 &centre, float half)
{
	gs_matrix_push();
	gs_matrix_translate3f(centre.x - half, centre.y - half, 0.0f);
	gs_draw_quadf(nullptr, 0, half * 2.0f, half * 2.0f);
	gs_matrix_pop();
}

/* The rotate handle: a small diamond, so it reads differently from the size handles. */
void dot(const vec3 &centre, float r)
{
	gs_matrix_push();
	gs_matrix_translate3f(centre.x, centre.y, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(45.0f));
	gs_matrix_translate3f(-r * 0.75f, -r * 0.75f, 0.0f);
	gs_draw_quadf(nullptr, 0, r * 1.5f, r * 1.5f);
	gs_matrix_pop();
}

void set_colour(gs_effect_t *solid, uint32_t argb)
{
	vec4 c;
	vec4_from_rgba(&c, (argb & 0xFF00FF00) | ((argb & 0xFF) << 16) | ((argb >> 16) & 0xFF));
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
}

/* Runs one draw pass with the solid effect in one colour. */
void with_colour(gs_effect_t *solid, uint32_t argb, const std::function<void()> &draw)
{
	set_colour(solid, argb);
	while (gs_effect_loop(solid, "Solid"))
		draw();
}

/* ---------- layers inside a layout source ---------- */

/* A canvas point in the source's own pixels (its 1080 x 1920 layout), through the
   layer's position, scale, rotation and crop. */
vec2 to_source(obs_sceneitem_t *item, const vec2 &pos)
{
	matrix4 draw, inv;
	obs_sceneitem_get_draw_transform(item, &draw);
	matrix4_inv(&inv, &draw);
	const vec3 p = transformed(pos.x, pos.y, inv);
	obs_sceneitem_crop cr;
	obs_sceneitem_get_crop(item, &cr);
	vec2 out;
	vec2_set(&out, p.x + (float)cr.left, p.y + (float)cr.top);
	return out;
}

/* The other way: a point in the source's own pixels, on the canvas. */
vec3 from_source(obs_sceneitem_t *item, float x, float y)
{
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	obs_sceneitem_crop cr;
	obs_sceneitem_get_crop(item, &cr);
	return transformed(x - (float)cr.left, y - (float)cr.top, draw);
}

/* The topmost shown layer with a picture under a point, locked or not. */
obs_sceneitem_t *top_item_at(obs_scene_t *scene, const vec2 &pos)
{
	struct Find {
		vec2 pos;
		obs_sceneitem_t *item = nullptr;
	} f{pos};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *f = static_cast<Find *>(param);
			if (!has_video(item) || !obs_sceneitem_visible(item))
				return true;
			matrix4 box, inv;
			obs_sceneitem_get_box_transform(item, &box);
			matrix4_inv(&inv, &box);
			const vec3 p = transformed(f->pos.x, f->pos.y, inv);
			if (p.x >= 0.0f && p.x <= 1.0f && p.y >= 0.0f && p.y <= 1.0f)
				f->item = item;
			return true;
		},
		&f);
	return f.item;
}

} // namespace

/* ---------- SvEdit ---------- */

SvEdit::~SvEdit()
{
	obs_sceneitem_release(handleItem);
	if (pieceItem)
		sv_source_set_draft(obs_sceneitem_get_source(pieceItem), std::string(), SvRect());
	obs_sceneitem_release(pieceItem);
	std::lock_guard<std::mutex> lock(hoverMtx);
	obs_sceneitem_release(hovered);
	obs_sceneitem_release(pieceHovered);
}

/* ---------- layers inside a layout source ---------- */

bool SvEdit::piece_at(obs_scene_t *scene, const vec2 &pos, float sc, PieceHit &hit)
{
	/* Moves are saved on the website, so only while linked, and never while
	   Hold changes keeps the stream as it is. */
	if (!scene || !sv_link_is_linked() || sv_link_holding())
		return false;
	obs_sceneitem_t *item = top_item_at(scene, pos);
	obs_source_t *src = item ? obs_sceneitem_get_source(item) : nullptr;
	std::vector<SvPieceInfo> pieces;
	if (!src || !sv_source_pieces(src, pieces) || pieces.empty())
		return false;
	const vec2 at = to_source(item, pos);
	const float reach = HANDLE_HIT_PX * sc;
	/* Top layer first. */
	for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
		const SvRect &b = it->box;
		/* A layer behind the gameplay can only be grabbed where it shows. */
		const SvRect &c = it->cover;
		if (c.w > 0 && at.x >= c.x && at.x <= c.x + c.w && at.y >= c.y && at.y <= c.y + c.h)
			continue;
		const vec3 corner = from_source(item, b.x + b.w, b.y + b.h);
		const bool onCorner = !it->locked && fabsf(corner.x - pos.x) <= reach && fabsf(corner.y - pos.y) <= reach;
		const bool inside = at.x >= b.x && at.x <= b.x + b.w && at.y >= b.y && at.y <= b.y + b.h;
		if (onCorner || inside) {
			hit.item = item;
			hit.id = it->id;
			hit.box = b;
			hit.locked = it->locked;
			hit.corner = onCorner;
			return true;
		}
	}
	return false;
}

void SvEdit::set_piece_hover(obs_sceneitem_t *item, const SvRect &box, bool locked)
{
	std::lock_guard<std::mutex> lock(hoverMtx);
	pieceHoverBox = box;
	pieceHoverLocked = locked;
	if (item == pieceHovered)
		return;
	obs_sceneitem_release(pieceHovered);
	pieceHovered = item;
	if (pieceHovered)
		obs_sceneitem_addref(pieceHovered);
}

void SvEdit::piece_drag(const vec2 &pos)
{
	const vec2 at = to_source(pieceItem, pos);
	const float dx = at.x - pieceGrab.x, dy = at.y - pieceGrab.y;
	const float W = (float)obs_source_get_width(obs_sceneitem_get_source(pieceItem));
	const float H = (float)obs_source_get_height(obs_sceneitem_get_source(pieceItem));
	SvRect b = pieceStart;
	if (pieceCorner) {
		/* The corner: the top left stays put and the shape is kept, so a round
		   minimap stays round and the camera keeps its picture. */
		const float aspect = std::max(pieceStart.h, 1.0f) / std::max(pieceStart.w, 1.0f);
		/* Whichever way the pointer went further leads. */
		const float byX = pieceStart.w + dx;
		const float byY = (pieceStart.h + dy) / aspect;
		float w = fabsf(byX - pieceStart.w) >= fabsf(byY - pieceStart.w) ? byX : byY;
		w = std::max(w, PIECE_MIN);
		if (W > 0)
			w = std::min(w, W);
		b.w = std::round(w);
		b.h = std::round(w * aspect);
	} else {
		b.x = std::round(pieceStart.x + dx);
		b.y = std::round(pieceStart.y + dy);
	}
	/* The gameplay (id "main") always spans the full width: it only moves up and
	   down, and its corner only makes it taller or shorter. */
	if (pieceId == "main") {
		b.x = 0;
		b.w = pieceStart.w;
		if (pieceCorner)
			b.h = std::round(std::max(PIECE_MIN, pieceStart.h + dy));
	}
	/* Kept on the phone: at least a third of the layer stays in sight. */
	if (W > 0 && H > 0) {
		b.x = std::clamp(b.x, -b.w * 0.66f, W - b.w * 0.34f);
		b.y = std::clamp(b.y, -b.h * 0.66f, H - b.h * 0.34f);
	}
	pieceBox = b;
	sv_source_set_draft(obs_sceneitem_get_source(pieceItem), pieceId, b);
	set_piece_hover(pieceItem, b, false);
}

void SvEdit::piece_release()
{
	obs_source_t *src = obs_sceneitem_get_source(pieceItem);
	if (!moved) {
		sv_source_set_draft(src, std::string(), SvRect());
		return;
	}
	/* Shown where it was dropped until the website has saved it; a move the
	   website refuses (the layer was locked or removed meanwhile) goes back,
	   and the panel says why. */
	obs_source_t *ref = obs_source_get_ref(src);
	if (!ref)
		return;
	const SvRect b = pieceBox;
	UndoStep step;
	step.target = sv_source_move_target(ref);
	step.piece = pieceId;
	step.box = pieceStart;
	step.unsaved = std::make_shared<std::atomic<bool>>(false);
	auto unsaved = step.unsaved;
	sv_link_move_piece(step.target, pieceId, b.x, b.y, b.w, b.h, [ref, unsaved](bool ok, const std::string &reason) {
		if (!ok) {
			unsaved->store(true);
			move_not_saved(reason);
		} else {
			move_saved();
		}
		sv_source_set_draft(ref, std::string(), SvRect());
		obs_source_release(ref);
	});
	undoSteps.push_back(std::move(step));
	if (undoSteps.size() > UNDO_STEPS)
		undoSteps.erase(undoSteps.begin());
}

/* Any thread: the panel's notice line says why a layer move did not happen. */
void SvEdit::move_not_saved(const std::string &reason)
{
	obs_log(LOG_INFO, "a moved layer went back (%s)", reason.empty() ? "not saved" : reason.c_str());
	const char *key = reason == "locked" ? "MoveLocked" : reason == "gone" ? "MoveGone" : "MoveNotSaved";
	sv_output_set_notice(obs_module_text(key), true);
}

/* Any thread: a move that saved takes away an earlier "not saved" line, never any other notice. */
void SvEdit::move_saved()
{
	const std::string shown = sv_output_notice().text;
	for (const char *key : {"MoveLocked", "MoveGone", "MoveNotSaved"})
		if (shown == obs_module_text(key)) {
			sv_output_set_notice(std::string(), false);
			return;
		}
}

obs_sceneitem_t *SvEdit::item_at(obs_scene_t *scene, const vec2 &pos)
{
	struct Find {
		vec2 pos;
		obs_sceneitem_t *item = nullptr;
	} f{pos};
	if (!scene)
		return nullptr;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *f = static_cast<Find *>(param);
			if (!has_video(item) || obs_sceneitem_locked(item) || !obs_sceneitem_visible(item))
				return true;
			matrix4 box, inv;
			obs_sceneitem_get_box_transform(item, &box);
			matrix4_inv(&inv, &box);
			vec3 p;
			vec3_set(&p, f->pos.x, f->pos.y, 0.0f);
			vec3_transform(&p, &p, &inv);
			if (p.x >= 0.0f && p.x <= 1.0f && p.y >= 0.0f && p.y <= 1.0f)
				f->item = item; /* later items are on top, so the last hit wins */
			return true;
		},
		&f);
	return f.item;
}

void SvEdit::select_only(obs_scene_t *scene, obs_sceneitem_t *only)
{
	if (!scene)
		return;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_select(item, item == static_cast<obs_sceneitem_t *>(param));
			return true;
		},
		only);
}

std::vector<obs_sceneitem_t *> SvEdit::selected(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> out;
	if (!scene)
		return out;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (obs_sceneitem_selected(item)) {
				obs_sceneitem_addref(item);
				static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			}
			return true;
		},
		&out);
	std::reverse(out.begin(), out.end());
	return out;
}

void SvEdit::nudge(obs_scene_t *scene, float dx, float dy)
{
	for (obs_sceneitem_t *item : selected(scene)) {
		if (!obs_sceneitem_locked(item)) {
			vec2 p;
			obs_sceneitem_get_pos(item, &p);
			p.x += dx;
			p.y += dy;
			obs_sceneitem_set_pos(item, &p);
		}
		obs_sceneitem_release(item);
	}
}

void SvEdit::remember(obs_scene_t *scene)
{
	if (!scene)
		return;
	obs_data_t *state = obs_scene_save_transform_states(scene, true);
	if (!state)
		return;
	UndoStep step;
	step.scene = obs_data_get_json(state);
	obs_data_release(state);
	undoSteps.push_back(std::move(step));
	if (undoSteps.size() > UNDO_STEPS)
		undoSteps.erase(undoSteps.begin());
}

bool SvEdit::undo()
{
	while (!undoSteps.empty()) {
		UndoStep step = std::move(undoSteps.back());
		undoSteps.pop_back();
		if (step.piece.empty()) {
			obs_scene_load_transform_states(step.scene.c_str());
			return true;
		}
		/* A move that never saved has nothing to take back: the next step is undone instead. */
		if (step.unsaved && step.unsaved->load())
			continue;
		/* A layer inside a layout: its old place is saved on the website, like the move was. */
		const SvRect &b = step.box;
		sv_link_move_piece(step.target, step.piece, b.x, b.y, b.w, b.h, [](bool ok, const std::string &reason) {
			if (!ok)
				move_not_saved(reason);
			else
				move_saved();
		});
		return true;
	}
	return false;
}

void SvEdit::find_handle(obs_scene_t *scene, const vec2 &pos, float sc)
{
	obs_sceneitem_release(handleItem);
	handleItem = nullptr;
	handle = None;

	struct Find {
		vec2 pos;
		float radius;
		float gap;
		float closest;
		obs_sceneitem_t *item = nullptr;
		uint32_t handle = 0;
	} f{pos, HANDLE_HIT_PX * sc, ROT_GAP_PX * sc, HANDLE_HIT_PX * sc};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *f = static_cast<Find *>(param);
			if (!obs_sceneitem_selected(item) || obs_sceneitem_locked(item))
				return true;
			matrix4 box;
			obs_sceneitem_get_box_transform(item, &box);
			vec3 p;
			vec3_set(&p, f->pos.x, f->pos.y, 0.0f);
			auto test = [&](const vec3 &h, uint32_t which) {
				const float d = vec3_dist(&h, &p);
				if (d < f->radius && d < f->closest) {
					f->closest = d;
					f->item = item;
					f->handle = which;
				}
			};
			test(transformed(0.0f, 0.0f, box), Left | Top);
			test(transformed(0.5f, 0.0f, box), Top);
			test(transformed(1.0f, 0.0f, box), Right | Top);
			test(transformed(0.0f, 0.5f, box), Left);
			test(transformed(1.0f, 0.5f, box), Right);
			test(transformed(0.0f, 1.0f, box), Left | Bottom);
			test(transformed(0.5f, 1.0f, box), Bottom);
			test(transformed(1.0f, 1.0f, box), Right | Bottom);
			test(rot_handle_pos(item, box, f->gap), Rot);
			return true;
		},
		&f);

	if (!f.item)
		return;
	handleItem = f.item;
	obs_sceneitem_addref(handleItem);
	handle = f.handle;

	matrix4 box;
	obs_sceneitem_get_box_transform(handleItem, &box);
	const float rot = obs_sceneitem_get_rot(handleItem);

	if (handle == Rot) {
		rotateAngle = rot;
		vec2_set(&rotatePoint, box.t.x + box.x.x / 2 + box.y.x / 2, box.t.y + box.x.y / 2 + box.y.y / 2);
		obs_sceneitem_get_pos(handleItem, &offsetPoint);
		offsetPoint.x -= rotatePoint.x;
		offsetPoint.y -= rotatePoint.y;
		rotate_pos(&offsetPoint, -RAD(rot));
		return;
	}

	itemSize = item_size(handleItem);
	vec3 ul;
	vec3_from_vec4(&ul, &box.t);
	matrix4_identity(&itemToScreen);
	matrix4_rotate_aa4f(&itemToScreen, &itemToScreen, 0.0f, 0.0f, 1.0f, RAD(rot));
	matrix4_translate3f(&itemToScreen, &itemToScreen, ul.x, ul.y, 0.0f);
	matrix4_identity(&screenToItem);
	matrix4_translate3f(&screenToItem, &screenToItem, -ul.x, -ul.y, 0.0f);
	matrix4_rotate_aa4f(&screenToItem, &screenToItem, 0.0f, 0.0f, 1.0f, RAD(-rot));

	obs_sceneitem_get_crop(handleItem, &startCrop);
	obs_source_t *source = obs_sceneitem_get_source(handleItem);
	cropSize.x = float(obs_source_get_width(source) - startCrop.left - startCrop.right);
	cropSize.y = float(obs_source_get_height(source) - startCrop.top - startCrop.bottom);
}

vec3 SvEdit::snap_offset(const vec3 &tl, const vec3 &br) const
{
	vec3 off;
	vec3_zero(&off);
	const float dist = SNAP_PX * scale;
	const float cx = br.x - (br.x - tl.x) / 2.0f;
	const float cy = br.y - (br.y - tl.y) / 2.0f;
	if (fabsf(tl.x) < dist)
		off.x = -tl.x;
	if (fabsf(off.x) < EPSILON && fabsf(CANVAS_W - br.x) < dist)
		off.x = CANVAS_W - br.x;
	if (fabsf(CANVAS_W - (br.x - tl.x)) > dist && fabsf(CANVAS_W / 2.0f - cx) < dist)
		off.x = CANVAS_W / 2.0f - cx;
	if (fabsf(tl.y) < dist)
		off.y = -tl.y;
	if (fabsf(off.y) < EPSILON && fabsf(CANVAS_H - br.y) < dist)
		off.y = CANVAS_H - br.y;
	if (fabsf(CANVAS_H - (br.y - tl.y)) > dist && fabsf(CANVAS_H / 2.0f - cy) < dist)
		off.y = CANVAS_H / 2.0f - cy;
	return off;
}

void SvEdit::move_items(obs_scene_t *scene, const vec2 &pos, const SvEditMods &mods)
{
	vec2 offset, step;
	vec2_sub(&offset, &pos, &startPos);
	vec2_sub(&step, &offset, &lastMove);

	if (!mods.ctrl) {
		/* The selection's outer box, where it would land. */
		struct Box {
			bool first = true;
			vec3 tl, br;
		} b;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (!obs_sceneitem_selected(item))
					return true;
				auto *b = static_cast<Box *>(param);
				matrix4 box;
				obs_sceneitem_get_box_transform(item, &box);
				vec3 tl, br;
				bounds_of(box, tl, br);
				if (b->first) {
					b->tl = tl;
					b->br = br;
					b->first = false;
				} else {
					vec3_min(&b->tl, &b->tl, &tl);
					vec3_max(&b->br, &b->br, &br);
				}
				return true;
			},
			&b);
		if (!b.first) {
			b.tl.x += step.x;
			b.tl.y += step.y;
			b.br.x += step.x;
			b.br.y += step.y;
			const vec3 screenSnap = snap_offset(b.tl, b.br);

			/* Edges of the other layers. */
			struct Edge {
				float dist;
				vec3 tl, br, off;
			} e{SNAP_PX * scale, b.tl, b.br, screenSnap};
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					if (obs_sceneitem_selected(item) || !obs_sceneitem_visible(item))
						return true;
					auto *e = static_cast<Edge *>(param);
					matrix4 box;
					obs_sceneitem_get_box_transform(item, &box);
					vec3 tl, br;
					bounds_of(box, tl, br);
					auto edge = [&](float a, float b, float &out, bool overlap) {
						const float d = fabsf(a - b);
						if (d < e->dist && fabsf(out) < EPSILON && overlap)
							out = a - b;
					};
					const bool ov_y = e->tl.y < br.y && e->br.y > tl.y;
					const bool ov_x = e->tl.x < br.x && e->br.x > tl.x;
					edge(tl.x, e->br.x, e->off.x, ov_y);
					edge(br.x, e->tl.x, e->off.x, ov_y);
					edge(tl.y, e->br.y, e->off.y, ov_x);
					edge(br.y, e->tl.y, e->off.y, ov_x);
					return true;
				},
				&e);
			step.x += e.off.x;
			step.y += e.off.y;
		}
	}

	vec2_add(&lastMove, &lastMove, &step);
	nudge(scene, step.x, step.y);
}

vec3 SvEdit::stretch_pos(const vec3 &tl, const vec3 &br) const
{
	const uint32_t a = obs_sceneitem_get_alignment(handleItem);
	vec3 p;
	vec3_zero(&p);
	p.x = (a & OBS_ALIGN_LEFT) ? tl.x : (a & OBS_ALIGN_RIGHT) ? br.x : (br.x - tl.x) * 0.5f + tl.x;
	p.y = (a & OBS_ALIGN_TOP) ? tl.y : (a & OBS_ALIGN_BOTTOM) ? br.y : (br.y - tl.y) * 0.5f + tl.y;
	return p;
}

void SvEdit::clamp_aspect(vec3 &tl, vec3 &br, vec2 &size, const vec2 &base) const
{
	const float baseAspect = base.x / base.y;
	const float aspect = size.x / size.y;
	const bool same = (size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f);
	const bool corner = (handle & (Left | Right)) && (handle & (Top | Bottom));
	if (corner) {
		if (aspect < baseAspect)
			size.x = size.y * baseAspect * (same ? 1.0f : -1.0f);
		else
			size.y = size.x / baseAspect * (same ? 1.0f : -1.0f);
	} else if (handle & (Top | Bottom)) {
		size.x = size.y * baseAspect * (same ? 1.0f : -1.0f);
	} else if (handle & (Left | Right)) {
		size.y = size.x / baseAspect * (same ? 1.0f : -1.0f);
	}
	size.x = std::round(size.x);
	size.y = std::round(size.y);
	if (handle & Left)
		tl.x = br.x - size.x;
	else if (handle & Right)
		br.x = tl.x + size.x;
	if (handle & Top)
		tl.y = br.y - size.y;
	else if (handle & Bottom)
		br.y = tl.y + size.y;
}

void SvEdit::snap_stretch(vec3 &tl, vec3 &br) const
{
	const vec3 nTL = transformed(tl.x, tl.y, itemToScreen);
	const vec3 nTR = transformed(br.x, tl.y, itemToScreen);
	const vec3 nBL = transformed(tl.x, br.y, itemToScreen);
	const vec3 nBR = transformed(br.x, br.y, itemToScreen);
	vec3 bTL = nTL, bBR = nTL;
	for (const vec3 *v : {&nTR, &nBL, &nBR}) {
		vec3_min(&bTL, &bTL, v);
		vec3_max(&bBR, &bBR, v);
	}
	vec3 off = snap_offset(bTL, bBR);
	vec3_add(&off, &off, &nTL);
	vec3_transform(&off, &off, &screenToItem);
	vec3_sub(&off, &off, &tl);
	if (handle & Left)
		tl.x += off.x;
	else if (handle & Right)
		br.x += off.x;
	if (handle & Top)
		tl.y += off.y;
	else if (handle & Bottom)
		br.y += off.y;
}

void SvEdit::stretch(const vec2 &pos, const SvEditMods &mods)
{
	const obs_bounds_type bt = obs_sceneitem_get_bounds_type(handleItem);
	vec3 tl, br, p;
	vec3_zero(&tl);
	vec3_set(&br, itemSize.x, itemSize.y, 0.0f);
	vec3_set(&p, pos.x, pos.y, 0.0f);
	vec3_transform(&p, &p, &screenToItem);

	if (handle & Left)
		tl.x = p.x;
	else if (handle & Right)
		br.x = p.x;
	if (handle & Top)
		tl.y = p.y;
	else if (handle & Bottom)
		br.y = p.y;

	if (!mods.ctrl)
		snap_stretch(tl, br);

	obs_source_t *source = obs_sceneitem_get_source(handleItem);
	const uint32_t cx = obs_source_get_width(source), cy = obs_source_get_height(source);
	/* A source that reports no size would be stuck invisible after a resize. */
	if (!cx || !cy)
		return;

	vec2 base;
	vec2_set(&base, float(cx), float(cy));
	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	if (bt != OBS_BOUNDS_NONE) {
		if (mods.shift)
			clamp_aspect(tl, br, size, base);
		if (tl.x > br.x)
			std::swap(tl.x, br.x);
		if (tl.y > br.y)
			std::swap(tl.y, br.y);
		vec2_abs(&size, &size);
		obs_sceneitem_set_bounds(handleItem, &size);
	} else {
		obs_sceneitem_crop c;
		obs_sceneitem_get_crop(handleItem, &c);
		base.x -= float(c.left + c.right);
		base.y -= float(c.top + c.bottom);
		if (!mods.shift)
			clamp_aspect(tl, br, size, base);
		vec2_div(&size, &size, &base);
		obs_sceneitem_set_scale(handleItem, &size);
	}

	p = stretch_pos(tl, br);
	vec3_transform(&p, &p, &itemToScreen);
	vec2 np;
	vec2_set(&np, std::round(p.x), std::round(p.y));
	obs_sceneitem_set_pos(handleItem, &np);
}

void SvEdit::crop(const vec2 &pos)
{
	const obs_bounds_type bt = obs_sceneitem_get_bounds_type(handleItem);
	const uint32_t align = obs_sceneitem_get_alignment(handleItem);
	vec3 tl, br, p;
	vec3_zero(&tl);
	vec3_set(&br, itemSize.x, itemSize.y, 0.0f);
	vec3_set(&p, pos.x, pos.y, 0.0f);
	vec3_transform(&p, &p, &screenToItem);

	obs_sceneitem_crop c = startCrop;
	vec2 raw, sc;
	obs_sceneitem_get_scale(handleItem, &raw);
	vec2_set(&sc, bt == OBS_BOUNDS_NONE ? raw.x : fabsf(raw.x), bt == OBS_BOUNDS_NONE ? raw.y : fabsf(raw.y));

	vec2 maxTL, maxBR;
	vec2_set(&maxTL, float(-c.left) * sc.x, float(-c.top) * sc.y);
	vec2_set(&maxBR, itemSize.x + c.right * sc.x, itemSize.y + c.bottom * sc.y);

	using mm = float (*)(float, float);
	const mm lo = [](float a, float b) { return a < b ? a : b; };
	const mm hi = [](float a, float b) { return a > b ? a : b; };
	const bool flipX = sc.x < 0.0f && bt == OBS_BOUNDS_NONE;
	const bool flipY = sc.y < 0.0f && bt == OBS_BOUNDS_NONE;
	const mm min_x = flipX ? hi : lo, max_x = flipX ? lo : hi;
	const mm min_y = flipY ? hi : lo, max_y = flipY ? lo : hi;

	p.x = max_x(min_x(p.x, maxBR.x), maxTL.x);
	p.y = max_y(min_y(p.y, maxBR.y), maxTL.y);

	if (handle & Left)
		p.x = tl.x = min_x(p.x, itemSize.x - 2.0f * sc.x);
	else if (handle & Right)
		p.x = br.x = max_x(p.x, 2.0f * sc.x);
	if (handle & Top)
		p.y = tl.y = min_y(p.y, itemSize.y - 2.0f * sc.y);
	else if (handle & Bottom)
		p.y = br.y = max_y(p.y, 2.0f * sc.y);

	const uint32_t AX = Left | Right, AY = Top | Bottom;
	vec3 np;
	vec3_zero(&np);
	const uint32_t ax = align & AX, ay = align & AY;
	if (ax == (handle & AX) && ax != 0)
		np.x = p.x;
	else if (align & Right)
		np.x = itemSize.x;
	else if (!(align & Left))
		np.x = itemSize.x * 0.5f;
	if (ay == (handle & AY) && ay != 0)
		np.y = p.y;
	else if (align & Bottom)
		np.y = itemSize.y;
	else if (!(align & Top))
		np.y = itemSize.y * 0.5f;

	c = startCrop;
	if (handle & Left)
		c.left += int(std::round(tl.x / sc.x));
	else if (handle & Right)
		c.right += int(std::round((itemSize.x - br.x) / sc.x));
	if (handle & Top)
		c.top += int(std::round(tl.y / sc.y));
	else if (handle & Bottom)
		c.bottom += int(std::round((itemSize.y - br.y) / sc.y));

	vec3_transform(&np, &np, &itemToScreen);
	np.x = std::round(np.x);
	np.y = std::round(np.y);

	obs_sceneitem_defer_update_begin(handleItem);
	obs_sceneitem_set_crop(handleItem, &c);
	if (bt == OBS_BOUNDS_NONE)
		obs_sceneitem_set_pos(handleItem, (vec2 *)&np);
	obs_sceneitem_defer_update_end(handleItem);
}

void SvEdit::rotate(const vec2 &pos, const SvEditMods &mods)
{
	float angle = atan2f(pos.y - rotatePoint.y, pos.x - rotatePoint.x) + RAD(90);
	auto snapTo = [&](float deg, float thresh) {
		if (fabsf(angle - RAD(deg)) < RAD(thresh))
			angle = RAD(deg);
	};
	if (mods.shift) {
		for (int i = 0; i <= 360 / 15; i++)
			snapTo((float)(i * 15 - 90), 7.5f);
	} else if (!mods.ctrl) {
		snapTo(rotateAngle, 5);
		for (float d : {-90.f, -45.f, 0.f, 45.f, 90.f, 135.f, 180.f, 225.f, 270.f, 315.f})
			snapTo(d, 5);
	}
	vec2 p = offsetPoint;
	rotate_pos(&p, angle);
	p.x += rotatePoint.x;
	p.y += rotatePoint.y;
	obs_sceneitem_set_rot(handleItem, DEG(angle));
	obs_sceneitem_set_pos(handleItem, &p);
}

SvCursor SvEdit::cursor_for(obs_sceneitem_t *item, uint32_t flags)
{
	if (!item || !flags)
		return SvCursor::Arrow;
	if (flags & Rot)
		return SvCursor::Rotate;
	float rotation = obs_sceneitem_get_rot(item);
	vec2 sc;
	obs_sceneitem_get_scale(item, &sc);
	if (rotation < 0.0f)
		rotation += 360.0f;
	const int octant = int(std::round(rotation / 45.0f));
	const bool corner = (flags & (flags - 1)) != 0;
	if (sc.x < 0.0f && corner)
		flags ^= Left | Right;
	if (sc.y < 0.0f && corner)
		flags ^= Top | Bottom;
	if (octant % 4 >= 2) {
		if (corner)
			flags ^= Top | Bottom;
		else
			flags = (flags >> 2) | (flags << 2);
	}
	if (octant % 2 == 1) {
		if (corner)
			flags &= (flags % 3 == 0) ? ~Top & ~Bottom : ~Left & ~Right;
		else
			flags = (flags % 4 == 0) ? flags | flags >> ((flags / 2) - 1) : flags | ((flags >> 2) | (flags << 2));
	}
	if ((flags & Left && flags & Top) || (flags & Right && flags & Bottom))
		return SvCursor::SizeFDiag;
	if ((flags & Left && flags & Bottom) || (flags & Right && flags & Top))
		return SvCursor::SizeBDiag;
	if (flags & (Left | Right))
		return SvCursor::SizeH;
	if (flags & (Top | Bottom))
		return SvCursor::SizeV;
	return SvCursor::Arrow;
}

void SvEdit::set_hover(obs_sceneitem_t *item)
{
	std::lock_guard<std::mutex> lock(hoverMtx);
	if (item == hovered)
		return;
	obs_sceneitem_release(hovered);
	hovered = item;
	if (hovered)
		obs_sceneitem_addref(hovered);
}

SvCursor SvEdit::press(obs_scene_t *scene, const vec2 &pos, float sc, const SvEditMods &mods)
{
	scale = sc;
	down = true;
	moved = false;
	cropping = mods.alt;
	startPos = pos;
	vec2_zero(&lastMove);

	obs_data_t *state = scene ? obs_scene_save_transform_states(scene, true) : nullptr;
	before = state ? obs_data_get_json(state) : "";
	obs_data_release(state);

	if (!scene)
		return SvCursor::Arrow;
	find_handle(scene, pos, sc);
	if (handle != None)
		return handle == Rot ? SvCursor::Rotating : cursor_for(handleItem, handle);

	/* A layer inside the vertical layout: that layer moves, not the whole layout.
	   A locked one does nothing at all. */
	PieceHit hit;
	if (piece_at(scene, pos, sc, hit)) {
		pieceDown = true;
		set_hover(nullptr);
		if (hit.locked)
			return SvCursor::Arrow;
		pieceItem = hit.item;
		obs_sceneitem_addref(pieceItem);
		pieceId = hit.id;
		pieceStart = pieceBox = hit.box;
		pieceCorner = hit.corner;
		pieceGrab = to_source(hit.item, pos);
		return pieceCorner ? SvCursor::SizeFDiag : SvCursor::Move;
	}

	/* Pressing on a layer that is not selected selects it straight away, so
	   it can be dragged in one go, like OBS. Ctrl adds or removes instead. */
	obs_sceneitem_t *under = item_at(scene, pos);
	if (under && !obs_sceneitem_selected(under)) {
		if (mods.ctrl)
			obs_sceneitem_select(under, true);
		else
			select_only(scene, under);
	}
	overSelected = under && obs_sceneitem_selected(under);
	return overSelected ? SvCursor::Move : SvCursor::Arrow;
}

SvCursor SvEdit::drag(obs_scene_t *scene, const vec2 &raw, const SvEditMods &mods)
{
	if (!down || !scene)
		return SvCursor::Arrow;
	vec2 pos;
	vec2_set(&pos, std::round(raw.x), std::round(raw.y));
	/* A tiny wobble while clicking is not a drag. */
	if (!moved && fabsf(raw.x - startPos.x) < 3.0f * scale && fabsf(raw.y - startPos.y) < 3.0f * scale)
		return SvCursor::Arrow;
	moved = true;

	if (pieceDown) {
		if (!pieceItem)
			return SvCursor::Arrow;
		piece_drag(raw);
		return pieceCorner ? SvCursor::SizeFDiag : SvCursor::Move;
	}

	if (handle != None && handleItem) {
		if (obs_sceneitem_locked(handleItem))
			return SvCursor::Arrow;
		if (handle == Rot) {
			rotate(pos, mods);
			return SvCursor::Rotating;
		}
		if (cropping)
			crop(pos);
		else
			stretch(pos, mods);
		return cursor_for(handleItem, handle);
	}
	if (overSelected) {
		move_items(scene, pos, mods);
		return SvCursor::Move;
	}
	return SvCursor::Arrow;
}

bool SvEdit::release(obs_scene_t *scene, const vec2 &pos, const SvEditMods &mods)
{
	if (!down)
		return false;
	down = false;
	cropping = false;

	if (pieceDown) {
		/* Saved on the website; piece_release keeps its own undo step. */
		pieceDown = false;
		if (pieceItem) {
			piece_release();
			obs_sceneitem_release(pieceItem);
			pieceItem = nullptr;
		}
		obs_sceneitem_release(handleItem);
		handleItem = nullptr;
		handle = None;
		before.clear();
		moved = false;
		return false;
	}

	if (!moved && scene && handle == None) {
		/* A plain click: select what is under the pointer, or nothing. */
		obs_sceneitem_t *under = item_at(scene, pos);
		if (mods.ctrl) {
			(void)under; /* Ctrl was handled on press */
		} else {
			select_only(scene, under);
		}
	}

	obs_sceneitem_release(handleItem);
	handleItem = nullptr;
	handle = None;

	bool changed = false;
	if (moved && scene && !before.empty()) {
		obs_data_t *after = obs_scene_save_transform_states(scene, true);
		if (after && before != obs_data_get_json(after)) {
			UndoStep step;
			step.scene = before;
			undoSteps.push_back(std::move(step));
			if (undoSteps.size() > UNDO_STEPS)
				undoSteps.erase(undoSteps.begin());
			changed = true;
		}
		obs_data_release(after);
	}
	before.clear();
	moved = false;
	return changed;
}

SvCursor SvEdit::hover(obs_scene_t *scene, const vec2 &pos, float sc)
{
	scale = sc;
	if (!scene) {
		set_hover(nullptr);
		set_piece_hover(nullptr, SvRect(), false);
		return SvCursor::Arrow;
	}
	find_handle(scene, pos, sc);
	const bool onHandle = handle != None;
	const SvCursor c = cursor_for(handleItem, handle);
	obs_sceneitem_release(handleItem);
	handleItem = nullptr;
	handle = None;
	/* A selected layer's own handles come first, then a layer inside the vertical layout. */
	PieceHit hit;
	if (!onHandle && piece_at(scene, pos, sc, hit)) {
		set_hover(nullptr);
		set_piece_hover(hit.item, hit.box, hit.locked);
		return hit.locked ? SvCursor::Arrow : hit.corner ? SvCursor::SizeFDiag : SvCursor::Move;
	}
	set_piece_hover(nullptr, SvRect(), false);
	set_hover(item_at(scene, pos));
	return c;
}

void SvEdit::leave()
{
	set_hover(nullptr);
	if (!pieceDown)
		set_piece_hover(nullptr, SvRect(), false);
}

void SvEdit::draw(obs_scene_t *scene, float pixel)
{
	if (!scene)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid)
		return;

	obs_sceneitem_t *hov = nullptr;
	{
		std::lock_guard<std::mutex> lock(hoverMtx);
		if (hovered) {
			hov = hovered;
			obs_sceneitem_addref(hov);
		}
	}

	struct Ctx {
		gs_effect_t *solid;
		float px;
		obs_sceneitem_t *hover;
	} ctx{solid, pixel, hov};

	gs_blend_state_push();
	gs_reset_blend_state();

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			const bool sel = obs_sceneitem_selected(item);
			if (!sel && item != c->hover) {
				/* A Replay Window shows nothing until a replay plays, so its place is
				   marked here, in the panel only, never on stream. */
				obs_source_t *src = obs_sceneitem_get_source(item);
				if (obs_sceneitem_visible(item) && src &&
				    std::strcmp(obs_source_get_id(src), "simply_vertical_replay_window") == 0) {
					matrix4 box;
					obs_sceneitem_get_box_transform(item, &box);
					const vec3 corners[4] = {transformed(0, 0, box), transformed(1, 0, box), transformed(1, 1, box),
								 transformed(0, 1, box)};
					with_colour(c->solid, COLOR_WINDOW, [&] {
						for (int i = 0; i < 4; i++) {
							const vec3 &a = corners[i], &b = corners[(i + 1) % 4];
							const float len = std::hypot(b.x - a.x, b.y - a.y);
							const float dash = 12.0f * c->px;
							for (float d = 0; d < len; d += dash * 2) {
								const float t0 = d / len, t1 = std::min(1.0f, (d + dash) / len);
								vec3 p0, p1;
								vec3_set(&p0, a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0, 0);
								vec3_set(&p1, a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1, 0);
								line(p0, p1, 2.0f * c->px);
							}
						}
					});
				}
				return true;
			}
			if (!obs_sceneitem_visible(item) || !has_video(item))
				return true;

			matrix4 box;
			obs_sceneitem_get_box_transform(item, &box);
			const vec3 tl = transformed(0, 0, box), tr = transformed(1, 0, box);
			const vec3 bl = transformed(0, 1, box), brr = transformed(1, 1, box);

			if (!sel) {
				with_colour(c->solid, COLOR_HOVER, [&] {
					line(tl, tr, c->px);
					line(tr, brr, c->px);
					line(brr, bl, c->px);
					line(bl, tl, c->px);
				});
				return true;
			}

			obs_sceneitem_crop cr;
			obs_sceneitem_get_crop(item, &cr);
			const float thick = 2.0f * c->px;
			auto edgeColour = [&](int cropped) { return cropped > 0 ? COLOR_CROP : COLOR_SELECT; };
			with_colour(c->solid, edgeColour(cr.top), [&] { line(tl, tr, thick); });
			with_colour(c->solid, edgeColour(cr.right), [&] { line(tr, brr, thick); });
			with_colour(c->solid, edgeColour(cr.bottom), [&] { line(brr, bl, thick); });
			with_colour(c->solid, edgeColour(cr.left), [&] { line(bl, tl, thick); });

			if (obs_sceneitem_locked(item))
				return true;

			const float half = HANDLE_PX * 0.5f * c->px;
			const vec3 rot = rot_handle_pos(item, box, ROT_GAP_PX * c->px);
			vec2 sc;
			obs_sceneitem_get_scale(item, &sc);
			const bool inverty = sc.y < 0.0f && obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE;
			const vec3 topMid = transformed(0.5f, inverty ? 1.0f : 0.0f, box);
			with_colour(c->solid, COLOR_HANDLE, [&] {
				for (const vec3 &h : {tl, tr, bl, brr, transformed(0.5f, 0, box), transformed(0.5f, 1, box),
						      transformed(0, 0.5f, box), transformed(1, 0.5f, box)})
					square(h, half);
				line(topMid, rot, c->px);
				dot(rot, half * 1.15f);
			});
			return true;
		},
		&ctx);

	/* The layer of the vertical layout under the pointer: an outline and, unless
	   it is locked, the corner handle. Only while the pointer is on it. */
	obs_sceneitem_t *pieceItemNow = nullptr;
	SvRect pb;
	bool pieceLocked = false;
	{
		std::lock_guard<std::mutex> lock(hoverMtx);
		if (pieceHovered) {
			pieceItemNow = pieceHovered;
			obs_sceneitem_addref(pieceItemNow);
			pb = pieceHoverBox;
			pieceLocked = pieceHoverLocked;
		}
	}
	if (pieceItemNow) {
		const vec3 tl = from_source(pieceItemNow, pb.x, pb.y);
		const vec3 tr = from_source(pieceItemNow, pb.x + pb.w, pb.y);
		const vec3 br = from_source(pieceItemNow, pb.x + pb.w, pb.y + pb.h);
		const vec3 bl = from_source(pieceItemNow, pb.x, pb.y + pb.h);
		const float thick = 2.0f * pixel;
		with_colour(solid, pieceLocked ? COLOR_PIECE_LOCKED : COLOR_PIECE, [&] {
			line(tl, tr, thick);
			line(tr, br, thick);
			line(br, bl, thick);
			line(bl, tl, thick);
			if (!pieceLocked)
				square(br, HANDLE_PX * 0.75f * pixel);
		});
		obs_sceneitem_release(pieceItemNow);
	}

	gs_blend_state_pop();
	gs_load_vertexbuffer(nullptr);
	gs_load_indexbuffer(nullptr);
	obs_sceneitem_release(hov);
}
