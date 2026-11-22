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

// Linking this PC to the member's account (see sv-link.h).
//
// Threads: one link thread does every network call and owns the identity and
// the connection; one reader thread per connection waits for messages. The
// reader only hands layouts on and raises flags; the link thread acts on them.
//
// Server side: the website's plugin addresses (/api/plugin/...).

#include "sv-link.h"
#include "sv-config.h"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t MAX_HTTP_BODY = 4 * 1024 * 1024; /* twelve saved layouts at their largest */
constexpr size_t MAX_WS_MESSAGE = 512 * 1024;
constexpr auto HEARTBEAT = std::chrono::seconds(25);
constexpr auto WAIT_LIMIT = std::chrono::minutes(10); /* a link request lasts ten minutes */
constexpr auto SAVE_GAP = std::chrono::seconds(2);    /* live moves are written to disk at most this often */
const int BACKOFF_S[] = {2, 5, 15, 30, 60};

/* ---------- small helpers ---------- */

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string narrow(const std::wstring &w)
{
	if (w.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

bool is_hex_lower(const std::string &s, size_t n)
{
	if (s.size() != n)
		return false;
	for (char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}

bool is_uuid(const std::string &s)
{
	if (s.size() != 36)
		return false;
	for (size_t i = 0; i < s.size(); i++) {
		const char c = s[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-')
				return false;
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

/* Tokens go straight into JSON text and headers, so only their own characters are allowed. */
bool is_token(const std::string &s)
{
	if (s.size() < 8 || s.size() > 4096)
		return false;
	for (char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
		      c == '_' || c == '.'))
			return false;
	return true;
}

bool is_code(const std::string &s)
{
	if (s.size() != 6)
		return false;
	for (char c : s)
		if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9')))
			return false;
	return true;
}

const char PLUG_PREFIX[] = "svert-plug:";
bool is_channel(const std::string &s)
{
	const size_t n = sizeof(PLUG_PREFIX) - 1;
	return s.size() == n + 32 && s.compare(0, n, PLUG_PREFIX) == 0 && is_hex_lower(s.substr(n), 32);
}

std::string str(obs_data_t *d, const char *key)
{
	const char *v = d ? obs_data_get_string(d, key) : nullptr;
	return v ? v : "";
}

std::string to_hex(const std::string &bytes)
{
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (unsigned char c : bytes) {
		out.push_back(digits[c >> 4]);
		out.push_back(digits[c & 15]);
	}
	return out;
}

bool from_hex(const std::string &hex, std::string &bytes)
{
	if (hex.size() % 2)
		return false;
	bytes.clear();
	bytes.reserve(hex.size() / 2);
	auto val = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = val(hex[i]), lo = val(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		bytes.push_back((char)(hi * 16 + lo));
	}
	return true;
}

/* ---------- files in OBS's plugin settings folder ---------- */

std::string config_file(const char *name)
{
	char *p = obs_module_config_path(name);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

bool write_text(const char *name, const std::string &text)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string path = config_file(name);
	return !path.empty() && os_quick_write_utf8_file_safe(path.c_str(), text.c_str(), text.size(), false, "tmp", nullptr);
}

std::string read_text(const char *name)
{
	const std::string path = config_file(name);
	char *t = path.empty() ? nullptr : os_quick_read_utf8_file(path.c_str());
	std::string s = t ? t : "";
	bfree(t);
	return s;
}

void remove_file(const char *name)
{
	const std::string path = config_file(name);
	if (!path.empty())
		os_unlink(path.c_str());
}

/* ---------- the PC's key, locked by Windows (DPAPI, this Windows user only) ---------- */

const char ENTROPY[] = "simply-vertical-identity-v1";

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

/* ---------- HTTPS ---------- */

HINTERNET g_session = nullptr;

struct Resp {
	int status = 0; /* 0: could not reach the server */
	std::string body;
};

struct Url {
	std::wstring host, path;
	INTERNET_PORT port = 0;
	bool secure = false;
};

bool crack(const std::string &url, Url &out)
{
	const std::wstring w = widen(url);
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	uc.dwHostNameLength = (DWORD)-1;
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc))
		return false;
	out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
	out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
	if (uc.lpszExtraInfo)
		out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
	out.port = uc.nPort;
	out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
	return !out.host.empty();
}

/* One request. `supabase` adds the public key the database's sign-in service wants. */
Resp http(const wchar_t *method, const std::string &url, const std::string &bearer, const std::string *body, bool supabase)
{
	Resp r;
	Url u;
	if (!g_session || !crack(url, u))
		return r;
	HINTERNET con = WinHttpConnect(g_session, u.host.c_str(), u.port, 0);
	if (!con)
		return r;
	HINTERNET req = WinHttpOpenRequest(con, method, u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
					   WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
	if (req) {
		/* A token must never follow a redirect to somewhere else. */
		DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
		std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
		if (supabase)
			headers += L"apikey: " + widen(SV_SUPABASE_ANON_KEY) + L"\r\n";
		if (!bearer.empty())
			headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
		LPVOID data = body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA;
		const DWORD len = body ? (DWORD)body->size() : 0;
		if (WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, data, len, len, 0) &&
		    WinHttpReceiveResponse(req, nullptr)) {
			DWORD code = 0, size = sizeof(code);
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
			std::string out;
			bool ok = true;
			for (;;) {
				DWORD avail = 0;
				if (!WinHttpQueryDataAvailable(req, &avail)) {
					ok = false;
					break;
				}
				if (!avail)
					break;
				if (out.size() + avail > MAX_HTTP_BODY) {
					ok = false;
					break;
				}
				const size_t at = out.size();
				out.resize(at + avail);
				DWORD got = 0;
				if (!WinHttpReadData(req, out.data() + at, avail, &got)) {
					ok = false;
					break;
				}
				out.resize(at + got);
			}
			if (ok) {
				r.status = (int)code;
				r.body = std::move(out);
			}
		}
		WinHttpCloseHandle(req);
	}
	WinHttpCloseHandle(con);
	return r;
}

/* ---------- shared state ---------- */

enum class St { NotLinked, Waiting, Linked };

struct Preset {
	std::string id;
	std::string name;
	std::string drawList;
	std::string game;
};

/* One program and the game it is, for Follow my game. */
struct GameExe {
	std::string game;
	std::string exe; /* lower case; ending in * matches any name starting with it */
};

/* A layer moved in the canvas panel, waiting for the link thread to save it. */
struct PieceMove {
	std::string preset, piece;
	float x = 0, y = 0, w = 0, h = 0;
	std::function<void(bool, const std::string &)> done;
};

struct Link {
	std::mutex m;
	std::condition_variable cv;
	bool quit = false;

	/* Raised by buttons and the reader, taken by the link thread. Under m. */
	bool wantLink = false, wantUnlink = false, wantLayout = false, wantPresets = false, gotUnlinked = false,
	     socketDead = false;
	bool devJoined = false;
	bool plugRefused = false, plugOk = false; /* replies to joining the private channel */

	/* What the settings window shows. Under m. */
	St st = St::NotLinked;
	bool offline = false;
	std::string code;
	std::string problem; /* a locale key, or empty */
	Clock::time_point waitUntil{};

	/* Under m: read by the reader thread. */
	std::string userId;
	std::string channel;    /* the private channel the server gave us */
	std::string account;    /* the member's name, for "Linked to <name>". Empty when the site sends none */
	std::string joinedPlug; /* the one joined on the current connection */
	std::string layout;     /* the website's current layout */
	std::string layoutGame; /* the game it was made for */
	std::vector<Preset> presets;
	std::vector<GameExe> gameExes;
	std::string activePreset; /* the saved layout picked in OBS, empty when following the website */
	bool hold = false;        /* Hold changes: the website's moves wait */
	std::string heldLayout;   /* what the stream showed when Hold changes went on */
	bool layoutDirty = false;
	Clock::time_point layoutSaved{};
	std::vector<PieceMove> moves; /* layers moved in the canvas panel, oldest first */
	/* How far the OBS setup has got, for the website's setup guide: what the panel
	   says now, and what the website last took. Sent only when they differ. */
	std::string setupWant, setupSent;
	Clock::time_point setupRetry{};

	std::string newVersion; /* a newer plugin on the website, empty when up to date. Under m. */

	/* Link thread only. */
	std::string refreshToken, accessToken;
	Clock::time_point accessUntil{};
	std::thread worker;
	std::thread updateCheck;
} g;

std::mutex g_lm; /* listeners */
std::map<int, SvLinkListener> g_listeners;
int g_nextId = 1;

void deliver_layout(const std::string &json)
{
	std::lock_guard<std::mutex> lock(g_lm);
	for (auto &it : g_listeners)
		if (it.second.onLayout)
			it.second.onLayout(json);
}

void notify_status()
{
	std::lock_guard<std::mutex> lock(g_lm);
	for (auto &it : g_listeners)
		if (it.second.onStatus)
			it.second.onStatus();
}

/* What the sources should paint right now. Under g.m. */
const std::string &showing_locked()
{
	if (!g.activePreset.empty()) {
		for (const Preset &p : g.presets) {
			if (p.id == g.activePreset)
				return p.drawList;
		}
	}
	return g.hold ? g.heldLayout : g.layout;
}

/* A new layout from the server or a live move. Never called with g.m held. */
std::string game_of(const std::string &json)
{
	obs_data_t *d = json.empty() ? nullptr : obs_data_create_from_json(json.c_str());
	const char *v = d ? obs_data_get_string(d, "game") : nullptr;
	std::string game = v ? v : "";
	obs_data_release(d);
	return game;
}

void accept_layout(const std::string &json)
{
	bool show;
	const std::string game = game_of(json);
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.layout = json;
		g.layoutGame = game;
		g.layoutDirty = true;
		show = g.activePreset.empty() && !g.hold;
	}
	if (show)
		deliver_layout(json);
}

/* ---------- identity ---------- */

void save_identity()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "id", g.userId.c_str());
	obs_data_set_string(d, "rt", g.refreshToken.c_str());
	const char *json = obs_data_get_json(d);
	std::string blob;
	if (json && protect(json, blob))
		write_text("identity.dat", to_hex(blob));
	else
		obs_log(LOG_WARNING, "link: could not lock the PC key with Windows");
	obs_data_release(d);
}

