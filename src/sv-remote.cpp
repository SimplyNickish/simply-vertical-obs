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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "sv-remote.h"

#include "sv-actions.h"
#include "sv-canvas.h"
#include "sv-follow.h"
#include "sv-link.h"
#include "sv-output.h"
#include "sv-zoom.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QMetaObject>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

constexpr int PORT_FIRST = 4480;
constexpr int PORT_COUNT = 5;
constexpr size_t MAX_HEAD = 8192;
constexpr DWORD SOCKET_WAIT_MS = 2000;
constexpr auto MAIN_WAIT = std::chrono::milliseconds(2000);

std::atomic<bool> g_run{false};
std::atomic<int> g_port{0};
SOCKET g_listen = INVALID_SOCKET;
std::thread g_thread;
bool g_wsa = false;

struct Req {
	std::string method;
	std::string path;
	std::map<std::string, std::string> query;
	std::map<std::string, std::string> headers; /* names in small letters */
};

struct Resp {
	int status = 200;
	std::string body;
};

/* ---------- small text helpers ---------- */

std::string lower(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
		b--;
	return s.substr(a, b - a);
}

int hex_of(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string url_decode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '+') {
			out += ' ';
		} else if (s[i] == '%' && i + 2 < s.size() && hex_of(s[i + 1]) >= 0 && hex_of(s[i + 2]) >= 0) {
			out += (char)(hex_of(s[i + 1]) * 16 + hex_of(s[i + 2]));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string json_text(const std::string &s)
{
	std::string out = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		} else {
			out += (char)c;
		}
	}
	return out + "\"";
}

Resp fail(int status, const std::string &error)
{
	return {status, "{\"ok\":false,\"error\":" + json_text(error) + "}"};
}

/* An obs_data object as the reply body, released after. */
Resp reply(obs_data_t *d)
{
	obs_data_set_bool(d, "ok", true);
	Resp r;
	const char *json = obs_data_get_json(d);
	r.body = json ? json : "{\"ok\":true}";
	obs_data_release(d);
	return r;
}

std::string arg(const Req &q, const char *name)
{
	auto it = q.query.find(name);
	return it == q.query.end() ? std::string() : it->second;
}

/* ---------- reading what is on, main thread ---------- */

bool running(const std::string &id)
{
	return sv_output_info(id).state != SvRunState::Off;
}

bool any_stream_running()
{
	for (const SvDest &d : sv_output_dests())
		if (running(d.id))
			return true;
	return false;
}

/* Finds a canvas by the name the member gave it, ignoring case. "Main" is the main
   one, which is also what no name means. False when no canvas has that name. */
bool find_canvas(const Req &q, std::string &canvas)
{
	const std::string want = lower(trim(arg(q, "canvas")));
	canvas.clear();
	if (want.empty() || want == "main")
		return true;
	for (const SvVCanvas &c : sv_canvases()) {
		if (!c.main && lower(trim(c.name)) == want) {
			canvas = c.uuid;
			return true;
		}
	}
	return false;
}

Resp no_canvas()
{
	return fail(404, "No canvas has that name. Check the names in the canvas line above the scenes in the "
			 "Simply Vertical panel, or leave canvas out for the main one.");
}

std::string scene_name(const std::string &canvas)
{
	const std::string now = sv_canvas_current(canvas);
	for (const SvVScene &s : sv_canvas_scenes(canvas))
		if (s.uuid == now)
			return s.name;
	return std::string();
}

/* Each canvas with its name, whether its platforms are live, its scene and its layout
   ("" on an extra canvas means the same layout as main). */
void put_canvases(obs_data_t *d)
{
	obs_data_array_t *arr = obs_data_array_create();
	if (sv_canvas_running()) {
		for (const SvVCanvas &c : sv_canvases()) {
			const std::string id = c.main ? std::string() : c.uuid;
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "name", c.main ? "Main" : c.name.c_str());
			obs_data_set_bool(o, "main", c.main);
			obs_data_set_bool(o, "live", sv_action_canvas_live(id));
			obs_data_set_string(o, "scene", scene_name(id).c_str());
			obs_data_set_string(o, "layout",
					    c.main ? sv_link_active_preset().c_str() : sv_canvas_layout_pick(id).c_str());
			obs_data_array_push_back(arr, o);
			obs_data_release(o);
		}
	}
	obs_data_set_array(d, "canvases", arr);
	obs_data_array_release(arr);
}

void put_zooms(obs_data_t *d)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (const SvZoomInfo &z : sv_zoom_list()) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "name", z.name.c_str());
		obs_data_set_bool(o, "zoomed", z.zoomed);
		obs_data_set_bool(o, "hold", z.hold);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "zooms", arr);
	obs_data_array_release(arr);
}

