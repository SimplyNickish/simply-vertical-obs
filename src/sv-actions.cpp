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

#include "sv-actions.h"

#include "sv-canvas.h"
#include "sv-follow.h"
#include "sv-link.h"
#include "sv-output.h"
#include "sv-remote.h"
#include "sv-source.h"
#include "sv-store.h"
#include "sv-zoom.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QMetaObject>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

constexpr int LAYOUT_HOTKEYS = 12; /* as many as the website lets a member save */

struct Hot {
	std::string name;
	std::string text;
	std::function<void()> run;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

std::vector<Hot> g_hot;
bool g_live = false;

/* Hotkeys arrive on OBS's hotkey thread. The work belongs on the main one. */
void fire(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	const std::function<void()> run = static_cast<Hot *>(data)->run;
	QMetaObject::invokeMethod(
		qApp,
		[run] {
			if (g_live)
				run();
		},
		Qt::QueuedConnection);
}

std::string store_key(const Hot &h)
{
	return "hotkey_" + h.name;
}

void add_hot(const char *name, const std::string &text, std::function<void()> run)
{
	Hot h;
	h.name = name;
	h.text = text;
	h.run = std::move(run);
	g_hot.push_back(std::move(h));
}

void start(const std::string &id)
{
	std::string problem;
	if (!sv_output_start(id, problem) && !problem.empty())
		sv_output_set_notice(problem, true);
}

void pick_slot(int slot)
{
	const std::vector<SvLayoutChoice> list = sv_link_presets();
	if (slot >= 0 && slot < (int)list.size())
		sv_action_pick_layout(list[(size_t)slot].id);
}

std::string current_collection()
{
	char *c = obs_frontend_get_current_scene_collection();
	std::string s = c ? c : "";
	bfree(c);
	return s;
}

void register_hot(Hot &h)
{
	h.id = obs_hotkey_register_frontend(h.name.c_str(), h.text.c_str(), fire, &h);
	obs_data_array_t *saved = sv_store_array(store_key(h).c_str());
	if (saved) {
		obs_hotkey_load(h.id, saved);
		obs_data_array_release(saved);
	}
}

/* Keeps the member's keys for next time and takes the hotkey out of OBS. */
void unregister_hot(Hot &h)
{
	if (h.id == OBS_INVALID_HOTKEY_ID)
		return;
	obs_data_array_t *keys = obs_hotkey_save(h.id);
	if (keys) {
		sv_store_set_array(store_key(h).c_str(), keys);
		obs_data_array_release(keys);
	}
	obs_hotkey_unregister(h.id);
	h.id = OBS_INVALID_HOTKEY_ID;
}

/* ---------- each extra canvas's own keys ---------- */

/* What every extra canvas gets, by the end of its hotkey name and its text. */
struct CanvasKeyKind {
	const char *suffix;
	const char *text;
};
constexpr CanvasKeyKind CANVAS_KEYS[] = {
	{"live", "HotCanvasLive"},          {"vscene_next", "HotCanvasVSceneNext"},
	{"vscene_prev", "HotCanvasVScenePrev"}, {"layout_next", "HotCanvasLayoutNext"},
	{"layout_prev", "HotCanvasLayoutPrev"}, {"layout_main", "HotCanvasLayoutMain"},
};

/* The keys of one extra canvas. Each Hot keeps its own address, since OBS holds it. */
struct CanvasHot {
	std::string canvas;
	std::string name;
	std::vector<std::unique_ptr<Hot>> keys;
};

std::vector<CanvasHot> g_canvas_hot;
bool g_sync_queued = false;

std::string canvas_text(const char *text, const std::string &name)
{
	char out[512];
	std::snprintf(out, sizeof(out), T(text), name.c_str());
	return out;
}

std::function<void()> canvas_run(const std::string &suffix, const std::string &canvas)
{
	if (suffix == "live")
		return [canvas] { sv_action_toggle_canvas(canvas); };
	if (suffix == "vscene_next")
		return [canvas] { sv_action_step_vscene(1, canvas); };
	if (suffix == "vscene_prev")
		return [canvas] { sv_action_step_vscene(-1, canvas); };
	if (suffix == "layout_next")
		return [canvas] { sv_action_step_layout(1, canvas); };
	if (suffix == "layout_prev")
		return [canvas] { sv_action_step_layout(-1, canvas); };
	return [canvas] { sv_action_pick_canvas_layout(canvas, ""); };
}

/* Makes the keys of canvases that have come, renames those of canvases that were
   renamed and takes out those of canvases that have gone. A canvas's keys are saved
   by its id, so they come back with it. Main thread. */
void sync_canvas_keys()
{
	g_sync_queued = false;
	if (!g_live)
		return;
	std::vector<SvVCanvas> now;
	if (sv_canvas_running())
		for (const SvVCanvas &c : sv_canvases())
			if (!c.main)
				now.push_back(c);

	for (auto it = g_canvas_hot.begin(); it != g_canvas_hot.end();) {
		const auto still = std::find_if(now.begin(), now.end(), [&](const SvVCanvas &c) { return c.uuid == it->canvas; });
		if (still != now.end()) {
			if (still->name != it->name) {
				it->name = still->name;
				for (size_t i = 0; i < it->keys.size(); i++) {
					it->keys[i]->text = canvas_text(CANVAS_KEYS[i].text, it->name);
					obs_hotkey_set_description(it->keys[i]->id, it->keys[i]->text.c_str());
				}
			}
			++it;
			continue;
		}
		for (auto &h : it->keys)
			unregister_hot(*h);
		it = g_canvas_hot.erase(it);
	}

	for (const SvVCanvas &c : now) {
		const bool have = std::any_of(g_canvas_hot.begin(), g_canvas_hot.end(),
					      [&](const CanvasHot &k) { return k.canvas == c.uuid; });
		if (have)
			continue;
		CanvasHot set;
		set.canvas = c.uuid;
		set.name = c.name;
		for (const CanvasKeyKind &kind : CANVAS_KEYS) {
			auto h = std::make_unique<Hot>();
			h->name = "simply_vertical.canvas." + c.uuid + "." + kind.suffix;
			h->text = canvas_text(kind.text, c.name);
			h->run = canvas_run(kind.suffix, c.uuid);
			register_hot(*h);
			set.keys.push_back(std::move(h));
		}
		g_canvas_hot.push_back(std::move(set));
	}
}

void queue_canvas_sync()
{
	QMetaObject::invokeMethod(
		qApp,
		[] {
			if (g_sync_queued)
				return;
			g_sync_queued = true;
			QMetaObject::invokeMethod(qApp, [] { sync_canvas_keys(); }, Qt::QueuedConnection);
		},
		Qt::QueuedConnection);
}

/* The platforms that show this canvas. A platform whose canvas has gone shows the
   main one. */
std::vector<std::string> dests_on(const std::string &canvas)
{
	const std::string mainId = sv_canvas_main_id();
	const bool isMain = canvas.empty() || canvas == mainId;
	std::vector<std::string> ids;
	for (const SvDest &d : sv_output_dests()) {
		const bool onMain = d.canvas.empty() || d.canvas == mainId || !sv_canvas_exists(d.canvas);
		if (isMain ? onMain : d.canvas == canvas)
			ids.push_back(d.id);
	}
	return ids;
}

} // namespace

