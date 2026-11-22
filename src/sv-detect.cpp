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

#include "sv-detect.h"
#include "sv-canvas.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace {

const char *const SV_ID = "simply_vertical_source";
/* A Simply Vertical piece on a horizontal scene: never a game, takes the same picks. */
const char *const SV_PIECE_ID = "simply_vertical_piece";

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool has(const std::string &text, const char *needle)
{
	return text.find(needle) != std::string::npos;
}

/* Words a capture card uses for its name, so it is not mistaken for a camera. */
bool looks_like_capture_card(const std::string &text)
{
	static const char *const words[] = {"elgato hd",    "elgato 4k", "hd60",        "4k60",
					    "4k pro",       "4k s",      "avermedia",   "live gamer",
					    "magewell",     "capture card", "game capture", "hdmi",
					    "usb3.0 capture", "usb capture", "video capture hd"};
	for (const char *w : words)
		if (has(text, w))
			return true;
	return false;
}

/* Which sources are on screen right now, walking inside scenes and groups. */
struct Walk {
	std::map<obs_source_t *, bool> seen;
	int depth = 0;
};

bool walk_item(obs_scene_t *, obs_sceneitem_t *item, void *param);

void walk_scene(obs_scene_t *scene, Walk *w)
{
	if (!scene || w->depth >= 8)
		return;
	w->depth++;
	obs_scene_enum_items(scene, walk_item, w);
	w->depth--;
}

bool walk_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *w = static_cast<Walk *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	const bool visible = obs_sceneitem_visible(item);
	bool &v = w->seen[src];
	v = v || visible;
	if (obs_scene_t *inner = obs_scene_from_source(src))
		walk_scene(inner, w);
	else if (obs_scene_t *group = obs_group_from_source(src))
		walk_scene(group, w);
	return true;
}

struct EnumCtx {
	std::vector<SvFound> *out;
	const Walk *walk;
};

bool enum_source(void *param, obs_source_t *src)
{
	auto *c = static_cast<EnumCtx *>(param);
	const char *id = obs_source_get_id(src);
	if (!id || std::strcmp(id, SV_ID) == 0 || std::strcmp(id, SV_PIECE_ID) == 0)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
		return true;

	SvFound f;
	f.name = obs_source_get_name(src) ? obs_source_get_name(src) : "";
	f.uuid = obs_source_get_uuid(src) ? obs_source_get_uuid(src) : "";
	if (f.uuid.empty() || f.name.empty())
		return true;

	const std::string name = lower(f.name);
	const std::string type = id;
	std::string device;
	if (type == "dshow_input") {
		obs_data_t *st = obs_source_get_settings(src);
		device = lower(obs_data_get_string(st, "video_device_id"));
		obs_data_release(st);
	}

	/* OBS's own output fed back in would show the stream inside itself. */
	if (has(device, "obs virtual camera") || has(name, "virtual cam"))
		return true;

	const bool card = looks_like_capture_card(name) || looks_like_capture_card(device);
	const bool camWord = has(name, "cam");

	if (type == "game_capture")
		f.gameScore = 100;
	else if (type == "dshow_input" && card)
		f.gameScore = 90;
	else if (type == "ndi_source" || type == "spout_capture")
		f.gameScore = camWord ? 0 : 70;
	else if (type == "window_capture")
		f.gameScore = 40;
	else if (type == "monitor_capture")
		f.gameScore = 30;

	if (type == "dshow_input" && !card)
		f.camScore = 60 + (camWord ? 20 : 0);
	else if ((type == "ndi_source" || type == "spout_capture") && camWord)
		f.camScore = 50;

	/* Something showing in the open scene beats something parked elsewhere. */
	const auto it = c->walk->seen.find(src);
	if (it != c->walk->seen.end()) {
		const int bonus = it->second ? 20 : 8;
		if (f.gameScore)
			f.gameScore += bonus;
		if (f.camScore)
			f.camScore += bonus;
	}
	if (f.gameScore && has(name, "game"))
		f.gameScore += 5;

	c->out->push_back(std::move(f));
	return true;
}

struct ModuleCtx {
	std::string name;
};

void enum_module(void *param, obs_module_t *module)
{
	auto *c = static_cast<ModuleCtx *>(param);
	if (!c->name.empty())
		return;
	const char *fileName = obs_get_module_file_name(module);
	if (!fileName)
		return;
	const std::string file = lower(fileName);
	if (has(file, "vertical-canvas") || has(file, "aitum-vertical"))
		c->name = "Aitum Vertical";
	else if (has(file, "streamelements") || has(file, "se-live") || has(file, "selive"))
		c->name = "SE.Live";
}

/* Aitum always names its vertical canvas this. */
constexpr const char *AITUM_CANVAS = "Aitum Vertical";

struct CanvasCtx {
	std::string app;
	std::string uuid;
};

/* The vertical canvas that belongs to the app found installed. OBS keeps a canvas in
   the scene collection even after the app that made it is removed, so a canvas only
   counts when it is that app's: Aitum's by its fixed name, SE.Live's (named on the
   StreamElements website) as any other portrait canvas that is neither ours nor
   Aitum's. Never the main canvas and never our own. */