/* Everything a Stream Deck key face needs. Main thread. */
obs_data_t *status_data()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "app", "simply-vertical");
	obs_data_set_string(d, "version", PLUGIN_VERSION);
	obs_data_set_bool(d, "linked", sv_link_is_linked());
	obs_data_set_bool(d, "holding", sv_link_holding());

	const std::string active = sv_link_active_preset();
	obs_data_set_bool(d, "followingWebsite", active.empty());
	obs_data_set_string(d, "activeLayout", active.c_str());
	obs_data_array_t *arr = obs_data_array_create();
	for (const SvLayoutChoice &c : sv_link_presets()) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", c.id.c_str());
		obs_data_set_string(o, "name", c.name.c_str());
		obs_data_set_bool(o, "active", c.id == active);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "layouts", arr);
	obs_data_array_release(arr);
	obs_data_set_bool(d, "followGame", sv_follow_on());
	obs_data_set_string(d, "game", sv_follow_seen().c_str());

	put_zooms(d);
	obs_data_set_bool(d, "live", any_stream_running());
	obs_data_set_bool(d, "recording", running(SV_RECORD_ID));
	obs_data_set_bool(d, "backtrack", running(SV_BACKTRACK_ID));
	put_canvases(d);
	return d;
}

/* The saved layouts with each one's drawing, for pictures on Stream Deck keys. */
Resp layouts_reply()
{
	obs_data_t *d = obs_data_create();
	const std::string active = sv_link_active_preset();
	obs_data_array_t *arr = obs_data_array_create();
	for (const SvLayoutChoice &c : sv_link_presets()) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", c.id.c_str());
		obs_data_set_string(o, "name", c.name.c_str());
		obs_data_set_bool(o, "active", c.id == active);
		if (!c.drawList.empty()) {
			obs_data_t *pic = obs_data_create_from_json(c.drawList.c_str());
			if (pic) {
				obs_data_set_obj(o, "drawList", pic);
				obs_data_release(pic);
			}
		}
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "layouts", arr);
	obs_data_array_release(arr);
	return reply(d);
}

/* ---------- the actions, main thread ---------- */

Resp done()
{
	return reply(status_data());
}

Resp do_layout(const Req &q)
{
	std::string canvas;
	if (!find_canvas(q, canvas))
		return no_canvas();
	const std::string what = lower(arg(q, "do"));
	if (what == "next" || what == "prev" || what == "previous") {
		sv_action_step_layout(what == "next" ? 1 : -1, canvas);
		return done();
	}
	if (what == "website") {
		if (!canvas.empty())
			return fail(400, "Following the website is for the main canvas. For this canvas use do=main to "
					 "show the same layout as main.");
		sv_action_pick_layout("");
		return done();
	}
	if (what == "main") {
		if (canvas.empty())
			return fail(400, "This is the main canvas. Use do=website to follow the website.");
		sv_action_pick_canvas_layout(canvas, "");
		return done();
	}
	const std::string id = arg(q, "id");
	const std::string name = lower(trim(arg(q, "name")));
	if (id.empty() && name.empty())
		return fail(400, "Say which layout, like /layout?name=Apex, or /layout?do=next.");
	for (const SvLayoutChoice &c : sv_link_presets()) {
		if ((!id.empty() && c.id == id) || (id.empty() && lower(trim(c.name)) == name)) {
			sv_action_pick_canvas_layout(canvas, c.id);
			return done();
		}
	}
	return fail(404, "No saved layout has that name. Check the name on the website, Game tab.");
}

Resp do_hold(const Req &q)
{
	const std::string what = lower(arg(q, "do"));
	if (what == "on")
		sv_action_set_hold(true);
	else if (what == "off")
		sv_action_set_hold(false);
	else
		sv_action_toggle_hold();
	return done();
}

/* Follow my game on or off, like the switch on the panel's Layout card. */
Resp do_follow_game(const Req &q)
{
	const std::string what = lower(arg(q, "do"));
	sv_follow_set(what == "on" ? true : what == "off" ? false : !sv_follow_on());
	return done();
}

Resp do_live(const Req &q)
{
	const std::string what = lower(arg(q, "do"));
	/* With a canvas named, only the platforms showing that canvas. */
	if (!trim(arg(q, "canvas")).empty()) {
		std::string canvas;
		if (!find_canvas(q, canvas))
			return no_canvas();
		if (what == "start" || what == "stop")
			sv_action_set_canvas_live(canvas, what == "start");
		else
			sv_action_toggle_canvas(canvas);
		return done();
	}
	if (what == "start")
		sv_action_go_live_all();
	else if (what == "stop")
		sv_action_stop_all();
	else
		sv_action_toggle_all();
	return done();
}

