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

#include "sv-output.h"

#include "sv-canvas.h"
#include "sv-link.h"
#include "sv-store.h"
#include "sv-sound.h"
#include "sv-replay.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>

#include <util/config-file.h>
#include <util/dstr.h>

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define T(s) obs_module_text(s)

static void stop_more_records(void *);

namespace {

/* Hardware first: it costs the game nothing. The processor is the last resort. */
const char *const ENCODER_ORDER[] = {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
const size_t ENCODER_COUNT = sizeof(ENCODER_ORDER) / sizeof(ENCODER_ORDER[0]);

/* Each maker's HEVC and AV1 encoder, beside its H.264 one above. x264 has none: making HEVC
   or AV1 on the processor would cost the game far too much. */
struct CodecSet {
	const char *h264;
	const char *hevc;
	const char *av1;
};
const CodecSet CODEC_SETS[] = {{"obs_nvenc_h264_tex", "obs_nvenc_hevc_tex", "obs_nvenc_av1_tex"},
			       {"h264_texture_amf", "h265_texture_amf", "av1_texture_amf"},
			       {"obs_qsv11_v2", "obs_qsv11_hevc", "obs_qsv11_av1"}};

/* The encoder for `codec` from the same maker as `h264`, or empty when this PC has none. */
std::string codec_variant(const std::string &h264, int codec)
{
	if (codec == SV_CODEC_H264)
		return h264;
	for (const CodecSet &c : CODEC_SETS) {
		if (h264 != c.h264)
			continue;
		const char *id = codec == SV_CODEC_HEVC ? c.hevc : codec == SV_CODEC_AV1 ? c.av1 : nullptr;
		return id && obs_get_encoder_codec(id) ? std::string(id) : std::string();
	}
	return std::string();
}

const char STREAMS_FILE[] = "streams.dat";
const char ENTROPY[] = "simply-vertical-streams-v1";

enum class Kind { Stream, Record, Backtrack, VCam };

Kind kind_of(const std::string &id)
{
	if (id == SV_RECORD_ID || id.rfind(SV_RECORD_MORE, 0) == 0)
		return Kind::Record;
	if (id == SV_BACKTRACK_ID)
		return Kind::Backtrack;
	if (id == SV_VCAM_ID)
		return Kind::VCam;
	return Kind::Stream;
}

struct Run {
	std::string id;
	std::string label;
	Kind kind = Kind::Stream;
	obs_output_t *out = nullptr;
	obs_service_t *svc = nullptr;
	SvRunState state = SvRunState::Off;
	std::string problem;
	uint64_t started_ns = 0;
	/* The recording only: paused now, since when, and how long all earlier pauses took. */
	bool paused = false;
	uint64_t paused_since_ns = 0;
	uint64_t paused_total_ns = 0;
	/* Health, measured once a second. */
	uint64_t last_bytes = 0;
	uint64_t last_sample_ns = 0;
	std::deque<float> kbps;
	std::deque<std::pair<uint64_t, uint64_t>> frames; /* dropped, total */
	int low_count = 0;
	uint64_t reconnect_until_ns = 0;
	int target_kbps = 0; /* the bitrate this stream was started with */
	std::string canvas;  /* the canvas it draws from, "" for the main one */
	/* Twitch Dual Format: OBS's own stream carries it, so out stays empty and the state
	   follows OBS's stream events. */
	bool obs_stream = false;
};

void reset_health(Run &run)
{
	run.last_bytes = 0;
	run.last_sample_ns = 0;
	run.kbps.clear();
	run.frames.clear();
	run.low_count = 0;
	run.reconnect_until_ns = 0;
}

/* Never held across obs_output_start or stop: those can signal straight back. */
std::mutex g_mtx;
std::vector<SvDest> g_dests;
std::map<std::string, Run> g_runs;
obs_encoder_t *g_video = nullptr;
obs_encoder_t *g_audio = nullptr;
/* The recording's own picture encoder. Always made on a graphics card, so pausing the
   recording never freezes a live stream or Backtrack. On the processor (x264) only when the
   recording has its own quality, since a second processor encode can make a stream drop frames;
   then pause is refused while something else shares it. */
obs_encoder_t *g_rec_video = nullptr;
/* The recording's own sound, so pausing it never pauses a stream's sound. Sound encoders cost
   next to nothing, so they are always made. */
obs_encoder_t *g_rec_audio = nullptr;
obs_encoder_t *g_rec_track_audio[6] = {};
/* Each recording besides the first has sound encoders of its own, by run id: the main track first,
   then the extra tracks in order. OBS cannot pause one encoder for two outputs, so this is what
   lets every recording pause together. */
std::map<std::string, std::vector<obs_encoder_t *>> g_more_rec_audio;
/* Only when clips have a quality of their own, different from both of the above. */
obs_encoder_t *g_clip_video = nullptr;
/* Platforms with their own picture, one encoder per different picture. Two platforms
   with the same bitrate, size and frame rate share one. */
std::map<std::string, obs_encoder_t *> g_dest_video;
/* The encoder picked for the shared picture, so a platform's own picture uses the same. */
std::string g_chosen;
/* The one recordings and clips use: the same as g_chosen, or that maker's HEVC or AV1 one. */
std::string g_chosen_rec;
/* More sound tracks for recordings and clips, by OBS track number minus one. */
obs_encoder_t *g_track_audio[6] = {};
/* Save clip with "keep going a few seconds": the save waits here, and shutting down cancels it. */
std::thread g_after_thread;
std::condition_variable g_after_cv;
bool g_after_cancel = false;
size_t g_skip_encoders = 0;
/* The card failed to make HEVC or AV1, so recordings use H.264 until the settings change. */
bool g_codec_failed = false;
std::function<void()> g_listener;
SvNotice g_notice;
SvClip g_clip;
uint64_t g_last_save_ns = 0;
/* The recording running now was started by "Record my vertical whenever I go
   live", so stopping the stream stops it too. A recording the member started stays theirs. */
bool g_auto_record = false;
bool g_auto_backtrack = false;
/* OBS's own stream, from its starting to its stopped event, and the canvas of ours it carries
   as Enhanced Broadcasting's Additional Canvas. That canvas must keep drawing while it runs,
   however the stream was started. */
bool g_obs_streaming = false;
std::string g_obs_extra_canvas;
/* Twitch Dual Format was set up while OBS's outputs were built without Enhanced
   Broadcasting, so OBS needs one restart before it can go live with it. */
bool g_dual_restart = false;

void notify()
{
	std::function<void()> l;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		l = g_listener;
	}
	if (l)
		l();
}

bool any_running_locked()
{
	return std::any_of(g_runs.begin(), g_runs.end(),
			   [](const auto &kv) { return kv.second.state != SvRunState::Off; });
}

bool any_stream_running_locked()
{
	return std::any_of(g_runs.begin(), g_runs.end(), [](const auto &kv) {
		return kv.second.kind == Kind::Stream && kv.second.state != SvRunState::Off;
	});
}

/* ---------- keep every stream ---------- */

/* On OBS's main thread, once a stream is live. */
void auto_record_start(void *)
{
	if (!sv_output_settings().record_with_live)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (!any_stream_running_locked())
			return;
		auto r = g_runs.find(SV_RECORD_ID);
		if (r != g_runs.end() && r->second.state != SvRunState::Off)
			return;
	}
	std::string problem;
	if (sv_output_start(SV_RECORD_ID, problem)) {
		std::lock_guard<std::mutex> lock(g_mtx);
		g_auto_record = true;
	} else {
		std::string text = T("OutAutoRecordFailed");
		if (!problem.empty())
			text += " " + problem;
		sv_output_set_notice(text, true);
	}
}

/* On OBS's main thread, once a stream is live: Backtrack set to start by itself. */
void auto_backtrack_start(void *)
{
	if (sv_output_settings().backtrack_auto != SV_BACKTRACK_AUTO_LIVE)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (!any_stream_running_locked())
			return;
		auto r = g_runs.find(SV_BACKTRACK_ID);
		if (r != g_runs.end() && r->second.state != SvRunState::Off)
			return;
	}
	std::string problem;
	if (sv_output_start(SV_BACKTRACK_ID, problem)) {
		std::lock_guard<std::mutex> lock(g_mtx);
		g_auto_backtrack = true;
	} else if (!problem.empty()) {
		sv_output_set_notice(problem, true);
	}
}

/* On OBS's main thread, once the member has stopped a stream. */
void auto_record_stop(void *)
{
	bool record = false, backtrack = false;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (any_stream_running_locked())
			return;
		record = g_auto_record;
		backtrack = g_auto_backtrack;
		g_auto_record = false;
		g_auto_backtrack = false;
	}
	if (record)
		sv_output_stop(SV_RECORD_ID);
	if (backtrack)
		sv_output_stop(SV_BACKTRACK_ID);
}

/* The virtual camera takes the picture as it is, so it never holds the encoders. */
bool encoders_in_use_locked()
{
	return std::any_of(g_runs.begin(), g_runs.end(), [](const auto &kv) {
		return kv.second.state != SvRunState::Off && kv.second.kind != Kind::VCam && !kv.second.obs_stream;
	});
}

/* Tells the canvases which of them are live or recording, so only those draw for
   outputs. Never call holding g_mtx. */
std::mutex g_users_mtx;
void update_canvas_users()
{
	std::lock_guard<std::mutex> users(g_users_mtx);
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (const auto &kv : g_runs) {
			if (kv.second.state != SvRunState::Off)
				ids.push_back(kv.second.canvas);
		}
		if (g_obs_streaming && !g_obs_extra_canvas.empty())
			ids.push_back(g_obs_extra_canvas);
	}
	sv_canvas_set_output_canvases(ids);
}

/* ---------- the saved destinations, locked by Windows ---------- */

bool protect(const std::string &plain, std::string &blob)
{
	DATA_BLOB in{(DWORD)plain.size(), (BYTE *)plain.data()};
	DATA_BLOB ent{(DWORD)(sizeof(ENTROPY) - 1), (BYTE *)ENTROPY};
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"Simply Vertical", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return false;
	blob.assign((const char *)out.pbData, out.cbData);
	LocalFree(out.pbData);
	return true;
}

bool unprotect(const std::string &blob, std::string &plain)
{
	DATA_BLOB in{(DWORD)blob.size(), (BYTE *)blob.data()};
	DATA_BLOB ent{(DWORD)(sizeof(ENTROPY) - 1), (BYTE *)ENTROPY};
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return false;
	plain.assign((const char *)out.pbData, out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return true;
}

std::string to_hex(const std::string &bytes)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(bytes.size() * 2);
	for (unsigned char c : bytes) {
		s.push_back(digits[c >> 4]);
		s.push_back(digits[c & 15]);
	}
	return s;
}

int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

bool from_hex(const std::string &hex, std::string &bytes)
{
	if (hex.size() % 2)
		return false;
	bytes.clear();
	bytes.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = hex_value(hex[i]);
		const int lo = hex_value(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		bytes.push_back((char)(hi * 16 + lo));
	}
	return true;
}

void wipe(std::string &s)
{
	if (!s.empty())
		SecureZeroMemory(s.data(), s.size());
	s.clear();
}

std::string streams_path()
{
	char *p = obs_module_config_path(STREAMS_FILE);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save_locked()
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const SvDest &d : g_dests) {
		obs_data_t *one = obs_data_create();
		obs_data_set_string(one, "id", d.id.c_str());
		obs_data_set_string(one, "platform", d.platform.c_str());
		obs_data_set_string(one, "name", d.name.c_str());
		obs_data_set_string(one, "server", d.server.c_str());
		obs_data_set_string(one, "key", d.key.c_str());
		obs_data_set_bool(one, "own_video", d.own_video);
		obs_data_set_int(one, "bitrate", d.bitrate);
		obs_data_set_int(one, "size", d.size);
		obs_data_set_int(one, "fps", d.fps);
		obs_data_set_string(one, "canvas", d.canvas.c_str());
		obs_data_set_bool(one, "off", !d.on);
		obs_data_set_bool(one, "dual", d.dual);
		obs_data_array_push_back(arr, one);
		obs_data_release(one);
	}
	obs_data_set_array(root, "dests", arr);
	obs_data_array_release(arr);

	std::string plain = obs_data_get_json(root);
	obs_data_release(root);

	std::string blob;
	const bool locked = protect(plain, blob);
	wipe(plain);
	if (!locked) {
		obs_log(LOG_WARNING, "could not lock the saved destinations, nothing was written");
		return;
	}

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string hex = to_hex(blob);
	const std::string path = streams_path();
	if (path.empty() || !os_quick_write_utf8_file_safe(path.c_str(), hex.c_str(), hex.size(), false, "tmp", nullptr))
		obs_log(LOG_WARNING, "could not save the destinations");
}

void load_locked()
{
	g_dests.clear();
	const std::string path = streams_path();
	char *text = path.empty() ? nullptr : os_quick_read_utf8_file(path.c_str());
	if (!text)
		return;
	const std::string hex = text;
	bfree(text);

	std::string blob, plain;
	if (!from_hex(hex, blob) || !unprotect(blob, plain)) {
		obs_log(LOG_WARNING, "the saved destinations could not be unlocked on this Windows account");
		return;
	}
	obs_data_t *root = obs_data_create_from_json(plain.c_str());
	wipe(plain);
	if (!root)
		return;
	obs_data_array_t *arr = obs_data_get_array(root, "dests");
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *one = obs_data_array_item(arr, i);
		SvDest d;
		d.id = obs_data_get_string(one, "id");
		d.platform = obs_data_get_string(one, "platform");
		d.name = obs_data_get_string(one, "name");
		d.server = obs_data_get_string(one, "server");
		d.key = obs_data_get_string(one, "key");
		d.has_key = !d.key.empty();
		d.own_video = obs_data_get_bool(one, "own_video");
		if (obs_data_has_user_value(one, "bitrate"))
			d.bitrate = std::clamp((int)obs_data_get_int(one, "bitrate"), 500, 50000);
		d.size = std::clamp((int)obs_data_get_int(one, "size"), 0, 2);
		d.fps = std::clamp((int)obs_data_get_int(one, "fps"), 0, 240);
		d.canvas = obs_data_get_string(one, "canvas");
		d.on = !obs_data_get_bool(one, "off");
		d.dual = obs_data_get_bool(one, "dual") && d.platform == "twitch";
		if (!d.id.empty())
			g_dests.push_back(d);
		obs_data_release(one);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
}

/* ---------- saying what went wrong ---------- */

