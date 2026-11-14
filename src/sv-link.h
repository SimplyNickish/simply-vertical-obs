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

// Linking this PC to the member's Simply Vertical account.
//
// One link per PC, shared by every Simply Vertical source. The PC has its own
// identity (an anonymous sign-in) whose key is locked to this Windows user by
// Windows itself (DPAPI) and never leaves the PC. The member approves the PC
// once in the browser. After that the plugin reads the saved layout (read
// only), listens for live moves on a private channel only approved PCs can
// hear, and keeps the last layout on disk so it still shows offline and after
// a restart. No OBS password, no polling.

#pragma once

#include <functional>
#include <string>
#include <vector>

void sv_link_module_load();
void sv_link_module_unload();

/* The "Link my account" button: opens the Approve page in the browser. */
void sv_link_request();
/* The "Unlink this PC" button. */
void sv_link_unlink();

/* Reads a public file (a brand kit logo): no sign-in sent, no redirects. False when it could not be read. */
bool sv_link_fetch_public(const std::string &url, std::string &bytes);

bool sv_link_is_linked();
/* A newer plugin version the website offers, or empty. Asked once, shortly after OBS starts. */
std::string sv_link_update_version();
/* One line for the settings window, already in the member's language. */
std::string sv_link_status();

/* The same as parts, for the panel's short account line. `problem` is a
   locale key, or empty. `code` is the code the browser page shows while waiting. */
enum class SvLinkKind { NotLinked, Waiting, Linked };
struct SvLinkState {
	SvLinkKind kind = SvLinkKind::NotLinked;
	bool offline = false;
	std::string code;
	std::string problem;
};
SvLinkState sv_link_state();

/* A source listens for layouts (draw list JSON) and status changes. Callbacks
   run on the link's own threads. `current` gets the layout to show right now
   (empty when there is none). Unlisten waits for a running callback. */
struct SvLinkListener {
	std::function<void(const std::string &json)> onLayout;
	std::function<void()> onStatus;
};
int sv_link_listen(SvLinkListener l, std::string *current);
void sv_link_unlisten(int id);

/* The member's saved layouts, read from the website and kept for offline use.
   Picking one is local to this PC and changes nothing on the website. An empty
   id goes back to following the website. While a saved layout is picked, moves
   made on the website wait until the member goes back to following it, so a
   website tab reopening can never swap the layout during a stream. */
struct SvLayoutChoice {
	std::string id;
	std::string name;
	std::string drawList; /* the layout as JSON, for drawing its small picture */
	std::string game;     /* the ready-made game it was made for, or empty */
};
std::vector<SvLayoutChoice> sv_link_presets();
/* The game the website's own layout was made for, or empty. */
std::string sv_link_site_game();
/* Which game a program is (its file name, lower case), from the list the
   website sends with the saved layouts. Empty when it is not a known game. */
std::string sv_link_game_for_exe(const std::string &exe);
std::string sv_link_active_preset();
bool sv_link_pick_preset(const std::string &id);

/* Hold changes: while on, the stream keeps the layout it shows now and moves
   made on the website wait. Turning it off sends the latest one. Picking a
   saved layout still works while holding. Not kept after OBS closes. */
void sv_link_set_hold(bool on);
bool sv_link_holding();