void load_identity()
{
	std::string blob, plain;
	const std::string hex = read_text("identity.dat");
	if (hex.empty() || !from_hex(hex, blob) || !unprotect(blob, plain))
		return;
	obs_data_t *d = obs_data_create_from_json(plain.c_str());
	SecureZeroMemory(plain.data(), plain.size());
	const std::string id = str(d, "id"), rt = str(d, "rt");
	obs_data_release(d);
	if (!is_uuid(id) || !is_token(rt))
		return;
	std::lock_guard<std::mutex> lock(g.m);
	g.userId = id;
	g.refreshToken = rt;
}

void forget_identity()
{
	remove_file("identity.dat");
	std::lock_guard<std::mutex> lock(g.m);
	g.userId.clear();
	g.refreshToken.clear();
	g.accessToken.clear();
}

/* Reads a sign-in answer into the identity. */
bool take_session(const Resp &r)
{
	obs_data_t *d = obs_data_create_from_json(r.body.c_str());
	if (!d)
		return false;
	obs_data_t *user = obs_data_get_obj(d, "user");
	const std::string at = str(d, "access_token"), rt = str(d, "refresh_token"), id = str(user, "id");
	const long long expiresIn = obs_data_get_int(d, "expires_in");
	obs_data_release(user);
	obs_data_release(d);
	if (!is_token(at) || !is_token(rt) || !is_uuid(id))
		return false;
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.userId.empty() && g.userId != id)
			return false;
		g.userId = id;
	}
	{
		/* "Get my key" reads the token from the window's own thread. */
		std::lock_guard<std::mutex> lock(g.m);
		g.accessToken = at;
	}
	g.refreshToken = rt;
	g.accessUntil = Clock::now() + std::chrono::seconds(expiresIn > 60 ? expiresIn : 3600);
	save_identity();
	return true;
}

bool sign_up()
{
	const std::string body = "{\"data\":{}}";
	const Resp r = http(L"POST", std::string(SV_SUPABASE_URL) + "/auth/v1/signup", SV_SUPABASE_ANON_KEY, &body, true);
	return r.status == 200 && take_session(r);
}

enum class Refresh { Ok, Offline, Lost };

Refresh refresh_session()
{
	const std::string body = "{\"refresh_token\":\"" + g.refreshToken + "\"}";
	const Resp r = http(L"POST", std::string(SV_SUPABASE_URL) + "/auth/v1/token?grant_type=refresh_token",
			    SV_SUPABASE_ANON_KEY, &body, true);
	if (r.status == 200 && take_session(r))
		return Refresh::Ok;
	if (r.status == 400 || r.status == 401 || r.status == 403)
		return Refresh::Lost;
	return Refresh::Offline;
}