std::string problem_for(const Run &run, long long code, const char *detail)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return "";
	case OBS_OUTPUT_BAD_PATH:
		return T(run.kind == Kind::Stream ? "OutBadAddress" : "OutBadFolder");
	case OBS_OUTPUT_CONNECT_FAILED:
		return T("OutConnectFailed");
	case OBS_OUTPUT_INVALID_STREAM:
		return T("OutBadKey");
	case OBS_OUTPUT_DISCONNECTED:
		return T("OutDisconnected");
	case OBS_OUTPUT_NO_SPACE:
		return T("OutNoSpace");
	case OBS_OUTPUT_ENCODE_ERROR:
		return T("OutEncoderFailed");
	default:
		break;
	}
	if (detail && *detail)
		return std::string(T("OutFailedDetail")) + " " + detail;
	return T("OutFailed");
}

/* ---------- signals from OBS, on its own threads ---------- */

void on_started(void *data, calldata_t *)
{
	bool stream;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run *run = static_cast<Run *>(data);
		run->state = SvRunState::Live;
		run->problem.clear();
		run->started_ns = os_gettime_ns();
		run->paused = false;
		run->paused_since_ns = 0;
		run->paused_total_ns = 0;
		reset_health(*run);
		stream = run->kind == Kind::Stream;
	}
	notify();
	/* Never from this thread: starting an output here could signal straight back. */
	if (stream) {
		obs_queue_task(OBS_TASK_UI, auto_record_start, nullptr, false);
		obs_queue_task(OBS_TASK_UI, auto_backtrack_start, nullptr, false);
	}
}

void on_stopped(void *data, calldata_t *cd)
{
	const long long code = calldata_int(cd, "code");
	bool idle, streamStopped, firstRecordEnded = false;
	std::string moreProblem;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run *run = static_cast<Run *>(data);
		const char *detail = run->out ? obs_output_get_last_error(run->out) : nullptr;
		/* Only a stream the member stopped. One that dropped keeps its recording going. */
		streamStopped = run->kind == Kind::Stream && run->state == SvRunState::Stopping;
		/* The first recording ended by itself (a full disk, say): the others end with it. */
		firstRecordEnded = run->id == SV_RECORD_ID && run->state != SvRunState::Stopping;
		/* Another recording that ended by itself has no line of its own in the panel. */
		if (run->id.rfind(SV_RECORD_MORE, 0) == 0 && run->state != SvRunState::Stopping &&
		    code != OBS_OUTPUT_SUCCESS)
			moreProblem = problem_for(*run, code, detail);
		run->state = SvRunState::Off;
		run->paused = false;
		run->problem = problem_for(*run, code, detail);
		/* HEVC or AV1 that the card turned down goes back to H.264 before trying another card. */
		if (code == OBS_OUTPUT_ENCODE_ERROR && g_chosen_rec != g_chosen)
			g_codec_failed = true;
		else if (code == OBS_OUTPUT_ENCODE_ERROR && sv_output_settings().encoder.empty())
			g_skip_encoders++;
		idle = !any_running_locked();
	}
	(void)idle;
	update_canvas_users();
	notify();
	if (streamStopped)
		obs_queue_task(OBS_TASK_UI, auto_record_stop, nullptr, false);
	if (firstRecordEnded)
		obs_queue_task(OBS_TASK_UI, stop_more_records, nullptr, false);
	if (!moreProblem.empty())
		sv_output_set_notice(moreProblem, true);
}

void on_reconnecting(void *data, calldata_t *cd)
{
	const long long wait = calldata_int(cd, "timeout_sec");
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run *run = static_cast<Run *>(data);
		run->state = SvRunState::Reconnecting;
		run->reconnect_until_ns = os_gettime_ns() + (uint64_t)(std::max)(0LL, wait) * 1000000000ULL;
	}
	notify();
}

void on_reconnected(void *data, calldata_t *)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run *run = static_cast<Run *>(data);
		run->state = SvRunState::Live;
		run->reconnect_until_ns = 0;
		/* The byte count carries on, but the gap would read as a burst. */
		run->last_sample_ns = 0;
	}
	notify();
}

/* The length of an MP4 file in seconds, from its movie header, or -1 when it cannot be read. */
double mp4_seconds(const std::string &path)
{
	FILE *f = os_fopen(path.c_str(), "rb");
	if (!f)
		return -1.0;
	/* The header sits at the end of a clip, or at the start of a file made for the web. */
	auto find_in = [f](int64_t from, size_t len) -> double {
		std::vector<unsigned char> buf(len);
		if (os_fseeki64(f, from, SEEK_SET) != 0)
			return -1.0;
		const size_t got = fread(buf.data(), 1, len, f);
		auto be32 = [&](size_t i) {
			return ((uint64_t)buf[i] << 24) | ((uint64_t)buf[i + 1] << 16) | ((uint64_t)buf[i + 2] << 8) |
			       (uint64_t)buf[i + 3];
		};
		for (size_t i = got >= 4 ? got - 4 : 0; i-- > 0;) {
			if (memcmp(&buf[i], "mvhd", 4) != 0)
				continue;
			const size_t v = i + 4;
			if (v + 32 > got)
				continue;
			uint64_t scale, dur;
			if (buf[v] == 1) {
				scale = be32(v + 20);
				dur = (be32(v + 24) << 32) | be32(v + 28);
			} else {
				scale = be32(v + 12);
				dur = be32(v + 16);
			}
			return scale ? (double)dur / (double)scale : -1.0;
		}
		return -1.0;
	};
	os_fseeki64(f, 0, SEEK_END);
	const int64_t size = os_ftelli64(f);
	const size_t window = 8 * 1024 * 1024;
	double secs = -1.0;
	if (size > 0) {
		const int64_t tailFrom = size > (int64_t)window ? size - (int64_t)window : 0;
		secs = find_in(tailFrom, (size_t)(size - tailFrom));
		if (secs < 0 && tailFrom > 0)
			secs = find_in(0, window);
	}
	fclose(f);
	return secs;
}

/* Backtrack finished writing a clip. */
void on_backtrack_saved(void *data, calldata_t *)
{
	obs_output_t *out;
	uint64_t started;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run *run = static_cast<Run *>(data);
		out = run->out ? obs_output_get_ref(run->out) : nullptr;
		started = run->started_ns;
	}
	if (!out)
		return;
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(out), "get_last_replay", &cd);
	const char *path = calldata_string(&cd, "path");
	std::string file = path ? path : "";
	calldata_free(&cd);
	obs_output_release(out);
	if (file.empty())
		return;
	/* What is really in the clip. If the file cannot be read: Backtrack can only hold what it has
	   seen since it was turned on, up to its length. */
	int length;
	const SvOutSettings set = sv_output_settings();
	const double inFile = mp4_seconds(file);
	if (inFile > 0) {
		length = (int)(inFile + 0.5);
	} else {
		const uint64_t held = started ? (os_gettime_ns() - started) / 1000000000ULL : 0;
		length = (int)(std::min<uint64_t>)((uint64_t)(set.backtrack_seconds + set.clip_after), held);
	}
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_clip.path = file;
		g_clip.seconds = (std::max)(1, length);
		/* The seconds after are locked while Backtrack runs, so this is what this clip used. */
		g_clip.after = std::clamp(set.clip_after, 0, g_clip.seconds);
		g_clip.seq++;
	}
	notify();
	/* Save and replay, or Play every clip as soon as it is saved. */
	sv_replay_clip_saved();
}

void attach(Run &run)
{
	signal_handler_t *sh = obs_output_get_signal_handler(run.out);
	signal_handler_connect(sh, "start", on_started, &run);
	signal_handler_connect(sh, "stop", on_stopped, &run);
	signal_handler_connect(sh, "reconnect", on_reconnecting, &run);
	signal_handler_connect(sh, "reconnect_success", on_reconnected, &run);
	if (run.kind == Kind::Backtrack)
		signal_handler_connect(sh, "saved", on_backtrack_saved, &run);
}

/* Lets go of a finished run's output. Must not be holding g_mtx. */
void release_parts(obs_output_t *out, obs_service_t *svc, Run *run)
{
	if (out) {
		signal_handler_t *sh = obs_output_get_signal_handler(out);
		signal_handler_disconnect(sh, "start", on_started, run);
		signal_handler_disconnect(sh, "stop", on_stopped, run);
		signal_handler_disconnect(sh, "reconnect", on_reconnecting, run);
		signal_handler_disconnect(sh, "reconnect_success", on_reconnected, run);
		signal_handler_disconnect(sh, "saved", on_backtrack_saved, run);
		obs_output_release(out);
	}
	obs_service_release(svc);
}

/* ---------- the shared encoders ---------- */

/* How a picture encoder spends its bits: a steady bitrate (CBR) for streams, a bitrate that
   rises in busy moments (VBR), or a steady look (a quality level, lower is sharper). */
struct Spec {
	int level = 0; /* 0: a bitrate */
	int bitrate = 6000;
	bool vbr = false;
	bool operator==(const Spec &o) const
	{
		return level == o.level && (level > 0 || (bitrate == o.bitrate && vbr == o.vbr));
	}
	bool operator!=(const Spec &o) const { return !(*this == o); }
};

Spec stream_spec(const SvOutSettings &set)
{
	return Spec{0, set.bitrate};
}

Spec rec_spec(const SvOutSettings &set)
{
	switch (set.rec_quality) {
	case SV_Q_LEVEL:
		return Spec{std::clamp(set.rec_level, 1, 51), 0};
	case SV_Q_BITRATE:
		return Spec{0, set.record_bitrate, false};
	case SV_Q_VBR:
		return Spec{0, set.record_bitrate, true};
	default:
		return stream_spec(set);
	}
}

Spec clip_spec(const SvOutSettings &set)
{
	switch (set.clip_quality) {
	case SV_CLIP_AS_STREAM:
		return stream_spec(set);
	case SV_CLIP_LEVEL:
		return Spec{std::clamp(set.clip_level, 1, 51), 0};
	case SV_CLIP_BITRATE:
		return Spec{0, set.clip_bitrate, false};
	case SV_CLIP_VBR:
		return Spec{0, set.clip_bitrate, true};
	default:
		return rec_spec(set);
	}
}

/* The older five steps, in each encoder's own words, from lightest to best picture. Only
   read for settings saved before the vertical used the encoder's presets by name. */
const char *const X264_EFFORT[] = {"ultrafast", "superfast", "veryfast", "faster", "fast"};
const char *const NVENC_EFFORT[] = {"p2", "p3", "p5", "p6", "p7"};
const char *const AMF_EFFORT[] = {"speed", "speed", "balanced", "quality", "quality"};
const char *const QSV_EFFORT[] = {"TU7", "TU6", "TU4", "TU2", "TU1"};

/* Every encoder's own presets, lightest first, with the words shown for each. */
struct PresetDef {
	const char *id;
	const char *words;
};
const PresetDef NVENC_PRESETS[] = {{"p1", "PresetP1"}, {"p2", "PresetP2"}, {"p3", "PresetP3"}, {"p4", "PresetP4"},
				   {"p5", "PresetP5"}, {"p6", "PresetP6"}, {"p7", "PresetP7"}};
const PresetDef X264_PRESETS[] = {{"ultrafast", "PresetUltrafast"}, {"superfast", "PresetSuperfast"},
				  {"veryfast", "PresetVeryfast"},   {"faster", "PresetFaster"},
				  {"fast", "PresetFast"},           {"medium", "PresetMedium"},
				  {"slow", "PresetSlow"},           {"slower", "PresetSlower"}};
const PresetDef AMF_PRESETS[] = {{"speed", "PresetSpeed"}, {"balanced", "PresetBalanced"}, {"quality", "PresetQuality"}};
const PresetDef QSV_PRESETS[] = {{"TU7", "PresetTU7"}, {"TU6", "PresetTU6"}, {"TU5", "PresetTU5"}, {"TU4", "PresetTU4"},
				 {"TU3", "PresetTU3"}, {"TU2", "PresetTU2"}, {"TU1", "PresetTU1"}};

enum class Family { None, Nvenc, X264, Amf, Qsv };

Family family_of(const std::string &id)
{
	if (id == "obs_x264")
		return Family::X264;
	if (id.rfind("obs_nvenc", 0) == 0)
		return Family::Nvenc;
	if (id.find("_amf") != std::string::npos)
		return Family::Amf;
	if (id.rfind("obs_qsv", 0) == 0)
		return Family::Qsv;
	return Family::None;
}

obs_data_t *video_settings(const char *id, const Spec &spec, const std::string &preset, int keyframe_sec)
{
	const bool x264 = strcmp(id, "obs_x264") == 0;
	obs_data_t *st = obs_data_create();
	if (spec.level > 0) {
		obs_data_set_string(st, "rate_control", x264 ? "CRF" : "CQP");
		obs_data_set_int(st, "crf", spec.level);
		obs_data_set_int(st, "cqp", spec.level);
		obs_data_set_int(st, "qpi", spec.level);
		obs_data_set_int(st, "qpp", spec.level);
		obs_data_set_int(st, "qpb", spec.level);
	} else if (spec.vbr) {
		/* Aims for the typed kbps; busy moments may go half as high again. */
		const int ceiling = spec.bitrate * 3 / 2;
		obs_data_set_int(st, "bitrate", spec.bitrate);
		obs_data_set_int(st, "max_bitrate", ceiling);
		if (x264) {
			/* x264's own VBR aims for a look instead of the kbps, so it uses its average
			   bitrate mode, with the ceiling set through its options. */
			obs_data_set_string(st, "rate_control", "ABR");
			const std::string opts = "vbv-maxrate=" + std::to_string(ceiling) +
						 " vbv-bufsize=" + std::to_string(ceiling);
			obs_data_set_string(st, "x264opts", opts.c_str());
		} else {
			obs_data_set_string(st, "rate_control", "VBR");
		}
	} else {
		obs_data_set_string(st, "rate_control", "CBR");
		obs_data_set_int(st, "bitrate", spec.bitrate);
	}
	obs_data_set_int(st, "keyint_sec", std::clamp(keyframe_sec, 1, 10));
	/* "high" is an H.264 word. HEVC and AV1 encoders all call their usual one "main". */
	const char *codec = obs_get_encoder_codec(id);
	obs_data_set_string(st, "profile", codec && strcmp(codec, "h264") != 0 ? "main" : "high");
	if (!preset.empty())
		obs_data_set_string(st, family_of(id) == Family::Qsv ? "target_usage" : "preset", preset.c_str());
	return st;
}

/* One picture encoder with the member's size, frame rate and preset. wideHeight is for OBS's
   own wide picture: 0 keeps OBS's size, otherwise the picture is scaled down to that many
   lines tall, never up. */
