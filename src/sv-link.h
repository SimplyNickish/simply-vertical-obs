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

/* "Get my key": asks the website for the member's stream key on a platform they connected on
   the dashboard ("twitch" or "kick"). Blocks for a moment, so never call it on OBS's own
   window thread. The website never keeps the key; the caller saves it like a typed one.
   `problem` is a locale key when it did not work. `server` is filled only for Kick. */
struct SvStreamKey {
	bool ok = false;
	std::string key;
	std::string server;
	std::string problem;
};
SvStreamKey sv_link_stream_key(const std::string &platform);

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
	std::string account; /* the member's name while linked, empty when unknown */
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
/* The layout the main vertical shows right now (draw list JSON), or empty. */
std::string sv_link_showing();
#ifdef SV_DEV_BUILD
/* Testing builds only: shows test-layout.json from the settings folder, saved nowhere. */
std::string sv_link_test_layout();
#endif
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

/* A layer moved or resized in the canvas panel: saved on the website (only that
   layer's place and size), to the saved layout `preset` or, when empty, to the
   website's own layout. The saved place is shown as soon as the website answers,
   then `done` runs on the link thread: ok false when it was not saved, with
   `reason` "locked", "gone" or empty. Any thread, never blocks. */
void sv_link_move_piece(const std::string &preset, const std::string &piece, float x, float y, float w, float h,
			std::function<void(bool ok, const std::string &reason)> done);

/* How far the OBS setup has got, for the setup guide on the website. */
struct SvSetupProgress {
	bool game = false;     /* a game source is picked */
	bool camera = false;   /* a camera source is picked */
	bool finished = false; /* Finish was pressed in the setup window */
	int platforms = 0;     /* platforms added to go live on */
	bool otherApp = false; /* Aitum Vertical or SE.Live does the streaming */
	bool wentLive = false; /* gone live through Simply Vertical at least once */
	/* The setup window's screen showing now ("account", "game", "camera", "app", "see",
	   "platforms", "ready"), empty while it is closed, so the website can say where they are. */
	std::string step;
};
/* The website is told once it is linked, and again only when something changed.
   Cheap to call often: an unchanged report sends nothing. Any thread. */
void sv_link_report_setup(const SvSetupProgress &p);
