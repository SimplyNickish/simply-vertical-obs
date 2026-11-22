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

#include "sv-canvas.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

constexpr const char *LAYOUT_ID = "simply_vertical_source";
constexpr const char *STORE_CURRENT = "vertical_scene";

/* One of our canvases. The first in the list is always the main vertical one. */
struct Cv {
	std::string uuid;
	std::string name;
	bool main = false;
	obs_canvas_t *canvas = nullptr;  /* referenced */
	obs_source_t *current = nullptr; /* the scene on its picture, referenced */
	obs_source_t *cover = nullptr;   /* an instant replay shown instead of the scene, referenced */
	unsigned users = 0;
};

/* Guards the canvases and their current scenes against the graphics thread. */
std::mutex g_mtx;
std::vector<Cv> g_cvs;
std::string g_view; /* the canvas the panel shows, "" for the main one */
std::function<void()> g_listener;
std::function<void()> g_watcher;

std::string current_collection()
{
	char *c = obs_frontend_get_current_scene_collection();
	std::string s = c ? c : "";
	bfree(c);
	return s;
}

bool vertical_video_info(obs_video_info &ovi)
{
	if (!obs_get_video_info(&ovi))
		return false;
	ovi.base_width = 1080;
	ovi.base_height = 1920;
	ovi.output_width = 1080;
	ovi.output_height = 1920;
	/* A vertical stream is always plain H.264 friendly, whatever the main one is. */
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	return true;
}

/* The canvas with this id, or the main one for an empty id. Lock held. */
Cv *find_locked(const std::string &id)
{
	if (g_cvs.empty())
		return nullptr;
	if (id.empty())
		return &g_cvs[0];
	for (Cv &c : g_cvs) {
		if (c.uuid == id)
			return &c;
	}
	return nullptr;
}

std::string store_key(const Cv &c)
{
	return c.main ? std::string(STORE_CURRENT) : std::string(STORE_CURRENT) + ":" + c.uuid;
}

/* OBS rebuilds canvases when its video settings change, keeping the old frame
   rate. Bring ours back in step with the main one. */
void on_video_reset(void *, calldata_t *)
{
	std::vector<obs_canvas_t *> all;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (Cv &c : g_cvs)
			all.push_back(obs_canvas_get_ref(c.canvas));
	}
	obs_video_info ovi = {};
	const bool ok = vertical_video_info(ovi);
	for (obs_canvas_t *canvas : all) {
		if (canvas && ok)
			obs_canvas_reset_video(canvas, &ovi);
		obs_canvas_release(canvas);
	}
}

void apply_channel_locked(Cv &c)
{
	if (c.canvas)
		obs_canvas_set_channel(c.canvas, 0, c.users ? (c.cover ? c.cover : c.current) : nullptr);
}

void notify()
{
	if (g_listener)
		g_listener();
	if (g_watcher)
		g_watcher();
}

obs_canvas_t *canvas_ref(const std::string &id)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(id);
	return c && c->canvas ? obs_canvas_get_ref(c->canvas) : nullptr;
}

/* The collection's saved vertical canvas, referenced, or null. */
obs_canvas_t *find_saved_canvas()
{
	obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	obs_canvas_t *found = nullptr;
	for (size_t i = 0; i < list.canvases.num; i++) {
		obs_canvas_t *c = list.canvases.array[i];
		const char *name = obs_canvas_get_name(c);
		if (!found && name && std::strcmp(name, SV_CANVAS_NAME) == 0)
			found = obs_canvas_get_ref(c);
	}
	obs_frontend_canvas_list_free(&list);
	return found;
}

/* The collection's saved extra canvases, referenced, in the order OBS keeps them. */
std::vector<obs_canvas_t *> find_saved_extras()
{
	std::vector<obs_canvas_t *> out;
	obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	const size_t prefix = std::strlen(SV_CANVAS_PREFIX);
	for (size_t i = 0; i < list.canvases.num; i++) {
		obs_canvas_t *c = list.canvases.array[i];
		const char *name = obs_canvas_get_name(c);
		if (name && std::strncmp(name, SV_CANVAS_PREFIX, prefix) == 0 && std::strlen(name) > prefix &&
		    !obs_canvas_removed(c))
			out.push_back(obs_canvas_get_ref(c));
	}
	obs_frontend_canvas_list_free(&list);
	return out;
}