/* ---------- the live connection (Supabase Realtime, Phoenix protocol) ---------- */

struct Socket {
	HINTERNET con = nullptr;
	HINTERNET ws = nullptr;
	std::thread reader;
	std::mutex sendM;
	unsigned ref = 0;
	std::string devRef, plugRef;
} g_sock;

std::string next_ref()
{
	return std::to_string(++g_sock.ref);
}

bool ws_send(const std::string &text)
{
	std::lock_guard<std::mutex> lock(g_sock.sendM);
	if (!g_sock.ws)
		return false;
	return WinHttpWebSocketSend(g_sock.ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
				    (DWORD)text.size()) == NO_ERROR;
}

std::string phx(const std::string &topic, const char *event, const std::string &payload, const std::string &ref,
		const std::string &joinRef)
{
	std::string m = "{\"topic\":\"" + topic + "\",\"event\":\"" + event + "\",\"payload\":" + payload +
			",\"ref\":\"" + ref + "\"";
	if (!joinRef.empty())
		m += ",\"join_ref\":\"" + joinRef + "\"";
	return m + "}";
}

/* Topics are built only from checked ids and keys, so they are safe inside JSON text. */
bool join(const std::string &topic, std::string &joinRef)
{
	joinRef = next_ref();
	const std::string payload =
		"{\"config\":{\"broadcast\":{\"ack\":false,\"self\":false},\"presence\":{\"key\":\"\",\"enabled\":false},\"postgres_changes\":[],\"private\":true},\"access_token\":\"" +
		g.accessToken + "\"}";
	return ws_send(phx("realtime:" + topic, "phx_join", payload, joinRef, joinRef));
}

void leave(const std::string &topic, std::string &joinRef)
{
	if (!joinRef.empty())
		ws_send(phx("realtime:" + topic, "phx_leave", "{}", next_ref(), joinRef));
	joinRef.clear();
}

void on_message(const std::string &text)
{
	obs_data_t *d = obs_data_create_from_json(text.c_str());
	if (!d)
		return;
	const std::string topic = str(d, "topic"), event = str(d, "event");
	obs_data_t *payload = obs_data_get_obj(d, "payload");
	std::string devT, plugT;
	{
		std::lock_guard<std::mutex> lock(g.m);
		devT = g.userId.empty() ? "" : "realtime:svert-dev:" + g.userId;
		plugT = g.joinedPlug.empty() ? "" : "realtime:" + g.joinedPlug;
	}
	bool wake = false;
	if (event == "broadcast" && payload) {
		const std::string what = str(payload, "event");
		if (!devT.empty() && topic == devT) {
			std::lock_guard<std::mutex> lock(g.m);
			if (what == "linked" || what == "rekey")
				g.wantLayout = wake = true;
			else if (what == "unlinked")
				g.gotUnlinked = wake = true;
		} else if (!plugT.empty() && topic == plugT && what == "presets") {
			std::lock_guard<std::mutex> lock(g.m);
			g.wantPresets = true;
		} else if (!plugT.empty() && topic == plugT && what == "layout") {
			obs_data_t *inner = obs_data_get_obj(payload, "payload");
			obs_data_t *drawList = inner ? obs_data_get_obj(inner, "drawList") : nullptr;
			const char *json = drawList ? obs_data_get_json(drawList) : nullptr;
			if (json)
				accept_layout(json);
			obs_data_release(drawList);
			obs_data_release(inner);
		}
	} else if (event == "phx_reply" && payload) {
		const bool ok = str(payload, "status") == "ok";
		std::lock_guard<std::mutex> lock(g.m);
		if (!devT.empty() && topic == devT) {
			g.devJoined = ok;
			/* Approved while the news channel was still joining: check once. */
			if (ok && g.st == St::Waiting)
				g.wantLayout = wake = true;
		} else if (!plugT.empty() && topic == plugT) {
			if (ok) {
				g.plugOk = true;
			} else {
				/* Refused: maybe removed, maybe a new key. The server knows. */
				g.joinedPlug.clear();
				g.plugRefused = true;
				g.wantLayout = wake = true;
			}
		}
	} else if (event == "phx_error" || event == "phx_close") {
		std::lock_guard<std::mutex> lock(g.m);
		if (topic == devT || topic == plugT) {
			/* The server dropped a channel: start the connection over. */
			g.socketDead = wake = true;
		}
	}
	obs_data_release(payload);
	obs_data_release(d);
	if (wake)
		g.cv.notify_all();
}

