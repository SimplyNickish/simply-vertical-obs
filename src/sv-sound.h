/*
Simply Vertical
Copyright (C) 2026 SimplyNickish
*/

#pragma once

#include <string>
#include <vector>

/* The vertical's own sound mix. When it is on, every sound source in OBS gets a hidden copy
   whose volume belongs to the vertical alone. OBS mixes the copies onto one of its six tracks,
   kept just for the vertical, and the vertical streams and records that track. The main
   stream's volumes never change. */

struct SvSoundDesk {
	bool own = false;        /* false: the vertical uses a main OBS track (SvOutSettings::audio_track) */
	int track = 6;           /* the OBS track the vertical's own mix goes on, 1 to 6 */
	int master = 100;        /* percent, over every source */
	bool follow_mute = true; /* a source muted in OBS is muted on the vertical too */
};

struct SvSoundSource {
	std::string uuid;
	std::string name;
	int volume = 100; /* percent, 0 to 200 */
	bool muted = false;
};

/* At module load, before any source exists. Starts paused until OBS has loaded. */
void sv_sound_load();
void sv_sound_unload();
/* Lets go of every copy while OBS changes or clears a scene collection. */
void sv_sound_pause(bool paused);

SvSoundDesk sv_sound_desk();
/* Applies straight away (copies made or removed, the track kept clear) and saves. */
void sv_sound_set_desk(const SvSoundDesk &desk);

/* Every sound source in OBS right now, in name order, with its vertical volume. */
std::vector<SvSoundSource> sv_sound_sources();
void sv_sound_set_source(const std::string &uuid, int volume, bool muted);
/* How loud a source is on the vertical right now, 0 (silent) to 1 (full), after its volume. */
float sv_sound_level(const std::string &uuid);

/* The OBS track the vertical's sound comes from: the desk's track when it is on, else `chosen`. */
int sv_sound_track(int chosen);

/* The tracks OBS's own stream and recording use, bit 0 is track 1. */
unsigned sv_sound_main_tracks();

/* The desk and every volume as one string, to put back when a settings window is cancelled. */
std::string sv_sound_snapshot();
void sv_sound_restore(const std::string &snapshot);
