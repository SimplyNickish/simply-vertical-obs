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

#include "sv-brand.h"
#include "sv-config.h"
#include "sv-link.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

/* A logo is drawn at most this big before it is fitted into its box. */
constexpr int MAX_LOGO_SIDE = 2048;
/* A logo that could not be loaded is tried again only when a layout arrives this long after. */
constexpr auto RETRY_AFTER = std::chrono::seconds(60);

struct Logo {
	enum class St { Loading, Ready, Failed } st = St::Loading;
	QImage img; /* premultiplied, ready to draw */
	Clock::time_point failedAt;
};

std::mutex g_m;
std::condition_variable g_cv;
std::map<std::string, Logo> g_logos;
std::deque<std::string> g_queue;
std::thread g_worker;
bool g_quit = false;
std::function<void()> g_onLogo;

/* ---------- loading a logo (download thread) ---------- */

QImage decode(const std::string &bytes)
{
	const QByteArray data(bytes.data(), (qsizetype)bytes.size());
	QImage img;
	if (!img.loadFromData(data)) {
		/* Not a picture Qt reads straight away: an SVG drawing, drawn at a good size. */
		QSvgRenderer svg(data);
		if (!svg.isValid())
			return QImage();
		QSizeF size = svg.defaultSize();
		if (size.width() <= 0 || size.height() <= 0)
			size = QSizeF(MAX_LOGO_SIDE, MAX_LOGO_SIDE);
		const qreal k = MAX_LOGO_SIDE / std::max(size.width(), size.height());
		img = QImage(std::max(1, (int)std::lround(size.width() * k)), std::max(1, (int)std::lround(size.height() * k)),
			     QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		svg.render(&p);
	}
	if (img.isNull())
		return img;
	if (img.width() > MAX_LOGO_SIDE || img.height() > MAX_LOGO_SIDE)
		img = img.scaled(MAX_LOGO_SIDE, MAX_LOGO_SIDE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

/* ---------- the saved copy on this PC ---------- */

/* Every logo is also kept in the plugin's settings folder, so it still shows when OBS
   starts without internet. Named after its address, so a new logo never shows an old one. */
constexpr int MAX_SAVED_LOGOS = 8;

QString saved_folder()
{
	char *p = obs_module_config_path("logos");
	const QString dir = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	return dir;
}

QString saved_path(const std::string &url)
{
	const QString dir = saved_folder();
	if (dir.isEmpty())
		return QString();
	const QByteArray hash = QCryptographicHash::hash(QByteArray(url.data(), (qsizetype)url.size()),
							 QCryptographicHash::Sha256)
					.toHex()
					.left(32);
	return dir + "/" + QString::fromLatin1(hash) + ".logo";
}

QByteArray read_saved(const std::string &url)
{
	QFile f(saved_path(url));
	return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void write_saved(const std::string &url, const std::string &bytes)
{
	const QString path = saved_path(url);
	if (path.isEmpty())
		return;
	QDir().mkpath(saved_folder());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), (qint64)bytes.size()) != (qint64)bytes.size() ||
	    !f.commit()) {
		obs_log(LOG_WARNING, "brand kit logo could not be saved on this PC");
		return;
	}
	/* Keep only the newest few. */
	QDir dir(saved_folder());
	const QFileInfoList all = dir.entryInfoList({"*.logo"}, QDir::Files, QDir::Time);
	for (int i = MAX_SAVED_LOGOS; i < all.size(); i++)
		QFile::remove(all[i].absoluteFilePath());
}

void publish(const std::string &url, QImage img)
{
	std::function<void()> notify;
	{
		std::lock_guard<std::mutex> lock(g_m);
		Logo &l = g_logos[url];
		l.st = Logo::St::Ready;
		l.img = std::move(img);
		notify = g_onLogo;
	}
	if (notify)
		notify();
}

void worker()
{
	for (;;) {
		std::string url;
		{
			std::unique_lock<std::mutex> lock(g_m);
			g_cv.wait(lock, [] { return g_quit || !g_queue.empty(); });
			if (g_quit)
				return;
			url = std::move(g_queue.front());
			g_queue.pop_front();
		}
		/* The saved copy shows straight away, then the download checks for a newer one. */
		const QByteArray saved = read_saved(url);
		bool showingSaved = false;
		if (!saved.isEmpty()) {
			QImage img = decode(std::string(saved.constData(), (size_t)saved.size()));
			if (!img.isNull()) {
				publish(url, std::move(img));
				showingSaved = true;
				obs_log(LOG_INFO, "brand kit logo shown from this PC");
			}
		}
		std::string bytes;
		QImage img;
		if (sv_link_fetch_public(url, bytes)) {
			img = decode(bytes);
			if (img.isNull())
				obs_log(LOG_WARNING, "brand kit logo downloaded (%zu bytes) but is not a picture we can read",
					bytes.size());
		}
		if (!img.isNull()) {
			const bool same = showingSaved && saved.size() == (qsizetype)bytes.size() &&
					  std::memcmp(saved.constData(), bytes.data(), bytes.size()) == 0;
			if (!same) {
				write_saved(url, bytes);
				publish(url, std::move(img));
			}
			obs_log(LOG_INFO, "brand kit logo loaded");
			continue;
		}
		if (showingSaved) {
			obs_log(LOG_INFO, "brand kit logo could not be downloaded, the copy on this PC stays");
			continue;
		}
		std::function<void()> notify;
		{
			std::lock_guard<std::mutex> lock(g_m);
			Logo &l = g_logos[url];
			l.st = Logo::St::Failed;
			l.failedAt = Clock::now();
			notify = g_onLogo;
		}
		obs_log(LOG_WARNING, "brand kit logo could not be loaded, it is left out");
		if (notify)
			notify();
	}
}

/* The logo for an address: ready to draw, or null while it loads or after it failed. */
QImage logo_for(const std::string &url)
{
	std::lock_guard<std::mutex> lock(g_m);
	if (g_quit)
		return QImage();
	auto it = g_logos.find(url);
	if (it != g_logos.end()) {
		if (it->second.st == Logo::St::Ready)
			return it->second.img;
		if (it->second.st == Logo::St::Loading || Clock::now() - it->second.failedAt < RETRY_AFTER)
			return QImage();
	}
	/* Logos change rarely: keep only a handful, the oldest go first. */
	if (g_logos.size() >= 8)
		for (auto i = g_logos.begin(); i != g_logos.end();)
			i = i->second.st == Logo::St::Loading ? std::next(i) : g_logos.erase(i);
	g_logos[url] = Logo();
	g_queue.push_back(url);
	if (!g_worker.joinable())
		g_worker = std::thread(worker);
	g_cv.notify_one();
	return QImage();
}

/* ---------- drawing ---------- */

SvColorBitmap to_bitmap(const QImage &img)
{
	SvColorBitmap b;
	const QImage straight = img.convertToFormat(QImage::Format_RGBA8888);
	b.w = straight.width();
	b.h = straight.height();
	b.rgba.resize((size_t)b.w * b.h * 4);
	for (int y = 0; y < b.h; y++)
		std::memcpy(b.rgba.data() + (size_t)y * b.w * 4, straight.constScanLine(y), (size_t)b.w * 4);
	return b;
}

QImage blank(const SvPiece &p)
{
	const int w = std::clamp((int)std::lround(p.box.w), 1, 4096);
	const int h = std::clamp((int)std::lround(p.box.h), 1, 4096);
	QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	return img;
}

/* The whole logo, as big as fits, centred: never stretched or cropped. */
SvColorBitmap draw_logo(const SvPiece &p)
{
	const QImage logo = logo_for(p.url);
	if (logo.isNull())
		return SvColorBitmap();
	QImage img = blank(p);
	const qreal k = std::min((qreal)img.width() / logo.width(), (qreal)img.height() / logo.height());
	const QSizeF size(logo.width() * k, logo.height() * k);
	const QRectF at(QPointF((img.width() - size.width()) / 2, (img.height() - size.height()) / 2), size);
	QPainter painter(&img);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.drawImage(at, logo);
	painter.end();
	return to_bitmap(img);
}

/* The member's font when this PC has it, otherwise Archivo Black (the copy that comes with the plugin). */
QFont name_font(const std::string &family)
{
	const QStringList installed = QFontDatabase::families();
	const QString want = QString::fromStdString(family);
	QString use;
	if (!want.isEmpty() && installed.contains(want, Qt::CaseInsensitive))
		use = want;
	else if (installed.contains("Archivo Black", Qt::CaseInsensitive))
		use = "Archivo Black";
	else if (installed.contains("Archivo", Qt::CaseInsensitive))
		use = "Archivo";
	else
		use = "Arial Black";
	for (const QString &name : installed)
		if (name.compare(use, Qt::CaseInsensitive) == 0) {
			use = name;
			break;
		}
	/* 900 when the font has it, otherwise its heaviest real weight, the same as
	   the website. Asking for more than the font has makes a faked bold. */
	int heaviest = 0;
	for (const QString &style : QFontDatabase::styles(use))
		if (!QFontDatabase::italic(use, style))
			heaviest = std::max(heaviest, QFontDatabase::weight(use, style));
	QFont f(use);
	f.setWeight(heaviest > 0 ? (QFont::Weight)std::min(900, heaviest) : QFont::Black);
	f.setHintingPreference(QFont::PreferNoHinting);
	f.setStyleStrategy(QFont::PreferAntialias);
	return f;
}

/* A soft blur of the see-through part only: three box blurs come close to a real blur. */
void blur_alpha(std::vector<float> &a, int w, int h, float sigma)
{
	const int r = std::max(1, (int)std::lround(sigma * 0.94f));
	std::vector<float> tmp(a.size());
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < h; y++) {
			float sum = 0;
			for (int x = -r; x <= r; x++)
				sum += a[(size_t)y * w + std::clamp(x, 0, w - 1)];
			for (int x = 0; x < w; x++) {
				tmp[(size_t)y * w + x] = sum / (2 * r + 1);
				sum += a[(size_t)y * w + std::min(x + r + 1, w - 1)] - a[(size_t)y * w + std::max(x - r, 0)];
			}
		}
		for (int x = 0; x < w; x++) {
			float sum = 0;
			for (int y = -r; y <= r; y++)
				sum += tmp[(size_t)std::clamp(y, 0, h - 1) * w + x];
			for (int y = 0; y < h; y++) {
				a[(size_t)y * w + x] = sum / (2 * r + 1);
				sum += tmp[(size_t)std::min(y + r + 1, h - 1) * w + x] - tmp[(size_t)std::max(y - r, 0) * w + x];
			}
		}
	}
}

