/*
Simply Vertical
Copyright (C) 2026 SimplyNickish
*/

#include "sv-sound.h"
#include "sv-store.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace {

const char COPY_ID[] = "sv_sound_copy";
const char STORE_KEY[] = "sound_desk";
/* One copy per channel of our own canvas, and a canvas has 64. */
constexpr int MAX_COPIES = 64;

struct Level {
	int volume = 100;
	bool muted = false;
};

/* One source's copy for the vertical. */
struct Copy {
	obs_source_t *orig = nullptr; /* held, so it cannot go away while its sound is copied */
	obs_source_t *clone = nullptr;
	obs_volmeter_t *meter = nullptr;
	int channel = -1;
	std::atomic<bool> follow_mute{true};
	std::atomic<float> peak{-100.0f};
	std::vector<float> zeros; /* only touched by the original's own sound thread */
};

std::mutex g_mtx; /* guards g_desk and g_levels */
SvSoundDesk g_desk;
std::map<std::string, Level> g_levels;
/* Source uuid: the track taken off it while the desk is on. UI thread only. */
std::map<std::string, int> g_moved;
/* UI thread only. */
std::map<std::string, std::unique_ptr<Copy>> g_copies;
obs_canvas_t *g_canvas = nullptr;
QTimer *g_timer = nullptr;
bool g_paused = true;

/* ---------- the hidden copy source ---------- */

const char *copy_name(void *)
{
	return "Simply Vertical sound copy";
}

void *copy_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

void copy_destroy(void *) {}

void register_copy()
{
	obs_source_info info = {};
	info.id = COPY_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* Never offered in OBS's Add Source menu. */
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = copy_name;
	info.create = copy_create;
	info.destroy = copy_destroy;
	obs_register_source(&info);
}

/* ---------- saving ---------- */

void save_locked()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "own", g_desk.own);
	obs_data_set_int(d, "track", g_desk.track);
	obs_data_set_int(d, "master", g_desk.master);
	obs_data_set_bool(d, "follow_mute", g_desk.follow_mute);
	obs_data_array_t *levels = obs_data_array_create();
	for (const auto &[uuid, l] : g_levels) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "uuid", uuid.c_str());
		obs_data_set_int(o, "v", l.volume);
		obs_data_set_bool(o, "m", l.muted);
		obs_data_array_push_back(levels, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "sources", levels);
	obs_data_array_release(levels);
	obs_data_array_t *moved = obs_data_array_create();
	for (const auto &[uuid, t] : g_moved) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "uuid", uuid.c_str());
		obs_data_set_int(o, "t", t);
		obs_data_array_push_back(moved, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "moved", moved);
	obs_data_array_release(moved);
	const char *json = obs_data_get_json(d);
	sv_store_set_string(STORE_KEY, json ? json : "");
	obs_data_release(d);
}

void save()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	save_locked();
}

/* Reads the desk and the volumes from a saved string. The moved list only when asked. */
void read_locked(const std::string &json, bool withMoved)
{
	g_desk = SvSoundDesk();
	g_levels.clear();
	if (json.empty())
		return;
	obs_data_t *d = obs_data_create_from_json(json.c_str());
	if (!d)
		return;
	obs_data_set_default_int(d, "track", 6);
	obs_data_set_default_int(d, "master", 100);
	obs_data_set_default_bool(d, "follow_mute", true);
	g_desk.own = obs_data_get_bool(d, "own");
	g_desk.track = std::clamp((int)obs_data_get_int(d, "track"), 1, 6);
	g_desk.master = std::clamp((int)obs_data_get_int(d, "master"), 0, 200);
	g_desk.follow_mute = obs_data_get_bool(d, "follow_mute");
	obs_data_array_t *levels = obs_data_get_array(d, "sources");
	for (size_t i = 0, n = obs_data_array_count(levels); i < n; i++) {
		obs_data_t *o = obs_data_array_item(levels, i);
		const char *uuid = obs_data_get_string(o, "uuid");
		if (uuid && *uuid)
			g_levels[uuid] = Level{std::clamp((int)obs_data_get_int(o, "v"), 0, 200), obs_data_get_bool(o, "m")};
		obs_data_release(o);
	}
	obs_data_array_release(levels);
	if (withMoved) {
		g_moved.clear();
		obs_data_array_t *moved = obs_data_get_array(d, "moved");
		for (size_t i = 0, n = obs_data_array_count(moved); i < n; i++) {
			obs_data_t *o = obs_data_array_item(moved, i);
			const char *uuid = obs_data_get_string(o, "uuid");
			const int t = (int)obs_data_get_int(o, "t");
			if (uuid && *uuid && t >= 1 && t <= 6)
				g_moved[uuid] = t;
			obs_data_release(o);
		}
		obs_data_array_release(moved);
	}
	obs_data_release(d);
}

