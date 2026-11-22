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

// Going live and recording from the vertical canvas. One shared video encoder
// feeds every destination and the recording, so a second platform costs almost
// nothing extra. Stream keys stay on this PC, encrypted by Windows, and are
// never sent to Simply Nickish.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/* The id the recording uses wherever a destination id is expected. With more than one picture
   ticked in Settings, the first one records under this id and each of the others under
   SV_RECORD_MORE followed by its canvas id. Starting, stopping, splitting, pausing and markers on
   SV_RECORD_ID always act on all of them. */
inline constexpr const char *SV_RECORD_ID = "record";
inline constexpr const char *SV_RECORD_MORE = "record:";
/* The same for Backtrack (keeps the last moments ready to save) and the virtual camera. */
inline constexpr const char *SV_BACKTRACK_ID = "backtrack";
inline constexpr const char *SV_VCAM_ID = "vcam";

struct SvDest {
	std::string id;
	std::string platform;
	std::string name;
	std::string server;
	std::string key;      /* only filled when saving, never handed back out */
	bool has_key = false; /* a key is saved for this destination */
	/* Its own picture instead of the usual one from Settings. Everything else
	   (encoder, preset, keyframes, sound) stays the usual. */
	bool own_video = false;
	int bitrate = 6000;
	int size = 0; /* SV_SIZE_* */
	int fps = 0;  /* 0: the usual frames a second */
	/* The vertical canvas this platform shows, "" for the main one. A canvas that
	   has since been removed falls back to the main one. */
	std::string canvas;
	/* Switched off: Go live on all, Start with OBS and a canvas's own Go live key leave it
	   out. Its own Go live key still starts it. */
	bool on = true;
	/* Twitch only: Twitch Dual Format. It goes out through OBS's own stream with OBS's
	   Enhanced Broadcasting, carrying OBS's wide picture and the canvas above together, so
	   none of the picture settings here apply. */
	bool dual = false;
};

/* The canvas id that means OBS's own wide picture, for a platform that streams the main
   scene instead of a vertical one. */
inline constexpr const char *SV_WIDE = "obs-wide";

enum class SvRunState { Off, Starting, Live, Reconnecting, Stopping };

/* How a live stream is doing over the last ten seconds. */
enum class SvHealth { Steady, Shaky, Dropping };

struct SvRunInfo {
	SvRunState state = SvRunState::Off;
	std::string problem; /* what went wrong and what to do, or empty */
	uint64_t seconds = 0;
	uint64_t dropped = 0;
	uint64_t total = 0;
	/* Streams only, filled by sv_output_sample once a second. */
	float kbps = 0;
	std::vector<float> history; /* the last minute of kbps, oldest first */
	SvHealth health = SvHealth::Steady;
	bool low_bitrate = false; /* well under the bitrate it was set to, for a while */
	float dropped_pct = 0;    /* over the last ten seconds */
	int reconnect_left = 0;   /* seconds until the next try, while reconnecting */
	bool paused = false;      /* the recording only; seconds stand still meanwhile */
};

struct SvOutSettings {
	int bitrate = 6000;
	int audio_track = 1;
	std::string encoder; /* empty: choose automatically */
	std::string folder;  /* empty: the same folder as OBS's own recordings */
	int backtrack_seconds = 60;
	int delay_seconds = 0;   /* holds streams back, never the recording */
	bool follow_obs = false; /* start and stop with OBS's own Start Streaming and Start Recording */
	/* Keep every stream: record the vertical while it is live (platforms delete
	   vertical streams after a few days). Stops when the member stops the stream,
	   keeps going if the stream drops. */
	bool record_with_live = false;

	int audio_bitrate = 160;  /* 128, 160, 192 or 320 */
	int reconnect_tries = 25; /* 0: never try again after a drop. 25 and 2 match OBS's own stream. */
	int reconnect_wait = 2;   /* seconds before the first try; OBS waits a little longer each time */
	int backtrack_auto = 0;   /* 0 never, 1 when OBS opens, 2 when a stream goes live */