/* One line, the biggest that fits, centred both ways, with a soft dark shadow so it reads over gameplay. */
SvColorBitmap draw_name(const SvPiece &p)
{
	QImage img = blank(p);
	const int w = img.width(), h = img.height();
	const QString text = QString::fromStdString(p.text);
	QFont f = name_font(p.font);
	qreal px = std::max(4.0, h * 0.7);
	f.setPixelSize((int)std::lround(px));
	for (int i = 0; i < 8; i++) {
		const qreal wide = QFontMetricsF(f).horizontalAdvance(text);
		if (wide <= w * 0.92 || px <= 4)
			break;
		px = std::max(4.0, std::floor(px * (w * 0.92) / wide));
		f.setPixelSize((int)px);
	}
	const QFontMetricsF fm(f);
	const qreal x = (w - fm.horizontalAdvance(text)) / 2;
	const qreal base = (h - (fm.ascent() + fm.descent())) / 2 + fm.ascent();

	/* The letters alone, to make the shadow from. */
	QImage shape(w, h, QImage::Format_ARGB32_Premultiplied);
	shape.fill(Qt::transparent);
	{
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::TextAntialiasing);
		sp.setFont(f);
		sp.setPen(Qt::white);
		sp.drawText(QPointF(x, base + 2), text); /* 2 px lower, the shadow's offset */
	}
	std::vector<float> a((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const QRgb *row = reinterpret_cast<const QRgb *>(shape.constScanLine(y));
		for (int xx = 0; xx < w; xx++)
			a[(size_t)y * w + xx] = qAlpha(row[xx]) / 255.0f;
	}
	blur_alpha(a, w, h, 3.0f); /* a 6 px web blur */
	for (int y = 0; y < h; y++) {
		QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int xx = 0; xx < w; xx++)
			row[xx] = qRgba(0, 0, 0, (int)std::lround(std::min(1.0f, a[(size_t)y * w + xx]) * 0.6f * 255));
	}

	QPainter painter(&img);
	painter.setRenderHint(QPainter::TextAntialiasing);
	painter.setFont(f);
	const uint32_t c = p.textColor;
	painter.setPen(QColor((c >> 16) & 255, (c >> 8) & 255, c & 255));
	painter.drawText(QPointF(x, base), text);
	painter.end();
	return to_bitmap(img);
}