/* ---------- copying the sound ---------- */

/* Runs on the original's own sound thread for every piece of sound it makes. */
void on_sound(void *param, obs_source_t *orig, const audio_data *data, bool muted)
{
	auto *c = static_cast<Copy *>(param);
	if (!c->clone || !data || !data->frames)
		return;
	obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai))
		return;
	/* Silent when OBS would not send it either: muted (if the member wants that), not on any
	   canvas, or set to monitor only. */
	const bool silent = (muted && c->follow_mute.load()) || !obs_source_active(orig) ||
			    obs_source_get_monitoring_type(orig) == OBS_MONITORING_TYPE_MONITOR_ONLY;
	obs_source_audio out = {};
	out.frames = data->frames;
	out.timestamp = data->timestamp;
	out.speakers = oai.speakers;
	out.samples_per_sec = oai.samples_per_sec;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	const size_t channels = get_audio_channels(oai.speakers);
	if (silent) {
		if (c->zeros.size() < data->frames)
			c->zeros.assign(data->frames, 0.0f);
		for (size_t i = 0; i < channels && i < MAX_AV_PLANES; i++)
			out.data[i] = reinterpret_cast<const uint8_t *>(c->zeros.data());
	} else {
		for (size_t i = 0; i < channels && i < MAX_AV_PLANES; i++)
			out.data[i] = data->data[i];
	}
	obs_source_output_audio(c->clone, &out);
}

void on_meter(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
	      const float[MAX_AUDIO_CHANNELS])
{
	auto *c = static_cast<Copy *>(param);
	float top = -100.0f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
		if (std::isfinite(peak[i]))
			top = std::max(top, peak[i]);
	c->peak.store(top);
}

void drop(std::unique_ptr<Copy> &c)
{
	if (!c)
		return;
	if (c->orig)
		obs_source_remove_audio_capture_callback(c->orig, on_sound, c.get());
	if (c->meter) {
		obs_volmeter_remove_callback(c->meter, on_meter, c.get());
		obs_volmeter_detach_source(c->meter);
		obs_volmeter_destroy(c->meter);
	}
	if (g_canvas && c->channel >= 0)
		obs_canvas_set_channel(g_canvas, (uint32_t)c->channel, nullptr);
	obs_source_release(c->clone);
	obs_source_release(c->orig);
	c.reset();
}

void drop_all()
{
	for (auto &[uuid, c] : g_copies)
		drop(c);
	g_copies.clear();
	if (g_canvas) {
		obs_canvas_remove(g_canvas);
		obs_canvas_release(g_canvas);
		g_canvas = nullptr;
	}
}

bool ensure_canvas()
{
	if (g_canvas)
		return true;
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return false;
	/* No picture is ever made from it, so it is as small as OBS allows. */
	ovi.base_width = 32;
	ovi.base_height = 32;
	ovi.output_width = 32;
	ovi.output_height = 32;
	g_canvas = obs_canvas_create_private("Simply Vertical sound", &ovi, MIX_AUDIO | ACTIVATE | EPHEMERAL);
	if (!g_canvas)
		obs_log(LOG_ERROR, "the vertical sound mix could not start");
	return g_canvas != nullptr;
}

int free_channel()
{
	bool used[MAX_COPIES] = {};
	for (const auto &[uuid, c] : g_copies)
		if (c && c->channel >= 0 && c->channel < MAX_COPIES)
			used[c->channel] = true;
	for (int i = 0; i < MAX_COPIES; i++)
		if (!used[i])
			return i;
	return -1;
}