std::vector<obs_source_t *> scene_sources(obs_canvas_t *canvas)
{
	std::vector<obs_source_t *> out;
	if (!canvas)
		return out;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *s) {
			if (!obs_source_is_group(s))
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(s));
			return true;
		},
		&out);
	return out;
}

void release_all(std::vector<obs_source_t *> &list)
{
	for (obs_source_t *s : list)
		obs_source_release(s);
	list.clear();
}

/* The collection's Simply Vertical layout source, made if it has none. Referenced. */
obs_source_t *layout_source()
{
	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			auto **out = static_cast<obs_source_t **>(param);
			if (std::strcmp(obs_source_get_id(s), LAYOUT_ID) == 0) {
				*out = obs_source_get_ref(s);
				return false;
			}
			return true;
		},
		&found);
	if (found)
		return found;

	const SvPicks picks = sv_store_current_picks();
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "game_source", picks.game.c_str());
	obs_data_set_string(st, "cam_source", picks.cam.c_str());
	std::string name = T("LayoutSourceName");
	for (int n = 2; n < 100; n++) {
		obs_source_t *clash = obs_get_source_by_name(name.c_str());
		if (!clash)
			break;
		obs_source_release(clash);
		name = std::string(T("LayoutSourceName")) + " " + std::to_string(n);
	}
	found = obs_source_create(LAYOUT_ID, name.c_str(), st, nullptr);
	obs_data_release(st);
	return found;
}

/* The layout source a canvas's scenes use, referenced, or null when none has one. */
obs_source_t *canvas_layout_source(obs_canvas_t *canvas)
{
	obs_source_t *found = nullptr;
	std::vector<obs_source_t *> scenes = scene_sources(canvas);
	for (obs_source_t *s : scenes) {
		if (found)
			break;
		obs_scene_enum_items(
			obs_scene_from_source(s),
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *src = obs_sceneitem_get_source(item);
				if (src && std::strcmp(obs_source_get_id(src), LAYOUT_ID) == 0) {
					*static_cast<obs_source_t **>(param) = obs_source_get_ref(src);
					return false;
				}
				return true;
			},
			&found);
	}
	release_all(scenes);
	return found;
}

void fit_full(obs_sceneitem_t *item)
{
	vec2 pos = {0.0f, 0.0f};
	vec2 bounds = {1080.0f, 1920.0f};
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_defer_update_end(item);
}

/* The first time a collection gets real vertical scenes, the sources it used
   to lay over the whole picture come along as layers above the layout. */
void bring_over_old_overlays(obs_scene_t *scene)
{
	const std::string collection = current_collection();
	const std::vector<std::string> old = sv_store_list(collection, SV_STORE_OVERLAYS);
	for (const std::string &uuid : old) {
		obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
		if (!src)
			continue;
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		obs_source_release(src);
		if (item)
			fit_full(item);
	}
	if (!old.empty())
		sv_store_set_list(collection, SV_STORE_OVERLAYS, {});
}

obs_source_t *find_scene(obs_canvas_t *canvas, const std::string &uuid)
{
	if (!canvas || uuid.empty())
		return nullptr;
	std::vector<obs_source_t *> all = scene_sources(canvas);
	obs_source_t *found = nullptr;
	for (obs_source_t *s : all) {
		if (!found && uuid == obs_source_get_uuid(s))
			found = obs_source_get_ref(s);
	}
	release_all(all);
	return found;
}

/* Which of our canvases holds this scene, "" when none does. */
std::string canvas_of_scene(const std::string &sceneUuid)
{
	std::vector<std::pair<std::string, obs_canvas_t *>> all;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (Cv &c : g_cvs)
			all.push_back({c.uuid, obs_canvas_get_ref(c.canvas)});
	}
	std::string found;
	for (auto &[id, canvas] : all) {
		if (found.empty()) {
			obs_source_t *s = find_scene(canvas, sceneUuid);
			if (s)
				found = id;
			obs_source_release(s);
		}
		obs_canvas_release(canvas);
	}
	return found;
}