/* The platform icons, 24 x 24 drawings, the same as the website.
   colour 0: the letters' colour (the website globe). */
struct SocialIcon {
	const char *id;
	uint32_t colour;
	const char *path;
};
const SocialIcon SOCIAL_ICONS[] = {
	{"twitch", 0xff9146ff, "M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143l-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714z"},
	{"youtube", 0xffff0033, "M23.495 6.205a3.007 3.007 0 0 0-2.088-2.088c-1.87-.501-9.396-.501-9.396-.501s-7.507-.01-9.396.501A3.007 3.007 0 0 0 .527 6.205a31.247 31.247 0 0 0-.522 5.805 31.247 31.247 0 0 0 .522 5.783 3.007 3.007 0 0 0 2.088 2.088c1.868.502 9.396.502 9.396.502s7.506 0 9.396-.502a3.007 3.007 0 0 0 2.088-2.088 31.247 31.247 0 0 0 .5-5.783 31.247 31.247 0 0 0-.5-5.805zM9.609 15.601V8.408l6.264 3.602z"},
	{"kick", 0xff53fc18, "M2 2h4v8l6-8h5l-7 9 7 11h-5l-6-9v9H2z"},
	{"tiktok", 0xffffffff, "M19.59 6.69a4.83 4.83 0 0 1-3.77-4.25V2h-3.45v13.67a2.89 2.89 0 0 1-2.88 2.5 2.89 2.89 0 0 1-2.89-2.89 2.89 2.89 0 0 1 2.89-2.89c.28 0 .54.04.79.1V9.01a6.33 6.33 0 0 0-.79-.05 6.34 6.34 0 0 0-6.34 6.34 6.34 6.34 0 0 0 6.34 6.34 6.34 6.34 0 0 0 6.33-6.34V9.21a8.16 8.16 0 0 0 4.78 1.52V7.28a4.85 4.85 0 0 1-1.01-.59z"},
	{"x", 0xffffffff, "M18.244 2.25h3.308l-7.227 8.26 8.502 11.24H16.17l-4.714-6.231-5.401 6.231H2.746l7.73-8.835L1.254 2.25H8.08l4.259 5.629zm-1.161 17.52h1.833L7.084 4.126H5.117z"},
	{"website", 0, "M12 1a11 11 0 1 0 0 22 11 11 0 0 0 0-22zm0 2c.9 0 2.4 2.6 2.8 7.9H9.2C9.6 5.6 11.1 3 12 3zm-2.6.4c-.8 1.7-1.4 4.2-1.6 7.5H3.1a9 9 0 0 1 6.3-7.5zm5.2 0a9 9 0 0 1 6.3 7.5h-4.7c-.2-3.3-.8-5.8-1.6-7.5zM3.1 12.9h4.7c.2 3.3.8 5.8 1.6 7.7a9 9 0 0 1-6.3-7.7zm6.1 0h5.6c-.4 5.3-1.9 8.1-2.8 8.1s-2.4-2.8-2.8-8.1zm7 0h4.7a9 9 0 0 1-6.3 7.7c.8-1.9 1.4-4.4 1.6-7.7z"},
};