obs_encoder_t *make_video(const char *id, const char *name, const Spec &spec, const SvOutSettings &set, video_t *video,
			  bool wide = false, int wideHeight = 0)
{
	obs_data_t *vs = video_settings(id, spec, sv_output_preset_for(id, set), set.keyframe_sec);
	obs_encoder_t *enc = obs_video_encoder_create(id, name, vs, nullptr);
	obs_data_release(vs);
	if (!enc)
		return nullptr;
	obs_encoder_set_video(enc, video);
	if (wide) {
		const uint32_t w = video_output_get_width(video), h = video_output_get_height(video);
		if (wideHeight > 0 && h > (uint32_t)wideHeight && w > 0) {
			/* Keeps OBS's own shape, with an even width the encoders accept. */
			const uint32_t sw = ((uint32_t)((uint64_t)w * (uint32_t)wideHeight / h) + 1) & ~1u;
			obs_encoder_set_scaled_size(enc, sw, (uint32_t)wideHeight);
			obs_encoder_set_gpu_scale_type(enc, set.sharp_scale ? OBS_SCALE_LANCZOS : OBS_SCALE_BICUBIC);
		}
	} else if (set.size != SV_SIZE_1080) {
		/* The canvas stays 1080 x 1920, the graphics card scales it on the way out. */
		if (set.size == SV_SIZE_900)
			obs_encoder_set_scaled_size(enc, 900, 1600);
		else
			obs_encoder_set_scaled_size(enc, 720, 1280);
		obs_encoder_set_gpu_scale_type(enc, set.sharp_scale ? OBS_SCALE_LANCZOS : OBS_SCALE_BICUBIC);
	}
	if (set.fps_div > 1)
		obs_encoder_set_frame_rate_divisor(enc, (uint32_t)set.fps_div);
	return enc;
}

void release_encoders()
{
	obs_encoder_release(g_video);
	obs_encoder_release(g_audio);
	obs_encoder_release(g_rec_video);
	obs_encoder_release(g_clip_video);
	g_video = nullptr;
	g_audio = nullptr;
	g_rec_video = nullptr;
	g_clip_video = nullptr;
	for (auto &kv : g_dest_video)
		obs_encoder_release(kv.second);
	g_dest_video.clear();
	for (obs_encoder_t *&a : g_track_audio) {
		obs_encoder_release(a);
		a = nullptr;
	}
	obs_encoder_release(g_rec_audio);
	g_rec_audio = nullptr;
	for (obs_encoder_t *&a : g_rec_track_audio) {
		obs_encoder_release(a);
		a = nullptr;
	}
	for (auto &kv : g_more_rec_audio)
		for (obs_encoder_t *a : kv.second)
			obs_encoder_release(a);
	g_more_rec_audio.clear();
}

/* The sound encoders of a recording besides the first, made the first time it is needed with the
   same tracks as the first recording. Holding g_mtx. Empty when one could not be made. */
const std::vector<obs_encoder_t *> &more_rec_audio(const std::string &id, const SvOutSettings &set)
{
	static const std::vector<obs_encoder_t *> none;
	auto it = g_more_rec_audio.find(id);
	if (it != g_more_rec_audio.end())
		return it->second;
	const int track = sv_sound_track(set.audio_track);
	std::vector<int> tracks{track};
	for (int t = 1; t <= 6; t++)
		if (t != track && (set.rec_tracks & (1u << (t - 1))))
			tracks.push_back(t);
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", set.audio_bitrate);
	std::vector<obs_encoder_t *> made;
	bool ok = true;
	for (int t : tracks) {
		const std::string name = "simply-vertical-" + id + "-audio-" + std::to_string(t);
		obs_encoder_t *a = obs_audio_encoder_create("ffmpeg_aac", name.c_str(), as, (size_t)(t - 1), nullptr);
		if (!a) {
			ok = false;
			break;
		}
		obs_encoder_set_audio(a, obs_get_audio());
		made.push_back(a);
	}
	obs_data_release(as);
	if (!ok) {
		for (obs_encoder_t *a : made)
			obs_encoder_release(a);
		return none;
	}
	return g_more_rec_audio[id] = made;
}

/* The picture encoder each kind of output uses. */
obs_encoder_t *video_for(Kind kind, const SvOutSettings &set)
{
	if (kind == Kind::Record)
		return g_rec_video ? g_rec_video : g_video;
	if (kind == Kind::Backtrack) {
		const Spec c = clip_spec(set);
		/* With HEVC or AV1 picked, clips never borrow the stream's H.264 picture. */
		if (c == stream_spec(set) && g_chosen_rec == g_chosen)
			return g_video;
		/* Borrows the recording's picture only on the processor, where no clip one was made. */
		if (c == rec_spec(set) && !g_clip_video)
			return g_rec_video ? g_rec_video : g_video;
		return g_clip_video ? g_clip_video : g_video;
	}
	return g_video;
}

/* The picture encoder a recording or Backtrack uses: the usual one on the main canvas, or one
   of its own with the same quality on another canvas. Holding g_mtx. Null only when that one
   could not start. */
obs_encoder_t *video_for_canvas(Kind kind, const SvOutSettings &set, video_t *video, const std::string &canvas)
{
	if (canvas.empty())
		return video_for(kind, set);
	const Spec spec = kind == Kind::Record ? rec_spec(set) : clip_spec(set);
	if (canvas == SV_WIDE) {
		/* OBS's own horizontal picture at OBS's own size and frame rate, with the recording's
		   quality. Never shared with a platform on OBS horizontal, so it can pause on its own. */
		SvOutSettings own = set;
		own.fps_div = 1;
		const std::string key = std::string(SV_WIDE) + "-rec-" + std::to_string(spec.level) + "-" +
					std::to_string(spec.bitrate) + (spec.vbr ? "v" : "c");
		auto w = g_dest_video.find(key);
		if (w != g_dest_video.end())
			return w->second;
		if (g_chosen_rec.empty())
			return nullptr;
		const std::string name = "simply-vertical-video-" + key;
		obs_encoder_t *enc = make_video(g_chosen_rec.c_str(), name.c_str(), spec, own, video, true, 0);
		if (!enc)
			return nullptr;
		obs_log(LOG_INFO, "horizontal picture for a recording: %s, level %d / %d kbps", g_chosen_rec.c_str(),
			spec.level, spec.bitrate);
		g_dest_video[key] = enc;
		return enc;
	}
	const std::string key = canvas + (kind == Kind::Record ? "-rec-" : "-clip-") + std::to_string(spec.level) +
				"-" + std::to_string(spec.bitrate) + (spec.vbr ? "v" : "c") + "-" +
				std::to_string(set.size) + "-" + std::to_string(set.fps_div);
	auto it = g_dest_video.find(key);
	if (it != g_dest_video.end())
		return it->second;
	if (g_chosen_rec.empty())
		return nullptr;
	const std::string name = "simply-vertical-video-" + key;
	obs_encoder_t *enc = make_video(g_chosen_rec.c_str(), name.c_str(), spec, set, video);
	if (!enc)
		return nullptr;
	obs_log(LOG_INFO, "own picture for %s on another canvas", kind == Kind::Record ? "recording" : "Backtrack");
	g_dest_video[key] = enc;
	return enc;
}

/* The usual settings with a platform's own bitrate, size and frame rate on top. */
SvOutSettings dest_settings(const SvDest &d, const SvOutSettings &set)
{
	SvOutSettings s = set;
	if (!d.own_video)
		return s;
	s.bitrate = d.bitrate;
	s.size = d.size;
	if (d.fps > 0) {
		s.fps = d.fps;
		s.fps_div = sv_output_fps_div(d.fps);
	}
	return s;
}

/* The picture encoder a platform streams with: the shared one, unless it has a picture of
   its own that differs from it or shows another canvas. Holding g_mtx. Null only when its
   own one could not start. */
obs_encoder_t *video_for_dest(const SvDest &d, const SvOutSettings &set, video_t *video, const std::string &canvas,
			      int wideKbps = 0)
{
	SvOutSettings own = dest_settings(d, set);
	if (canvas == SV_WIDE) {
		/* OBS's wide picture: OBS's own size and frame rate and its stream's bitrate, unless
		   this platform has video settings of its own. */
		if (!d.own_video) {
			own.bitrate = wideKbps > 0 ? wideKbps : 6000;
			own.fps_div = 1;
		} else if (d.fps <= 0) {
			own.fps_div = 1; /* "Usual" on the wide picture is OBS's own frame rate */
		}
		const int lines[] = {1080, 900, 720};
		const int height = d.own_video ? lines[std::clamp(own.size, 0, 2)] : 0;
		const std::string key = std::string(SV_WIDE) + "-" + std::to_string(own.bitrate) + "-" +
					std::to_string(height) + "-" + std::to_string(own.fps_div);
		auto it = g_dest_video.find(key);
		if (it != g_dest_video.end())
			return it->second;
		if (g_chosen.empty())
			return nullptr;
		const std::string name = "simply-vertical-video-" + key;
		obs_encoder_t *enc =
			make_video(g_chosen.c_str(), name.c_str(), Spec{0, own.bitrate}, own, video, true, height);
		if (!enc)
			return nullptr;
		obs_log(LOG_INFO, "wide picture for a platform: %s at %d kbps, %d lines (0 is OBS's own), frame rate / %d",
			g_chosen.c_str(), own.bitrate, height, own.fps_div);
		g_dest_video[key] = enc;
		return enc;
	}
	/* YouTube takes HEVC and AV1 when the member asks for it. Every other platform only
	   takes H.264, so it always gets that. */
	const bool newer = set.codec_youtube && d.platform == "youtube" && !g_chosen_rec.empty() &&
			   g_chosen_rec != g_chosen;
	const std::string &id = newer ? g_chosen_rec : g_chosen;
	if (!newer && canvas.empty() && own.bitrate == set.bitrate && own.size == set.size &&
	    own.fps_div == set.fps_div)
		return g_video;
	std::string key = std::to_string(own.bitrate) + "-" + std::to_string(own.size) + "-" +
			  std::to_string(own.fps_div);
	if (newer)
		key = "codec" + std::to_string(set.rec_codec) + "-" + key;
	if (!canvas.empty())
		key = canvas + "-" + key;
	auto it = g_dest_video.find(key);
	if (it != g_dest_video.end())
		return it->second;
	if (id.empty())
		return nullptr;
	const std::string name = "simply-vertical-video-" + key;
	obs_encoder_t *enc = make_video(id.c_str(), name.c_str(), Spec{0, own.bitrate}, own, video);
	if (!enc)
		return nullptr;
	const char *sizes[] = {"1080x1920", "900x1600", "720x1280"};
	obs_log(LOG_INFO, "own picture for a platform: %s at %d kbps, %s, frame rate / %d", id.c_str(), own.bitrate,
		sizes[std::clamp(own.size, 0, 2)], own.fps_div);
	g_dest_video[key] = enc;
	return enc;
}

bool make_encoders(video_t *video, std::string &problem)
{
	const SvOutSettings set = sv_output_settings();
	release_encoders();

	const char *chosen = nullptr;
	if (!set.encoder.empty() && obs_get_encoder_codec(set.encoder.c_str())) {
		chosen = set.encoder.c_str();
	} else {
		size_t skip = g_skip_encoders;
		for (size_t i = 0; i < ENCODER_COUNT && !chosen; i++) {
			if (!obs_get_encoder_codec(ENCODER_ORDER[i]))
				continue;
			if (skip > 0 && i + 1 < ENCODER_COUNT) {
				skip--;
				continue;
			}
			chosen = ENCODER_ORDER[i];
		}
	}
	if (!chosen) {
		problem = T("OutNoEncoder");
		return false;
	}
	g_chosen = chosen;
	/* HEVC or AV1 for recordings and clips, from the same maker. A card that cannot make it
	   (or the processor) keeps H.264, so recording always works. */
	g_chosen_rec = g_codec_failed ? std::string() : codec_variant(g_chosen, set.rec_codec);
	if (g_chosen_rec.empty()) {
		obs_log(LOG_INFO, "%s cannot make the picked video format, recordings stay H.264", chosen);
		g_chosen_rec = g_chosen;
	}
	const bool newer = g_chosen_rec != g_chosen;

	const Spec ss = stream_spec(set), rs = rec_spec(set), cs = clip_spec(set);
	g_video = make_video(chosen, "simply-vertical-video", ss, set, video);
	/* A graphics card encoder can run a second copy at little cost, so the recording always gets
	   its own there and can be paused on its own. An encoder only works while something uses it,
	   so this costs nothing until the recording and a stream run together. */
	const bool card = family_of(g_chosen) != Family::X264;
	const bool ownRecording = rs != ss || newer || card;
	const bool ownClips = newer ? (cs != rs || card) : cs != ss && (cs != rs || card);
	if (g_video && ownRecording)
		g_rec_video = make_video(g_chosen_rec.c_str(), "simply-vertical-record-video", rs, set, video);
	if (g_video && ownClips)
		g_clip_video = make_video(g_chosen_rec.c_str(), "simply-vertical-clip-video", cs, set, video);

	/* With its own sound mix on, the vertical's sound is the desk's track. */
	const int track = sv_sound_track(set.audio_track);
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", set.audio_bitrate);
	g_audio = obs_audio_encoder_create("ffmpeg_aac", "simply-vertical-audio", as, (size_t)(track - 1), nullptr);
	g_rec_audio = obs_audio_encoder_create("ffmpeg_aac", "simply-vertical-record-audio", as, (size_t)(track - 1), nullptr);
	bool tracksOk = true;
	for (int t = 1; t <= 6; t++) {
		if (t == track || !(set.rec_tracks & (1u << (t - 1))))
			continue;
		const std::string name = "simply-vertical-audio-track-" + std::to_string(t);
		g_track_audio[t - 1] = obs_audio_encoder_create("ffmpeg_aac", name.c_str(), as, (size_t)(t - 1), nullptr);
		if (g_track_audio[t - 1])
			obs_encoder_set_audio(g_track_audio[t - 1], obs_get_audio());
		else
			tracksOk = false;
		const std::string recName = "simply-vertical-record-audio-track-" + std::to_string(t);
		g_rec_track_audio[t - 1] = obs_audio_encoder_create("ffmpeg_aac", recName.c_str(), as, (size_t)(t - 1), nullptr);
		if (g_rec_track_audio[t - 1])
			obs_encoder_set_audio(g_rec_track_audio[t - 1], obs_get_audio());
		else
			tracksOk = false;
	}
	obs_data_release(as);

	if (!g_video || !g_audio || !g_rec_audio || (ownRecording && !g_rec_video) || (ownClips && !g_clip_video) ||
	    !tracksOk) {
		release_encoders();
		problem = T("OutNoEncoder");
		return false;
	}

	obs_encoder_set_audio(g_audio, obs_get_audio());
	obs_encoder_set_audio(g_rec_audio, obs_get_audio());
	const char *sizes[] = {"1080x1920", "900x1600", "720x1280"};
	obs_log(LOG_INFO,
		"vertical encoder: %s at %d kbps (recording level %d / %d kbps, clips level %d / %d kbps), %s, frame rate / %d, "
		"preset %s, keyframe %d s, audio track %d at %d kbps, extra tracks 0x%x, scaling %s",
		chosen, set.bitrate, rs.level, rs.bitrate, cs.level, cs.bitrate, sizes[std::clamp(set.size, 0, 2)],
		set.fps_div, sv_output_preset_for(chosen, set).c_str(), set.keyframe_sec, track, set.audio_bitrate,
		set.rec_tracks,
		obs_encoder_get_scale_type(g_video) == OBS_SCALE_LANCZOS   ? "sharp (lanczos)"
		: obs_encoder_get_scale_type(g_video) == OBS_SCALE_BICUBIC ? "smooth (bicubic)"
									   : "none (full size)");
	return true;
}

