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

// Instant replay: plays the last clip Backtrack saved over the whole phone, on the
// vertical only, then goes back to the layout by itself. The main OBS picture and the
// main stream's sound never change.

#pragma once

#include "sv-output.h"

#include <string>
#include <vector>

/* At module load: the hidden filter the slow motion needs. */
void sv_replay_register();
/* Plays the last saved clip, or stops it if one is playing. Any thread: the work
   always happens on OBS's main thread. */
void sv_replay_toggle();
/* Main thread. False with a reason when there is no clip to play. */
bool sv_replay_play(std::string &problem);
/* Main thread. Ends it straight away. Safe to call when nothing plays. */
void sv_replay_stop();
/* Main thread. Fades back to the live layout, then ends. Safe to call when nothing plays. */
void sv_replay_finish();
/* Main thread. */
bool sv_replay_playing();
/* The controls while a replay plays. Any thread. They do nothing while it slides in or
   fades out. Back and Restart always play on, even from a pause. */
void sv_replay_pause_toggle();
/* Back 5 seconds of the clip, to the full picture at or before that. */
void sv_replay_back();
/* From the start of the replay again, slow part and all. */
void sv_replay_restart();
/* The slow part's speed (or the whole replay's with one player): 75, 50, 25, 100, round. */
void sv_replay_speed_next();
/* Main thread. */
bool sv_replay_paused();
/* Main thread. The speed the Speed key shows, 0 when nothing plays. */
int sv_replay_speed();
/* Any thread. The Slow motion key, with What slows down on When I press: slows the replay
   from about a second back, for the length picked; pressed again while slow, back to
   normal speed on the next full picture. */
void sv_replay_slow_press();
/* Main thread. The replay playing slows when the Slow motion key is pressed. */
bool sv_replay_press_mode();
/* Main thread. Pressed slow motion is playing right now. */
bool sv_replay_press_slowed();
/* Main thread. Plays this clip. False with a reason when it cannot. */
bool sv_replay_play_clip(const SvClip &clip, std::string &problem);
/* Any thread. Plays these clips one after another, in the order given. Stop ends them all. */
void sv_replay_play_row(const std::vector<SvClip> &clips);
/* Any thread. Saves a clip, then plays it as soon as it is written (after the seconds
   Backtrack keeps going, if any). */
void sv_replay_save_and_play();
/* Any thread. Backtrack wrote a clip: plays it when Save and replay asked for it, or when
   "Play every clip as soon as it is saved" is on. */
void sv_replay_clip_saved();
/* Any thread. Plays the clip before (older, 1) or after (newer, -1) the one played last,
   from the recent clips. With none played yet, the newest. */
void sv_replay_step_clip(int older);
/* Main thread. Clips still waiting in a row after the one playing. */
int sv_replay_row_left();
/* Main thread. The file of the last clip started. */
std::string sv_replay_last_played();