const SocialIcon &social_icon(const std::string &id)
{
	for (const SocialIcon &i : SOCIAL_ICONS)
		if (id == i.id)
			return i;
	return SOCIAL_ICONS[5];
}

/* An icon in one colour, drawn into a square. */
void paint_icon(QPainter &painter, const SocialIcon &icon, uint32_t argb, const QRectF &at)
{
	char hex[8];
	std::snprintf(hex, sizeof(hex), "#%06x", (unsigned)(argb & 0xffffff));
	const std::string svg = std::string("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='") + hex +
				"' d='" + icon.path + "'/></svg>";
	QSvgRenderer r(QByteArray(svg.data(), (qsizetype)svg.size()));
	if (r.isValid())
		r.render(&painter, at);
}

/* The words a rotator puts beside each icon, all at one size: as big as 60% of
   the box's height, smaller when the longest link would pass 92% of its width
   (the icon and the gap after it count, 1.3 times the size). */
qreal social_px(const SvPiece &p, const QFont &base, int w, int h)
{
	const qreal start = std::max(4.0, h * 0.6);
	QFont f = base;
	f.setPixelSize((int)std::lround(start));
	qreal widest = 0;
	for (const auto &l : p.socials)
		widest = std::max(widest, start * 1.3 + QFontMetricsF(f).horizontalAdvance(QString::fromStdString(l.second)));
	return widest > w * 0.92 ? std::max(4.0, std::floor(start * (w * 0.92) / widest)) : start;
}