void reader_main(HINTERNET ws)
{
	std::string msg;
	std::vector<char> buf(16 * 1024);
	bool skipping = false;
	for (;;) {
		DWORD got = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
		if (WinHttpWebSocketReceive(ws, buf.data(), (DWORD)buf.size(), &got, &type) != NO_ERROR)
			break;
		if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
			break;
		const bool binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
				    type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
		if (!skipping && !binary) {
			msg.append(buf.data(), got);
			if (msg.size() > MAX_WS_MESSAGE) {
				skipping = true;
				msg.clear();
			}
		}
		const bool end = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
				 type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
		if (end) {
			if (!skipping && !binary)
				on_message(msg);
			msg.clear();
			skipping = false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.socketDead = true;
	}
	g.cv.notify_all();
}

void ws_close()
{
	HINTERNET ws;
	{
		std::lock_guard<std::mutex> lock(g_sock.sendM);
		ws = g_sock.ws;
		g_sock.ws = nullptr;
	}
	if (ws)
		WinHttpCloseHandle(ws); /* wakes the reader with an error */
	if (g_sock.reader.joinable())
		g_sock.reader.join();
	if (g_sock.con)
		WinHttpCloseHandle(g_sock.con);
	g_sock.con = nullptr;
	g_sock.devRef.clear();
	g_sock.plugRef.clear();
	std::lock_guard<std::mutex> lock(g.m);
	g.joinedPlug.clear();
	g.devJoined = false;
	g.socketDead = false;
}

bool ws_open()
{
	Url u;
	if (!crack(std::string(SV_SUPABASE_URL) + "/realtime/v1/websocket?apikey=" + SV_SUPABASE_ANON_KEY + "&vsn=1.0.0", u))
		return false;
	g_sock.con = WinHttpConnect(g_session, u.host.c_str(), u.port, 0);
	if (!g_sock.con)
		return false;
	HINTERNET req = WinHttpOpenRequest(g_sock.con, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
					   WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
	HINTERNET ws = nullptr;
	if (req) {
		/* Replies to our heartbeat arrive every 25 seconds, so a minute of
		   silence means the connection is gone. */
		WinHttpSetTimeouts(req, 5000, 5000, 10000, 70000);
		if (WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
		    WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		    WinHttpReceiveResponse(req, nullptr)) {
			DWORD code = 0, size = sizeof(code);
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
			if (code == 101)
				ws = WinHttpWebSocketCompleteUpgrade(req, 0);
		}
		WinHttpCloseHandle(req);
	}
	if (!ws) {
		WinHttpCloseHandle(g_sock.con);
		g_sock.con = nullptr;
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_sock.sendM);
		g_sock.ws = ws;
	}
	g_sock.reader = std::thread(reader_main, ws);
	return true;
}

/* Joins the right private channels on the open connection. */
void sync_joins()
{
	std::string userId, channel, joined;
	bool linked;
	{
		std::lock_guard<std::mutex> lock(g.m);
		userId = g.userId;
		channel = g.channel;
		joined = g.joinedPlug;
		linked = g.st == St::Linked;
	}
	if (!g_sock.ws)
		return;
	if (g_sock.devRef.empty() && is_uuid(userId))
		join("svert-dev:" + userId, g_sock.devRef);
	const std::string want = linked && is_channel(channel) ? channel : "";
	if (joined != want) {
		if (!joined.empty())
			leave(joined, g_sock.plugRef);
		if (!want.empty())
			join(want, g_sock.plugRef);
		std::lock_guard<std::mutex> lock(g.m);
		g.joinedPlug = want;
	}
}

void push_token()
{
	std::string joined, userId;
	{
		std::lock_guard<std::mutex> lock(g.m);
		joined = g.joinedPlug;
		userId = g.userId;
	}
	const std::string payload = "{\"access_token\":\"" + g.accessToken + "\"}";
	if (!g_sock.devRef.empty())
		ws_send(phx("realtime:svert-dev:" + userId, "access_token", payload, next_ref(), g_sock.devRef));
	if (!g_sock.plugRef.empty() && !joined.empty())
		ws_send(phx("realtime:" + joined, "access_token", payload, next_ref(), g_sock.plugRef));
}

/* ---------- what the buttons and the server lead to (link thread) ---------- */

void set_problem(const char *key)
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.problem = key ? key : "";
	}
	notify_status();
}

void set_not_linked(bool dropSaved)
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.st = St::NotLinked;
		g.channel.clear();
		g.code.clear();
		g.offline = false;
		/* Linked again later, perhaps to another account: that one is told afresh. */
		g.setupSent.clear();
		if (dropSaved) {
			g.account.clear();
			g.layout.clear();
			g.layoutDirty = false;
			g.presets.clear();
			g.activePreset.clear();
			g.wantPresets = false;
		}
	}
	if (dropSaved) {
		remove_file("layout.json");
		remove_file("account.txt");
		remove_file("presets.json");
		remove_file("picked-layout.txt");
		/* An empty layout tells every source to stop showing it straight away. */
		deliver_layout("");
	}
	sync_joins();
	notify_status();
}

std::string pc_name()
{
	wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
	if (!GetComputerNameW(name, &n))
		return "My PC";
	return narrow(std::wstring(name, n));
}

void do_link()
{
	bool haveId;
	{
		std::lock_guard<std::mutex> lock(g.m);
		haveId = !g.userId.empty();
	}
	if (!haveId && !sign_up()) {
		set_problem("LinkProblemOffline");
		return;
	}
	if (g.accessToken.empty() || Clock::now() > g.accessUntil - std::chrono::minutes(5)) {
		const Refresh r = refresh_session();
		if (r == Refresh::Lost) {
			forget_identity();
			if (!sign_up()) {
				set_problem("LinkProblemOffline");
				return;
			}
		} else if (r == Refresh::Offline) {
			set_problem("LinkProblemOffline");
			return;
		}
	}
	obs_data_t *b = obs_data_create();
	obs_data_set_string(b, "label", pc_name().c_str());
	const std::string body = obs_data_get_json(b);
	obs_data_release(b);
	const Resp r = http(L"POST", std::string(SV_SITE_URL) + "/api/plugin/link/start", g.accessToken, &body, false);
	if (r.status == 429) {
		set_problem("LinkProblemTooMany");
		return;
	}
	obs_data_t *d = r.status == 200 ? obs_data_create_from_json(r.body.c_str()) : nullptr;
	const std::string code = str(d, "code"), url = str(d, "approveUrl"), id = str(d, "requestId");
	obs_data_release(d);
	/* Only ever open our own Approve page. */
	const std::string expect = std::string(SV_SITE_URL) + "/link?r=" + id;
	if (!is_code(code) || !is_uuid(id) || url != expect) {
		set_problem(r.status == 0 ? "LinkProblemOffline" : "LinkProblemServer");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.st = St::Waiting;
		g.code = code;
		g.problem.clear();
		g.waitUntil = Clock::now() + WAIT_LIMIT;
	}
	notify_status();
	ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void do_unlink()
{
	const std::string body = "{}";
	const Resp r = g.accessToken.empty()
			       ? Resp{}
			       : http(L"POST", std::string(SV_SITE_URL) + "/api/plugin/unlink", g.accessToken, &body, false);
	if (r.status != 200) {
		set_problem(r.status == 0 ? "LinkProblemOffline" : "LinkProblemServer");
		return;
	}
	set_problem(nullptr);
	set_not_linked(true);
}

enum class Fetch { Done, Retry };

Fetch fetch_layout()
{
	const Resp r = http(L"GET", std::string(SV_SITE_URL) + "/api/plugin/layout", g.accessToken, nullptr, false);
	if (r.status == 404) {
		bool waiting;
		{
			std::lock_guard<std::mutex> lock(g.m);
			waiting = g.st == St::Waiting;
		}
		/* Not linked (or removed while OBS was closed). Still waiting for approval: keep waiting. */
		if (!waiting)
			set_not_linked(true);
		return Fetch::Done;
	}
	if (r.status != 200)
		return Fetch::Retry;
	obs_data_t *d = obs_data_create_from_json(r.body.c_str());
	obs_data_t *drawList = d ? obs_data_get_obj(d, "drawList") : nullptr;
	const char *json = drawList ? obs_data_get_json(drawList) : nullptr;
	const std::string layout = json ? json : "";
	const std::string channel = str(d, "channel");
	const std::string account = str(d, "account");
	obs_data_release(drawList);
	obs_data_release(d);
	if (layout.empty() || !is_channel(channel))
		return Fetch::Retry;
	bool accountChanged;
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.st = St::Linked;
		g.offline = false;
		g.code.clear();
		g.problem.clear();
		g.channel = channel;
		accountChanged = g.account != account;
		g.account = account;
	}
	/* Kept on disk so the name still shows offline and straight after OBS starts. */
	if (accountChanged)
		write_text("account.txt", account);
	accept_layout(layout);
	sync_joins();
	notify_status();
	return Fetch::Done;
}