std::string file_safe(const std::string &found);

/* The game on screen right now, from the first game capture that has a game hooked:
   its window title, or the program's name. Empty when there is none. Safe in a file name. */
std::string game_name()
{
	std::string found;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *out = static_cast<std::string *>(param);
			if (strcmp(obs_source_get_unversioned_id(src), "game_capture") != 0 || !obs_source_active(src))
				return true;
			calldata_t cd = {};
			if (proc_handler_call(obs_source_get_proc_handler(src), "get_hooked", &cd) &&
			    calldata_bool(&cd, "hooked")) {
				const char *title = calldata_string(&cd, "title");
				const char *exe = calldata_string(&cd, "executable");
				std::string name = title && *title ? title : (exe ? exe : "");
				if (!(title && *title) && name.size() > 4 &&
				    _stricmp(name.c_str() + name.size() - 4, ".exe") == 0)
					name.resize(name.size() - 4);
				*out = name;
			}
			calldata_free(&cd);
			return out->empty();
		},
		&found);
	return file_safe(found);
}

/* Leaves out what a file name cannot hold, at most 40 characters. */
std::string file_safe(const std::string &found)
{
	std::string clean;
	for (char ch : found) {
		const unsigned char u = (unsigned char)ch;
		if (u < 32 || strchr("\\/:*?\"<>|%", ch))
			continue;
		clean += ch;
	}
	while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
		clean.pop_back();
	while (!clean.empty() && clean.front() == ' ')
		clean.erase(clean.begin());
	if (clean.size() > 40)
		clean.resize(40);
	return clean;
}

/* "Vertical Valorant %CCYY-%MM-%DD %hh-%mm-%ss" */
std::string name_format(const char *start, bool withGame)
{
	std::string f = start;
	if (withGame) {
		const std::string g = game_name();
		if (!g.empty())
			f += " " + g;
	}
	return f + " %CCYY-%MM-%DD %hh-%mm-%ss";
}

bool output_type_exists(const char *wanted)
{
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_output_types(i, &id); i++) {
		if (strcmp(id, wanted) == 0)
			return true;
	}
	return false;
}

std::string record_folder(const SvOutSettings &set)
{
	if (!set.folder.empty())
		return set.folder;
	char *p = obs_frontend_get_current_record_output_path();
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

/* The picture a recording run records: the first ticked one for SV_RECORD_ID, otherwise the
   canvas id after SV_RECORD_MORE. "" is the main vertical. */
std::string record_canvas_of(const std::string &id, const SvOutSettings &set)
{
	if (id == SV_RECORD_ID)
		return sv_output_record_canvases(set).front();
	return id.substr(strlen(SV_RECORD_MORE));
}

/* How a recording's file name starts: "Vertical" for the main vertical, "Vertical Twitch" for a
   canvas named Twitch, "Horizontal" for OBS's own picture. */
std::string record_start_word(const std::string &canvas)
{
	if (canvas == SV_WIDE)
		return "Horizontal";
	if (canvas.empty() || canvas == sv_canvas_main_id())
		return "Vertical";
	for (const SvVCanvas &c : sv_canvases()) {
		if (!c.main && c.uuid == canvas) {
			const std::string n = file_safe(c.name);
			return n.empty() ? "Vertical" : "Vertical " + n;
		}
	}
	return "Vertical";
}

/* Every recording run now, the first one first. Holding g_mtx. */
std::vector<Run *> record_runs_locked()
{
	std::vector<Run *> list;
	auto first = g_runs.find(SV_RECORD_ID);
	if (first != g_runs.end())
		list.push_back(&first->second);
	for (auto &kv : g_runs)
		if (kv.first.rfind(SV_RECORD_MORE, 0) == 0)
			list.push_back(&kv.second);
	return list;
}

} // namespace

void sv_output_load()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	load_locked();
}

void sv_output_set_listener(std::function<void()> listener)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	g_listener = std::move(listener);
}

std::vector<SvDest> sv_output_dests()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	std::vector<SvDest> out = g_dests;
	for (SvDest &d : out) {
		d.has_key = !d.key.empty();
		wipe(d.key);
	}
	return out;
}

std::string sv_output_save_dest(const SvDest &dest)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	std::string id = dest.id;
	auto it = std::find_if(g_dests.begin(), g_dests.end(), [&](const SvDest &d) { return d.id == dest.id; });
	if (dest.id.empty() || it == g_dests.end()) {
		SvDest d = dest;
		d.id = std::to_string(os_gettime_ns());
		d.dual = d.dual && d.platform == "twitch";
		id = d.id;
		g_dests.push_back(d);
	} else {
		it->platform = dest.platform;
		it->name = dest.name;
		it->server = dest.server;
		it->own_video = dest.own_video;
		it->bitrate = dest.bitrate;
		it->size = dest.size;
		it->fps = dest.fps;
		it->canvas = dest.canvas;
		it->on = dest.on;
		it->dual = dest.dual && dest.platform == "twitch";
		if (!dest.key.empty())
			it->key = dest.key;
	}
	save_locked();
	return id;
}

void sv_output_set_dest_on(const std::string &id, bool on)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		auto it = std::find_if(g_dests.begin(), g_dests.end(), [&](const SvDest &d) { return d.id == id; });
		if (it == g_dests.end() || it->on == on)
			return;
		it->on = on;
		save_locked();
	}
	notify();
}

bool sv_output_remove_dest(const std::string &id)
{
	obs_output_t *out = nullptr;
	obs_service_t *svc = nullptr;
	Run *run = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		auto r = g_runs.find(id);
		if (r != g_runs.end()) {
			if (r->second.state != SvRunState::Off)
				return false;
			out = r->second.out;
			svc = r->second.svc;
			run = &r->second;
			r->second.out = nullptr;
			r->second.svc = nullptr;
		}
		g_dests.erase(std::remove_if(g_dests.begin(), g_dests.end(), [&](const SvDest &d) { return d.id == id; }),
			      g_dests.end());
		save_locked();
	}
	release_parts(out, svc, run);
	return true;
}

void sv_output_forget_canvas(const std::string &canvas)
{
	if (canvas.empty())
		return;
	std::lock_guard<std::mutex> lock(g_mtx);
	const std::string prefix = canvas + "-";
	for (auto it = g_dest_video.begin(); it != g_dest_video.end();) {
		if (it->first.compare(0, prefix.size(), prefix) == 0 && !obs_encoder_active(it->second)) {
			obs_encoder_release(it->second);
			it = g_dest_video.erase(it);
		} else {
			++it;
		}
	}
}

bool sv_output_canvas_in_use(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return std::any_of(g_runs.begin(), g_runs.end(), [&](const auto &kv) {
		return kv.second.state != SvRunState::Off && kv.second.canvas == canvas;
	});
}

bool sv_output_canvas_streaming(const std::string &canvas)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return std::any_of(g_runs.begin(), g_runs.end(), [&](const auto &kv) {
		return kv.second.kind == Kind::Stream && kv.second.state != SvRunState::Off && kv.second.canvas == canvas;
	});
}

/* ---------- Twitch Dual Format, through OBS's own stream ---------- */

/* The canvas of ours a Twitch Dual Format platform carries: its own, or the main one. */
static std::string dual_canvas(const SvDest &d)
{
	if (!d.canvas.empty() && d.canvas != SV_WIDE && sv_canvas_exists(d.canvas))
		return d.canvas;
	return sv_canvas_main_id();
}

/* Enhanced Broadcasting's Additional Canvas in OBS's settings, "" when it is off or has none. */
static std::string obs_extra_canvas()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg || !config_get_bool(cfg, "Stream1", "EnableMultitrackVideo"))
		return std::string();
	const char *uuid = config_get_string(cfg, "Stream1", "MultitrackExtraCanvas");
	return uuid ? uuid : "";
}

/* True when OBS's own stream is Twitch with this platform's key and Enhanced Broadcasting on.
   With fixCanvas, an Additional Canvas that no longer matches (the canvas came back with a
   new id) is pointed at this platform's canvas again; that pick was made in our panel. */
static bool dual_matches(const SvDest &d, bool fixCanvas)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	config_t *cfg = obs_frontend_get_profile_config();
	if (!svc || !cfg || d.key.empty())
		return false;
	obs_data_t *st = obs_service_get_settings(svc);
	const bool twitch = strcmp(obs_service_get_type(svc), "rtmp_common") == 0 &&
			    astrcmpi(obs_data_get_string(st, "service"), "Twitch") == 0;
	const bool sameKey = d.key == obs_data_get_string(st, "key");
	obs_data_release(st);
	if (!twitch || !sameKey || !config_get_bool(cfg, "Stream1", "EnableMultitrackVideo"))
		return false;
	const std::string want = dual_canvas(d);
	if (want.empty())
		return false;
	if (obs_extra_canvas() == want)
		return true;
	if (!fixCanvas)
		return false;
	config_set_string(cfg, "Stream1", "MultitrackExtraCanvas", want.c_str());
	config_save_safe(cfg, "tmp", nullptr);
	obs_log(LOG_INFO, "Twitch Dual Format: Additional Canvas pointed at the vertical again");
	return true;
}

/* A saved platform with its key, or false. */
static bool find_dest(const std::string &id, SvDest &out)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	auto it = std::find_if(g_dests.begin(), g_dests.end(), [&](const SvDest &d) { return d.id == id; });
	if (it == g_dests.end())
		return false;
	out = *it;
	return true;
}

/* Goes live through OBS's own stream. OBS shows its own message if Twitch refuses. */
static bool start_dual(SvDest &d, std::string &problem)
{
	if (!sv_output_dual_possible()) {
		problem = T("DualNeedsObs");
		return false;
	}
	if (g_dual_restart) {
		problem = T("DualRestartFirst");
		return false;
	}
	if (!dual_matches(d, true)) {
		problem = T("DualChanged");
		return false;
	}
	const bool already = obs_frontend_streaming_active();
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		Run &run = g_runs[d.id];
		if (run.state != SvRunState::Off)
			return true;
		run.id = d.id;
		run.kind = Kind::Stream;
		run.label = d.name;
		run.obs_stream = true;
		run.canvas = dual_canvas(d);
		run.problem.clear();
		run.target_kbps = 0;
		reset_health(run);
		run.started_ns = os_gettime_ns();
		run.state = already ? SvRunState::Live : SvRunState::Starting;
	}
	update_canvas_users();
	notify();
	if (!already)
		obs_frontend_streaming_start();
	return true;
}

#ifdef SV_DEV_BUILD
/* Testing builds only: the next extra recording fails as if its folder had gone. */
static std::atomic<bool> g_dev_fail_more{false};
void sv_output_dev_fail_more()
{
	g_dev_fail_more = true;
}
#endif