void set_current(const std::string &id, obs_source_t *scene)
{
	std::string key;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(id);
		if (!c || scene == c->current)
			return;
		obs_source_t *old = c->current;
		c->current = scene ? obs_source_get_ref(scene) : nullptr;
		apply_channel_locked(*c);
		obs_source_release(old);
		key = store_key(*c);
	}
	sv_store_set_list(current_collection(), key.c_str(), {scene ? std::string(obs_source_get_uuid(scene)) : std::string()});
}

bool name_taken(obs_canvas_t *canvas, const std::string &name, const std::string &exceptUuid)
{
	bool taken = false;
	std::vector<obs_source_t *> all = scene_sources(canvas);
	for (obs_source_t *s : all) {
		if (name == obs_source_get_name(s) && exceptUuid != obs_source_get_uuid(s))
			taken = true;
	}
	release_all(all);
	return taken;
}

obs_scene_t *make_scene(obs_canvas_t *canvas, const std::string &name)
{
	/* Its layout layer is the canvas's own layout source, found before the new scene exists. */
	obs_source_t *layout = canvas_layout_source(canvas);
	if (!layout)
		layout = layout_source();
	obs_scene_t *scene = obs_canvas_scene_create(canvas, name.c_str());
	if (scene && layout) {
		if (obs_sceneitem_t *item = obs_scene_add(scene, layout))
			fit_full(item);
	}
	obs_source_release(layout);
	return scene;
}

/* Every layer of one scene onto another, bottom layer first so they stack the
   same way. The sources themselves are shared, not copied. */
struct CopyCtx {
	obs_scene_t *to;
	obs_source_t *swapFrom; /* a source to replace with swapTo in the copy, or null */
	obs_source_t *swapTo;
};

void copy_items(obs_scene_t *from, obs_scene_t *to, obs_source_t *swapFrom = nullptr, obs_source_t *swapTo = nullptr)
{
	CopyCtx ctx{to, swapFrom, swapTo};
	obs_scene_enum_items(
		from,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<CopyCtx *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (c->swapFrom && src == c->swapFrom)
				src = c->swapTo;
			obs_sceneitem_t *added = obs_scene_add(c->to, src);
			if (!added)
				return true;
			obs_transform_info info;
			obs_sceneitem_crop crop;
			obs_sceneitem_get_info2(item, &info);
			obs_sceneitem_get_crop(item, &crop);
			obs_sceneitem_defer_update_begin(added);
			obs_sceneitem_set_info2(added, &info);
			obs_sceneitem_set_crop(added, &crop);
			obs_sceneitem_set_visible(added, obs_sceneitem_visible(item));
			obs_sceneitem_set_locked(added, obs_sceneitem_locked(item));
			obs_sceneitem_set_scale_filter(added, obs_sceneitem_get_scale_filter(item));
			obs_sceneitem_set_blending_mode(added, obs_sceneitem_get_blending_mode(item));
			obs_sceneitem_defer_update_end(added);
			return true;
		},
		&ctx);
}

/* The scene to show on a canvas: the one saved for it, or its first. Gives one
   to a canvas that has none. Returns a reference. */
obs_source_t *pick_scene(obs_canvas_t *canvas, const std::string &key, bool firstCanvas)
{
	std::vector<obs_source_t *> scenes = scene_sources(canvas);
	if (scenes.empty()) {
		std::string name = T("VSceneBase");
		name += " 1";
		obs_scene_t *first = make_scene(canvas, name);
		if (first) {
			if (firstCanvas)
				bring_over_old_overlays(first);
			obs_scene_release(first);
		}
		scenes = scene_sources(canvas);
	}
	const std::vector<std::string> saved = sv_store_list(current_collection(), key.c_str());
	obs_source_t *pick = nullptr;
	for (obs_source_t *s : scenes) {
		if (!pick && !saved.empty() && saved[0] == obs_source_get_uuid(s))
			pick = s;
	}
	if (!pick && !scenes.empty())
		pick = scenes[0];
	obs_source_t *out = pick ? obs_source_get_ref(pick) : nullptr;
	release_all(scenes);
	return out;
}

std::string display_name(obs_canvas_t *canvas)
{
	const char *n = obs_canvas_get_name(canvas);
	std::string s = n ? n : "";
	const size_t prefix = std::strlen(SV_CANVAS_PREFIX);
	return s.compare(0, prefix, SV_CANVAS_PREFIX) == 0 ? s.substr(prefix) : s;
}