bool parse_presets(const std::string &body, std::vector<Preset> &out, std::vector<GameExe> &exes)
{
	obs_data_t *d = obs_data_create_from_json(body.c_str());
	if (!d)
		return false;
	obs_data_array_t *ex = obs_data_get_array(d, "gameExes");
	const size_t exCount = ex ? obs_data_array_count(ex) : 0;
	for (size_t i = 0; i < exCount; i++) {
		obs_data_t *item = obs_data_array_item(ex, i);
		GameExe e{str(item, "game"), str(item, "exe")};
		std::transform(e.exe.begin(), e.exe.end(), e.exe.begin(),
			       [](unsigned char c) { return (char)std::tolower(c); });
		if (!e.game.empty() && !e.exe.empty())
			exes.push_back(std::move(e));
		obs_data_release(item);
	}
	obs_data_array_release(ex);
	obs_data_array_t *arr = obs_data_get_array(d, "presets");
	const bool ok = arr != nullptr;
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		obs_data_t *drawList = obs_data_get_obj(item, "drawList");
		const char *json = drawList ? obs_data_get_json(drawList) : nullptr;
		Preset p;
		p.id = str(item, "id");
		p.name = str(item, "name");
		p.drawList = json ? json : "";
		p.game = drawList ? str(drawList, "game") : "";
		if (!p.id.empty() && !p.drawList.empty())
			out.push_back(std::move(p));
		obs_data_release(drawList);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(d);
	return ok;
}

Fetch fetch_presets()
{
	const Resp r = http(L"GET", std::string(SV_SITE_URL) + "/api/plugin/presets", g.accessToken, nullptr, false);
	if (r.status == 404)
		return Fetch::Done;
	if (r.status != 200)
		return Fetch::Retry;
	std::vector<Preset> list;
	std::vector<GameExe> exes;
	if (!parse_presets(r.body, list, exes))
		return Fetch::Retry;

	bool repaint = false, pickGone = false;
	std::string show;
	{
		std::lock_guard<std::mutex> lock(g.m);
		const std::string before = showing_locked();
		g.presets = std::move(list);
		g.gameExes = std::move(exes);
		if (!g.activePreset.empty()) {
			pickGone = std::none_of(g.presets.begin(), g.presets.end(),
						[](const Preset &p) { return p.id == g.activePreset; });
			if (pickGone)
				g.activePreset.clear();
		}
		show = showing_locked();
		repaint = show != before && !show.empty();
	}
	write_text("presets.json", r.body);
	if (pickGone)
		remove_file("picked-layout.txt");
	if (repaint)
		deliver_layout(show);
	notify_status();
	return Fetch::Done;
}

/* Layers moved while this PC cannot save them: each one goes back where it was. */
void fail_moves()
{
	std::vector<PieceMove> moves;
	{
		std::lock_guard<std::mutex> lock(g.m);
		moves.swap(g.moves);
	}
	for (PieceMove &mv : moves)
		if (mv.done)
			mv.done(false, std::string());
}

/* Saves one moved layer (POST /api/plugin/layout) and shows the saved layout straight away. */
void save_move(PieceMove &mv)
{
	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "piece", mv.piece.c_str());
	if (!mv.preset.empty())
		obs_data_set_string(req, "preset", mv.preset.c_str());
	obs_data_set_double(req, "x", mv.x);
	obs_data_set_double(req, "y", mv.y);
	obs_data_set_double(req, "w", mv.w);
	obs_data_set_double(req, "h", mv.h);
	const char *text = obs_data_get_json(req);
	const std::string body = text ? text : "";
	obs_data_release(req);

	const Resp r = http(L"POST", std::string(SV_SITE_URL) + "/api/plugin/layout", g.accessToken, &body, false);
	obs_data_t *d = r.body.empty() ? nullptr : obs_data_create_from_json(r.body.c_str());
	obs_data_t *drawList = d ? obs_data_get_obj(d, "drawList") : nullptr;
	const char *json = drawList ? obs_data_get_json(drawList) : nullptr;
	const std::string layout = json ? json : "";
	const std::string reason = str(d, "reason");
	obs_data_release(drawList);
	obs_data_release(d);

	const bool ok = r.status == 200 && !layout.empty();
	if (!ok) {
		obs_log(LOG_WARNING, "link: a moved layer was not saved (%d %s)", r.status, reason.c_str());
		if (r.status == 404) {
			/* Perhaps no longer linked: reading the layout finds out. */
			std::lock_guard<std::mutex> lock(g.m);
			g.wantLayout = true;
		}
	} else if (mv.preset.empty()) {
		accept_layout(layout);
	} else {
		/* The saved layout picked in OBS: changed here at once, the server's list follows. */
		bool repaint = false;
		std::string show;
		{
			std::lock_guard<std::mutex> lock(g.m);
			const std::string before = showing_locked();
			for (Preset &p : g.presets)
				if (p.id == mv.preset)
					p.drawList = layout;
			show = showing_locked();
			repaint = show != before && !show.empty();
		}
		if (repaint)
			deliver_layout(show);
		notify_status();
	}
	if (mv.done)
		mv.done(ok, ok ? std::string() : reason);
}

void save_moves()
{
	std::vector<PieceMove> moves;
	bool linked;
	{
		std::lock_guard<std::mutex> lock(g.m);
		linked = g.st == St::Linked;
		moves.swap(g.moves);
	}
	for (PieceMove &mv : moves) {
		if (linked)
			save_move(mv);
		else if (mv.done)
			mv.done(false, std::string());
	}
}