void sv_actions_load()
{
	g_live = true;
	g_hot.clear();
	g_hot.reserve(LAYOUT_HOTKEYS + 8);

	add_hot("simply_vertical.toggle_all", T("HotToggleAll"), [] { sv_action_toggle_all(); });
	add_hot("simply_vertical.go_live_all", T("HotGoLiveAll"), [] { sv_action_go_live_all(); });
	add_hot("simply_vertical.stop_all", T("HotStopAll"), [] { sv_action_stop_all(); });
	add_hot("simply_vertical.record", T("HotRecord"), [] { sv_action_toggle(SV_RECORD_ID); });
	add_hot("simply_vertical.backtrack_save", T("HotBacktrackSave"), [] { sv_action_save_backtrack(); });
	add_hot("simply_vertical.layout_next", T("HotLayoutNext"), [] { sv_action_step_layout(1); });
	add_hot("simply_vertical.layout_prev", T("HotLayoutPrev"), [] { sv_action_step_layout(-1); });
	add_hot("simply_vertical.layout_site", T("HotLayoutSite"), [] { sv_action_pick_layout(""); });
	add_hot("simply_vertical.vscene_next", T("HotVSceneNext"), [] { sv_action_step_vscene(1); });
	add_hot("simply_vertical.vscene_prev", T("HotVScenePrev"), [] { sv_action_step_vscene(-1); });
	add_hot("simply_vertical.hold", T("HotHold"), [] { sv_action_toggle_hold(); });
	for (int i = 0; i < LAYOUT_HOTKEYS; i++) {
		char name[64];
		char text[256];
		std::snprintf(name, sizeof(name), "simply_vertical.layout_%d", i + 1);
		std::snprintf(text, sizeof(text), T("HotLayoutN"), i + 1);
		add_hot(name, text, [i] { pick_slot(i); });
	}

	for (Hot &h : g_hot)
		register_hot(h);
	/* Each extra canvas's own keys, as the canvases come and go. */
	sv_canvas_set_watcher([] { queue_canvas_sync(); });
	queue_canvas_sync();
	/* A zoom key for each piece with zoom set up, by its name. */
	sv_zoom_load();
	/* Stream Deck, Streamer.bot, Touch Portal and the like, from this PC only. */
	sv_remote_start();
	/* Follow my game, if the member turned it on. */
	sv_follow_start();
}