	/* Picture */
	int size = 0;             /* SV_SIZE_*: what leaves the PC; the canvas itself is always 1080 x 1920 */
	int fps = 0;              /* the frames a second picked; 0: OBS's own rate */
	int fps_div = 1;          /* worked out from fps: 1 is OBS's own rate, 2 half of it, and so on */
	bool sharp_scale = true;  /* when shrinking: sharp (Lanczos) or smooth (bicubic) */
	int keyframe_sec = 2;     /* a full picture this often; TikTok and YouTube ask for 2 */
	int effort = 2;           /* the older five steps, 0 lightest to 4 best; only used when no preset is saved */
	std::string preset;      /* the encoder's own preset, for example p5 or veryfast; empty: from effort */

	/* Recording */
	int rec_format = 0;        /* SV_FMT_* */
	int rec_quality = 0;       /* SV_Q_* */
	int record_bitrate = 12000; /* used when rec_quality is SV_Q_BITRATE or SV_Q_VBR */
	int rec_level = 20;         /* used when rec_quality is SV_Q_LEVEL: 1 to 51, lower is sharper */
	int split_mode = 0;        /* SV_SPLIT_* */
	int split_minutes = 30;
	int split_mb = 2048;
	bool rec_game_name = false; /* the game's name in the file name */
	unsigned rec_tracks = 0;   /* more OBS sound tracks in recordings and clips, bit 0 is track 1 */
	std::string rec_canvas;    /* the first canvas recorded (rec_canvases[0]); empty: the main one */
	/* Every picture Record records, each to a file of its own, in the order Settings shows them:
	   "" the main vertical, a canvas id, or SV_WIDE for OBS's own horizontal picture. Never empty. */
	std::vector<std::string> rec_canvases{std::string()};
	/* SV_CODEC_*: the video format of recordings and Backtrack clips. HEVC and AV1 look better
	   at the same size, but only newer graphics cards make them. Streams stay H.264, the one
	   format every platform takes, except YouTube when codec_youtube is on. */
	int rec_codec = 0;
	bool codec_youtube = false;

	/* Backtrack */
	int clip_after = 0;         /* seconds Backtrack keeps going after Save clip */
	std::string clip_folder;    /* empty: the recordings folder */
	int clip_quality = 0;       /* SV_CLIP_* */
	int clip_level = 20;        /* used when clip_quality is SV_CLIP_LEVEL */
	int clip_bitrate = 12000;   /* used when clip_quality is SV_CLIP_BITRATE or SV_CLIP_VBR */
	bool clip_game_name = false;
	int clip_max_mb = 0;        /* 0: no memory limit, the length alone decides */
	std::string clip_canvas;    /* the canvas Backtrack keeps; empty: the main one */
	int replay_slow = 50;       /* Play last clip: slow motion speed, 100 (none), 75, 50 or 25 */
	bool replay_slow_whole = false; /* slow the whole replay, not just the moment you pressed Save clip */
	bool replay_slow_press = false; /* the replay plays at normal speed and slows when the Slow motion key is pressed */
	int replay_slow_seconds = 3; /* how long before the press the slow motion starts, 1 to 30 */
	bool replay_sound = false;  /* Play last clip with its sound, on the vertical's own mix only */
	int replay_seconds = 15;    /* Play last clip: only its last this many seconds; 0 plays all of it */
	bool replay_tag = true;     /* Play last clip: a lit frame and a REPLAY tag in the layout's colour */
	bool replay_auto = false;   /* play every clip on the vertical as soon as Backtrack has saved it */
	int replay_transition = 3;  /* how a replay comes on and goes off: 0 wipe, 1 slide, 2 fade, 3 cinematic */
	bool replay_line = true;    /* a lit line under the REPLAY tag that fills as it plays */
	bool replay_cam_live = false; /* whole phone replays: the live facecam stays on top */
	int replay_zoom = 10;       /* the slow push in on the moment, 0 (off) to 25 percent */
};