/* Tells the website how far the OBS setup has got (POST /api/plugin/setup), when it
   changed since the website last took it. A refusal waits a minute before trying again. */
void send_setup()
{
	std::string body;
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (g.st != St::Linked || g.setupWant.empty() || g.setupWant == g.setupSent || Clock::now() < g.setupRetry)
			return;
		body = g.setupWant;
	}
	const Resp r = http(L"POST", std::string(SV_SITE_URL) + "/api/plugin/setup", g.accessToken, &body, false);
	std::lock_guard<std::mutex> lock(g.m);
	if (r.status == 200)
		g.setupSent = body;
	else
		g.setupRetry = Clock::now() + std::chrono::minutes(1);
}

void save_layout_if_due(bool now)
{
	std::string layout;
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.layoutDirty || g.st != St::Linked)
			return;
		if (!now && Clock::now() - g.layoutSaved < SAVE_GAP)
			return;
		g.layoutDirty = false;
		g.layoutSaved = Clock::now();
		layout = g.layout;
	}
	write_text("layout.json", layout);
}

void set_offline(bool off)
{
	bool changed;
	{
		std::lock_guard<std::mutex> lock(g.m);
		changed = g.offline != off;
		g.offline = off;
	}
	if (changed)
		notify_status();
}

void worker_main()
{
	load_identity();
	{
		/* The last layout, so the source shows it straight away, even offline. */
		const std::string saved = read_text("layout.json");
		const std::string savedPresets = read_text("presets.json");
		const std::string picked = read_text("picked-layout.txt");
		const std::string savedAccount = read_text("account.txt");
		std::vector<Preset> list;
		std::vector<GameExe> exes;
		if (!savedPresets.empty())
			parse_presets(savedPresets, list, exes);
		const std::string savedGame = game_of(saved);
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.userId.empty()) {
			g.wantLayout = true;
			if (!saved.empty()) {
				g.layout = saved;
				g.account = savedAccount;
				g.layoutGame = savedGame;
				g.presets = std::move(list);
				g.gameExes = std::move(exes);
				if (std::any_of(g.presets.begin(), g.presets.end(),
						[&](const Preset &p) { return p.id == picked; }))
					g.activePreset = picked;
				g.st = St::Linked; /* shown as linked until the server says otherwise */
				g.offline = true;
			}
		}
	}
	{
		std::string show;
		{
			std::lock_guard<std::mutex> lock(g.m);
			show = showing_locked();
		}
		if (!show.empty())
			deliver_layout(show);
	}
	notify_status();

	int fails = 0, plugFails = 0;
	Clock::time_point retryAt{}, nextBeat{};
	auto failed = [&]() {
		retryAt = Clock::now() + std::chrono::seconds(BACKOFF_S[fails < 4 ? fails : 4]);
		fails++;
	};

	for (;;) {
		bool link, unlink, unlinked, dead, wantLayout, haveId, plugRefused, plugOk, expired = false;
		{
			std::unique_lock<std::mutex> lock(g.m);
			/* A local check once a second at most; nothing is asked of the server unless something changed. */
			g.cv.wait_for(lock, std::chrono::seconds(1), [] {
				return g.quit || g.wantLink || g.wantUnlink || g.gotUnlinked || g.socketDead ||
				       !g.moves.empty();
			});
			if (g.quit)
				break;
			link = g.wantLink;
			unlink = g.wantUnlink;
			unlinked = g.gotUnlinked;
			dead = g.socketDead;
			g.wantLink = g.wantUnlink = g.gotUnlinked = false;
			plugRefused = g.plugRefused;
			plugOk = g.plugOk;
			g.plugRefused = g.plugOk = false;
			wantLayout = g.wantLayout;
			haveId = !g.userId.empty();
			if (g.st == St::Waiting && Clock::now() > g.waitUntil) {
				g.st = St::NotLinked;
				g.code.clear();
				g.problem = "LinkProblemExpired";
				expired = true;
			}
		}
		const auto now = Clock::now();
		if (expired)
			notify_status();
		if (plugOk)
			plugFails = 0;
		if (plugRefused) {
			/* Wait longer each time, so a refusal never turns into a loop. */
			retryAt = now + std::chrono::seconds(BACKOFF_S[plugFails < 4 ? plugFails : 4]);
			plugFails++;
		}
		if (dead) {
			ws_close();
			failed();
		}
		if (unlinked) {
			obs_log(LOG_INFO, "link: this PC was removed from the account");
			set_not_linked(true);
		}
		if (unlink)
			do_unlink();
		if (link) {
			do_link();
			fails = 0;
			retryAt = {};
		}
		{
			std::lock_guard<std::mutex> lock(g.m);
			haveId = !g.userId.empty();
		}
		if (!haveId) {
			fail_moves();
			continue;
		}

		/* A fresh sign-in before the old one runs out. */
		if ((g.accessToken.empty() || now > g.accessUntil - std::chrono::minutes(5)) && now >= retryAt) {
			const Refresh r = refresh_session();
			if (r == Refresh::Lost) {
				obs_log(LOG_WARNING, "link: the PC key is no longer valid, link again");
				ws_close();
				forget_identity();
				set_not_linked(true);
				continue;
			}
			if (r == Refresh::Offline) {
				failed();
				set_offline(true);
				continue;
			}
			push_token();
		}
		if (g.accessToken.empty()) {
			fail_moves();
			continue;
		}
		/* A moved layer is saved before anything else, so it lands quickly. */
		save_moves();

		/* The connection, joined to this PC's news and, once linked, the private channel. */
		if (!g_sock.ws && now >= retryAt) {
			if (ws_open()) {
				nextBeat = now + HEARTBEAT;
				/* Moves made while the connection was down were missed: read the layout once. */
				std::lock_guard<std::mutex> lock(g.m);
				if (g.st == St::Linked)
					g.wantLayout = true;
			} else {
				failed();
				set_offline(true);
				continue;
			}
		}
		sync_joins();

		{
			std::lock_guard<std::mutex> lock(g.m);
			wantLayout = g.wantLayout;
			if (g.devJoined && !g.wantLayout)
				fails = 0; /* the server took us back, so the next drop starts short again */
		}
		if (wantLayout && now >= retryAt) {
			if (fetch_layout() == Fetch::Done) {
				std::lock_guard<std::mutex> lock(g.m);
				g.wantLayout = false;
				g.wantPresets = g.st == St::Linked;
				fails = 0;
			} else {
				failed();
				set_offline(true);
			}
		}

		bool readPresets;
		{
			std::lock_guard<std::mutex> lock(g.m);
			readPresets = g.wantPresets && !g.wantLayout && g.st == St::Linked;
		}
		if (readPresets && Clock::now() >= retryAt) {
			if (fetch_presets() == Fetch::Done) {
				std::lock_guard<std::mutex> lock(g.m);
				g.wantPresets = false;
			} else {
				failed();
			}
		}

		if (!wantLayout)
			send_setup();

		if (g_sock.ws && now >= nextBeat) {
			ws_send(phx("phoenix", "heartbeat", "{}", next_ref(), ""));
			nextBeat = now + HEARTBEAT;
		}
		save_layout_if_due(false);
	}
	save_layout_if_due(true);
	ws_close();
}

} // namespace

