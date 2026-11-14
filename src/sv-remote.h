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

// The remote control hook. A tiny web address on this PC only
// (http://127.0.0.1:4480) that the SimplyNickish Stream Deck plugin,
// Streamer.bot, Touch Portal, SAMMI or anything else that can open a web
// address uses to zoom pieces, pick layouts, hold changes, go live, record and
// save clips. No password and no OBS WebSocket: it only answers this PC, and
// it turns away web pages, so a website can never press anything.
//
//   /status                      what is on right now
//   /layouts                     saved layouts, with each one's drawing
//   /zoom?piece=Minimap          zoom a piece in or back (do=in, out, down, up)
//   /layout?name=Apex            pick a saved layout (or do=next, prev, website)
//   /hold                        hold changes or send them (do=on, off)
//   /live                        go live or stop, all platforms (do=start, stop)
//   /record                      record or stop (do=start, stop)
//   /clip                        save a Backtrack clip
//   /vscene?do=next              next or previous vertical scene
//
// When 4480 is taken the next free one up to 4484 is used.

#pragma once

/* Main thread only. */
void sv_remote_start();
void sv_remote_stop();

/* The port it answers on, 0 when it is not running. */
int sv_remote_port();