bool canvas_name_taken(const std::string &name, const std::string &exceptId)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	for (const Cv &c : g_cvs) {
		if (c.uuid != exceptId && c.name == name)
			return true;
	}
	return false;
}

} // namespace

bool sv_canvas_is_ours(const char *obsName)
{
	if (!obsName)
		return false;
	return std::strcmp(obsName, SV_CANVAS_NAME) == 0 ||
	       std::strncmp(obsName, SV_CANVAS_PREFIX, std::strlen(SV_CANVAS_PREFIX)) == 0;
}

std::vector<obs_source_t *> sv_canvas_extra_layout_sources()
{
	std::vector<obs_canvas_t *> extras;
	obs_enum_canvases(
		[](void *param, obs_canvas_t *c) {
			const char *n = obs_canvas_get_name(c);
			if (n && std::strncmp(n, SV_CANVAS_PREFIX, std::strlen(SV_CANVAS_PREFIX)) == 0)
				static_cast<std::vector<obs_canvas_t *> *>(param)->push_back(obs_canvas_get_ref(c));
			return true;
		},
		&extras);
	std::vector<obs_source_t *> out;
	for (obs_canvas_t *c : extras) {
		if (obs_source_t *s = canvas_layout_source(c)) {
			out.push_back(s);
			obs_source_release(s);
		}
		obs_canvas_release(c);
	}
	return out;
}

obs_sceneitem_t *sv_canvas_add_layout_item(obs_scene_t *scene, const std::string &canvas)
{
	obs_canvas_t *ref = canvas_ref(canvas);
	obs_source_t *layout = ref ? canvas_layout_source(ref) : nullptr;
	obs_canvas_release(ref);
	if (!layout)
		layout = layout_source();
	if (!layout)
		return nullptr;
	obs_sceneitem_t *item = obs_scene_add(scene, layout);
	obs_source_release(layout);
	if (item)
		fit_full(item);
	return item;
}

bool sv_canvas_start(const SvPicks &picks)
{
	sv_canvas_set_picks(picks);
	if (sv_canvas_running())
		return true;

	obs_video_info ovi = {};
	if (!vertical_video_info(ovi))
		return false;

	obs_canvas_t *canvas = find_saved_canvas();
	if (canvas) {
		/* A canvas loaded from the collection has no picture settings yet. */
		obs_canvas_reset_video(canvas, &ovi);
	} else {
		canvas = obs_frontend_add_canvas(SV_CANVAS_NAME, &ovi, ACTIVATE | SCENE_REF);
	}
	if (!canvas) {
		obs_log(LOG_ERROR, "the vertical canvas could not start");
		return false;
	}

	std::vector<Cv> found;
	{
		Cv main;
		main.uuid = obs_canvas_get_uuid(canvas);
		main.name = SV_CANVAS_NAME;
		main.main = true;
		main.canvas = canvas;
		main.current = pick_scene(canvas, STORE_CURRENT, true);
		found.push_back(main);
	}
	for (obs_canvas_t *extra : find_saved_extras()) {
		obs_canvas_reset_video(extra, &ovi);
		Cv c;
		c.uuid = obs_canvas_get_uuid(extra);
		c.name = display_name(extra);
		c.canvas = extra;
		c.current = pick_scene(extra, std::string(STORE_CURRENT) + ":" + c.uuid, false);
		found.push_back(c);
	}

	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_cvs = std::move(found);
		for (Cv &c : g_cvs)
			apply_channel_locked(c);
	}

	signal_handler_connect(obs_get_signal_handler(), "video_reset", on_video_reset, nullptr);
	obs_log(LOG_INFO, "vertical canvas started (%ux%u at %u/%u), %d extra", ovi.base_width, ovi.base_height,
		ovi.fps_num, ovi.fps_den, (int)g_cvs.size() - 1);
	notify();
	return true;
}

void sv_canvas_stop()
{
	std::vector<Cv> all;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		all.swap(g_cvs);
		g_view.clear();
	}
	if (all.empty())
		return;
	signal_handler_disconnect(obs_get_signal_handler(), "video_reset", on_video_reset, nullptr);
	for (Cv &c : all) {
		obs_canvas_set_channel(c.canvas, 0, nullptr);
		obs_source_release(c.current);
		obs_source_release(c.cover);
		obs_canvas_release(c.canvas);
	}
	obs_log(LOG_INFO, "vertical canvas stopped");
	notify();
}