namespace {

/* "1.2.10" as numbers, so 1.2.10 counts as newer than 1.2.9. Anything but digits and dots is no version. */
bool read_version(const std::string &v, std::vector<int> &out)
{
	out.clear();
	if (v.empty() || v.size() > 20)
		return false;
	int n = 0;
	bool digit = false;
	for (const char c : v) {
		if (c >= '0' && c <= '9') {
			n = n * 10 + (c - '0');
			digit = true;
			if (n > 99999)
				return false;
		} else if (c == '.' && digit) {
			out.push_back(n);
			n = 0;
			digit = false;
		} else {
			return false;
		}
	}
	if (!digit)
		return false;
	out.push_back(n);
	return true;
}

/* One look at the website's version file, a little after OBS starts so it never slows the start.
   A plain public file, so it costs nothing to serve and needs no sign-in. */
void check_for_update()
{
	{
		std::unique_lock<std::mutex> lock(g.m);
		if (g.cv.wait_for(lock, std::chrono::seconds(20), [] { return g.quit; }))
			return;
	}
	const Resp r = http(L"GET", std::string(SV_SITE_URL) + "/obs-plugin.json", std::string(), nullptr, false);
	if (r.status != 200 || r.body.empty() || r.body.size() > 4096)
		return;
	obs_data_t *d = obs_data_create_from_json(r.body.c_str());
	if (!d)
		return;
	const std::string offered = obs_data_get_string(d, "version");
	obs_data_release(d);
	std::vector<int> theirs, ours;
	if (!read_version(offered, theirs) || !read_version(PLUGIN_VERSION, ours))
		return;
	while (theirs.size() < ours.size())
		theirs.push_back(0);
	while (ours.size() < theirs.size())
		ours.push_back(0);
	if (!(theirs > ours))
		return;
	obs_log(LOG_INFO, "update: version %s is out (this is %s)", offered.c_str(), PLUGIN_VERSION);
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.newVersion = offered;
	}
	notify_status();
}

} // namespace

/* ---------- public ---------- */

void sv_link_module_load()
{
	const std::wstring agent = L"SimplyVertical-OBS/" + widen(PLUGIN_VERSION);
	g_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS, 0);
	if (!g_session) {
		obs_log(LOG_ERROR, "link: could not start the network");
		return;
	}
	WinHttpSetTimeouts(g_session, 5000, 5000, 10000, 15000);
	g.worker = std::thread(worker_main);
	g.updateCheck = std::thread(check_for_update);
}

std::string sv_link_update_version()
{
	std::lock_guard<std::mutex> lock(g.m);
	return g.newVersion;
}

void sv_link_module_unload()
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.quit = true;
	}
	g.cv.notify_all();
	if (g.worker.joinable())
		g.worker.join();
	if (g.updateCheck.joinable())
		g.updateCheck.join();
	if (g_session)
		WinHttpCloseHandle(g_session);
	g_session = nullptr;
}

void sv_link_request()
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.wantLink = true;
		g.problem.clear();
	}
	g.cv.notify_all();
}

void sv_link_unlink()
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.wantUnlink = true;
	}
	g.cv.notify_all();
}

bool sv_link_is_linked()
{
	std::lock_guard<std::mutex> lock(g.m);
	return g.st == St::Linked;
}

std::string sv_link_status()
{
	St st;
	bool offline;
	bool picked;
	std::string code, problem, account;
	{
		std::lock_guard<std::mutex> lock(g.m);
		st = g.st;
		account = g.account;
		offline = g.offline;
		picked = !g.activePreset.empty();
		code = g.code;
		problem = g.problem;
	}
	std::string line;
	if (st == St::Linked && !account.empty()) {
		char buf[768];
		std::snprintf(buf, sizeof(buf), T(offline ? "LinkOfflineName" : picked ? "LinkLinkedPickedName" : "LinkLinkedName"),
			      account.c_str());
		line = buf;
	} else if (st == St::Linked) {
		line = T(offline ? "LinkOffline" : picked ? "LinkLinkedPicked" : "LinkLinked");
	} else if (st == St::Waiting) {
		char buf[512];
		std::snprintf(buf, sizeof(buf), T("LinkWaiting"), (code.substr(0, 3) + " " + code.substr(3)).c_str());
		line = buf;
	} else {
		line = T("LinkNotLinked");
	}
	if (!problem.empty())
		line += std::string("\n") + T(problem.c_str());
	return line;
}

SvLinkState sv_link_state()
{
	SvLinkState s;
	std::lock_guard<std::mutex> lock(g.m);
	s.kind = g.st == St::Linked	 ? SvLinkKind::Linked
		 : g.st == St::Waiting ? SvLinkKind::Waiting
				       : SvLinkKind::NotLinked;
	s.offline = g.offline;
	if (g.code.size() > 3)
		s.code = g.code.substr(0, 3) + " " + g.code.substr(3);
	else
		s.code = g.code;
	s.problem = g.problem;
	if (g.st == St::Linked)
		s.account = g.account;
	return s;
}

int sv_link_listen(SvLinkListener l, std::string *current)
{
	std::lock_guard<std::mutex> lock(g_lm);
	const int id = g_nextId++;
	g_listeners[id] = std::move(l);
	if (current) {
		std::lock_guard<std::mutex> lock2(g.m);
		*current = showing_locked();
	}
	return id;
}

std::vector<SvLayoutChoice> sv_link_presets()
{
	std::lock_guard<std::mutex> lock(g.m);
	std::vector<SvLayoutChoice> out;
	for (const Preset &p : g.presets)
		out.push_back({p.id, p.name, p.drawList, p.game});
	return out;
}