void sv_actions_shutdown()
{
	g_live = false;
	sv_follow_stop();
	sv_remote_stop();
	sv_canvas_set_watcher(nullptr);
	for (Hot &h : g_hot)
		unregister_hot(h);
	for (CanvasHot &set : g_canvas_hot)
		for (auto &h : set.keys)
			unregister_hot(*h);
	g_canvas_hot.clear();
	sv_zoom_shutdown();
}

void sv_action_toggle(const std::string &id)
{
	sv_output_set_notice("", false);
	if (sv_output_info(id).state == SvRunState::Off)
		start(id);
	else
		sv_output_stop(id);
}

void sv_action_go_live_all()
{
	sv_output_set_notice("", false);
	for (const SvDest &d : sv_output_dests()) {
		if (sv_output_info(d.id).state == SvRunState::Off)
			start(d.id);
	}
}

void sv_action_stop_all()
{
	sv_output_set_notice("", false);
	for (const SvDest &d : sv_output_dests()) {
		if (sv_output_info(d.id).state != SvRunState::Off)
			sv_output_stop(d.id);
	}
}

void sv_action_toggle_all()
{
	const std::vector<SvDest> dests = sv_output_dests();
	const bool anyLive = std::any_of(dests.begin(), dests.end(), [](const SvDest &d) {
		return sv_output_info(d.id).state != SvRunState::Off;
	});
	if (anyLive)
		sv_action_stop_all();
	else
		sv_action_go_live_all();
}

void sv_action_save_backtrack()
{
	if (sv_output_save_backtrack())
		sv_output_set_notice(T("BacktrackSaving"), false);
	else
		sv_output_set_notice(T("BacktrackIsOff"), true);
}

void sv_action_pick_layout(const std::string &id)
{
	if (id != sv_link_active_preset())
		sv_source_arm_switch();
	sv_link_pick_preset(id);
}

void sv_action_step_layout(int direction, const std::string &canvas)
{
	const bool isMain = canvas.empty() || canvas == sv_canvas_main_id();
	if (!isMain && !sv_canvas_exists(canvas))
		return;
	/* The first step is the website on the main canvas, and the main canvas's layout on the others. */
	std::vector<std::string> ids{""};
	for (const SvLayoutChoice &c : sv_link_presets())
		ids.push_back(c.id);
	const std::string active = isMain ? sv_link_active_preset() : sv_canvas_layout_pick(canvas);
	const auto it = std::find(ids.begin(), ids.end(), active);
	const int at = it == ids.end() ? 0 : (int)(it - ids.begin());
	const int count = (int)ids.size();
	const std::string &next = ids[(size_t)((at + direction + count) % count)];
	if (isMain)
		sv_action_pick_layout(next);
	else
		sv_action_pick_canvas_layout(canvas, next);
}

void sv_action_pick_canvas_layout(const std::string &canvas, const std::string &id)
{
	if (canvas.empty() || canvas == sv_canvas_main_id()) {
		sv_action_pick_layout(id);
		return;
	}
	if (!sv_canvas_exists(canvas))
		return;
	if (id != sv_canvas_layout_pick(canvas))
		sv_source_arm_switch();
	sv_canvas_set_layout_pick(canvas, id);
}