bool sv_canvas_running()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return !g_cvs.empty();
}

void sv_canvas_set_picks(const SvPicks &picks)
{
	struct Ctx {
		const SvPicks &picks;
	} ctx{picks};
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			/* Pieces on a horizontal scene draw from the same game and camera. */
			if (std::strcmp(obs_source_get_id(s), LAYOUT_ID) != 0 &&
			    std::strcmp(obs_source_get_id(s), "simply_vertical_piece") != 0)
				return true;
			const SvPicks &p = static_cast<Ctx *>(param)->picks;
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, "game_source", p.game.c_str());
			obs_data_set_string(st, "cam_source", p.cam.c_str());
			obs_source_update(s, st);
			obs_data_release(st);
			return true;
		},
		&ctx);
}

void sv_canvas_set_user(SvCanvasUser user, bool using_it, const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(canvas);
	if (!c)
		return;
	const unsigned before = c->users;
	c->users = using_it ? (c->users | user) : (c->users & ~(unsigned)user);
	if ((before != 0) != (c->users != 0))
		apply_channel_locked(*c);
}

void sv_canvas_set_output_canvases(const std::vector<std::string> &canvases)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	for (Cv &c : g_cvs) {
		bool wanted = false;
		for (const std::string &id : canvases) {
			if (id == c.uuid || (id.empty() && c.main))
				wanted = true;
		}
		const unsigned before = c.users;
		c.users = wanted ? (c.users | SV_CANVAS_OUTPUT) : (c.users & ~(unsigned)SV_CANVAS_OUTPUT);
		if ((before != 0) != (c.users != 0))
			apply_channel_locked(c);
	}
}

std::vector<SvVCanvas> sv_canvases()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	std::vector<SvVCanvas> out;
	for (const Cv &c : g_cvs)
		out.push_back({c.uuid, c.name, c.main});
	return out;
}

std::string sv_canvas_main_id()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_cvs.empty() ? "" : g_cvs[0].uuid;
}

bool sv_canvas_exists(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return find_locked(canvas) != nullptr;
}

std::string sv_canvas_free_canvas_name(const std::string &base)
{
	if (!canvas_name_taken(base, ""))
		return base;
	for (int n = 2; n < 100; n++) {
		const std::string name = base + " " + std::to_string(n);
		if (!canvas_name_taken(name, ""))
			return name;
	}
	return base;
}

