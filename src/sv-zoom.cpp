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

#include "sv-zoom.h"

#include "sv-canvas.h"
#include "sv-store.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#define T(s) obs_module_text(s)

namespace {

/* How one piece's zoom moves, from its layout. */
struct Spec {
	std::string name;
	SvZoomMotion motion = SvZoomMotion::Smooth;
	float ms = 350;
	bool hold = false;
	float back = 0;
	std::string bind; /* SvPiece::zoomBind */
};

/* Where a piece's zoom is heading: from one amount to another, starting at a
   time. Starting each move from wherever it is now keeps it smooth when a key
   is pressed again halfway through. */
struct State {
	Spec spec;
	bool known = false; /* in some source's layout */
	float from = 0, to = 0;
	uint64_t at = 0;
};

std::mutex g_mtx;
std::map<std::string, State> g_state;
/* Each vertical source's zooms, by key, from the layout it shows. */
std::map<obs_source_t *, std::vector<std::pair<std::string, Spec>>> g_owners;

/* Works out which zooms exist from every source's layout. The source whose
   layout just arrived goes first, so its set up wins for a name two layouts
   share. Returns true when a name is new or its key changed. g_mtx held. */
bool recount(obs_source_t *first, uint64_t now)
{
	bool fresh = false;
	for (auto &kv : g_state)
		kv.second.known = false;
	auto take = [&](const std::vector<std::pair<std::string, Spec>> &zooms) {
		for (const auto &z : zooms) {
			State &st = g_state[z.first];
			if (st.known)
				continue; /* two pieces with one name share the first one's set up */
			fresh = fresh || st.spec.name.empty() || st.spec.bind != z.second.bind;
			st.known = true;
			st.spec = z.second;
		}
	};
	const auto it = g_owners.find(first);
	if (it != g_owners.end())
		take(it->second);
	for (const auto &o : g_owners)
		if (o.first != first)
			take(o.second);
	/* A zoom switched off, or in no layout now: back home at once. */
	for (auto &kv : g_state)
		if (!kv.second.known) {
			kv.second.from = kv.second.to = 0;
			kv.second.at = now;
		}
	return fresh;
}

/* The same moves as the website's lib/zoomEase.ts, so Try zoom matches the stream. */
constexpr float PUNCH = 1.6f;

float in_out(float p)
{
	return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.0f) / 2;
}

float out_back(float p)
{
	const float q = p - 1;
	return 1 + (PUNCH + 1) * q * q * q + PUNCH * q * q;
}

float ms_of(const Spec &s)
{
	return s.motion == SvZoomMotion::Snap ? 0.0f : std::max(1.0f, s.ms);
}

float amount_at(const State &st, uint64_t now)
{
	const float ms = ms_of(st.spec);
	if (ms <= 0 || now < st.at)
		return now < st.at ? st.from : st.to;
	const float p = std::min(1.0f, (float)((now - st.at) / 1e6) / ms);
	const bool goingIn = st.to > st.from;
	const float e = st.spec.motion == SvZoomMotion::Punch && goingIn ? out_back(p) : in_out(p);
	return st.from + (st.to - st.from) * e;
}

void head_to(State &st, float to, uint64_t now)
{
	st.from = amount_at(st, now);
	st.to = to;
	st.at = now;
}

/* ---------- OBS hotkeys, main thread only ---------- */

struct Hot {
	std::string key;
	std::string obsName;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

bool g_loaded = false;
std::vector<std::unique_ptr<Hot>> g_hot;

/* A name OBS can keep: small letters, numbers and underscores. */
std::string obs_name_for(const std::string &key)
{
	std::string out = "simply_vertical.zoom.";
	for (unsigned char c : key) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out += (char)c;
		else if (c >= 0x80) {
			char hex[4];
			std::snprintf(hex, sizeof(hex), "%02x", c);
			out += hex;
		} else
			out += '_';
	}
	return out;
}