static bool start_one(const std::string &id, std::string &problem)
{
	const Kind kind = kind_of(id);
	const SvOutSettings set = sv_output_settings();

#ifdef SV_DEV_BUILD
	if (id.rfind(SV_RECORD_MORE, 0) == 0 && g_dev_fail_more.exchange(false)) {
		problem = T("OutBadFolder");
		return false;
	}
#endif
	if (!sv_link_is_linked()) {
		problem = T("OutNotLinked");
		return false;
	}
	if (kind == Kind::Stream) {
		SvDest peek;
		if (find_dest(id, peek) && peek.dual) {
			const bool ok = start_dual(peek, problem);
			wipe(peek.key);
			return ok;
		}
	}
	video_t *video = sv_canvas_video();
	if (!video) {
		problem = T("OutNoCanvas");
		return false;
	}
	video_t *mainVideo = video;

	SvDest dest;
	std::string path;
	std::string folder;
	if (kind == Kind::Record || kind == Kind::Backtrack) {
		folder = record_folder(set);
		if (folder.empty() || !os_file_exists(folder.c_str())) {
			problem = T("OutBadFolder");
			return false;
		}
	}
	/* OBS's Hybrid MP4 keeps what was recorded if OBS or the PC crashes, like MKV does.
	   Without it, a fragmented MP4 does the same. */
	const bool hybrid = set.rec_format == SV_FMT_MP4 && output_type_exists("mp4_output");
	const char *ext = set.rec_format == SV_FMT_MKV ? "mkv" : set.rec_format == SV_FMT_MOV ? "mov" : "mp4";
	std::string format;
	if (kind == Kind::Record) {
		format = name_format(record_start_word(record_canvas_of(id, set)).c_str(), set.rec_game_name);
		char *name = os_generate_formatted_filename(ext, true, format.c_str());
		path = folder + "/" + (name ? name : (std::string("Vertical.") + ext));
		bfree(name);
	}
	if (kind == Kind::Backtrack && !set.clip_folder.empty()) {
		if (!os_file_exists(set.clip_folder.c_str())) {
			problem = T("OutBadClipFolder");
			return false;
		}
		folder = set.clip_folder;
	}
	if (kind == Kind::VCam) {
		if (!output_type_exists("virtualcam_output")) {
			problem = T("OutNoVcam");
			return false;
		}
		/* There is one OBS virtual camera on a PC, and OBS's own button may hold it. */
		if (obs_frontend_virtualcam_active()) {
			problem = T("OutVcamBusy");
			return false;
		}
	}

	/* A platform on OBS's wide picture streams at the bitrate OBS's own stream uses. Read
	   before taking the lock, since it reads OBS's settings. */
	int wideKbps = 0;
	if (kind == Kind::Stream) {
		const SvMainEncoder m = sv_output_main_encoder();
		if (m.found)
			wideKbps = m.bitrate;
	}

	obs_output_t *old_out = nullptr;
	obs_service_t *old_svc = nullptr;
	Run *run = nullptr;
	obs_encoder_t *destVideo = nullptr;
	std::vector<obs_encoder_t *> moreAudio;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (kind == Kind::Stream) {
			auto it = std::find_if(g_dests.begin(), g_dests.end(), [&](const SvDest &d) { return d.id == id; });
			if (it == g_dests.end())
				return true;
			dest = *it;
		}
		/* A platform on another canvas streams that canvas's picture. One whose
		   canvas has gone shows the main one. */
		std::string canvasId;
		if (kind == Kind::Stream && dest.canvas == SV_WIDE) {
			/* OBS's own wide picture, the main scene exactly as OBS shows it. */
			video = obs_get_video();
			canvasId = SV_WIDE;
		} else if (kind == Kind::Stream && !dest.canvas.empty() && dest.canvas != sv_canvas_main_id()) {
			video_t *own = sv_canvas_video(dest.canvas);
			if (own) {
				canvasId = dest.canvas;
				video = own;
			}
		}
		/* Recording and Backtrack can each keep another canvas, picked in Settings. A recording
		   can also keep OBS's own horizontal picture. */
		const std::string fileCanvas = kind == Kind::Record      ? record_canvas_of(id, set)
					       : kind == Kind::Backtrack ? set.clip_canvas
									 : std::string();
		if (fileCanvas == SV_WIDE) {
			video = obs_get_video();
			canvasId = SV_WIDE;
		} else if (!fileCanvas.empty() && fileCanvas != sv_canvas_main_id()) {
			video_t *own = sv_canvas_video(fileCanvas);
			if (own) {
				canvasId = fileCanvas;
				video = own;
			}
		}
		run = &g_runs[id];
		if (run->state != SvRunState::Off)
			return true;
		old_out = run->out;
		old_svc = run->svc;
		run->out = nullptr;
		run->svc = nullptr;
		run->id = id;
		run->kind = kind;
		run->label = dest.name;
		run->canvas = canvasId;
		run->obs_stream = false;
		run->problem.clear();

		/* The encoders follow the current settings whenever nothing is using them. */
		if (kind != Kind::VCam && !encoders_in_use_locked() && !make_encoders(mainVideo, problem)) {
			run->out = old_out;
			run->svc = old_svc;
			run->problem = problem;
			return false;
		}
		if (kind == Kind::Stream) {
			destVideo = video_for_dest(dest, set, video, canvasId, wideKbps);
			if (!destVideo) {
				run->out = old_out;
				run->svc = old_svc;
				problem = T("OutNoEncoder");
				run->problem = problem;
				return false;
			}
			run->target_kbps = canvasId == SV_WIDE && !dest.own_video ? (wideKbps > 0 ? wideKbps : 6000)
										   : dest_settings(dest, set).bitrate;
		} else if (kind == Kind::Record || kind == Kind::Backtrack) {
			destVideo = video_for_canvas(kind, set, video, canvasId);
			if (destVideo && kind == Kind::Record && id != SV_RECORD_ID) {
				moreAudio = more_rec_audio(id, set);
				if (moreAudio.empty())
					destVideo = nullptr;
			}
			if (!destVideo) {
				run->out = old_out;
				run->svc = old_svc;
				problem = T("OutNoEncoder");
				run->problem = problem;
				return false;
			}
		}
	}
	release_parts(old_out, old_svc, run);

	obs_output_t *out = nullptr;
	obs_service_t *svc = nullptr;
	if (kind == Kind::Record) {
		obs_data_t *st = obs_data_create();
		obs_data_set_string(st, "path", path.c_str());
		if (set.rec_format == SV_FMT_MP4 && !hybrid)
			obs_data_set_string(st, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");
		/* Each new part is named the same way as the first. Splitting is always ready, so the
		   Split recording key works even when parts are set to Never; then only the key splits. */
		obs_data_set_bool(st, "split_file", true);
		obs_data_set_string(st, "directory", folder.c_str());
		obs_data_set_string(st, "format", format.c_str());
		obs_data_set_string(st, "extension", ext);
		obs_data_set_bool(st, "allow_spaces", true);
		if (set.split_mode == SV_SPLIT_TIME)
			obs_data_set_int(st, "max_time_sec", (int64_t)set.split_minutes * 60);
		else if (set.split_mode == SV_SPLIT_SIZE)
			obs_data_set_int(st, "max_size_mb", set.split_mb);
		out = obs_output_create(hybrid ? "mp4_output" : "ffmpeg_muxer", "simply-vertical-record", st, nullptr);
		obs_data_release(st);
	} else if (kind == Kind::Backtrack) {
		obs_data_t *st = obs_data_create();
		obs_data_set_string(st, "directory", folder.c_str());
		obs_data_set_string(st, "format", name_format("Vertical Backtrack", false).c_str());
		obs_data_set_string(st, "extension", "mp4");
		obs_data_set_bool(st, "allow_spaces", true);
		/* It holds the seconds after Save clip too, so the clip is still as long as asked. */
		obs_data_set_int(st, "max_time_sec", set.backtrack_seconds + set.clip_after);
		obs_data_set_int(st, "max_size_mb", set.clip_max_mb); /* 0: the length alone decides */
		out = obs_output_create("replay_buffer", "simply-vertical-backtrack", st, nullptr);
		obs_data_release(st);
	} else if (kind == Kind::VCam) {
		out = obs_output_create("virtualcam_output", "simply-vertical-vcam", nullptr, nullptr);
	} else {
		obs_data_t *st = obs_data_create();
		obs_data_set_string(st, "server", dest.server.c_str());
		obs_data_set_string(st, "key", dest.key.c_str());
		svc = obs_service_create_private("rtmp_custom", "simply-vertical", st);
		obs_data_release(st);
		out = obs_output_create("rtmp_output", "simply-vertical-stream", nullptr, nullptr);
		if (out && svc) {
			obs_output_set_service(out, svc);
			obs_output_set_reconnect_settings(out, set.reconnect_tries, set.reconnect_wait);
			if (set.delay_seconds > 0)
				obs_output_set_delay(out, (uint32_t)set.delay_seconds, OBS_OUTPUT_DELAY_PRESERVE);
		}
	}
	wipe(dest.key);

	if (!out || (kind == Kind::Stream && !svc)) {
		obs_output_release(out);
		obs_service_release(svc);
		problem = T("OutFailed");
		return false;
	}

	/* On the processor a paused recording can share its picture with what starts now. That
	   would start frozen, so the recording carries on first. */
	bool resumeRecording = false;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (kind == Kind::VCam) {
			obs_output_set_media(out, video, obs_get_audio());
		} else {
			obs_encoder_t *picture = destVideo ? destVideo : video_for(kind, set);
			if (kind != Kind::Record) {
				auto rec = g_runs.find(SV_RECORD_ID);
				resumeRecording = rec != g_runs.end() && rec->second.paused && rec->second.out &&
						  obs_output_get_video_encoder(rec->second.out) == picture;
			}
			obs_output_set_video_encoder(out, picture);
			if (kind == Kind::Record && id != SV_RECORD_ID) {
				/* Another picture recorded beside the first: sound encoders of its own. */
				size_t idx = 0;
				for (obs_encoder_t *a : moreAudio)
					obs_output_set_audio_encoder(out, a, idx++);
			} else {
				/* The recording has its own sound, so it can be paused on its own. */
				obs_output_set_audio_encoder(out, kind == Kind::Record ? g_rec_audio : g_audio, 0);
			}
			/* Files can carry more sound tracks, each one apart for editing. Streams carry one. */
			if ((kind == Kind::Record && id == SV_RECORD_ID) || kind == Kind::Backtrack) {
				obs_encoder_t *const *tracks = kind == Kind::Record ? g_rec_track_audio : g_track_audio;
				size_t idx = 1;
				for (size_t t = 0; t < 6; t++)
					if (tracks[t])
						obs_output_set_audio_encoder(out, tracks[t], idx++);
			}
		}
		run->out = out;
		run->svc = svc;
		run->state = SvRunState::Starting;
		attach(*run);
	}
	if (resumeRecording && sv_output_pause_record(0) == SvPause::Resumed)
		sv_output_set_notice(T("RecordResumedShared"), false);
	update_canvas_users();
	notify();

	if (!obs_output_start(out)) {
		const char *detail = obs_output_get_last_error(out);
		bool idle;
		{
			std::lock_guard<std::mutex> lock(g_mtx);
			run->state = SvRunState::Off;
			run->problem = problem_for(*run, OBS_OUTPUT_ERROR, detail);
			problem = run->problem;
			idle = !any_running_locked();
		}
		(void)idle;
		update_canvas_users();
		notify();
		return false;
	}
	return true;
}

/* "Canvas could not record: why" for the panel's line. */
static std::string more_failed(const std::string &canvas, const std::string &why)
{
	std::string name;
	if (canvas == SV_WIDE) {
		name = T("DestCanvasWide");
	} else {
		name = T("DestCanvasMain");
		for (const SvVCanvas &c : sv_canvases())
			if (!c.main && c.uuid == canvas)
				name = c.name;
	}
	std::string text = T("RecordMoreFailed");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, name);
	if (!why.empty())
		text += " " + why;
	return text;
}

bool sv_output_start(const std::string &id, std::string &problem)
{
	if (id != SV_RECORD_ID)
		return start_one(id, problem);
	if (!start_one(id, problem))
		return false;
	/* Every other picture ticked in Settings records too, each to a file of its own. */
	const std::vector<std::string> list = sv_output_record_canvases(sv_output_settings());
	std::string trouble;
	for (size_t i = 1; i < list.size(); i++) {
		/* OBS's own recording already keeps the horizontal picture. */
		if (list[i] == SV_WIDE && obs_frontend_recording_active()) {
			obs_log(LOG_INFO, "horizontal recording left out: OBS is recording it already");
			trouble = T("RecordMoreObsHas");
			continue;
		}
		std::string why;
		if (!start_one(std::string(SV_RECORD_MORE) + list[i], why)) {
			obs_log(LOG_WARNING, "a second recording could not start: %s", why.c_str());
			trouble = more_failed(list[i], why);
		}
	}
	if (!trouble.empty())
		sv_output_set_notice(trouble, trouble != T("RecordMoreObsHas"));
	return true;
}

/* Stops every recording besides the first. Never holding g_mtx. */
static void stop_more_records(void *);

static void stop_one(const std::string &id)
{
	obs_output_t *out = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		if (id == SV_RECORD_ID)
			g_auto_record = false;
		if (id == SV_BACKTRACK_ID)
			g_auto_backtrack = false;
		auto r = g_runs.find(id);
		if (r != g_runs.end() && r->second.obs_stream && r->second.state != SvRunState::Off) {
			r->second.state = SvRunState::Stopping;
		} else {
			if (r == g_runs.end() || !r->second.out || r->second.state == SvRunState::Off)
				return;
			r->second.state = SvRunState::Stopping;
			out = obs_output_get_ref(r->second.out);
		}
	}
	if (!out) {
		/* Twitch Dual Format: stopping OBS's own stream stops it. */
		if (obs_frontend_streaming_active()) {
			notify();
			obs_frontend_streaming_stop();
		} else {
			{
				std::lock_guard<std::mutex> lock(g_mtx);
				auto r = g_runs.find(id);
				if (r != g_runs.end()) {
					r->second.state = SvRunState::Off;
					r->second.obs_stream = false;
				}
			}
			update_canvas_users();
			notify();
		}
		return;
	}
	notify();
	obs_output_stop(out);
	obs_output_release(out);
}

static void stop_more_records(void *)
{
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (const auto &kv : g_runs)
			if (kv.first.rfind(SV_RECORD_MORE, 0) == 0 && kv.second.state != SvRunState::Off)
				ids.push_back(kv.first);
	}
	for (const std::string &more : ids)
		stop_one(more);
}

void sv_output_stop(const std::string &id)
{
	stop_one(id);
	if (id == SV_RECORD_ID)
		stop_more_records(nullptr);
}

SvRunInfo sv_output_info(const std::string &id)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	SvRunInfo info;
	auto r = g_runs.find(id);
	if (r == g_runs.end())
		return info;
	const Run &run = r->second;
	info.state = run.state;
	info.problem = run.problem;
	if (run.state == SvRunState::Live || run.state == SvRunState::Reconnecting) {
		const uint64_t now = os_gettime_ns();
		/* A paused recording's clock stands still, so it shows how long the file is. */
		uint64_t pausedNs = run.paused_total_ns;
		if (run.paused && run.paused_since_ns)
			pausedNs += now - run.paused_since_ns;
		const uint64_t ranNs = now - run.started_ns;
		info.seconds = (ranNs > pausedNs ? ranNs - pausedNs : 0) / 1000000000ULL;
		info.paused = run.paused;
		if (run.out) {
			info.dropped = (uint64_t)obs_output_get_frames_dropped(run.out);
			info.total = (uint64_t)obs_output_get_total_frames(run.out);
		} else if (!run.frames.empty()) {
			info.dropped = run.frames.back().first;
			info.total = run.frames.back().second;
		}
		if (run.kind == Kind::Stream) {
			info.history.assign(run.kbps.begin(), run.kbps.end());
			info.kbps = run.kbps.empty() ? 0.0f : run.kbps.back();
			info.low_bitrate = run.low_count >= 10;
			if (run.frames.size() >= 2) {
				const uint64_t d = run.frames.back().first - run.frames.front().first;
				const uint64_t t = run.frames.back().second - run.frames.front().second;
				info.dropped_pct = t > 0 ? 100.0f * (float)d / (float)t : 0.0f;
			}
			if (run.state == SvRunState::Reconnecting) {
				info.health = SvHealth::Dropping;
				if (run.reconnect_until_ns > now)
					info.reconnect_left = (int)((run.reconnect_until_ns - now + 999999999ULL) / 1000000000ULL);
			} else if (info.dropped_pct > 2.0f) {
				info.health = SvHealth::Dropping;
			} else if (info.dropped_pct > 0.2f || info.low_bitrate) {
				info.health = SvHealth::Shaky;
			}
		}
	}
	return info;
}