enum { SV_BACKTRACK_AUTO_NEVER = 0, SV_BACKTRACK_AUTO_OPEN = 1, SV_BACKTRACK_AUTO_LIVE = 2 };
enum { SV_SIZE_1080 = 0, SV_SIZE_900 = 1, SV_SIZE_720 = 2 };
enum { SV_FMT_MP4 = 0, SV_FMT_MKV = 1, SV_FMT_MOV = 2 };
/* Recording quality: the stream's, a steady quality level (CQP, or CRF on x264, lower is sharper),
   a steady bitrate (CBR) or a bitrate that rises in busy moments (VBR). The numbers stay as they
   were saved before VBR was added. */
enum { SV_Q_SAME = 0, SV_Q_LEVEL = 1, SV_Q_BITRATE = 2, SV_Q_VBR = 3 };
enum { SV_CLIP_AS_RECORDING = 0, SV_CLIP_AS_STREAM = 1, SV_CLIP_LEVEL = 2, SV_CLIP_BITRATE = 3, SV_CLIP_VBR = 4 };
enum { SV_SPLIT_NEVER = 0, SV_SPLIT_TIME = 1, SV_SPLIT_SIZE = 2 };
enum { SV_CODEC_H264 = 0, SV_CODEC_HEVC = 1, SV_CODEC_AV1 = 2 };

/* The last clip Backtrack saved. `seq` goes up by one for every new clip. `after` is how many
   seconds it kept going after Save clip was pressed, so the press sits that far from its end. */
struct SvClip {
	std::string path;
	int seconds = 0;
	int after = 0;
	uint64_t seq = 0;
};

/* One line for the panel: the last thing that went wrong, or a clip being saved. */
struct SvNotice {
	std::string text;
	bool problem = false;
};

struct SvEncoderChoice {
	std::string id;
	std::string name;
};

void sv_output_load();
void sv_output_shutdown();

/* Called from any thread when anything changes. */
void sv_output_set_listener(std::function<void()> listener);

/* Keys are left blank in what comes back. */
std::vector<SvDest> sv_output_dests();

/* A blank key keeps the saved one. A blank id adds a new destination. Returns its id. */
std::string sv_output_save_dest(const SvDest &dest);
bool sv_output_remove_dest(const std::string &id);
/* Switches a platform on or off for Go live on all, keeping everything else. */
void sv_output_set_dest_on(const std::string &id, bool on);

/* ---------- Twitch Dual Format, through OBS's own stream. Main thread only. ---------- */

/* This OBS can do it (31.1 or newer). */
bool sv_output_dual_possible();
/* What OBS's own Stream settings say now, for the box that shows what will change. */
struct SvObsStream {
	std::string service; /* "Twitch", "YouTube - RTMPS", "Custom"... */
	bool has_key = false;
	bool enhanced = false;     /* Enhanced Broadcasting is on */
	std::string extra_canvas;  /* the Additional Canvas's name, empty for none */
};
SvObsStream sv_output_obs_stream();
/* True when OBS's own stream is set up exactly for this saved Twitch Dual Format platform. */
bool sv_output_dual_ready(const std::string &id);
/* Points OBS's own stream at this saved platform: Twitch with its key, Enhanced Broadcasting
   on, and its canvas as the Additional Canvas. restartNeeded: OBS built its outputs without
   Enhanced Broadcasting, so it takes one restart of OBS before going live. */
bool sv_output_setup_dual(const std::string &id, bool &restartNeeded, std::string &problem);
/* OBS's own stream is starting, started, stopping or stopped (an obs_frontend_event). */
void sv_output_obs_stream_event(int event);
/* Lets go of any picture encoder made for this canvas, before the canvas goes. */
void sv_output_forget_canvas(const std::string &canvas);
/* True while anything live or recording draws from this canvas. */
bool sv_output_canvas_in_use(const std::string &canvas);
/* True only while a stream goes out from this canvas (recording, Backtrack and the virtual camera do not count). */
bool sv_output_canvas_streaming(const std::string &canvas);

/* Returns false and fills problem when it could not even begin. */
bool sv_output_start(const std::string &id, std::string &problem);
void sv_output_stop(const std::string &id);

SvRunInfo sv_output_info(const std::string &id);
bool sv_output_any_running();
/* A stream, recording or Backtrack holds the encoders (the virtual camera does not). */
bool sv_output_encoders_in_use();