bool enum_canvas(void *param, obs_canvas_t *canvas)
{
	auto *c = static_cast<CanvasCtx *>(param);
	if (obs_canvas_get_flags(canvas) & (MAIN | EPHEMERAL))
		return true;
	const char *n = obs_canvas_get_name(canvas);
	const std::string name = n ? n : "";
	if (sv_canvas_is_ours(n))
		return true;
	const bool aitumCanvas = name == AITUM_CANVAS;
	if (aitumCanvas != (c->app == "Aitum Vertical"))
		return true;
	obs_video_info ovi = {};
	if (!obs_canvas_get_video_info(canvas, &ovi))
		return true;
	if (ovi.base_height <= ovi.base_width)
		return true;
	const char *uuid = obs_canvas_get_uuid(canvas);
	if (uuid)
		c->uuid = uuid;
	return c->uuid.empty();
}

/* SE.Live's canvas that other plugins can see has no scenes of its own: the scene the
   member builds and streams lives on a second, hidden canvas named after SE.Live's
   composition. So when a canvas has no scene, take the scene on show on any other
   portrait canvas that is not the main one, ours or Aitum's. */
struct HiddenScene {
	obs_source_t *scene = nullptr;
	obs_canvas_t *canvas = nullptr;
};

bool find_hidden_scene(void *param, obs_source_t *src)
{
	if (!obs_scene_from_source(src) || !obs_source_showing(src))
		return true;
	obs_canvas_t *canvas = obs_source_get_canvas(src);
	if (!canvas)
		return true;
	const char *n = obs_canvas_get_name(canvas);
	const std::string name = n ? n : "";
	obs_video_info ovi = {};
	const bool fits = !(obs_canvas_get_flags(canvas) & MAIN) && !sv_canvas_is_ours(n) &&
			  name != AITUM_CANVAS && obs_canvas_get_video_info(canvas, &ovi) &&
			  ovi.base_height > ovi.base_width;
	if (!fits) {
		obs_canvas_release(canvas);
		return true;
	}
	auto *h = static_cast<HiddenScene *>(param);
	h->scene = obs_source_get_ref(src);
	h->canvas = canvas;
	return false;
}

bool scene_has_ours(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && std::strcmp(obs_source_get_id(src), SV_ID) == 0) {
		*static_cast<bool *>(param) = true;
		return false;
	}
	return true;
}

bool apply_one(void *param, obs_source_t *src)
{
	if (std::strcmp(obs_source_get_id(src), SV_ID) != 0 && std::strcmp(obs_source_get_id(src), SV_PIECE_ID) != 0)
		return true;
	const auto *p = static_cast<const SvPicks *>(param);
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "game_source", p->game.c_str());
	obs_data_set_string(st, "cam_source", p->cam.c_str());
	obs_source_update(src, st);
	obs_data_release(st);
	return true;
}

} // namespace

std::vector<SvFound> sv_detect_sources()
{
	Walk walk;
	obs_source_t *current = obs_frontend_get_current_scene();
	if (current) {
		walk_scene(obs_scene_from_source(current), &walk);
		obs_source_release(current);
	}
	std::vector<SvFound> out;
	EnumCtx ctx{&out, &walk};
	obs_enum_sources(enum_source, &ctx);
	return out;
}

SvPicks sv_detect_best(const std::vector<SvFound> &found)
{
	SvPicks p;
	int bestGame = 0, bestCam = 0;
	for (const SvFound &f : found) {
		if (f.gameScore > bestGame) {
			bestGame = f.gameScore;
			p.game = f.uuid;
		}
		if (f.camScore > bestCam) {
			bestCam = f.camScore;
			p.cam = f.uuid;
		}
	}
	return p;
}

SvOtherApp sv_detect_other_app()
{
	SvOtherApp app;
	ModuleCtx mods;
	obs_enum_modules(enum_module, &mods);
	app.name = mods.name;
	if (app.name.empty())
		return app;
	CanvasCtx ctx;
	ctx.app = app.name;
	obs_enum_canvases(enum_canvas, &ctx);
	app.canvas = ctx.uuid;
	return app;
}

