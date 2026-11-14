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

#include "sv-store.h"

#include <obs-module.h>
#include <util/platform.h>

#include <mutex>

namespace {

std::mutex g_mtx;
obs_data_t *g_data = nullptr;
SvPicks g_current;

std::string settings_path()
{
	char *p = obs_module_config_path("settings.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

int g_batch = 0;
bool g_batch_dirty = false;

void save_locked()
{
	if (!g_data)
		return;
	/* Inside a batch the file is written once, when the batch ends. */
	if (g_batch > 0) {
		g_batch_dirty = true;
		return;
	}
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string path = settings_path();
	if (!path.empty())
		obs_data_save_json_safe(g_data, path.c_str(), "tmp", "bak");
}

/* The saved entry for a scene collection, made if it is new. The caller releases it. */
obs_data_t *collection_locked(const std::string &collection)
{
	obs_data_t *all = obs_data_get_obj(g_data, "collections");
	if (!all) {
		all = obs_data_create();
		obs_data_set_obj(g_data, "collections", all);
	}
	obs_data_t *one = obs_data_get_obj(all, collection.c_str());
	if (!one) {
		one = obs_data_create();
		obs_data_set_obj(all, collection.c_str(), one);
	}
	obs_data_release(all);
	return one;
}

} // namespace

void sv_store_load()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (g_data)
		return;
	const std::string path = settings_path();
	if (!path.empty() && os_file_exists(path.c_str()))
		g_data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!g_data)
		g_data = obs_data_create();
}

void sv_store_unload()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	obs_data_release(g_data);
	g_data = nullptr;
}

void sv_store_batch_begin()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	g_batch++;
}

void sv_store_batch_end()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (g_batch > 0)
		g_batch--;
	if (g_batch == 0 && g_batch_dirty) {
		g_batch_dirty = false;
		save_locked();
	}
}

int sv_store_int(const char *name, int fallback)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data || !obs_data_has_user_value(g_data, name))
		return fallback;
	return (int)obs_data_get_int(g_data, name);
}

void sv_store_set_int(const char *name, int value)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_set_int(g_data, name, value);
	save_locked();
}

std::string sv_store_string(const char *name)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_data ? obs_data_get_string(g_data, name) : "";
}

void sv_store_set_string(const char *name, const std::string &value)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_set_string(g_data, name, value.c_str());
	save_locked();
}

obs_data_array_t *sv_store_array(const char *name)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_data ? obs_data_get_array(g_data, name) : nullptr;
}

void sv_store_set_array(const char *name, obs_data_array_t *value)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_set_array(g_data, name, value);
	save_locked();
}

std::vector<std::string> sv_store_list(const std::string &collection, const char *name)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	std::vector<std::string> values;
	if (!g_data)
		return values;
	obs_data_t *all = obs_data_get_obj(g_data, "collections");
	obs_data_t *one = all ? obs_data_get_obj(all, collection.c_str()) : nullptr;
	obs_data_array_t *arr = one ? obs_data_get_array(one, name) : nullptr;
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		values.push_back(obs_data_get_string(item, "v"));
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(one);
	obs_data_release(all);
	return values;
}

void sv_store_set_list(const std::string &collection, const char *name, const std::vector<std::string> &values)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_t *one = collection_locked(collection);
	obs_data_array_t *arr = obs_data_array_create();
	for (const std::string &v : values) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "v", v.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(one, name, arr);
	obs_data_array_release(arr);
	obs_data_release(one);
	save_locked();
}

bool sv_store_welcome_done()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_data && obs_data_get_bool(g_data, "welcome_done");
}

void sv_store_set_welcome_done(bool done)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_set_bool(g_data, "welcome_done", done);
	save_locked();
}

std::string sv_store_route()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_data ? obs_data_get_string(g_data, "route") : "";
}

void sv_store_set_route(const std::string &route)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_set_string(g_data, "route", route.c_str());
	save_locked();
}

bool sv_store_has_picks(const std::string &collection)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return false;
	obs_data_t *all = obs_data_get_obj(g_data, "collections");
	if (!all)
		return false;
	obs_data_t *one = obs_data_get_obj(all, collection.c_str());
	const bool has = one && obs_data_has_user_value(one, "game");
	obs_data_release(one);
	obs_data_release(all);
	return has;
}

SvPicks sv_store_picks(const std::string &collection)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	SvPicks p;
	if (!g_data)
		return p;
	obs_data_t *all = obs_data_get_obj(g_data, "collections");
	if (!all)
		return p;
	obs_data_t *one = obs_data_get_obj(all, collection.c_str());
	if (one) {
		p.game = obs_data_get_string(one, "game");
		p.cam = obs_data_get_string(one, "cam");
		obs_data_release(one);
	}
	obs_data_release(all);
	return p;
}

void sv_store_set_picks(const std::string &collection, const SvPicks &picks)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (!g_data)
		return;
	obs_data_t *one = collection_locked(collection);
	obs_data_set_string(one, "game", picks.game.c_str());
	obs_data_set_string(one, "cam", picks.cam.c_str());
	obs_data_release(one);
	save_locked();
}

SvPicks sv_store_current_picks()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_current;
}

void sv_store_set_current_picks(const SvPicks &picks)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	g_current = picks;
}