std::string sv_canvas_add(const std::string &name, const std::string &copyFrom)
{
	if (name.empty() || name == SV_CANVAS_NAME || canvas_name_taken(name, ""))
		return "";
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (g_cvs.empty() || (int)g_cvs.size() >= SV_CANVAS_MAX)
			return "";
	}
	obs_video_info ovi = {};
	if (!vertical_video_info(ovi))
		return "";

	obs_canvas_t *from = canvas_ref(copyFrom);
	std::string fromCurrent = sv_canvas_current(copyFrom);
	const std::string obsName = std::string(SV_CANVAS_PREFIX) + name;
	obs_canvas_t *canvas = obs_frontend_add_canvas(obsName.c_str(), &ovi, ACTIVATE | SCENE_REF);
	if (!canvas) {
		obs_canvas_release(from);
		obs_log(LOG_ERROR, "a new vertical canvas could not be made");
		return "";
	}

	/* Its own copy of the layout source, so it can show another layout. */
	obs_source_t *fromLayout = canvas_layout_source(from);
	obs_source_t *ownLayout = nullptr;
	if (fromLayout) {
		std::string layoutName = std::string(T("LayoutSourceName")) + " (" + name + ")";
		for (int n = 2; n < 100; n++) {
			obs_source_t *clash = obs_get_source_by_name(layoutName.c_str());
			if (!clash)
				break;
			obs_source_release(clash);
			layoutName = std::string(T("LayoutSourceName")) + " (" + name + " " + std::to_string(n) + ")";
		}
		ownLayout = obs_source_duplicate(fromLayout, layoutName.c_str(), false);
		if (ownLayout) {
			/* A new canvas starts out following the main canvas's layout. */
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, "layout_pick", "");
			obs_source_update(ownLayout, st);
			obs_data_release(st);
		}
	}

	/* Same scenes, same names, same layers. The copy of the scene on show on the
	   first canvas is the one on show here. */
	obs_source_t *show = nullptr;
	std::vector<obs_source_t *> scenes = scene_sources(from);
	for (obs_source_t *s : scenes) {
		obs_scene_t *copy = obs_canvas_scene_create(canvas, obs_source_get_name(s));
		if (!copy)
			continue;
		copy_items(obs_scene_from_source(s), copy, ownLayout ? fromLayout : nullptr, ownLayout);
		if (!show && fromCurrent == obs_source_get_uuid(s))
			show = obs_source_get_ref(obs_scene_get_source(copy));
		obs_scene_release(copy);
	}
	release_all(scenes);
	obs_canvas_release(from);
	obs_source_release(fromLayout);
	obs_source_release(ownLayout);

	Cv c;
	c.uuid = obs_canvas_get_uuid(canvas);
	c.name = name;
	c.canvas = canvas;
	if (show) {
		c.current = show;
		const std::string key = std::string(STORE_CURRENT) + ":" + c.uuid;
		sv_store_set_list(current_collection(), key.c_str(), {obs_source_get_uuid(show)});
	} else {
		c.current = pick_scene(canvas, std::string(STORE_CURRENT) + ":" + c.uuid, false);
	}
	const std::string id = c.uuid;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_cvs.push_back(c);
	}
	obs_log(LOG_INFO, "vertical canvas added: %s", name.c_str());
	notify();
	return id;
}

bool sv_canvas_rename(const std::string &canvas, const std::string &name)
{
	if (name.empty() || name == SV_CANVAS_NAME || canvas_name_taken(name, canvas))
		return false;
	obs_canvas_t *ref = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(canvas);
		if (!c || c->main)
			return false;
		c->name = name;
		ref = obs_canvas_get_ref(c->canvas);
	}
	const std::string obsName = std::string(SV_CANVAS_PREFIX) + name;
	obs_canvas_set_name(ref, obsName.c_str());
	obs_canvas_release(ref);
	notify();
	return true;
}

bool sv_canvas_remove(const std::string &canvas)
{
	Cv gone;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(canvas);
		if (!c || c->main || (c->users & SV_CANVAS_OUTPUT))
			return false;
		gone = *c;
		g_cvs.erase(std::remove_if(g_cvs.begin(), g_cvs.end(), [&](const Cv &x) { return x.uuid == gone.uuid; }),
			    g_cvs.end());
		if (g_view == gone.uuid) {
			g_view.clear();
			if ((gone.users & SV_CANVAS_PREVIEW) && !g_cvs.empty()) {
				const bool was = g_cvs[0].users != 0;
				g_cvs[0].users |= SV_CANVAS_PREVIEW;
				if (!was)
					apply_channel_locked(g_cvs[0]);
			}
		}
	}
	/* Its own layout source goes with it, unless another canvas uses it too. */
	obs_source_t *ownLayout = canvas_layout_source(gone.canvas);
	if (ownLayout) {
		std::vector<obs_canvas_t *> others;
		{
			std::lock_guard<std::mutex> lock(g_mtx);
			for (Cv &o : g_cvs)
				others.push_back(obs_canvas_get_ref(o.canvas));
		}
		for (obs_canvas_t *o : others) {
			obs_source_t *theirs = canvas_layout_source(o);
			if (ownLayout && theirs == ownLayout) {
				obs_source_release(ownLayout);
				ownLayout = nullptr;
			}
			obs_source_release(theirs);
			obs_canvas_release(o);
		}
	}
	obs_canvas_set_channel(gone.canvas, 0, nullptr);
	obs_source_release(gone.current);
	obs_source_release(gone.cover);

	std::vector<obs_source_t *> scenes = scene_sources(gone.canvas);
	const std::string collection = current_collection();
	std::vector<std::string> keep;
	for (const std::string &link : sv_store_list(collection, SV_STORE_VSCENE_LINKS)) {
		const size_t bar = link.find('|');
		bool linked = false;
		for (obs_source_t *s : scenes) {
			if (bar != std::string::npos && link.substr(bar + 1) == obs_source_get_uuid(s))
				linked = true;
		}
		if (!linked)
			keep.push_back(link);
	}
	sv_store_set_list(collection, SV_STORE_VSCENE_LINKS, keep);
	sv_store_set_list(collection, store_key(gone).c_str(), {});
	sv_store_set_list(collection, sv_store_canvas_links(gone.uuid).c_str(), {});
	for (obs_source_t *s : scenes)
		obs_source_remove(s);
	release_all(scenes);
	if (ownLayout) {
		obs_source_remove(ownLayout);
		obs_source_release(ownLayout);
	}

	/* OBS takes it out of the collection when it hears the canvas is gone. */
	obs_canvas_remove(gone.canvas);
	obs_canvas_release(gone.canvas);
	obs_log(LOG_INFO, "vertical canvas removed: %s", gone.name.c_str());
	notify();
	return true;
}

