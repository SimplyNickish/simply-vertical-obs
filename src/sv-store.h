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

// The plugin's own remembered choices, kept in OBS's plugin settings folder.
// Nothing secret lives here: stream keys have their own encrypted file.

#pragma once

#include <obs.h>

#include <string>
#include <vector>

/* The game and camera a scene collection uses. Source ids change between
   collections, so each collection keeps its own. */
struct SvPicks {
	std::string game;
	std::string cam;
};

/* Where the vertical picture lives. "" means the member has not chosen yet. */
inline constexpr const char *SV_ROUTE_OWN = "own";
inline constexpr const char *SV_ROUTE_OTHER = "other";

void sv_store_load();
void sv_store_unload();

/* Plain remembered values, never anything secret. */
int sv_store_int(const char *name, int fallback);
void sv_store_set_int(const char *name, int value);
/* Between these, saved values are kept in memory and the file is written once at the end.
   They nest. */
void sv_store_batch_begin();
void sv_store_batch_end();
std::string sv_store_string(const char *name);
void sv_store_set_string(const char *name, const std::string &value);

/* A saved list of OBS data (hotkeys). The caller releases what comes back. */
obs_data_array_t *sv_store_array(const char *name);
void sv_store_set_array(const char *name, obs_data_array_t *value);

/* A list of plain values kept per scene collection, for anything that names
   sources or scenes, because those ids change between collections. */
std::vector<std::string> sv_store_list(const std::string &collection, const char *name);
void sv_store_set_list(const std::string &collection, const char *name, const std::vector<std::string> &values);

/* The per-collection lists. A scene link is "scene uuid|layout id". */
inline constexpr const char *SV_STORE_OVERLAYS = "overlays";
inline constexpr const char *SV_STORE_SCENE_LINKS = "scene_links";
/* "main scene uuid|vertical scene uuid": switching the main scene switches the vertical one. */
inline constexpr const char *SV_STORE_VSCENE_LINKS = "vscene_links";
inline constexpr const char *SV_SCENE_LINK_SITE = "@website";
/* An extra canvas keeps its own layout links, "scene uuid|layout id", in the list named
   by sv_store_canvas_links. SV_SCENE_LINK_MAIN there means "the same as the main canvas".
   Its vertical scene links share SV_STORE_VSCENE_LINKS, since scene ids are unique. */
inline constexpr const char *SV_SCENE_LINK_MAIN = "@main";
inline std::string sv_store_canvas_links(const std::string &canvas)
{
	return std::string(SV_STORE_SCENE_LINKS) + ":" + canvas;
}

bool sv_store_welcome_done();
void sv_store_set_welcome_done(bool done);

std::string sv_store_route();
void sv_store_set_route(const std::string &route);

bool sv_store_has_picks(const std::string &collection);
SvPicks sv_store_picks(const std::string &collection);
void sv_store_set_picks(const std::string &collection, const SvPicks &picks);

/* The picks for whichever collection is open right now. Safe from any thread,
   so a newly added source can start with them. */
SvPicks sv_store_current_picks();
void sv_store_set_current_picks(const SvPicks &picks);