void fire(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	sv_zoom_key(static_cast<Hot *>(data)->key, pressed);
}

void register_hot(const std::string &key, const std::string &name)
{
	for (const auto &h : g_hot)
		if (h.get()->key == key)
			return;
	auto h = std::make_unique<Hot>();
	h->key = key;
	h->obsName = obs_name_for(key);
	char text[256];
	std::snprintf(text, sizeof(text), T("HotZoom"), name.c_str());
	h->id = obs_hotkey_register_frontend(h->obsName.c_str(), text, fire, h.get());
	obs_data_array_t *saved = sv_store_array(("hotkey_" + h->obsName).c_str());
	if (saved) {
		obs_hotkey_load(h->id, saved);
		obs_data_array_release(saved);
	}
	g_hot.push_back(std::move(h));
}

/* Registers a hotkey for every zoom in the latest layouts. Main thread. */
void register_known()
{
	if (!g_loaded)
		return;
	std::vector<std::pair<std::string, std::string>> want;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (const auto &kv : g_state)
			if (kv.second.known)
				want.emplace_back(kv.first, kv.second.spec.name);
	}
	for (const auto &w : want)
		register_hot(w.first, w.second);
}

/* The keys set with "Set key" on the website, made into the zooms' OBS hotkeys.
   "zoombind_<hotkey>" remembers the last key set from the website, so a key
   changed later in OBS Settings, Hotkeys stays until the website sets a new one,
   and removing the key on the website only clears a key the website set. Main
   thread, after register_known. */
void apply_binds()
{
	if (!g_loaded)
		return;
	std::vector<std::pair<std::string, std::string>> want;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (const auto &kv : g_state)
			if (kv.second.known)
				want.emplace_back(kv.first, kv.second.spec.bind);
	}
	for (const auto &w : want) {
		Hot *h = nullptr;
		for (const auto &x : g_hot)
			if (x->key == w.first)
				h = x.get();
		if (!h || h->id == OBS_INVALID_HOTKEY_ID)
			continue;
		const std::string mark = "zoombind_" + h->obsName;
		const std::string was = sv_store_string(mark.c_str());
		if (w.second == was)
			continue;
		obs_data_array_t *keys = obs_data_array_create();
		if (!w.second.empty()) {
			/* "OBS_KEY_F8:1:0:1": the key, then ctrl, shift and alt. */
			const size_t c = w.second.find(':');
			const std::string key = w.second.substr(0, c);
			const std::string mods = c == std::string::npos ? std::string() : w.second.substr(c);
			obs_data_t *k = obs_data_create();
			obs_data_set_string(k, "key", key.c_str());
			obs_data_set_bool(k, "control", mods.size() > 1 && mods[1] == '1');
			obs_data_set_bool(k, "shift", mods.size() > 3 && mods[3] == '1');
			obs_data_set_bool(k, "alt", mods.size() > 5 && mods[5] == '1');
			obs_data_set_bool(k, "command", false);
			obs_data_array_push_back(keys, k);
			obs_data_release(k);
		} else if (was.empty()) {
			obs_data_array_release(keys);
			continue;
		}
		/* Loading replaces every key the hotkey had. */
		obs_hotkey_load(h->id, keys);
		sv_store_set_array(("hotkey_" + h->obsName).c_str(), keys);
		sv_store_set_string(mark.c_str(), w.second);
		obs_data_array_release(keys);
	}
}

} // namespace

std::string sv_zoom_key_for(const std::string &name)
{
	std::string out;
	for (unsigned char c : name)
		out += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
	return out;
}