std::vector<SvVScene> sv_canvas_scenes(const std::string &canvas)
{
	std::vector<SvVScene> out;
	obs_canvas_t *ref = canvas_ref(canvas);
	std::vector<obs_source_t *> all = scene_sources(ref);
	for (obs_source_t *s : all)
		out.push_back({obs_source_get_uuid(s), obs_source_get_name(s)});
	release_all(all);
	obs_canvas_release(ref);
	return out;
}

std::string sv_canvas_current(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(canvas);
	return c && c->current ? obs_source_get_uuid(c->current) : "";
}

void sv_canvas_switch(const std::string &uuid)
{
	const std::string id = canvas_of_scene(uuid);
	if (id.empty())
		return;
	obs_canvas_t *ref = canvas_ref(id);
	obs_source_t *scene = find_scene(ref, uuid);
	obs_canvas_release(ref);
	if (!scene)
		return;
	set_current(id, scene);
	obs_source_release(scene);
	notify();
}

std::string sv_canvas_free_name(const std::string &base, const std::string &canvas)
{
	obs_canvas_t *ref = canvas_ref(canvas);
	std::string name;
	for (int n = 1; n < 1000; n++) {
		name = base + " " + std::to_string(n);
		if (!ref || !name_taken(ref, name, ""))
			break;
	}
	obs_canvas_release(ref);
	return name;
}