Resp do_record(const Req &q)
{
	const std::string what = lower(arg(q, "do"));
	const bool on = running(SV_RECORD_ID);
	if ((what == "start" && !on) || (what == "stop" && on) || (what != "start" && what != "stop"))
		sv_action_toggle(SV_RECORD_ID);
	return done();
}

Resp do_clip()
{
	if (!running(SV_BACKTRACK_ID))
		return fail(409, "Backtrack is off. Start Backtrack in the Simply Vertical panel in OBS first.");
	sv_action_save_backtrack();
	return done();
}

Resp do_vscene(const Req &q)
{
	std::string canvas;
	if (!find_canvas(q, canvas))
		return no_canvas();
	const std::string what = lower(arg(q, "do"));
	sv_action_step_vscene(what == "prev" || what == "previous" ? -1 : 1, canvas);
	return done();
}

/* Runs fn on OBS's main thread and waits for its answer. */
Resp on_main(std::function<Resp()> fn)
{
	if (!qApp)
		return fail(503, "OBS is busy. Try again in a moment.");
	auto promise = std::make_shared<std::promise<Resp>>();
	std::future<Resp> answer = promise->get_future();
	QMetaObject::invokeMethod(
		qApp,
		[promise, fn] {
			if (!g_run) {
				promise->set_value(fail(503, "Simply Vertical is closing."));
				return;
			}
			promise->set_value(fn());
		},
		Qt::QueuedConnection);
	if (answer.wait_for(MAIN_WAIT) != std::future_status::ready)
		return fail(503, "OBS is busy. Try again in a moment.");
	return answer.get();
}

/* Zooming is safe from any thread and needs to be instant, so it skips the queue. */
Resp do_zoom(const Req &q)
{
	const std::string piece = trim(arg(q, "piece"));
	if (piece.empty())
		return fail(400, "Say which piece, like /zoom?piece=Minimap.");
	const std::string key = sv_zoom_key_for(piece);
	bool found = false;
	for (const SvZoomInfo &z : sv_zoom_list())
		found = found || sv_zoom_key_for(z.name) == key;
	if (!found)
		return fail(404, "No piece with that name has Zoom on a key switched on. Switch it on in the Pieces tab.");

	const std::string what = lower(arg(q, "do"));
	if (what == "down" || what == "up")
		sv_zoom_key(key, what == "down");
	else if (what == "in")
		sv_zoom_do(key, SvZoomDo::In);
	else if (what == "out")
		sv_zoom_do(key, SvZoomDo::Out);
	else
		sv_zoom_do(key, SvZoomDo::Flip);

	obs_data_t *d = obs_data_create();
	put_zooms(d);
	return reply(d);
}

Resp route(const Req &q)
{
	const std::string &p = q.path;
	if (p == "/zoom")
		return do_zoom(q);
	if (p == "/" || p == "/status")
		return on_main([] { return done(); });
	if (p == "/layouts")
		return on_main([] { return layouts_reply(); });
	if (p == "/layout")
		return on_main([q] { return do_layout(q); });
	if (p == "/hold")
		return on_main([q] { return do_hold(q); });
	if (p == "/follow-game")
		return on_main([q] { return do_follow_game(q); });
	if (p == "/live")
		return on_main([q] { return do_live(q); });
	if (p == "/record")
		return on_main([q] { return do_record(q); });
	if (p == "/clip")
		return on_main([] { return do_clip(); });
	if (p == "/vscene")
		return on_main([q] { return do_vscene(q); });
	return fail(404, "Unknown address. Try /status.");
}

/* ---------- HTTP ---------- */

bool parse(const std::string &head, Req &q)
{
	const size_t lineEnd = head.find("\r\n");
	const std::string first = head.substr(0, lineEnd);
	const size_t a = first.find(' ');
	const size_t b = first.find(' ', a == std::string::npos ? a : a + 1);
	if (a == std::string::npos || b == std::string::npos)
		return false;
	q.method = first.substr(0, a);
	std::string target = first.substr(a + 1, b - a - 1);

	const size_t mark = target.find('?');
	std::string query = mark == std::string::npos ? "" : target.substr(mark + 1);
	q.path = lower(url_decode(target.substr(0, mark)));
	while (q.path.size() > 1 && q.path.back() == '/')
		q.path.pop_back();
	size_t at = 0;
	while (at <= query.size() && !query.empty()) {
		size_t amp = query.find('&', at);
		if (amp == std::string::npos)
			amp = query.size();
		const std::string pair = query.substr(at, amp - at);
		const size_t eq = pair.find('=');
		if (!pair.empty())
			q.query[lower(url_decode(pair.substr(0, eq)))] =
				eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
		at = amp + 1;
	}

	size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
	while (pos < head.size()) {
		size_t end = head.find("\r\n", pos);
		if (end == std::string::npos)
			end = head.size();
		const std::string line = head.substr(pos, end - pos);
		const size_t colon = line.find(':');
		if (colon != std::string::npos)
			q.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		pos = end + 2;
	}
	return true;
}