std::string sv_link_showing()
{
	std::lock_guard<std::mutex> lock(g.m);
	return showing_locked();
}

#ifdef SV_DEV_BUILD
/* Testing builds only: shows the draw list in the settings folder's test-layout.json on
   the layout sources, without saving it or telling the website. The next layout from
   the website replaces it. */
std::string sv_link_test_layout()
{
	const std::string json = read_text("test-layout.json");
	if (json.empty())
		return "no test-layout.json";
	deliver_layout(json);
	return "showing test-layout.json, " + std::to_string(json.size()) + " bytes";
}
#endif

std::string sv_link_site_game()
{
	std::lock_guard<std::mutex> lock(g.m);
	return g.layoutGame;
}

std::string sv_link_game_for_exe(const std::string &exe)
{
	if (exe.empty())
		return "";
	std::lock_guard<std::mutex> lock(g.m);
	for (const GameExe &e : g.gameExes) {
		if (e.exe.back() == '*' ? exe.rfind(e.exe.substr(0, e.exe.size() - 1), 0) == 0 : exe == e.exe)
			return e.game;
	}
	return "";
}

std::string sv_link_active_preset()
{
	std::lock_guard<std::mutex> lock(g.m);
	return g.activePreset;
}

bool sv_link_pick_preset(const std::string &id)
{
	std::string show;
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (id == g.activePreset)
			return true;
		if (!id.empty() &&
		    std::none_of(g.presets.begin(), g.presets.end(), [&](const Preset &p) { return p.id == id; }))
			return false;
		g.activePreset = id;
		show = showing_locked();
	}
	if (id.empty())
		remove_file("picked-layout.txt");
	else
		write_text("picked-layout.txt", id);
	if (!show.empty())
		deliver_layout(show);
	notify_status();
	return true;
}

void sv_link_set_hold(bool on)
{
	std::string show;
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (on == g.hold)
			return;
		if (on)
			g.heldLayout = g.layout;
		g.hold = on;
		if (!on)
			g.heldLayout.clear();
		show = showing_locked();
	}
	/* Letting go sends whatever the website moved while it was held. */
	if (!on && !show.empty())
		deliver_layout(show);
	notify_status();
}

bool sv_link_holding()
{
	std::lock_guard<std::mutex> lock(g.m);
	return g.hold;
}

void sv_link_move_piece(const std::string &preset, const std::string &piece, float x, float y, float w, float h,
			std::function<void(bool ok, const std::string &reason)> done)
{
	{
		std::lock_guard<std::mutex> lock(g.m);
		/* A runaway never piles up: the website takes at most 60 a minute anyway. */
		if (g.moves.size() < 8) {
			g.moves.push_back(PieceMove{preset, piece, x, y, w, h, std::move(done)});
			done = nullptr;
		}
	}
	if (done)
		done(false, std::string());
	g.cv.notify_all();
}

void sv_link_report_setup(const SvSetupProgress &p)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "game", p.game);
	obs_data_set_bool(d, "camera", p.camera);
	obs_data_set_bool(d, "finished", p.finished);
	obs_data_set_int(d, "platforms", p.platforms);
	obs_data_set_bool(d, "otherApp", p.otherApp);
	obs_data_set_bool(d, "wentLive", p.wentLive);
	obs_data_set_string(d, "step", p.step.c_str());
	const char *text = obs_data_get_json(d);
	const std::string body = text ? text : "";
	obs_data_release(d);
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (body == g.setupWant)
			return;
		g.setupWant = body;
	}
	g.cv.notify_all();
}

SvStreamKey sv_link_stream_key(const std::string &platform)
{
	SvStreamKey k;
	std::string token;
	bool linked;
	{
		std::lock_guard<std::mutex> lock(g.m);
		token = g.accessToken;
		linked = g.st == St::Linked;
	}
	if (platform != "twitch" && platform != "kick") {
		k.problem = "KeyFailed";
		return k;
	}
	const bool twitch = platform == "twitch";
	if (!linked || token.empty()) {
		k.problem = twitch ? "KeyNotLinked" : "KeyNotLinkedKick";
		return k;
	}
	const Resp r = http(L"GET", std::string(SV_SITE_URL) + "/api/plugin/stream-key?platform=" + platform, token,
			    nullptr, false);
	obs_data_t *d = r.body.empty() ? nullptr : obs_data_create_from_json(r.body.c_str());
	const std::string error = str(d, "error");
	if (r.status == 200) {
		k.key = str(d, "key");
		k.server = str(d, "server");
	}
	obs_data_release(d);
	/* A stream key is short and plain: anything else is not one. */
	const bool plain = !k.key.empty() && k.key.size() < 256 &&
			   std::all_of(k.key.begin(), k.key.end(), [](unsigned char c) { return c > 32 && c < 127; });
	const bool serverOk = k.server.empty() || k.server.rfind("rtmp", 0) == 0;
	if (r.status == 200 && plain && serverOk) {
		k.ok = true;
		obs_log(LOG_INFO, "link: got the %s stream key from the website", platform.c_str());
		return k;
	}
	k.key.clear();
	k.server.clear();
	if (r.status == 0)
		k.problem = "KeyOffline";
	else if (r.status == 404)
		k.problem = twitch ? "KeyNotLinked" : "KeyNotLinkedKick";
	else if (error == "not_connected")
		k.problem = twitch ? "KeyConnectTwitch" : "KeyConnectKick";
	else if (error == "reconnect")
		k.problem = twitch ? "KeyReconnectTwitch" : "KeyReconnectKick";
	else if (r.status == 429)
		k.problem = "KeyTooMany";
	else
		k.problem = twitch ? "KeyFailed" : "KeyFailedKick";
	obs_log(LOG_INFO, "link: no %s stream key from the website (answer %d, %s)", platform.c_str(), r.status,
		k.problem.c_str());
	return k;
}

bool sv_link_fetch_public(const std::string &url, std::string &bytes)
{
	Resp r = http(L"GET", url, std::string(), nullptr, false);
	if (r.status != 200 || r.body.empty()) {
		obs_log(LOG_WARNING, "link: a picture download got answer %d with %zu bytes", r.status, r.body.size());
		return false;
	}
	bytes = std::move(r.body);
	return true;
}

void sv_link_unlisten(int id)
{
	std::lock_guard<std::mutex> lock(g_lm);
	g_listeners.erase(id);
}