bool sv_action_canvas_live(const std::string &canvas)
{
	for (const std::string &id : dests_on(canvas))
		if (sv_output_info(id).state != SvRunState::Off)
			return true;
	return false;
}

void sv_action_set_canvas_live(const std::string &canvas, bool on)
{
	sv_output_set_notice("", false);
	for (const std::string &id : dests_on(canvas)) {
		const bool running = sv_output_info(id).state != SvRunState::Off;
		if (on && !running)
			start(id);
		else if (!on && running)
			sv_output_stop(id);
	}
}

void sv_action_toggle_canvas(const std::string &canvas)
{
	sv_action_set_canvas_live(canvas, !sv_action_canvas_live(canvas));
}

/* An OBS scene switched to while holding, whose routes wait for Hold changes to end. */
static bool g_routes_waiting = false;

void sv_action_set_hold(bool on)
{
	if (on == sv_link_holding())
		return;
	const bool catchUp = !on && g_routes_waiting;
	g_routes_waiting = false;
	sv_link_set_hold(on);
	sv_output_set_notice(on ? T("HoldOn") : T("HoldOff"), false);
	/* Letting go catches up with an OBS scene switched to during the hold. Without one,
	   layouts picked by hand while holding stay as they are. */
	if (catchUp)
		sv_action_follow_scene();
}

void sv_action_toggle_hold()
{
	sv_action_set_hold(!sv_link_holding());
}

void sv_action_follow_scene()
{
	/* Hold changes keeps the stream as it is, scene routes included. */
	if (sv_link_holding()) {
		g_routes_waiting = true;
		return;
	}
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;
	const std::string prefix = std::string(obs_source_get_uuid(scene)) + "|";
	/* A route to a layout deleted on the website does nothing, on every canvas. */
	const std::vector<SvLayoutChoice> presets = sv_link_presets();
	const auto known = [&presets](const std::string &id) {
		return std::any_of(presets.begin(), presets.end(), [&id](const SvLayoutChoice &c) { return c.id == id; });
	};
	obs_source_release(scene);
	const std::string collection = current_collection();
	/* Our own vertical scenes only switch while Simply Vertical runs the vertical. */
	const std::vector<std::string> vlinks =
		sv_store_route() == SV_ROUTE_OWN ? sv_store_list(collection, SV_STORE_VSCENE_LINKS) : std::vector<std::string>();
	/* One link per canvas at most, and each scene id finds its own canvas. */
	for (const std::string &link : vlinks) {
		if (link.rfind(prefix, 0) == 0)
			sv_canvas_switch(link.substr(prefix.size()));
	}
	/* Extra canvases follow their own layout links. */
	if (sv_store_route() == SV_ROUTE_OWN && sv_canvas_running()) {
		for (const SvVCanvas &c : sv_canvases()) {
			if (c.main)
				continue;
			for (const std::string &link : sv_store_list(collection, sv_store_canvas_links(c.uuid).c_str())) {
				if (link.rfind(prefix, 0) != 0)
					continue;
				const std::string value = link.substr(prefix.size());
				if (value == SV_SCENE_LINK_MAIN || known(value))
					sv_action_pick_canvas_layout(c.uuid, value == SV_SCENE_LINK_MAIN ? "" : value);
				break;
			}
		}
	}
	for (const std::string &link : sv_store_list(collection, SV_STORE_SCENE_LINKS)) {
		if (link.rfind(prefix, 0) != 0)
			continue;
		const std::string value = link.substr(prefix.size());
		if (value == SV_SCENE_LINK_SITE || known(value))
			sv_action_pick_layout(value == SV_SCENE_LINK_SITE ? "" : value);
		return;
	}
}

void sv_action_step_vscene(int direction, const std::string &canvas)
{
	if (!canvas.empty() && !sv_canvas_exists(canvas))
		return;
	const std::vector<SvVScene> scenes = sv_canvas_scenes(canvas);
	if (scenes.empty())
		return;
	const std::string now = sv_canvas_current(canvas);
	int at = 0;
	for (size_t i = 0; i < scenes.size(); i++)
		if (scenes[i].uuid == now)
			at = (int)i;
	const int count = (int)scenes.size();
	sv_canvas_switch(scenes[(size_t)((at + direction + count) % count)].uuid);
}