std::unique_ptr<Copy> make_copy(obs_source_t *orig, int channel)
{
	auto c = std::make_unique<Copy>();
	const std::string name = std::string("Simply Vertical copy of ") + obs_source_get_name(orig);
	c->clone = obs_source_create_private(COPY_ID, name.c_str(), nullptr);
	if (!c->clone)
		return nullptr;
	c->orig = obs_source_get_ref(orig);
	c->channel = channel;
	c->meter = obs_volmeter_create(OBS_FADER_LOG);
	if (c->meter) {
		obs_volmeter_attach_source(c->meter, c->clone);
		obs_volmeter_add_callback(c->meter, on_meter, c.get());
	}
	obs_canvas_set_channel(g_canvas, (uint32_t)channel, c->clone);
	obs_source_add_audio_capture_callback(c->orig, on_sound, c.get());
	return c;
}

/* Puts back every track taken off a source, except the one the desk is using now. */
void put_back(int keep)
{
	for (auto it = g_moved.begin(); it != g_moved.end();) {
		if (it->second == keep) {
			++it;
			continue;
		}
		obs_source_t *s = obs_get_source_by_uuid(it->first.c_str());
		if (!s) {
			++it; /* in another scene collection; put back when that one is open */
			continue;
		}
		obs_source_set_audio_mixers(s, obs_source_get_audio_mixers(s) | (1u << (it->second - 1)));
		obs_source_release(s);
		it = g_moved.erase(it);
	}
}

/* Keeps the copies in step with OBS: new and removed sources, mutes, volumes, sync offsets,
   and the desk's track clear of everything but the copies. UI thread only. */
void sync()
{
	if (g_paused)
		return;
	SvSoundDesk desk;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		desk = g_desk;
	}
	const size_t movedBefore = g_moved.size();
	bool dirty = false;
	put_back(desk.own ? desk.track : 0);
	if (g_moved.size() != movedBefore)
		dirty = true;
	if (!desk.own) {
		drop_all();
		if (dirty)
			save();
		return;
	}
	if (!ensure_canvas())
		return;

	std::vector<obs_source_t *> list;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT &&
			    (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) && !obs_source_removed(s) &&
			    strcmp(obs_source_get_unversioned_id(s), COPY_ID) != 0)
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(s));
			return true;
		},
		&list);

	const uint32_t bit = 1u << (desk.track - 1);
	std::set<std::string> seen;
	for (obs_source_t *s : list) {
		const char *u = obs_source_get_uuid(s);
		if (!u)
			continue;
		const std::string uuid = u;
		seen.insert(uuid);
		const uint32_t mixers = obs_source_get_audio_mixers(s);
		if (mixers & bit) {
			obs_source_set_audio_mixers(s, mixers & ~bit);
			g_moved[uuid] = desk.track;
			dirty = true;
		}
		Level level;
		{
			std::lock_guard<std::mutex> lock(g_mtx);
			auto it = g_levels.find(uuid);
			if (it == g_levels.end()) {
				/* A new source starts at its main volume, so the vertical sounds the same at first. */
				const int v = (int)std::lround(obs_source_get_volume(s) * 100.0f);
				it = g_levels.emplace(uuid, Level{std::clamp(v, 0, 200), false}).first;
				dirty = true;
			}
			level = it->second;
		}
		auto &c = g_copies[uuid];
		if (!c) {
			const int ch = free_channel();
			if (ch < 0)
				continue;
			c = make_copy(s, ch);
			if (!c)
				continue;
		}
		c->follow_mute.store(desk.follow_mute);
		const float vol = (float)level.volume / 100.0f * (float)desk.master / 100.0f;
		if (std::abs(obs_source_get_volume(c->clone) - vol) > 0.0001f)
			obs_source_set_volume(c->clone, vol);
		if (obs_source_muted(c->clone) != level.muted)
			obs_source_set_muted(c->clone, level.muted);
		if (obs_source_get_audio_mixers(c->clone) != bit)
			obs_source_set_audio_mixers(c->clone, bit);
		const int64_t off = obs_source_get_sync_offset(s);
		if (obs_source_get_sync_offset(c->clone) != off)
			obs_source_set_sync_offset(c->clone, off);
	}
	for (obs_source_t *s : list)
		obs_source_release(s);

	for (auto it = g_copies.begin(); it != g_copies.end();) {
		if (!it->second || !seen.count(it->first) || obs_source_removed(it->second->orig)) {
			drop(it->second);
			it = g_copies.erase(it);
		} else {
			++it;
		}
	}
	if (dirty)
		save();
}

} // namespace

