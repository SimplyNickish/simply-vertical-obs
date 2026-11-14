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

/* The id the recording uses wherever a destination id is expected. */
inline constexpr const char *SV_RECORD_ID = "record";
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
};

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
	std::string rec_canvas;    /* the canvas recorded; empty: the main one */

	/* Backtrack */
	int clip_after = 0;         /* seconds Backtrack keeps going after Save clip */
	std::string clip_folder;    /* empty: the recordings folder */
	int clip_quality = 0;       /* SV_CLIP_* */
	int clip_level = 20;        /* used when clip_quality is SV_CLIP_LEVEL */
	int clip_bitrate = 12000;   /* used when clip_quality is SV_CLIP_BITRATE or SV_CLIP_VBR */
	bool clip_game_name = false;
	int clip_max_mb = 0;        /* 0: no memory limit, the length alone decides */
	std::string clip_canvas;    /* the canvas Backtrack keeps; empty: the main one */
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

/* The last clip Backtrack saved. `seq` goes up by one for every new clip. */
struct SvClip {
	std::string path;
	int seconds = 0;
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

/* A blank key keeps the saved one. A blank id adds a new destination. */
void sv_output_save_dest(const SvDest &dest);
bool sv_output_remove_dest(const std::string &id);
/* Lets go of any picture encoder made for this canvas, before the canvas goes. */
void sv_output_forget_canvas(const std::string &canvas);
/* True while anything live or recording draws from this canvas. */
bool sv_output_canvas_in_use(const std::string &canvas);

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
SvClip sv_output_last_clip();

/* Measures each live stream's speed. The panel calls it once a second while anything runs.
   Reads counters on this PC only, nothing goes over the network. */
void sv_output_sample();

SvNotice sv_output_notice();
void sv_output_set_notice(const std::string &text, bool problem);

SvOutSettings sv_output_settings();
void sv_output_set_settings(const SvOutSettings &settings);
/* OBS's own frame rate as a whole number (59.94 is 60). */
int sv_output_obs_fps();
/* How far OBS's frame rate is divided to give `fps`: 1 when it is OBS's rate or above it. */
int sv_output_fps_div(int fps);
std::vector<SvEncoderChoice> sv_output_encoders();
/* The encoder Automatic picks on this PC. */
std::string sv_output_auto_encoder();

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
