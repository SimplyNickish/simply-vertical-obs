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

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#include "sv-brand.h"
#include "sv-link.h"
#include "sv-replay.h"
#include "sv-ui.h"

extern void sv_register_source();

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Simply Vertical: your finished vertical layout as one OBS source.";
}

bool obs_module_load(void)
{
	sv_register_source();
	sv_replay_register();
	sv_link_module_load();
	sv_ui_load();
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	sv_ui_unload();
	sv_brand_unload();
	sv_link_module_unload();
	obs_log(LOG_INFO, "unloaded");
}