void sv_output_sample()
{
	const int target = sv_output_settings().bitrate;
	/* OBS's own stream, for a Twitch Dual Format platform. */
	obs_output_t *obsOut = obs_frontend_get_streaming_output();
	const bool obsActive = obs_frontend_streaming_active();
	bool gaveUp = false;
	{
	std::lock_guard<std::mutex> lock(g_mtx);
	const uint64_t now = os_gettime_ns();
	for (auto &kv : g_runs) {
		Run &run = kv.second;
		if (run.obs_stream && run.state == SvRunState::Starting && !obsActive &&
		    now - run.started_ns > 60ULL * 1000000000ULL) {
			/* OBS never went live, and never said it stopped. */
			run.state = SvRunState::Off;
			run.obs_stream = false;
			run.problem = T("DualDidNotStart");
			gaveUp = true;
			continue;
		}
		obs_output_t *src = run.obs_stream ? obsOut : run.out;
		if (run.kind != Kind::Stream || !src)
			continue;
		if (run.state != SvRunState::Live && run.state != SvRunState::Reconnecting)
			continue;
		const uint64_t bytes = obs_output_get_total_bytes(src);
		const uint64_t dropped = (uint64_t)obs_output_get_frames_dropped(src);
		const uint64_t total = (uint64_t)obs_output_get_total_frames(src);
		float kbps = 0;
		if (run.state == SvRunState::Live && run.last_sample_ns && now > run.last_sample_ns && bytes >= run.last_bytes)
			kbps = (float)((double)(bytes - run.last_bytes) * 8.0 / 1000.0 /
				       ((double)(now - run.last_sample_ns) / 1e9));
		const bool first = run.last_sample_ns == 0 && run.state == SvRunState::Live;
		run.last_bytes = bytes;
		run.last_sample_ns = now;
		if (first)
			continue;
		run.kbps.push_back(kbps);
		while (run.kbps.size() > 60)
			run.kbps.pop_front();
		run.frames.push_back({dropped, total});
		while (run.frames.size() > 11)
			run.frames.pop_front();
		/* Give the connection fifteen seconds to settle before judging its speed. */
		const bool settled = now - run.started_ns > 15ULL * 1000000000ULL;
		const int aim = run.target_kbps > 0 ? run.target_kbps : target;
		/* Twitch picks a Dual Format stream's bitrate, so there is no aim to judge it by. */
		if (!run.obs_stream && settled && run.state == SvRunState::Live && kbps < 0.5f * (float)aim)
			run.low_count++;
		else
			run.low_count = 0;
	}
	}
	obs_output_release(obsOut);
	if (gaveUp) {
		update_canvas_users();
		notify();
	}
}

SvClip sv_output_last_clip()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_clip;
}

namespace {

/* "Vertical Backtrack Valorant 2026-11-20 23-47-07.mp4" gives "Valorant". */
std::string game_in_name(const std::string &name)
{
	static const char start[] = "Vertical Backtrack ";
	std::string rest = name.substr(sizeof(start) - 1);
	const size_t dot = rest.rfind('.');
	if (dot != std::string::npos)
		rest.resize(dot);
	/* The date and time at the end: "2026-11-20 23-47-07", 19 characters. */
	if (rest.size() < 19)
		return std::string();
	rest.resize(rest.size() - 19);
	while (!rest.empty() && rest.back() == ' ')
		rest.pop_back();
	return rest;
}

bool clip_file_name(const char *name)
{
	static const char start[] = "Vertical Backtrack ";
	if (strncmp(name, start, sizeof(start) - 1) != 0)
		return false;
	const char *ext = strrchr(name, '.');
	return ext && (_stricmp(ext, ".mp4") == 0 || _stricmp(ext, ".mkv") == 0 || _stricmp(ext, ".mov") == 0);
}

} // namespace

std::vector<SvClipInfo> sv_output_recent_clips(size_t max)
{
	const SvOutSettings set = sv_output_settings();
	const SvClip last = sv_output_last_clip();
	std::vector<std::string> folders;
	folders.push_back(!set.clip_folder.empty() ? set.clip_folder : record_folder(set));
	/* The newest clip may have gone to a folder the settings no longer name. */
	if (!last.path.empty()) {
		const size_t slash = last.path.find_last_of("/\\");
		if (slash != std::string::npos)
			folders.push_back(last.path.substr(0, slash));
	}
	std::vector<SvClipInfo> found;
	std::vector<std::string> seen;
	for (std::string folder : folders) {
		for (char &c : folder)
			if (c == '\\')
				c = '/';
		while (!folder.empty() && folder.back() == '/')
			folder.pop_back();
		if (folder.empty() || std::find(seen.begin(), seen.end(), folder) != seen.end())
			continue;
		seen.push_back(folder);
		os_dir_t *dir = os_opendir(folder.c_str());
		if (!dir)
			continue;
		while (os_dirent *e = os_readdir(dir)) {
			if (e->directory || !clip_file_name(e->d_name))
				continue;
			SvClipInfo info;
			info.clip.path = folder + "/" + e->d_name;
			struct stat st = {};
			if (os_stat(info.clip.path.c_str(), &st) != 0)
				continue;
			info.savedAt = (int64_t)st.st_mtime;
			info.game = game_in_name(e->d_name);
			found.push_back(info);
		}
		os_closedir(dir);
	}
	std::sort(found.begin(), found.end(),
		  [](const SvClipInfo &a, const SvClipInfo &b) { return a.savedAt > b.savedAt; });
	if (found.size() > max)
		found.resize(max);
	/* Only the ones shown are opened, to read their length. */
	for (SvClipInfo &info : found) {
		std::string a = info.clip.path, b = last.path;
		for (std::string *s : {&a, &b})
			for (char &c : *s)
				if (c == '\\')
					c = '/';
		if (!last.path.empty() && _stricmp(a.c_str(), b.c_str()) == 0) {
			info.clip.seconds = last.seconds;
			info.clip.after = last.after;
			continue;
		}
		const double secs = mp4_seconds(info.clip.path);
		info.clip.seconds = secs > 0 ? (std::max)(1, (int)(secs + 0.5)) : (std::max)(1, set.backtrack_seconds);
		/* An older clip used the seconds after that were set when it was saved. They are
		   only changed while Backtrack is off, so today's setting is nearly always right. */
		info.clip.after = std::clamp(set.clip_after, 0, info.clip.seconds);
	}
	return found;
}

bool sv_output_any_running()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return any_running_locked();
}

bool sv_output_encoders_in_use()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return encoders_in_use_locked();
}

bool sv_output_dual_possible()
{
	/* Twitch Dual Format's Additional Canvas arrived in OBS 31.1. */
	return obs_get_version() >= MAKE_SEMANTIC_VERSION(31, 1, 0);
}

SvObsStream sv_output_obs_stream()
{
	SvObsStream s;
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (svc) {
		obs_data_t *st = obs_service_get_settings(svc);
		if (strcmp(obs_service_get_type(svc), "rtmp_custom") == 0)
			s.service = "Custom";
		else
			s.service = obs_data_get_string(st, "service");
		s.has_key = *obs_data_get_string(st, "key") != '\0';
		obs_data_release(st);
	}
	config_t *cfg = obs_frontend_get_profile_config();
	s.enhanced = cfg && config_get_bool(cfg, "Stream1", "EnableMultitrackVideo");
	const std::string extra = obs_extra_canvas();
	if (!extra.empty()) {
		obs_canvas_t *c = obs_get_canvas_by_uuid(extra.c_str());
		if (c) {
			const char *name = obs_canvas_get_name(c);
			s.extra_canvas = name ? name : "";
			obs_canvas_release(c);
		}
	}
	return s;
}

bool sv_output_dual_ready(const std::string &id)
{
	SvDest d;
	if (!find_dest(id, d))
		return false;
	const bool ok = d.dual && !g_dual_restart && dual_matches(d, false);
	wipe(d.key);
	return ok;
}

bool sv_output_setup_dual(const std::string &id, bool &restartNeeded, std::string &problem)
{
	restartNeeded = false;
	SvDest d;
	if (!find_dest(id, d) || !d.dual || d.key.empty()) {
		problem = T("DualChanged");
		return false;
	}
	if (!sv_output_dual_possible()) {
		problem = T("DualNeedsObs");
		wipe(d.key);
		return false;
	}
	if (obs_frontend_streaming_active()) {
		problem = T("DualObsLive");
		wipe(d.key);
		return false;
	}
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		problem = T("DualChanged");
		wipe(d.key);
		return false;
	}

	/* What OBS built its stream with when it started. It only rebuilds it on a restart. */
	bool before = false;
	if (config_has_user_value(cfg, "Stream1", "EnableMultitrackVideo")) {
		before = config_get_bool(cfg, "Stream1", "EnableMultitrackVideo");
	} else if (obs_service_t *svc = obs_frontend_get_streaming_service()) {
		obs_data_t *st = obs_service_get_settings(svc);
		before = obs_data_has_user_value(st, "multitrack_video_configuration_url");
		obs_data_release(st);
	}

	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "service", "Twitch");
	obs_data_set_string(st, "server", "auto");
	obs_data_set_string(st, "key", d.key.c_str());
	obs_service_t *svc = obs_service_create("rtmp_common", "default_service", st, nullptr);
	obs_data_release(st);
	wipe(d.key);
	if (!svc) {
		problem = T("DualChanged");
		return false;
	}
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);

	config_set_bool(cfg, "Stream1", "EnableMultitrackVideo", true);
	config_set_string(cfg, "Stream1", "MultitrackExtraCanvas", dual_canvas(d).c_str());
	config_save_safe(cfg, "tmp", nullptr);

	if (!before)
		g_dual_restart = true;
	restartNeeded = g_dual_restart;
	obs_log(LOG_INFO, "Twitch Dual Format set up in OBS's Stream settings%s",
		restartNeeded ? ", OBS needs a restart" : "");
	return true;
}

void sv_output_obs_stream_event(int event)
{
	bool changed = false;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING) {
		g_obs_streaming = true;
		g_obs_extra_canvas = obs_extra_canvas();
		/* OBS's own Start Streaming counts for a Twitch dual platform set up the same way. */
		std::vector<SvDest> dests = sv_output_dests();
		for (SvDest &d : dests) {
			if (d.dual && dual_matches(d, false)) {
				std::lock_guard<std::mutex> lock(g_mtx);
				Run &run = g_runs[d.id];
				if (run.state == SvRunState::Off) {
					run.id = d.id;
					run.kind = Kind::Stream;
					run.label = d.name;
					run.obs_stream = true;
					run.canvas = dual_canvas(d);
					run.problem.clear();
					run.target_kbps = 0;
					reset_health(run);
					run.started_ns = os_gettime_ns();
					run.state = SvRunState::Starting;
				}
			}
			wipe(d.key);
		}
		changed = true;
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED || event == OBS_FRONTEND_EVENT_STREAMING_STOPPING ||
		   event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		std::lock_guard<std::mutex> lock(g_mtx);
		const uint64_t now = os_gettime_ns();
		for (auto &kv : g_runs) {
			Run &run = kv.second;
			if (!run.obs_stream || run.state == SvRunState::Off)
				continue;
			if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
				run.state = SvRunState::Live;
				run.started_ns = now;
				reset_health(run);
			} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
				run.state = SvRunState::Stopping;
			} else {
				if (run.state == SvRunState::Starting)
					run.problem = T("DualDidNotStart");
				run.state = SvRunState::Off;
				run.obs_stream = false;
			}
			changed = true;
		}
		if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
			g_obs_streaming = false;
			g_obs_extra_canvas.clear();
			changed = true;
		}
	}
	if (changed) {
		update_canvas_users();
		notify();
	}
}

namespace {

std::string fill_number(const char *fmt, int n)
{
	std::string f = fmt ? fmt : "";
	const size_t at = f.find("%1");
	if (at != std::string::npos)
		f.replace(at, 2, std::to_string(n));
	return f;
}

/* Names the clip after the game on screen now, then writes it. */
void save_now(obs_output_t *out)
{
	const SvOutSettings set = sv_output_settings();
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "format", name_format("Vertical Backtrack", set.clip_game_name).c_str());
	obs_output_update(out, st);
	obs_data_release(st);
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(out), "save", &cd);
	calldata_free(&cd);
}

void stop_after_thread()
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_after_cancel = true;
	}
	g_after_cv.notify_all();
	if (g_after_thread.joinable())
		g_after_thread.join();
	std::lock_guard<std::mutex> lock(g_mtx);
	g_after_cancel = false;
}

} // namespace

bool sv_output_save_backtrack()
{
	const int after = sv_output_settings().clip_after;
	obs_output_t *out = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		auto r = g_runs.find(SV_BACKTRACK_ID);
		if (r == g_runs.end() || !r->second.out || r->second.state != SvRunState::Live)
			return false;
		/* Clip names go down to the second, so a double press must never make two in one second.
		   While a clip waits for its seconds after, more presses add nothing. */
		const uint64_t now = os_gettime_ns();
		const uint64_t gap = 1200000000ULL + (uint64_t)after * 1000000000ULL;
		if (g_last_save_ns && now - g_last_save_ns < gap)
			return true;
		g_last_save_ns = now;
		out = obs_output_get_ref(r->second.out);
	}
	if (after <= 0) {
		save_now(out);
		obs_output_release(out);
		return true;
	}
	if (g_after_thread.joinable())
		g_after_thread.join(); /* the last wait is over, the gap above makes sure */
	sv_output_set_notice(fill_number(T("ClipSoon"), after), false);
	g_after_thread = std::thread([out, after] {
		bool cancelled;
		{
			std::unique_lock<std::mutex> lock(g_mtx);
			cancelled = g_after_cv.wait_for(lock, std::chrono::seconds(after), [] { return g_after_cancel; });
		}
		if (!cancelled && obs_output_active(out))
			save_now(out);
		obs_output_release(out);
	});
	return true;
}

/* Every live recording's output, the first one first, each with a reference to release. */
static std::vector<obs_output_t *> live_record_outputs()
{
	std::vector<obs_output_t *> outs;
	std::lock_guard<std::mutex> lock(g_mtx);
	auto first = g_runs.find(SV_RECORD_ID);
	if (first == g_runs.end() || !first->second.out || first->second.state != SvRunState::Live)
		return outs;
	for (Run *r : record_runs_locked())
		if (r->out && r->state == SvRunState::Live)
			if (obs_output_t *o = obs_output_get_ref(r->out))
				outs.push_back(o);
	return outs;
}