/* One link: the icon then the words, centred, with the same soft dark shadow as a channel name. */
SvColorBitmap draw_social(const SvPiece &p, const std::pair<std::string, std::string> &link, const QFont &base, qreal px)
{
	QImage img = blank(p);
	const int w = img.width(), h = img.height();
	const QString text = QString::fromStdString(link.second);
	QFont f = base;
	f.setPixelSize((int)std::lround(px));
	const QFontMetricsF fm(f);
	const qreal total = px * 1.3 + fm.horizontalAdvance(text);
	const qreal x0 = (w - total) / 2;
	const qreal baseY = (h - (fm.ascent() + fm.descent())) / 2 + fm.ascent();
	const QRectF iconAt(x0, (h - px) / 2, px, px);
	const SocialIcon &icon = social_icon(link.first);

	/* The icon and letters alone, 2 px lower, to make the shadow from. */
	QImage shape(w, h, QImage::Format_ARGB32_Premultiplied);
	shape.fill(Qt::transparent);
	{
		QPainter sp(&shape);
		sp.setRenderHint(QPainter::Antialiasing);
		sp.setRenderHint(QPainter::TextAntialiasing);
		paint_icon(sp, icon, 0xffffffff, iconAt.translated(0, 2));
		sp.setFont(f);
		sp.setPen(Qt::white);
		sp.drawText(QPointF(x0 + px * 1.3, baseY + 2), text);
	}
	std::vector<float> a((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const QRgb *row = reinterpret_cast<const QRgb *>(shape.constScanLine(y));
		for (int xx = 0; xx < w; xx++)
			a[(size_t)y * w + xx] = qAlpha(row[xx]) / 255.0f;
	}
	blur_alpha(a, w, h, 3.0f);
	for (int y = 0; y < h; y++) {
		QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int xx = 0; xx < w; xx++)
			row[xx] = qRgba(0, 0, 0, (int)std::lround(std::min(1.0f, a[(size_t)y * w + xx]) * 0.6f * 255));
	}

	QPainter painter(&img);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::TextAntialiasing);
	paint_icon(painter, icon, icon.colour ? icon.colour : p.textColor, iconAt);
	painter.setFont(f);
	const uint32_t c = p.textColor;
	painter.setPen(QColor((c >> 16) & 255, (c >> 8) & 255, c & 255));
	painter.drawText(QPointF(x0 + px * 1.3, baseY), text);
	painter.end();
	return to_bitmap(img);
}

} // namespace

std::vector<SvColorBitmap> sv_brand_socials(const SvPiece &p)
{
	std::vector<SvColorBitmap> out;
	if (p.src != SvSrc::Social || p.socials.empty())
		return out;
	const QFont f = name_font(p.font);
	const QImage size = blank(p);
	const qreal px = social_px(p, f, size.width(), size.height());
	for (const auto &l : p.socials)
		out.push_back(draw_social(p, l, f, px));
	return out;
}

void sv_brand_social_at(size_t count, float secs, uint64_t ns, size_t &index, float &alpha)
{
	index = 0;
	alpha = 1.0f;
	if (count <= 1)
		return;
	/* Each link shows for secs; the last 350 ms fades out and the first 350 ms of the next fades in. */
	const uint64_t each = (uint64_t)std::max(1.0, (double)secs * 1000.0) * 1000000ULL;
	const uint64_t t = ns % (each * count);
	index = (size_t)(t / each);
	const double local = (double)(t - index * each) / 1e6;
	const double fade = 350.0;
	alpha = (float)std::clamp(std::min(local / fade, ((double)each / 1e6 - local) / fade), 0.0, 1.0);
}

void sv_brand_blur(std::vector<float> &a, int w, int h, float sigma)
{
	if (w > 0 && h > 0 && a.size() == (size_t)w * h)
		blur_alpha(a, w, h, sigma);
}

bool sv_brand_logo_url_ok(const std::string &url)
{
	const std::string prefix = std::string(SV_SUPABASE_URL) + "/storage/v1/object/public/brand-kits/";
	if (url.size() <= prefix.size() || url.size() > 1024 || url.compare(0, prefix.size(), prefix) != 0)
		return false;
	for (const char c : url.substr(prefix.size()))
		if ((unsigned char)c <= 0x20 || c == '\\' || c == '"' || c == '<' || c == '>' || (unsigned char)c >= 0x7f)
			return false;
	return url.find("..") == std::string::npos;
}

SvColorBitmap sv_brand_picture(const SvPiece &p)
{
	if (p.src == SvSrc::Logo)
		return draw_logo(p);
	if (p.src == SvSrc::Name && !p.text.empty())
		return draw_name(p);
	return SvColorBitmap();
}

void sv_brand_on_logo(std::function<void()> f)
{
	std::lock_guard<std::mutex> lock(g_m);
	g_onLogo = std::move(f);
}

void sv_brand_unload()
{
	{
		std::lock_guard<std::mutex> lock(g_m);
		g_quit = true;
		g_onLogo = nullptr;
	}
	g_cv.notify_all();
	if (g_worker.joinable())
		g_worker.join();
	std::lock_guard<std::mutex> lock(g_m);
	g_logos.clear();
	g_queue.clear();
}
