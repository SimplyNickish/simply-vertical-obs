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

#include "sv-follow.h"

#include "sv-actions.h"
#include "sv-link.h"
#include "sv-output.h"
#include "sv-store.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QTimer>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define T(key) obs_module_text(key)

namespace {

/* How long a game has to stay in front before the layout follows it, so a
   quick look at another window never switches anything. */
constexpr uint64_t SETTLE_NS = 3000000000ULL;

QTimer *g_timer = nullptr;
bool g_on = false;
std::string g_candidate;  /* the game in front now, not yet settled */
uint64_t g_since = 0;     /* when it came to the front */
std::string g_seen;       /* the last game that settled */
bool g_acted = false;     /* the layout has followed g_seen already */
std::function<void()> g_listener;

/* The file name of the program in front, lower case, or empty. */
std::string foreground_exe()
{
	HWND w = GetForegroundWindow();
	if (!w)
		return "";
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	if (!pid || pid == GetCurrentProcessId())
		return "";
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return "";
	wchar_t path[MAX_PATH * 2];
	DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
	const bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
	CloseHandle(h);
	if (!ok || len == 0)
		return "";
	std::wstring name(path, len);
	const size_t slash = name.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		name = name.substr(slash + 1);
	std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)(bytes > 0 ? bytes : 0), '\0');
	if (bytes > 0)
		WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), out.data(), bytes, nullptr, nullptr);
	return out;
}

std::string pick_key(const std::string &game)
{
	return "follow_game_pick_" + game;
}

/* The member picked a layout made for the game they are playing: that one is
   the one to come back to next time this game is in front. */
void remember(const std::string &game)
{
	const std::string active = sv_link_active_preset();
	if (active.empty())
		return;
	for (const SvLayoutChoice &c : sv_link_presets()) {
		if (c.id == active && c.game == game) {
			if (sv_store_string(pick_key(game).c_str()) != active)
				sv_store_set_string(pick_key(game).c_str(), active);
			return;
		}
	}
}

void follow(const std::string &game)
{
	const std::vector<SvLayoutChoice> list = sv_link_presets();
	const std::string active = sv_link_active_preset();
	const std::string site = sv_link_site_game();
	for (const SvLayoutChoice &c : list)
		if (c.id == active && c.game == game)
			return; /* already showing a layout for it */
	if (active.empty() && site == game)
		return;

	const std::string remembered = sv_store_string(pick_key(game).c_str());
	const SvLayoutChoice *choice = nullptr;
	for (const SvLayoutChoice &c : list) {
		if (c.game != game)
			continue;
		if (!choice || c.id == remembered)
			choice = &c;
		if (c.id == remembered)
			break;
	}
	std::string name;
	if (choice) {
		sv_action_pick_layout(choice->id);
		name = choice->name;
	} else if (!active.empty() && site == game) {
		sv_action_pick_layout("");
		name = T("LayoutWebsiteTile");
	} else {
		return; /* no layout made for this game: leave the stream as it is */
	}
	std::string text = T("FollowSwitched");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, name);
	sv_output_set_notice(text, false);
}

void tick()
{
	if (!g_on)
		return;
	const std::string game = sv_link_game_for_exe(foreground_exe());
	if (game.empty()) {
		/* OBS, a browser or the desktop in front: nothing changes. */
		g_candidate.clear();
		return;
	}
	const uint64_t now = os_gettime_ns();
	if (game != g_candidate) {
		g_candidate = game;
		g_since = now;
		return;
	}
	if (now - g_since < SETTLE_NS)
		return;
	if (game != g_seen) {
		g_seen = game;
		g_acted = false;
		obs_log(LOG_INFO, "Follow my game: %s is in front", game.c_str());
		/* The panel says which game it sees. */
		if (g_listener)
			g_listener();
	}
	remember(game);
	/* Once per game: after following it, the member's own picks stand. */
	if (g_acted || sv_link_holding())
		return;
	g_acted = true;
	follow(game);
}

void run_timer()
{
	if (!g_timer)
		return;
	if (g_on && !g_timer->isActive())
		g_timer->start();
	else if (!g_on && g_timer->isActive())
		g_timer->stop();
}

} // namespace

void sv_follow_start()
{
	g_on = sv_store_int("follow_game", 0) != 0;
	if (!g_timer) {
		g_timer = new QTimer();
		g_timer->setInterval(1000);
		QObject::connect(g_timer, &QTimer::timeout, [] { tick(); });
	}
	run_timer();
}

void sv_follow_stop()
{
	if (g_timer) {
		g_timer->stop();
		delete g_timer;
		g_timer = nullptr;
	}
}

bool sv_follow_on()
{
	return g_on;
}

void sv_follow_set(bool on)
{
	if (on == g_on)
		return;
	g_on = on;
	sv_store_set_int("follow_game", on ? 1 : 0);
	/* Turning it on follows the game in front straight away, once it settles. */
	g_candidate.clear();
	g_seen.clear();
	g_acted = false;
	run_timer();
	if (g_listener)
		g_listener();
}

void sv_follow_set_listener(std::function<void()> listener)
{
	g_listener = std::move(listener);
}

std::string sv_follow_seen()
{
	return g_on ? g_seen : std::string();
}