/* Only programs on this PC, never a web page. A browser always says where a
   request came from (Origin, Referer or Sec-Fetch-Site), and a page that
   tricks its way to 127.0.0.1 by a made up name still carries that name in
   Host. */
bool allowed(const Req &q, std::string &why)
{
	if (q.headers.count("origin") || q.headers.count("referer") || q.headers.count("sec-fetch-site")) {
		why = "Web pages can't use this. Use the Stream Deck plugin, Streamer.bot, Touch Portal or similar.";
		return false;
	}
	auto it = q.headers.find("host");
	std::string host = it == q.headers.end() ? "" : lower(it->second);
	const std::string port = ":" + std::to_string(g_port.load());
	if (host.size() > port.size() && host.compare(host.size() - port.size(), port.size(), port) == 0)
		host.resize(host.size() - port.size());
	if (host != "127.0.0.1" && host != "localhost") {
		why = "Use http://127.0.0.1" + port + " on this PC.";
		return false;
	}
	if (q.method != "GET" && q.method != "POST") {
		why = "Use GET or POST.";
		return false;
	}
	return true;
}

const char *status_text(int s)
{
	switch (s) {
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 409:
		return "Conflict";
	case 431:
		return "Request Header Fields Too Large";
	default:
		return "Service Unavailable";
	}
}

void send_all(SOCKET c, const std::string &data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		const int n = send(c, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 20), 0);
		if (n <= 0)
			return;
		sent += (size_t)n;
	}
}

void serve(SOCKET c)
{
	DWORD wait = SOCKET_WAIT_MS;
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof(wait));
	setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&wait, sizeof(wait));

	std::string head;
	char buf[2048];
	bool whole = false;
	while (head.size() < MAX_HEAD) {
		const int n = recv(c, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		head.append(buf, (size_t)n);
		if (head.find("\r\n\r\n") != std::string::npos) {
			whole = true;
			break;
		}
	}

	Resp r;
	Req q;
	std::string why;
	if (!whole)
		r = head.size() >= MAX_HEAD ? fail(431, "Too much was sent.") : fail(400, "That request was not complete.");
	else if (!parse(head.substr(0, head.find("\r\n\r\n")), q))
		r = fail(400, "That request could not be read.");
	else if (!allowed(q, why))
		r = fail(403, why);
	else
		r = route(q);

	char top[256];
	std::snprintf(top, sizeof(top),
		      "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
		      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
		      r.status, status_text(r.status), r.body.size());
	send_all(c, top + r.body);
	shutdown(c, SD_SEND);
	closesocket(c);
}

void loop()
{
	while (g_run) {
		SOCKET c = accept(g_listen, nullptr, nullptr);
		if (c == INVALID_SOCKET) {
			if (!g_run)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		serve(c);
	}
}

SOCKET open_port(int port)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return s;
	/* No other program can take the same port while we have it. */
	BOOL only = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&only, sizeof(only));
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

} // namespace

void sv_remote_start()
{
	if (g_run)
		return;
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		obs_log(LOG_WARNING, "remote control could not start (network)");
		return;
	}
	g_wsa = true;
	for (int i = 0; i < PORT_COUNT && g_listen == INVALID_SOCKET; i++) {
		g_listen = open_port(PORT_FIRST + i);
		if (g_listen != INVALID_SOCKET)
			g_port = PORT_FIRST + i;
	}
	if (g_listen == INVALID_SOCKET) {
		obs_log(LOG_WARNING, "remote control could not start: ports %d to %d are all taken", PORT_FIRST,
			PORT_FIRST + PORT_COUNT - 1);
		WSACleanup();
		g_wsa = false;
		return;
	}
	g_run = true;
	g_thread = std::thread(loop);
	obs_log(LOG_INFO, "remote control on http://127.0.0.1:%d", g_port.load());
}

void sv_remote_stop()
{
	if (!g_run && !g_wsa)
		return;
	g_run = false;
	if (g_listen != INVALID_SOCKET) {
		closesocket(g_listen);
		g_listen = INVALID_SOCKET;
	}
	if (g_thread.joinable())
		g_thread.join();
	g_port = 0;
	if (g_wsa) {
		WSACleanup();
		g_wsa = false;
	}
}

int sv_remote_port()
{
	return g_port.load();
}