/* Writes the moments Backtrack is holding to a file. False when Backtrack is off.
   A second press within a moment of the first is ignored, so no clip is ever overwritten. */
bool sv_output_save_backtrack();
/* Ends the recording's file and carries straight on in a new one, nothing lost in between.
   Works whatever "Split into parts" is set to. False when not recording. */
bool sv_output_split_record();
/* Marks this moment in the recording, so editors and players can jump straight to it.
   Only MP4 recordings keep markers. */
enum class SvChapter { Added, NotRecording, NotMp4 };
SvChapter sv_output_add_chapter();
/* Pauses or carries on the recording, one file with the paused part left out. want: 1 pause,
   0 carry on, -1 the other way round from now. The recording has encoders of its own, so live
   streams and Backtrack never pause with it. Graphics card encoders only: NeedsCard on the
   processor (x264). Shared is a safety net if the recording ever shares an encoder anyway. */
enum class SvPause { Paused, Resumed, NotRecording, NeedsCard, Shared, NotPossible };
SvPause sv_output_pause_record(int want);
/* False on the processor (x264), where the Pause key is hidden. */
bool sv_output_can_pause();
bool sv_output_record_paused();
SvClip sv_output_last_clip();

/* One of the recent clips in the clip folder, newest first. */
struct SvClipInfo {
	SvClip clip;
	int64_t savedAt = 0; /* seconds since 1970, from the file */
	std::string game;    /* from the file name, empty when the clip was not named after a game */
};
/* Backtrack's clips in its folder, newest first, at most `max`. Reads the folder, so it
   is still there after OBS restarts. Any thread. */
std::vector<SvClipInfo> sv_output_recent_clips(size_t max = 10);

/* Measures each live stream's speed. The panel calls it once a second while anything runs.
   Reads counters on this PC only, nothing goes over the network. */
void sv_output_sample();

SvNotice sv_output_notice();
void sv_output_set_notice(const std::string &text, bool problem);
#ifdef SV_DEV_BUILD
void sv_output_dev_fail_more();
#endif

SvOutSettings sv_output_settings();
void sv_output_set_settings(const SvOutSettings &settings);
/* OBS's own frame rate as a whole number (59.94 is 60). */
int sv_output_obs_fps();
/* How far OBS's frame rate is divided to give `fps`: 1 when it is OBS's rate or above it. */
int sv_output_fps_div(int fps);
std::vector<SvEncoderChoice> sv_output_encoders();
/* The encoder Automatic picks on this PC. */
std::string sv_output_auto_encoder();
/* The same maker's HEVC or AV1 encoder for one of sv_output_encoders(), or empty when this PC
   cannot make that format with it (the processor never can, it would be far too slow). */
std::string sv_output_codec_encoder(const std::string &encoder, int codec);

/* One of an encoder's own presets, with its name in plain words. */
struct SvPreset {
	std::string id;
	std::string name;
};
/* An encoder's presets from lightest to best picture, and the one OBS uses by default.
   Empty for an encoder we do not know. */
std::vector<SvPreset> sv_output_presets(const std::string &encoder, std::string *usual = nullptr);
/* The preset the vertical uses with this encoder. */
std::string sv_output_preset_for(const std::string &encoder, const SvOutSettings &settings);

/* What OBS's own stream uses, to copy onto the vertical. found is false when it could not be read. */
struct SvMainEncoder {
	bool found = false;
	std::string encoder; /* one of sv_output_encoders(), or empty */
	int bitrate = 0;
	std::string preset; /* in the vertical encoder's own words, or empty */
	int keyframe_sec = 2;
	bool keyframe_auto = false; /* OBS's stream picks its own, so keyframe_sec is the usual 2 */
};
SvMainEncoder sv_output_main_encoder();

/* Where recordings land right now. */
std::string sv_output_record_folder();

/* The pictures Record will record now: the ticked ones whose canvas still exists, never empty. */
std::vector<std::string> sv_output_record_canvases(const SvOutSettings &settings);