bool sv_output_split_record()
{
	const std::vector<obs_output_t *> outs = live_record_outputs();
	if (outs.empty())
		return false;
	bool split = false;
	for (size_t i = 0; i < outs.size(); i++) {
		calldata_t cd = {};
		const bool called = proc_handler_call(obs_output_get_proc_handler(outs[i]), "split_file", &cd);
		const bool done = called && calldata_bool(&cd, "split_file_enabled");
		calldata_free(&cd);
		if (i == 0)
			split = done;
		obs_output_release(outs[i]);
	}
	obs_log(LOG_INFO, "recording split %s (%zu files)", split ? "asked for" : "not possible", outs.size());
	return split;
}

SvChapter sv_output_add_chapter()
{
	std::vector<obs_output_t *> outs = live_record_outputs();
	if (outs.empty())
		return SvChapter::NotRecording;
	obs_output_t *out = outs.front();
	/* Only OBS's Hybrid MP4 writer keeps chapters. Left unnamed, OBS calls them "Unnamed 1" and
	   so on, so they get "Marker 1", "Marker 2" instead. The count starts again with each new
	   recording: a frame count lower than at the last marker means a new file. */
	static std::mutex countMtx;
	static obs_output_t *countOut = nullptr;
	static uint32_t countFrames = 0;
	static int count = 0;
	std::string name;
	{
		std::lock_guard<std::mutex> lock(countMtx);
		const uint32_t frames = (uint32_t)obs_output_get_total_frames(out);
		if (out != countOut || frames < countFrames)
			count = 0;
		countOut = out;
		countFrames = frames;
		name = T("ChapterName");
		const size_t at = name.find("%1");
		const std::string num = std::to_string(count + 1);
		if (at != std::string::npos)
			name.replace(at, 2, num);
		else
			name += " " + num;
	}
	/* The same marker in every file, so they line up in an editor. */
	bool added = false;
	for (size_t i = 0; i < outs.size(); i++) {
		calldata_t cd = {};
		calldata_set_string(&cd, "chapter_name", name.c_str());
		const bool done = proc_handler_call(obs_output_get_proc_handler(outs[i]), "add_chapter", &cd);
		calldata_free(&cd);
		if (i == 0)
			added = done;
		obs_output_release(outs[i]);
	}
	if (added) {
		std::lock_guard<std::mutex> lock(countMtx);
		count++;
	}
	obs_log(LOG_INFO, "recording chapter marker %s", added ? "added" : "not possible (not an MP4 recording)");
	return added ? SvChapter::Added : SvChapter::NotMp4;
}

/* True when another running output uses any of this one's encoders, so pausing it would
   pause that one too. Holding g_mtx. */
bool shares_encoders_locked(obs_output_t *out)
{
	std::vector<obs_encoder_t *> mine;
	if (obs_encoder_t *v = obs_output_get_video_encoder(out))
		mine.push_back(v);
	for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++)
		if (obs_encoder_t *a = obs_output_get_audio_encoder(out, i))
			mine.push_back(a);
	for (const auto &kv : g_runs) {
		const Run &other = kv.second;
		if (!other.out || other.out == out || other.state == SvRunState::Off || other.kind == Kind::VCam)
			continue;
		const auto uses = [&](obs_encoder_t *e) {
			return e && std::find(mine.begin(), mine.end(), e) != mine.end();
		};
		if (uses(obs_output_get_video_encoder(other.out)))
			return true;
		for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++)
			if (uses(obs_output_get_audio_encoder(other.out, i)))
				return true;
	}
	return false;
}

SvPause sv_output_pause_record(int want)
{
	/* Every recording pauses and carries on together, the first one deciding which way. */
	std::vector<std::pair<std::string, obs_output_t *>> outs;
	bool pause = false;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		auto r = g_runs.find(SV_RECORD_ID);
		if (r == g_runs.end() || !r->second.out || r->second.state != SvRunState::Live)
			return SvPause::NotRecording;
		pause = want < 0 ? !r->second.paused : want > 0;
		if (pause == r->second.paused)
			return pause ? SvPause::Paused : SvPause::Resumed;
		/* On the processor the recording would have to share the stream's encoder or encode the
		   picture twice, so pause is a graphics card feature only. */
		if (pause && family_of(g_chosen) == Family::X264) {
			obs_log(LOG_INFO, "recording pause refused: it needs a graphics card encoder");
			return SvPause::NeedsCard;
		}
		for (Run *rec : record_runs_locked()) {
			if (!rec->out || rec->state != SvRunState::Live || rec->paused == pause)
				continue;
			if (pause && shares_encoders_locked(rec->out)) {
				obs_log(LOG_INFO, "recording pause refused: its encoder is shared with a running output");
				for (auto &o : outs)
					obs_output_release(o.second);
				return SvPause::Shared;
			}
			if (obs_output_t *o = obs_output_get_ref(rec->out))
				outs.emplace_back(rec->id, o);
		}
	}
	if (outs.empty())
		return SvPause::NotRecording;
	/* Never holding g_mtx: OBS signals the pause straight back. */
	bool ok = false;
	for (size_t i = 0; i < outs.size(); i++) {
		obs_output_t *out = outs[i].second;
		const bool done = obs_output_pause(out, pause) && obs_output_paused(out) == pause;
		if (i == 0)
			ok = done;
		if (done) {
			std::lock_guard<std::mutex> lock(g_mtx);
			auto r = g_runs.find(outs[i].first);
			if (r != g_runs.end() && r->second.out == out) {
				const uint64_t now = os_gettime_ns();
				if (pause) {
					r->second.paused_since_ns = now;
				} else if (r->second.paused_since_ns) {
					r->second.paused_total_ns += now - r->second.paused_since_ns;
					r->second.paused_since_ns = 0;
				}
				r->second.paused = pause;
			}
		} else {
			obs_log(LOG_WARNING, "recording %s: %s could not", pause ? "pause" : "carry on", outs[i].first.c_str());
		}
		obs_output_release(out);
	}
	obs_log(LOG_INFO, "recording %s %s (%zu files)", pause ? "pause" : "carry on", ok ? "done" : "not possible",
		outs.size());
	notify();
	if (!ok)
		return SvPause::NotPossible;
	return pause ? SvPause::Paused : SvPause::Resumed;
}

bool sv_output_can_pause()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return !g_chosen.empty() && family_of(g_chosen) != Family::X264;
}

bool sv_output_record_paused()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	auto r = g_runs.find(SV_RECORD_ID);
	return r != g_runs.end() && r->second.state == SvRunState::Live && r->second.paused;
}

SvNotice sv_output_notice()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_notice;
}

void sv_output_set_notice(const std::string &text, bool problem)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_notice.text = text;
		g_notice.problem = problem;
	}
	notify();
}

int sv_output_obs_fps()
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.fps_den || !ovi.fps_num)
		return 60;
	if (ovi.fps_den == 1001)
		return (int)(ovi.fps_num / 1000);
	return (std::max)(1, (int)std::lround((double)ovi.fps_num / ovi.fps_den));
}

int sv_output_fps_div(int fps)
{
	const int whole = sv_output_obs_fps();
	if (fps <= 0 || fps >= whole)
		return 1;
	if (whole % fps == 0)
		return std::clamp(whole / fps, 1, 12);
	return std::clamp((int)std::lround((double)whole / fps), 1, 12);
}

SvOutSettings sv_output_settings()
{
	SvOutSettings s;
	s.bitrate = std::clamp(sv_store_int("out_bitrate", s.bitrate), 1000, 20000);
	s.audio_track = std::clamp(sv_store_int("out_audio_track", s.audio_track), 1, 6);
	s.encoder = sv_store_string("out_encoder");
	s.folder = sv_store_string("out_folder");
	s.backtrack_seconds = std::clamp(sv_store_int("out_backtrack_seconds", s.backtrack_seconds), 10, 600);
	s.delay_seconds = std::clamp(sv_store_int("out_delay_seconds", s.delay_seconds), 0, 600);
	s.follow_obs = sv_store_int("out_follow_obs", 0) != 0;
	s.record_with_live = sv_store_int("out_record_with_live", 0) != 0;
	const int ab = sv_store_int("out_audio_bitrate", s.audio_bitrate);
	s.audio_bitrate = (ab == 128 || ab == 160 || ab == 192 || ab == 320) ? ab : 160;
	s.reconnect_tries = std::clamp(sv_store_int("out_reconnect_tries", s.reconnect_tries), 0, 100);
	s.reconnect_wait = std::clamp(sv_store_int("out_reconnect_wait", s.reconnect_wait), 1, 60);
	s.backtrack_auto = std::clamp(sv_store_int("out_backtrack_auto", 0), 0, 2);

	/* Older settings carry over: two sizes, half or full frame rate, three effort steps,
	   MP4 or MKV, and a recording bitrate where 0 meant the stream's. */
	const bool oldSmall = sv_store_int("out_small_size", 0) != 0;
	const bool oldHalf = sv_store_int("out_half_fps", 0) != 0;
	const int oldEffort = std::clamp(sv_store_int("out_effort", 1), 0, 2);
	const bool oldMkv = sv_store_int("out_mkv", 0) != 0;
	const int oldRb = sv_store_int("out_record_bitrate", 0);

	s.size = std::clamp(sv_store_int("out_size", oldSmall ? SV_SIZE_720 : SV_SIZE_1080), 0, 2);
	/* The frame rate used to be kept as a share of OBS's; now it is the number picked. */
	const int storedFps = sv_store_int("out_fps", -1);
	if (storedFps >= 0) {
		s.fps = std::clamp(storedFps, 0, 240);
	} else {
		const int div = std::clamp(sv_store_int("out_fps_div", oldHalf ? 2 : 1), 1, 12);
		s.fps = div > 1 ? sv_output_obs_fps() / div : 0;
	}
	s.fps_div = sv_output_fps_div(s.fps);
	s.sharp_scale = sv_store_int("out_sharp_scale", 1) != 0;
	s.keyframe_sec = std::clamp(sv_store_int("out_keyframe_sec", 2), 1, 10);
	s.effort = std::clamp(sv_store_int("out_effort5", oldEffort + 1), 0, 4);
	s.preset = sv_store_string("out_preset");

	s.rec_format = std::clamp(sv_store_int("out_rec_format", oldMkv ? SV_FMT_MKV : SV_FMT_MP4), 0, 2);
	/* Recording quality used to be four fixed steps: the stream's, high (level 23), looks
	   perfect (level 16) or its own bitrate. */
	const int recMode = sv_store_int("out_rec_mode", -1);
	if (recMode >= 0) {
		s.rec_quality = std::clamp(recMode, 0, 3);
		s.rec_level = std::clamp(sv_store_int("out_rec_level", 20), 1, 51);
	} else {
		const int old = std::clamp(sv_store_int("out_rec_quality", oldRb > 0 ? 3 : 0), 0, 3);
		s.rec_quality = old == 0 ? SV_Q_SAME : old == 3 ? SV_Q_BITRATE : SV_Q_LEVEL;
		s.rec_level = old == 2 ? 16 : old == 1 ? 23 : 20;
	}
	s.record_bitrate = std::clamp(sv_store_int("out_rec_bitrate", oldRb > 0 ? oldRb : 12000), 1000, 50000);
	s.split_mode = std::clamp(sv_store_int("out_split_mode", 0), 0, 2);
	s.split_minutes = std::clamp(sv_store_int("out_split_minutes", 30), 1, 600);
	s.split_mb = std::clamp(sv_store_int("out_split_mb", 2048), 100, 100000);
	s.rec_game_name = sv_store_int("out_rec_game_name", 0) != 0;
	s.rec_tracks = (unsigned)std::clamp(sv_store_int("out_rec_tracks", 0), 0, 63);
	s.rec_codec = std::clamp(sv_store_int("out_rec_codec", SV_CODEC_H264), 0, 2);
	s.codec_youtube = sv_store_int("out_codec_youtube", 0) != 0;

	s.clip_after = std::clamp(sv_store_int("out_clip_after", 0), 0, 30);
	s.clip_folder = sv_store_string("out_clip_folder");
	const int clipMode = sv_store_int("out_clip_mode", -1);
	if (clipMode >= 0) {
		s.clip_quality = std::clamp(clipMode, 0, 4);
		s.clip_level = std::clamp(sv_store_int("out_clip_level", 20), 1, 51);
	} else {
		const int old = std::clamp(sv_store_int("out_clip_quality", 0), 0, 3);
		s.clip_quality = old <= 1 ? old : SV_CLIP_LEVEL;
		s.clip_level = old == 3 ? 16 : old == 2 ? 23 : 20;
	}
	s.clip_bitrate = std::clamp(sv_store_int("out_clip_bitrate", s.record_bitrate), 1000, 50000);
	s.clip_game_name = sv_store_int("out_clip_game_name", 0) != 0;
	s.clip_max_mb = std::clamp(sv_store_int("out_clip_max_mb", 0), 0, 64000);
	s.rec_canvas = sv_store_string("out_rec_canvas");
	/* "main,<canvas id>,obs-wide". Saved before more than one could be ticked: rec_canvas alone. */
	const std::string many = sv_store_string("out_rec_canvases");
	s.rec_canvases.clear();
	size_t from = 0;
	while (from <= many.size() && !many.empty()) {
		const size_t comma = many.find(',', from);
		const std::string one = many.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
		const std::string canvas = one == "main" ? std::string() : one;
		if (!one.empty() && std::find(s.rec_canvases.begin(), s.rec_canvases.end(), canvas) == s.rec_canvases.end())
			s.rec_canvases.push_back(canvas);
		if (comma == std::string::npos)
			break;
		from = comma + 1;
	}
	if (s.rec_canvases.empty())
		s.rec_canvases.push_back(s.rec_canvas);
	s.rec_canvas = s.rec_canvases.front();
	s.clip_canvas = sv_store_string("out_clip_canvas");
	/* Slow motion. Before it slowed on the moment, the only choice was half speed for the whole
	   clip, so a member who had picked that keeps it. */
	const int slow = sv_store_int("out_replay_slow", -1);
	if (slow < 0) {
		const bool half = sv_store_int("out_replay_speed", 100) == 50;
		s.replay_slow = 50;
		s.replay_slow_whole = half;
	} else {
		s.replay_slow = slow >= 88 ? 100 : slow >= 63 ? 75 : slow >= 38 ? 50 : 25;
		s.replay_slow_whole = sv_store_int("out_replay_slow_whole", 0) != 0;
		s.replay_slow_press = !s.replay_slow_whole && sv_store_int("out_replay_slow_press", 0) != 0;
	}
	s.replay_slow_seconds = std::clamp(sv_store_int("out_replay_slow_seconds", 3), 1, 30);
	s.replay_sound = sv_store_int("out_replay_sound", 0) != 0;
	s.replay_seconds = std::clamp(sv_store_int("out_replay_seconds", 15), 0, 60);
	s.replay_tag = sv_store_int("out_replay_tag", 1) != 0;
	s.replay_auto = sv_store_int("out_replay_auto", 0) != 0;
	s.replay_transition = std::clamp((int)sv_store_int("out_replay_transition", 3), 0, 3);
	s.replay_line = sv_store_int("out_replay_line", 1) != 0;
	s.replay_cam_live = sv_store_int("out_replay_cam_live", 0) != 0;
	s.replay_zoom = std::clamp((int)sv_store_int("out_replay_zoom", 10), 0, 25);
	return s;
}