bool sv_detect_add_to_canvas(const std::string &canvasUuid, const SvPicks &picks)
{
	if (canvasUuid.empty())
		return false;
	obs_canvas_t *canvas = obs_get_canvas_by_uuid(canvasUuid.c_str());
	if (!canvas)
		return false;

	bool ok = false;
	obs_video_info ovi = {};
	/* The scene on show. Aitum Vertical, like OBS itself, puts a transition in front of it,
	   so look through that. With nothing on show yet, the canvas's first scene. */
	obs_source_t *channel = obs_canvas_get_channel(canvas, 0);
	obs_source_t *shown = nullptr;
	if (channel && obs_source_get_type(channel) == OBS_SOURCE_TYPE_TRANSITION)
		shown = obs_transition_get_active_source(channel);
	else if (channel)
		shown = obs_source_get_ref(channel);
	if (!shown || !obs_scene_from_source(shown)) {
		obs_source_release(shown);
		shown = nullptr;
		obs_canvas_enum_scenes(
			canvas,
			[](void *param, obs_source_t *src) {
				if (!obs_scene_from_source(src))
					return true;
				*static_cast<obs_source_t **>(param) = obs_source_get_ref(src);
				return false;
			},
			&shown);
	}
	/* No scene on the canvas itself: SE.Live's real scene is on its hidden canvas. */
	if (!shown) {
		HiddenScene hidden;
		obs_enum_all_sources(find_hidden_scene, &hidden);
		if (hidden.scene) {
			shown = hidden.scene;
			obs_canvas_release(canvas);
			canvas = hidden.canvas;
		}
	}
	obs_scene_t *scene = shown ? obs_scene_from_source(shown) : nullptr;
	if (scene && obs_canvas_get_video_info(canvas, &ovi)) {
		bool already = false;
		obs_scene_enum_items(scene, scene_has_ours, &already);
		if (already) {
			ok = true;
		} else {
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, "game_source", picks.game.c_str());
			obs_data_set_string(st, "cam_source", picks.cam.c_str());

			/* Reuse one with our name, or pick a name OBS is not already using. */
			obs_source_t *source = nullptr;
			std::string name = obs_module_text("SimplyVertical");
			for (int i = 1; i < 50 && !source; i++) {
				const std::string attempt = i == 1 ? name : name + " " + std::to_string(i);
				obs_source_t *existing = obs_get_source_by_name(attempt.c_str());
				if (!existing) {
					source = obs_source_create(SV_ID, attempt.c_str(), st, nullptr);
				} else if (std::strcmp(obs_source_get_id(existing), SV_ID) == 0) {
					obs_source_update(existing, st);
					source = existing;
					existing = nullptr;
				}
				obs_source_release(existing);
			}
			obs_data_release(st);

			if (source) {
				obs_sceneitem_t *item = obs_scene_add(scene, source);
				if (item) {
					vec2 pos = {0.0f, 0.0f};
					vec2 bounds = {(float)ovi.base_width, (float)ovi.base_height};
					obs_sceneitem_set_pos(item, &pos);
					obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
					obs_sceneitem_set_bounds(item, &bounds);
					ok = true;
				}
				obs_source_release(source);
			}
		}
	}
	obs_source_release(shown);
	obs_source_release(channel);
	obs_canvas_release(canvas);
	return ok;
}

void sv_detect_remove_doubles()
{
	std::vector<obs_source_t *> scenes;
	obs_enum_all_sources(
		[](void *param, obs_source_t *src) {
			if (!obs_scene_from_source(src))
				return true;
			obs_canvas_t *canvas = obs_source_get_canvas(src);
			if (!canvas)
				return true;
			const char *n = obs_canvas_get_name(canvas);
			const bool other = !(obs_canvas_get_flags(canvas) & MAIN) && !sv_canvas_is_ours(n);
			obs_canvas_release(canvas);
			if (other)
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(src));
			return true;
		},
		&scenes);
	for (obs_source_t *src : scenes) {
		std::vector<obs_sceneitem_t *> ours;
		obs_scene_enum_items(
			obs_scene_from_source(src),
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *s = obs_sceneitem_get_source(item);
				if (s && std::strcmp(obs_source_get_id(s), SV_ID) == 0) {
					obs_sceneitem_addref(item);
					static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
				}
				return true;
			},
			&ours);
		/* Keep the newest: that is the app's saved copy, with the member's own placing. */
		std::sort(ours.begin(), ours.end(), [](obs_sceneitem_t *a, obs_sceneitem_t *b) {
			return obs_sceneitem_get_id(a) < obs_sceneitem_get_id(b);
		});
		for (size_t i = 0; i + 1 < ours.size(); i++) {
			obs_sceneitem_remove(ours[i]);
			obs_log(LOG_INFO, "removed a second copy of the layout from %s", obs_source_get_name(src));
		}
		for (obs_sceneitem_t *item : ours)
			obs_sceneitem_release(item);
		obs_source_release(src);
	}
}

void sv_detect_apply_picks(const SvPicks &picks)
{
	SvPicks copy = picks;
	obs_enum_sources(apply_one, &copy);
}

bool sv_detect_source_picks(const SvPicks &stored, SvPicks &out)
{
	struct Look {
		const SvPicks *stored;
		SvPicks *out;
		bool found;
	} look{&stored, &out, false};
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (std::strcmp(obs_source_get_id(src), SV_ID) != 0)
				return true;
			auto *l = static_cast<Look *>(param);
			obs_data_t *st = obs_source_get_settings(src);
			const std::string game = obs_data_get_string(st, "game_source");
			const std::string cam = obs_data_get_string(st, "cam_source");
			obs_data_release(st);
			if (game == l->stored->game && cam == l->stored->cam)
				return true;
			l->out->game = game;
			l->out->cam = cam;
			l->found = true;
			return false;
		},
		&look);
	return look.found;
}