void sv_zoom_note_layout(obs_source_t *owner, const SvLayout &layout)
{
	std::vector<std::pair<std::string, Spec>> zooms;
	for (const SvPiece &p : layout.pieces) {
		if (!p.zoom || p.zoomKey.empty())
			continue;
		Spec sp;
		sp.bind = p.zoomBind;
		sp.name = p.name;
		sp.motion = p.zoomMotion;
		sp.ms = p.zoomMs;
		sp.hold = p.zoomHold;
		sp.back = p.zoomBack;
		zooms.emplace_back(p.zoomKey, std::move(sp));
	}
	bool fresh = false;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_owners[owner] = std::move(zooms);
		fresh = recount(owner, os_gettime_ns());
	}
	if (fresh && qApp)
		QMetaObject::invokeMethod(
			qApp,
			[] {
				register_known();
				apply_binds();
			},
			Qt::QueuedConnection);
}

void sv_zoom_forget(obs_source_t *owner)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (g_owners.erase(owner))
		recount(nullptr, os_gettime_ns());
}

float sv_zoom_amount(const std::string &key)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	auto it = g_state.find(key);
	if (it == g_state.end() || !it->second.known)
		return 0.0f;
	State &st = it->second;
	const uint64_t now = os_gettime_ns();
	/* Coming back by itself: once it has been fully zoomed for that long. */
	if (st.to >= 1.0f && !st.spec.hold && st.spec.back > 0 &&
	    now >= st.at + (uint64_t)(ms_of(st.spec) * 1e6) + (uint64_t)(st.spec.back * 1e9))
		head_to(st, 0.0f, now);
	return amount_at(st, now);
}

void sv_zoom_key(const std::string &key, bool down)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	auto it = g_state.find(key);
	if (it == g_state.end() || !it->second.known)
		return;
	State &st = it->second;
	const uint64_t now = os_gettime_ns();
	if (st.spec.hold)
		head_to(st, down ? 1.0f : 0.0f, now);
	else if (down)
		head_to(st, st.to >= 0.5f ? 0.0f : 1.0f, now);
}

std::vector<SvZoomInfo> sv_zoom_list()
{
	/* Found before taking the lock: it looks through OBS's canvases. */
	const std::vector<obs_source_t *> skip = sv_canvas_running() ? std::vector<obs_source_t *>() : sv_canvas_extra_layout_sources();
	std::set<std::string> inUse;
	std::lock_guard<std::mutex> lock(g_mtx);
	/* Only zooms in layouts in use. With our canvases running that is every
	   canvas, live or not, so a key set up before going live already works. With
	   another app running the vertical, our extra canvases are not used, so the
	   layouts they keep offer no keys. */
	for (const auto &o : g_owners) {
		if (std::find(skip.begin(), skip.end(), o.first) != skip.end())
			continue;
		for (const auto &z : o.second)
			inUse.insert(z.first);
	}
	std::vector<SvZoomInfo> out;
	for (const auto &kv : g_state) {
		if (!kv.second.known || !inUse.count(kv.first))
			continue;
		SvZoomInfo z;
		z.name = kv.second.spec.name;
		z.zoomed = kv.second.to >= 0.5f;
		z.hold = kv.second.spec.hold;
		out.push_back(std::move(z));
	}
	return out;
}

void sv_zoom_do(const std::string &key, SvZoomDo what)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	auto it = g_state.find(key);
	if (it == g_state.end() || !it->second.known)
		return;
	State &st = it->second;
	const float to = what == SvZoomDo::In ? 1.0f : what == SvZoomDo::Out ? 0.0f : (st.to >= 0.5f ? 0.0f : 1.0f);
	head_to(st, to, os_gettime_ns());
}

void sv_zoom_load()
{
	g_loaded = true;
	register_known();
	apply_binds();
}

void sv_zoom_shutdown()
{
	g_loaded = false;
	for (auto &h : g_hot) {
		if (h->id == OBS_INVALID_HOTKEY_ID)
			continue;
		obs_data_array_t *keys = obs_hotkey_save(h->id);
		if (keys) {
			sv_store_set_array(("hotkey_" + h->obsName).c_str(), keys);
			obs_data_array_release(keys);
		}
		obs_hotkey_unregister(h->id);
		h->id = OBS_INVALID_HOTKEY_ID;
	}
	g_hot.clear();
}
