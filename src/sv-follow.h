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

// Follow my game: once a second, looks at which program is in front on this
// PC. When it has been one of the known games for a few seconds, the vertical
// switches to the saved layout made for that game (or back to the website's,
// if that is the one made for it). Close Valorant, open Apex, and the
// vertical changes with it. Reads the program's name on this PC only; nothing
// is sent anywhere. Saved on this PC, off until the member turns it on.

#pragma once

#include <functional>
#include <string>

/* Main thread. Start once the panel is up, stop before OBS closes. */
void sv_follow_start();
void sv_follow_stop();

/* Main thread. Turning it on or off from anywhere (panel, Stream Deck) tells the listener. */
bool sv_follow_on();
void sv_follow_set(bool on);
void sv_follow_set_listener(std::function<void()> listener);

/* The game seen in front right now (its id, as the website names it), or empty. */
std::string sv_follow_seen();