std::string sv_canvas_add_scene(const std::string &name, const std::string &canvas)
{
	obs_canvas_t *ref = canvas_ref(canvas);
	if (!ref)
		return "";
	std::string uuid;
	if (!name.empty() && !name_taken(ref, name, "")) {
		obs_scene_t *scene = make_scene(ref, name);
		if (scene) {
			uuid = obs_source_get_uuid(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
	}
	obs_canvas_release(ref);
	if (!uuid.empty())
		sv_canvas_switch(uuid);
	return uuid;
}

std::string sv_canvas_duplicate_scene(const std::string &uuid, const std::string &name)
{
	const std::string id = canvas_of_scene(uuid);
	if (id.empty())
		return "";
	obs_canvas_t *ref = canvas_ref(id);
	obs_source_t *from = find_scene(ref, uuid);
	std::string made;
	if (ref && from && !name.empty() && !name_taken(ref, name, "")) {
		obs_scene_t *copy = obs_canvas_scene_create(ref, name.c_str());
		if (copy) {
			copy_items(obs_scene_from_source(from), copy);
			made = obs_source_get_uuid(obs_scene_get_source(copy));
			obs_scene_release(copy);
		}
	}
	obs_source_release(from);
	obs_canvas_release(ref);
	if (!made.empty())
		sv_canvas_switch(made);
	return made;
}

bool sv_canvas_rename_scene(const std::string &uuid, const std::string &name)
{
	const std::string id = canvas_of_scene(uuid);
	obs_canvas_t *ref = id.empty() ? nullptr : canvas_ref(id);
	obs_source_t *scene = find_scene(ref, uuid);
	bool ok = false;
	if (scene && !name.empty() && !name_taken(ref, name, uuid)) {
		obs_source_set_name(scene, name.c_str());
		ok = true;
	}
	obs_source_release(scene);
	obs_canvas_release(ref);
	if (ok)
		notify();
	return ok;
}

bool sv_canvas_remove_scene(const std::string &uuid)
{
	const std::string id = canvas_of_scene(uuid);
	if (id.empty())
		return false;
	obs_canvas_t *ref = canvas_ref(id);
	std::vector<obs_source_t *> all = scene_sources(ref);
	obs_source_t *gone = nullptr;
	obs_source_t *next = nullptr;
	for (obs_source_t *s : all) {
		if (uuid == obs_source_get_uuid(s))
			gone = s;
		else if (!next)
			next = s;
	}
	const bool ok = gone && next;
	if (ok) {
		if (sv_canvas_current(id) == uuid)
			set_current(id, next);
		obs_source_remove(gone);
		/* Main scenes linked to it go back to leaving the vertical as it is. */
		const std::string collection = current_collection();
		std::vector<std::string> keep;
		for (const std::string &link : sv_store_list(collection, SV_STORE_VSCENE_LINKS)) {
			const size_t bar = link.find('|');
			if (bar == std::string::npos || link.substr(bar + 1) != uuid)
				keep.push_back(link);
		}
		sv_store_set_list(collection, SV_STORE_VSCENE_LINKS, keep);
	}
	release_all(all);
	obs_canvas_release(ref);
	if (ok)
		notify();
	return ok;
}

obs_scene_t *sv_canvas_scene_ref(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(canvas);
	if (!c || !c->current)
		return nullptr;
	obs_scene_t *scene = obs_scene_from_source(c->current);
	if (scene)
		obs_scene_get_ref(scene);
	return scene;
}

void sv_canvas_set_listener(std::function<void()> listener)
{
	g_listener = std::move(listener);
}

void sv_canvas_set_watcher(std::function<void()> watcher)
{
	g_watcher = std::move(watcher);
}

video_t *sv_canvas_video(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(canvas);
	return c && c->canvas ? obs_canvas_get_video(c->canvas) : nullptr;
}

void sv_canvas_render(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	Cv *c = find_locked(canvas);
	if (c && c->canvas)
		obs_render_canvas_texture(c->canvas);
}

std::string sv_canvas_view()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_view;
}

void sv_canvas_set_view(const std::string &canvas)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *to = find_locked(canvas);
		Cv *from = find_locked(g_view);
		if (!to || to == from)
			return;
		/* The panel's picture moves with it. */
		if (from && (from->users & SV_CANVAS_PREVIEW)) {
			from->users &= ~(unsigned)SV_CANVAS_PREVIEW;
			if (!from->users)
				apply_channel_locked(*from);
			const bool was = to->users != 0;
			to->users |= SV_CANVAS_PREVIEW;
			if (!was)
				apply_channel_locked(*to);
		}
		g_view = to->main ? std::string() : to->uuid;
	}
	notify();
}

void sv_canvas_set_cover(const std::string &canvas, obs_source_t *cover)
{
	obs_source_t *old = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(canvas);
		if (!c || c->cover == cover)
			return;
		old = c->cover;
		c->cover = cover ? obs_source_get_ref(cover) : nullptr;
		apply_channel_locked(*c);
	}
	obs_source_release(old);
	notify();
}

std::string sv_canvas_layout_pick(const std::string &canvas)
{
	obs_canvas_t *ref = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(canvas);
		if (!c || c->main)
			return "";
		ref = obs_canvas_get_ref(c->canvas);
	}
	obs_source_t *layout = canvas_layout_source(ref);
	obs_canvas_release(ref);
	std::string pick;
	if (layout) {
		obs_data_t *st = obs_source_get_settings(layout);
		pick = obs_data_get_string(st, "layout_pick");
		obs_data_release(st);
		obs_source_release(layout);
	}
	return pick;
}

void sv_canvas_set_layout_pick(const std::string &canvas, const std::string &id)
{
	obs_canvas_t *ref = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Cv *c = find_locked(canvas);
		if (!c || c->main)
			return;
		ref = obs_canvas_get_ref(c->canvas);
	}
	obs_source_t *layout = canvas_layout_source(ref);
	obs_canvas_release(ref);
	if (!layout)
		return;
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "layout_pick", id.c_str());
	obs_source_update(layout, st);
	obs_data_release(st);
	obs_source_release(layout);
	notify();
}