void sv_sound_load()
{
	register_copy();
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		read_locked(sv_store_string(STORE_KEY), true);
	}
	g_paused = true;
	g_timer = new QTimer();
	g_timer->setInterval(500);
	QObject::connect(g_timer, &QTimer::timeout, [] { sync(); });
	g_timer->start();
}

void sv_sound_unload()
{
	if (g_timer) {
		g_timer->stop();
		delete g_timer;
		g_timer = nullptr;
	}
	drop_all();
}

void sv_sound_pause(bool paused)
{
	g_paused = paused;
	if (paused)
		drop_all();
	else
		sync();
}

SvSoundDesk sv_sound_desk()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_desk;
}

void sv_sound_set_desk(const SvSoundDesk &desk)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_desk = desk;
		g_desk.track = std::clamp(g_desk.track, 1, 6);
		g_desk.master = std::clamp(g_desk.master, 0, 200);
		save_locked();
	}
	sync();
}

std::vector<SvSoundSource> sv_sound_sources()
{
	std::vector<SvSoundSource> out;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT ||
			    !(obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) || obs_source_removed(s))
				return true;
			SvSoundSource row;
			row.uuid = obs_source_get_uuid(s) ? obs_source_get_uuid(s) : "";
			row.name = obs_source_get_name(s) ? obs_source_get_name(s) : "";
			row.volume = std::clamp((int)std::lround(obs_source_get_volume(s) * 100.0f), 0, 200);
			static_cast<std::vector<SvSoundSource> *>(param)->push_back(row);
			return true;
		},
		&out);
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		for (SvSoundSource &row : out) {
			auto it = g_levels.find(row.uuid);
			if (it != g_levels.end()) {
				row.volume = it->second.volume;
				row.muted = it->second.muted;
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const SvSoundSource &a, const SvSoundSource &b) {
		return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
	});
	return out;
}

void sv_sound_set_source(const std::string &uuid, int volume, bool muted)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		g_levels[uuid] = Level{std::clamp(volume, 0, 200), muted};
		save_locked();
	}
	sync();
}

float sv_sound_level(const std::string &uuid)
{
	auto it = g_copies.find(uuid);
	if (it == g_copies.end() || !it->second)
		return 0.0f;
	/* OBS measures before the mute, so a muted copy would still show sound. */
	if (obs_source_muted(it->second->clone))
		return 0.0f;
	const float db = it->second->peak.load();
	if (db <= -60.0f)
		return 0.0f;
	return std::min(1.0f, (db + 60.0f) / 60.0f);
}

int sv_sound_track(int chosen)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_desk.own ? g_desk.track : chosen;
}

unsigned sv_sound_main_tracks()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 1;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && strcmp(mode, "Advanced") == 0) {
		const int stream = (int)config_get_int(cfg, "AdvOut", "TrackIndex");
		unsigned mask = (unsigned)config_get_int(cfg, "AdvOut", "RecTracks") & 63u;
		if (stream >= 1 && stream <= 6)
			mask |= 1u << (stream - 1);
		return mask ? mask : 1u;
	}
	unsigned mask = 1u;
	mask |= (unsigned)config_get_int(cfg, "SimpleOutput", "RecTracks") & 63u;
	return mask;
}

std::string sv_sound_snapshot()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	save_locked();
	return sv_store_string(STORE_KEY);
}

void sv_sound_restore(const std::string &snapshot)
{
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		read_locked(snapshot, false);
		save_locked();
	}
	sync();
}