void sv_output_set_settings(const SvOutSettings &settings)
{
	sv_store_batch_begin();
	sv_store_set_int("out_bitrate", settings.bitrate);
	sv_store_set_int("out_audio_track", settings.audio_track);
	sv_store_set_string("out_encoder", settings.encoder);
	sv_store_set_string("out_folder", settings.folder);
	sv_store_set_int("out_backtrack_seconds", settings.backtrack_seconds);
	sv_store_set_int("out_delay_seconds", settings.delay_seconds);
	sv_store_set_int("out_follow_obs", settings.follow_obs ? 1 : 0);
	sv_store_set_int("out_record_with_live", settings.record_with_live ? 1 : 0);
	sv_store_set_int("out_audio_bitrate", settings.audio_bitrate);
	sv_store_set_int("out_reconnect_tries", settings.reconnect_tries);
	sv_store_set_int("out_reconnect_wait", settings.reconnect_wait);
	sv_store_set_int("out_backtrack_auto", settings.backtrack_auto);
	sv_store_set_int("out_size", settings.size);
	sv_store_set_int("out_fps", settings.fps);
	sv_store_set_int("out_fps_div", settings.fps_div);
	sv_store_set_int("out_sharp_scale", settings.sharp_scale ? 1 : 0);
	sv_store_set_int("out_keyframe_sec", settings.keyframe_sec);
	sv_store_set_int("out_effort5", settings.effort);
	sv_store_set_string("out_preset", settings.preset);
	sv_store_set_int("out_rec_format", settings.rec_format);
	sv_store_set_int("out_rec_mode", settings.rec_quality);
	sv_store_set_int("out_rec_level", settings.rec_level);
	sv_store_set_int("out_rec_bitrate", settings.record_bitrate);
	sv_store_set_int("out_split_mode", settings.split_mode);
	sv_store_set_int("out_split_minutes", settings.split_minutes);
	sv_store_set_int("out_split_mb", settings.split_mb);
	sv_store_set_int("out_rec_game_name", settings.rec_game_name ? 1 : 0);
	sv_store_set_int("out_rec_tracks", (int)settings.rec_tracks);
	sv_store_set_int("out_rec_codec", settings.rec_codec);
	sv_store_set_int("out_codec_youtube", settings.codec_youtube ? 1 : 0);
	sv_store_set_int("out_clip_after", settings.clip_after);
	sv_store_set_string("out_clip_folder", settings.clip_folder);
	sv_store_set_int("out_clip_mode", settings.clip_quality);
	sv_store_set_int("out_clip_level", settings.clip_level);
	sv_store_set_int("out_clip_bitrate", settings.clip_bitrate);
	sv_store_set_int("out_clip_game_name", settings.clip_game_name ? 1 : 0);
	sv_store_set_int("out_clip_max_mb", settings.clip_max_mb);
	{
		std::vector<std::string> list = settings.rec_canvases;
		if (list.empty())
			list.push_back(settings.rec_canvas);
		std::string many;
		for (const std::string &c : list)
			many += (many.empty() ? "" : ",") + (c.empty() ? std::string("main") : c);
		sv_store_set_string("out_rec_canvas", list.front());
		sv_store_set_string("out_rec_canvases", many);
	}
	sv_store_set_string("out_clip_canvas", settings.clip_canvas);
	sv_store_set_int("out_replay_slow", settings.replay_slow);
	sv_store_set_int("out_replay_slow_whole", settings.replay_slow_whole ? 1 : 0);
	sv_store_set_int("out_replay_slow_press", settings.replay_slow_press ? 1 : 0);
	sv_store_set_int("out_replay_slow_seconds", settings.replay_slow_seconds);
	sv_store_set_int("out_replay_sound", settings.replay_sound ? 1 : 0);
	sv_store_set_int("out_replay_seconds", settings.replay_seconds);
	sv_store_set_int("out_replay_tag", settings.replay_tag ? 1 : 0);
	sv_store_set_int("out_replay_auto", settings.replay_auto ? 1 : 0);
	sv_store_set_int("out_replay_transition", settings.replay_transition);
	sv_store_set_int("out_replay_line", settings.replay_line ? 1 : 0);
	sv_store_set_int("out_replay_cam_live", settings.replay_cam_live ? 1 : 0);
	sv_store_set_int("out_replay_zoom", settings.replay_zoom);
	sv_store_batch_end();
	std::lock_guard<std::mutex> lock(g_mtx);
	g_skip_encoders = 0;
	g_codec_failed = false;
}

std::vector<SvEncoderChoice> sv_output_encoders()
{
	std::vector<SvEncoderChoice> list;
	for (size_t i = 0; i < ENCODER_COUNT; i++) {
		if (!obs_get_encoder_codec(ENCODER_ORDER[i]))
			continue;
		const char *name = obs_encoder_get_display_name(ENCODER_ORDER[i]);
		list.push_back({ENCODER_ORDER[i], name ? name : ENCODER_ORDER[i]});
	}
	return list;
}

std::string sv_output_auto_encoder()
{
	for (size_t i = 0; i < ENCODER_COUNT; i++)
		if (obs_get_encoder_codec(ENCODER_ORDER[i]))
			return ENCODER_ORDER[i];
	return std::string();
}

std::string sv_output_codec_encoder(const std::string &encoder, int codec)
{
	return codec == SV_CODEC_H264 ? encoder : codec_variant(encoder, codec);
}

std::vector<SvPreset> sv_output_presets(const std::string &encoder, std::string *usual)
{
	const PresetDef *defs = nullptr;
	size_t n = 0;
	const char *def = "";
	switch (family_of(encoder)) {
	case Family::Nvenc:
		defs = NVENC_PRESETS;
		n = sizeof(NVENC_PRESETS) / sizeof(NVENC_PRESETS[0]);
		def = "p5";
		break;
	case Family::X264:
		defs = X264_PRESETS;
		n = sizeof(X264_PRESETS) / sizeof(X264_PRESETS[0]);
		def = "veryfast";
		break;
	case Family::Amf:
		defs = AMF_PRESETS;
		n = sizeof(AMF_PRESETS) / sizeof(AMF_PRESETS[0]);
		def = "quality";
		break;
	case Family::Qsv:
		defs = QSV_PRESETS;
		n = sizeof(QSV_PRESETS) / sizeof(QSV_PRESETS[0]);
		def = "TU4";
		break;
	default:
		break;
	}
	std::vector<SvPreset> list;
	for (size_t i = 0; i < n; i++)
		list.push_back({defs[i].id, T(defs[i].words)});
	if (usual)
		*usual = def;
	return list;
}

std::string sv_output_preset_for(const std::string &encoder, const SvOutSettings &set)
{
	std::string usual;
	const std::vector<SvPreset> list = sv_output_presets(encoder, &usual);
	if (list.empty())
		return std::string();
	for (const SvPreset &p : list)
		if (p.id == set.preset)
			return p.id;
	/* Settings from before presets had names: the middle step was always OBS's usual one. */
	const int e = std::clamp(set.effort, 0, 4);
	if (e == 2)
		return usual;
	switch (family_of(encoder)) {
	case Family::Nvenc:
		return NVENC_EFFORT[e];
	case Family::X264:
		return X264_EFFORT[e];
	case Family::Amf:
		return AMF_EFFORT[e];
	case Family::Qsv:
		return QSV_EFFORT[e];
	default:
		return usual;
	}
}

namespace {

/* OBS's own preset, as one of the vertical encoder's presets, or empty when it is not one.
   OBS's simple mode names Intel's presets speed, balanced and quality. */
std::string our_preset(const std::string &encoder, std::string preset)
{
	if (family_of(encoder) == Family::Qsv) {
		if (preset == "speed")
			preset = "TU7";
		else if (preset == "balanced")
			preset = "TU4";
		else if (preset == "quality")
			preset = "TU1";
	}
	for (const SvPreset &p : sv_output_presets(encoder))
		if (p.id == preset)
			return preset;
	return std::string();
}

/* OBS's encoder, as one the vertical can use: same maker, always H.264. */
std::string our_encoder(const std::string &theirs)
{
	std::string want;
	if (theirs.find("nvenc") != std::string::npos)
		want = "obs_nvenc_h264_tex";
	else if (theirs.find("amf") != std::string::npos || theirs.find("amd") != std::string::npos)
		want = "h264_texture_amf";
	else if (theirs.find("qsv") != std::string::npos)
		want = "obs_qsv11_v2";
	else if (theirs.find("x264") != std::string::npos)
		want = "obs_x264";
	return !want.empty() && obs_get_encoder_codec(want.c_str()) ? want : std::string();
}

/* The keyframe OBS's own stream ends up with: the streaming service's rule (Twitch and YouTube ask
   for 2 s) on top of the encoder's own, the way OBS works it out when it starts streaming. 0 means
   OBS leaves it on automatic. */
int keyframe_after_service(int keyint_sec)
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return keyint_sec;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "keyint_sec", keyint_sec);
	obs_service_apply_encoder_settings(service, d, nullptr);
	const int enforced = (int)obs_data_get_int(d, "keyint_sec");
	obs_data_release(d);
	return keyint_sec != 0 && keyint_sec < enforced ? keyint_sec : enforced;
}

} // namespace

SvMainEncoder sv_output_main_encoder()
{
	SvMainEncoder m;
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return m;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && strcmp(mode, "Advanced") == 0) {
		const char *enc = config_get_string(cfg, "AdvOut", "Encoder");
		const std::string id = enc ? enc : "";
		m.encoder = our_encoder(id);
		char *dir = obs_frontend_get_current_profile_path();
		obs_data_t *st = nullptr;
		if (dir) {
			const std::string path = std::string(dir) + "/streamEncoder.json";
			st = obs_data_create_from_json_file_safe(path.c_str(), "bak");
			bfree(dir);
		}
		obs_data_t *def = id.empty() ? nullptr : obs_encoder_defaults(id.c_str());
		auto get_int = [&](const char *k) {
			if (st && obs_data_has_user_value(st, k))
				return (int)obs_data_get_int(st, k);
			return def ? (int)obs_data_get_int(def, k) : 0;
		};
		auto get_str = [&](const char *k) {
			const char *v = st && obs_data_has_user_value(st, k) ? obs_data_get_string(st, k)
									    : def ? obs_data_get_string(def, k) : "";
			return std::string(v ? v : "");
		};
		m.bitrate = get_int("bitrate");
		int kf = get_int("keyint_sec");
		if (config_get_bool(cfg, "AdvOut", "ApplyServiceSettings"))
			kf = keyframe_after_service(kf);
		m.keyframe_auto = kf <= 0;
		m.keyframe_sec = kf > 0 ? kf : 2;
		std::string preset = get_str("preset");
		const bool pNumber = preset.size() == 2 && preset[0] == 'p' && preset[1] >= '1' && preset[1] <= '7';
		if (preset.empty() || (id.find("nvenc") != std::string::npos && !pNumber)) {
			const std::string p2 = get_str("preset2");
			if (!p2.empty())
				preset = p2;
		}
		if (id.find("qsv") != std::string::npos)
			preset = get_str("target_usage");
		m.preset = our_preset(m.encoder, preset);
		obs_data_release(st);
		obs_data_release(def);
	} else {
		const char *enc = config_get_string(cfg, "SimpleOutput", "StreamEncoder");
		const std::string id = enc ? enc : "";
		m.encoder = our_encoder(id);
		m.bitrate = (int)config_get_uint(cfg, "SimpleOutput", "VBitrate");
		/* Simple mode always takes the streaming service's rule. */
		const int kf = keyframe_after_service(0);
		m.keyframe_auto = kf <= 0;
		m.keyframe_sec = kf > 0 ? kf : 2;
		const char *key = id.find("nvenc") != std::string::npos ? "NVENCPreset2"
				  : id.find("amd") != std::string::npos ? "AMDPreset"
				  : id.find("qsv") != std::string::npos ? "QSVPreset"
									  : "Preset";
		const char *preset = config_get_string(cfg, "SimpleOutput", key);
		m.preset = our_preset(m.encoder, preset ? preset : "");
		if (id == "x264_lowcpu")
			m.preset = "ultrafast";
	}
	m.found = !m.encoder.empty() || m.bitrate > 0;
	return m;
}

std::vector<std::string> sv_output_record_canvases(const SvOutSettings &set)
{
	std::vector<std::string> list;
	const std::string mainId = sv_canvas_main_id();
	for (const std::string &c : set.rec_canvases) {
		/* A canvas that has gone is left out; the main one may be kept by its own id. */
		const std::string canvas = c == mainId ? std::string() : c;
		if (!canvas.empty() && canvas != SV_WIDE && !sv_canvas_exists(canvas))
			continue;
		if (std::find(list.begin(), list.end(), canvas) == list.end())
			list.push_back(canvas);
	}
	if (list.empty())
		list.push_back(std::string());
	return list;
}

std::string sv_output_record_folder()
{
	return record_folder(sv_output_settings());
}

void sv_output_shutdown()
{
	sv_output_set_listener(nullptr);
	stop_after_thread();

	std::vector<std::pair<obs_output_t *, obs_service_t *>> parts;
	std::vector<Run *> runs;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (auto &kv : g_runs) {
			parts.push_back({kv.second.out, kv.second.svc});
			runs.push_back(&kv.second);
			kv.second.out = nullptr;
			kv.second.svc = nullptr;
		}
	}
	for (size_t i = 0; i < parts.size(); i++) {
		if (parts[i].first && obs_output_active(parts[i].first))
			obs_output_force_stop(parts[i].first);
		release_parts(parts[i].first, parts[i].second, runs[i]);
	}
	release_encoders(); /* every output is gone by now */
	sv_canvas_set_output_canvases({});

	std::lock_guard<std::mutex> lock(g_mtx);
	g_runs.clear();
	for (SvDest &d : g_dests)
		wipe(d.key);
	g_dests.clear();
}
