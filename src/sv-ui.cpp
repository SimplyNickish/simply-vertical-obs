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

#include "sv-ui.h"

#include "sv-actions.h"
#include "sv-canvas.h"
#include "sv-config.h"
#include "sv-detect.h"
#include "sv-edit.h"
#include "sv-follow.h"
#include "sv-layout.h"
#include "sv-link.h"
#include <thread>
#include "sv-look.h"
#include "sv-output.h"
#include "sv-replay.h"
#include "sv-source.h"
#include "sv-sound.h"
#include "sv-store.h"
#include "sv-zoom.h"

#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QDateTime>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <map>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define T(s) obs_module_text(s)

namespace {

const QColor PROBLEM(255, 154, 138);
const QColor AMBER_INK(255, 210, 138);
const QColor RED_INK(255, 193, 193);
const QColor GREEN_INK(185, 245, 204);

QString qs(const std::string &s)
{
	return QString::fromStdString(s);
}

/* 1:02:33 once past an hour, 58:10 before. */
QString clock_text(uint64_t seconds)
{
	const uint64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
	if (h > 0)
		return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

/* Each canvas has its own colour so they read apart at a glance, the same colour
   everywhere it shows: the canvas keys, its scene keys, the platforms going to it and the
   canvas choices in Settings. A canvas named after a platform takes that platform's
   colour; the main one stays blue. */
QColor canvas_hue(const SvVCanvas &c, int index)
{
	if (c.main)
		return svl::BLUE;
	const QString n = QString::fromStdString(c.name).toLower();
	if (n.contains("twitch"))
		return QColor(169, 112, 255);
	if (n.contains("tiktok"))
		return QColor(37, 244, 238);
	if (n.contains("instagram") || n.contains("reels"))
		return QColor(214, 112, 255);
	if (n.contains("kick"))
		return QColor(83, 252, 24);
	static const QColor others[] = {svl::ORANGE, svl::PINK, svl::EMERALD, svl::LIME};
	return others[(std::max)(0, index - 1) % 4];
}

/* The canvas a platform shows, from its saved canvas id: the main one when it has none or
   its canvas has gone. */
struct DestCanvas {
	QString name;
	QColor hue;
};

DestCanvas dest_canvas(const std::string &canvasId, const std::vector<SvVCanvas> &canvases)
{
	/* OBS's own wide picture is gold, a colour no vertical canvas uses. */
	if (canvasId == SV_WIDE)
		return {QString(obs_module_text("DestCanvasWide")), svl::YELLOW};
	for (int i = 0; i < (int)canvases.size(); i++)
		if (!canvases[i].main && !canvasId.empty() && canvases[i].uuid == canvasId)
			return {QString::fromStdString(canvases[i].name), canvas_hue(canvases[i], i)};
	return {QString(obs_module_text("DestCanvasMain")), svl::BLUE};
}

svl::Words *say(const QString &text, const QColor &colour = svl::INK2, qreal px = 14)
{
	auto *w = new svl::Words(nullptr, px);
	w->setText(text, colour, false);
	return w;
}

QLabel *icon_label(const QString &name, const QColor &colour, int px)
{
	auto *l = new QLabel();
	l->setPixmap(svl::icon(name, colour, px, qApp->devicePixelRatio()));
	l->setFixedSize(px, px);
	return l;
}

svl::Key *key(const QString &text, const QString &iconName = QString(), const QColor &accent = svl::BLUE)
{
	auto *k = new svl::Key(text);
	if (!iconName.isEmpty())
		k->setIconName(iconName);
	k->setAccent(accent);
	return k;
}

std::string current_collection()
{
	char *c = obs_frontend_get_current_scene_collection();
	std::string s = c ? c : "";
	bfree(c);
	return s;
}

bool source_exists(const std::string &uuid)
{
	if (uuid.empty())
		return false;
	obs_source_t *s = obs_get_source_by_uuid(uuid.c_str());
	const bool ok = s != nullptr;
	obs_source_release(s);
	return ok;
}

void open_site()
{
	QDesktopServices::openUrl(QUrl(QStringLiteral(SV_SITE_URL)));
}

/* Help: the same page, which then opens its Go live tab, the OBS plugin box and the help
   chat (and the video guide once it is out), so the website decides what helps most. */
void open_help()
{
	QDesktopServices::openUrl(QUrl(QStringLiteral(SV_SITE_URL "/?help=obs")));
}

/* ---------- children laid out in rows that wrap ---------- */

class Flow : public QWidget {
public:
	explicit Flow(int gap, QWidget *parent = nullptr) : QWidget(parent), gap(gap)
	{
		QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
	}

	void add(QWidget *w)
	{
		w->setParent(this);
		items.push_back(w);
		w->show();
		relayout();
	}

	void clear()
	{
		/* Later, not now: a key can clear the row it sits in from its own click. */
		for (QWidget *w : items) {
			w->hide();
			w->deleteLater();
		}
		items.clear();
		relayout();
	}

	void relayout()
	{
		if (beforePlace && width() > 0)
			beforePlace(width());
		updateGeometry();
		place(width(), true);
	}

	/* Called with the row's width before the children are placed, so they can size themselves to it. */
	std::function<void(int)> beforePlace;

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override
	{
		/* Children that size themselves to the row must do so first, or the height is for their old size. */
		if (beforePlace && w > 0)
			beforePlace(w);
		return place(w, false);
	}
	QSize sizeHint() const override
	{
		const int w = width() > 0 ? width() : 280;
		return QSize(w, place(w, false));
	}
	QSize minimumSizeHint() const override
	{
		int w = 1;
		for (QWidget *i : items) {
			if (!i->isHidden())
				w = (std::max)(w, i->sizeHint().width());
		}
		/* Height at the width it really has, not stacked one per row, or the
		   panel below grows a gap. */
		return QSize(w, place((std::max)(w, width()), false));
	}

protected:
	bool event(QEvent *e) override
	{
		if (e->type() == QEvent::LayoutRequest) {
			relayout();
			return true;
		}
		return QWidget::event(e);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (beforePlace)
			beforePlace(width());
		place(width(), true);
	}

private:
	int place(int width, bool apply) const
	{
		std::vector<QWidget *> shown;
		for (QWidget *i : items) {
			if (!i->isHidden())
				shown.push_back(i);
		}
		int y = 0;
		size_t i = 0;
		while (i < shown.size()) {
			int x = 0, rowH = 0;
			size_t j = i;
			while (j < shown.size()) {
				const QSize s = shown[j]->sizeHint();
				if (j > i && x + s.width() > width)
					break;
				x += s.width() + gap;
				rowH = (std::max)(rowH, s.height());
				j++;
			}
			if (apply) {
				int xx = 0;
				for (size_t k = i; k < j; k++) {
					const QSize s = shown[k]->sizeHint();
					shown[k]->setGeometry(xx, y + (rowH - s.height()) / 2, s.width(), s.height());
					xx += s.width() + gap;
				}
			}
			y += rowH;
			i = j;
			if (i < shown.size())
				y += gap;
		}
		return y;
	}

	std::vector<QWidget *> items;
	int gap;
};

/* ---------- a title with a soft glow ---------- */

class Title : public QWidget {
public:
	Title(const QString &t, qreal size, QWidget *parent = nullptr) : QWidget(parent), text(t), px(size)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
	QSize sizeHint() const override
	{
		const QFontMetricsF fm(svl::head(px));
		return QSize((int)fm.horizontalAdvance(text) + 8, (int)std::ceil(fm.height()) + 6);
	}
	QSize minimumSizeHint() const override { return QSize(40, sizeHint().height()); }
	void setText(const QString &t)
	{
		text = t;
		updateGeometry();
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::TextAntialiasing);
		p.setFont(svl::head(px));
		svl::glow_text(p, QRectF(rect()), Qt::AlignLeft | Qt::AlignVCenter, text, Qt::white, svl::BLUE, 0.5);
	}

private:
	QString text;
	qreal px;
};

/* ---------- our windows: same backdrop, inputs and keys as the panel ---------- */

class SvWindow : public QDialog {
public:
	SvWindow(QWidget *parent, const QString &title, const QString &iconName) : QDialog(parent)
	{
		svl::load_fonts();
		svl::mark_tips(this);
		setWindowTitle(title);
		setStyleSheet(svl::sheet());
		setModal(true);
		v = new QVBoxLayout(this);
		v->setContentsMargins(20, 18, 20, 18);
		v->setSpacing(10);
		auto *top = new QHBoxLayout();
		top->setSpacing(9);
		top->addWidget(icon_label(iconName, svl::BLUE, 18));
		top->addWidget(new Title(title, 17), 1);
		v->addLayout(top);
		v->addSpacing(2);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		svl::paint_backdrop(p, rect(), true);
	}

	/* A drop-down whose edge takes the colour of the tab it sits on. */
	static void hue_combo(QComboBox *c, const QColor &h)
	{
		const QString rgb = QString("%1,%2,%3").arg(h.red()).arg(h.green()).arg(h.blue());
		c->setStyleSheet(QString("QComboBox { border: 1px solid rgba(%1,0.35); }"
					 "QComboBox:focus, QComboBox:hover { border: 1px solid rgba(%1,0.8); }")
					 .arg(rgb));
	}

	/* A small uppercase field title with an (i) hover beside it. */
	QHBoxLayout *field_row(const QString &label, const QString &tip = QString())
	{
		auto *row = new QHBoxLayout();
		row->setSpacing(6);
		row->addWidget(svl::field(label, rowHue));
		if (!tip.isEmpty())
			row->addWidget(new svl::Info(tip));
		row->addStretch(1);
		return row;
	}

	/* The colour field titles take; a tabbed window sets it per tab. */
	QColor rowHue = svl::BLUE;

	QHBoxLayout *button_row()
	{
		auto *b = new QHBoxLayout();
		b->setSpacing(8);
		v->addSpacing(6);
		v->addLayout(b);
		return b;
	}

	QVBoxLayout *v = nullptr;
};

/* ---------- the live picture in the panel ---------- */

enum class Zones { Off = 0, TikTok, YouTube, Instagram, Twitch };

struct ZoneBox {
	int x, y, w, h;
	const char *label; /* a locale key, or null */
	bool side;         /* a strip phones cut off, drawn in amber */
};

struct ZoneSet {
	const char *name;
	QColor colour;
	std::vector<ZoneBox> boxes;
};

/* What each app's own buttons and chat cover on a phone, in 1080 by 1920 pixels. */
const ZoneSet &zone_set(Zones z)
{
	static const ZoneSet tiktok{"ZonesTikTok",
				    QColor(37, 244, 238),
				    {{0, 0, 1080, 100, "ZoneTikTokTop", false},
				     {0, 1250, 810, 670, "ZoneTikTokComments", false}}};
	static const ZoneSet youtube{"ZonesYouTube",
				     QColor(255, 59, 59),
				     {{0, 0, 1080, 250, "ZoneYouTubeTop", false},
				      {0, 1270, 793, 500, "ZoneYouTubeChat", false},
				      {0, 1770, 1080, 150, "ZoneYouTubeChatBox", false},
				      {0, 0, 55, 1920, "ZoneCutOff", true},
				      {1025, 0, 55, 1920, nullptr, true}}};
	static const ZoneSet instagram{"ZonesInstagram",
				       QColor(208, 98, 255),
				       {{0, 0, 1080, 125, "ZoneInstagramTop", false},
					{0, 1220, 1048, 700, "ZoneInstagramComments", false}}};
	static const ZoneSet twitch{"ZonesTwitch",
				    QColor(145, 71, 255),
				    {{0, 0, 1080, 150, "ZoneTwitchTop", false},
				     {0, 1245, 1080, 675, "ZoneTwitchChat", false}}};
	static const ZoneSet none{"ZonesOff", svl::BLUE, {}};
	switch (z) {
	case Zones::TikTok:
		return tiktok;
	case Zones::YouTube:
		return youtube;
	case Zones::Instagram:
		return instagram;
	case Zones::Twitch:
		return twitch;
	default:
		return none;
	}
}

QPixmap stripes()
{
	static QPixmap tile;
	if (tile.isNull()) {
		QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
		for (int y = 0; y < 16; y++) {
			for (int x = 0; x < 16; x++) {
				const int a = ((x + y) % 8) < 4 ? 46 : 13;
				img.setPixel(x, y, qRgba(a, a, a, a));
			}
		}
		tile = QPixmap::fromImage(img);
	}
	return tile;
}

/* ---------- Real view: each app's live screen, as viewers see it ---------- */

/* A lifelike copy of one app's live screen over the canvas, so a streamer sees how the layout
   really looks with the app on top. Every piece sits where the website's Real view puts it,
   on a 1080 by 1920 sheet. Names and messages are made
   up. Only on the panel's picture, never in what goes out. */
namespace realview {

struct Chat {
	const char *name;
	const char *msg;
	QColor hue;
};

const std::vector<Chat> &chat()
{
	static const std::vector<Chat> lines{{"pixelpanda", "that clutch was insane", QColor(0xff9f43)},
					     {"nova_fps", "what sens do you play on?", QColor(0x54a0ff)},
					     {"kaito.gg", "W stream as always", QColor(0x1dd1a1)},
					     {"mossy_main", "first time here, love the setup", QColor(0xff6b9d)},
					     {"lunarbyte", "GG", QColor(0xc56cf0)}};
	return lines;
}

class Sheet {
public:
	Sheet(QPainter &painter, const QRectF &pic, qreal dpr) : p(painter), dpr(dpr), s(pic.width() / 1080.0)
	{
		p.translate(pic.topLeft());
		p.scale(s, s);
	}

	static QFont font(int px, int weight) { return svl::body_exact(px, weight); }
	static qreal width_of(const QFont &f, const QString &t) { return QFontMetricsF(f).horizontalAdvance(t); }

	/* Text with the soft dark shadow the apps put under words over video. */
	void text(const QPointF &at, const QFont &f, const QString &t, const QColor &c, bool shadow = true)
	{
		p.setFont(f);
		const QFontMetricsF fm(f);
		const QPointF base(at.x(), at.y() + fm.ascent());
		if (shadow) {
			p.setPen(QColor(0, 0, 0, 150));
			p.drawText(base + QPointF(0, 2), t);
			p.setPen(QColor(0, 0, 0, 70));
			p.drawText(base + QPointF(0, 4), t);
		}
		p.setPen(c);
		p.drawText(base, t);
	}
	/* Text centred on a point's row, from x. */
	void text_mid(qreal x, qreal midY, const QFont &f, const QString &t, const QColor &c, bool shadow = true)
	{
		text(QPointF(x, midY - QFontMetricsF(f).height() / 2), f, t, c, shadow);
	}

	void icon(const char *name, qreal x, qreal y, int size, const QColor &c, bool shadow = false)
	{
		const int px = (std::max)(4, (int)std::lround(size * s));
		const QPixmap pm = svl::icon(name, c, px, dpr);
		if (shadow)
			p.drawPixmap(QRectF(x, y + 2, size, size), svl::icon(name, QColor(0, 0, 0, 150), px, dpr),
				     QRectF(QPointF(0, 0), QSizeF(pm.size())));
		p.drawPixmap(QRectF(x, y, size, size), pm, QRectF(QPointF(0, 0), QSizeF(pm.size())));
	}

	void round(const QRectF &r, const QColor &fill, qreal radius = -1)
	{
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		const qreal rr = radius < 0 ? r.height() / 2 : radius;
		p.drawRoundedRect(r, rr, rr);
	}

	/* A see-through round button, the way TikTok and Instagram draw them over video:
	   an icon and words, centred. */
	void glass(const QRectF &r, const char *iconName, const QColor &iconColour, int iconSize, const QString &t,
		   int px, int weight = QFont::DemiBold, const QColor &fill = QColor(0, 0, 0, 77))
	{
		round(r, fill);
		const QFont f = font(px, weight);
		const qreal gap = iconName && !t.isEmpty() ? 10 : 0;
		const qreal w = (iconName ? iconSize : 0) + gap + (t.isEmpty() ? 0 : width_of(f, t));
		qreal x = r.center().x() - w / 2;
		if (iconName) {
			icon(iconName, x, r.center().y() - iconSize / 2.0, iconSize, iconColour);
			x += iconSize + gap;
		}
		if (!t.isEmpty())
			text_mid(x, r.center().y(), f, t, Qt::white, false);
	}

	void avatar(qreal x, qreal y, int size, const QColor &hue, const QString &letter)
	{
		const QRectF r(x, y, size, size);
		QRadialGradient g(QPointF(x + size * 0.3, y + size * 0.25), size * 0.75);
		g.setColorAt(0, svl::mix(hue, Qt::white, 0.3));
		g.setColorAt(0.55, hue);
		g.setColorAt(1, svl::mix(hue, Qt::black, 0.45));
		p.setPen(Qt::NoPen);
		p.setBrush(g);
		p.drawEllipse(r);
		const QFont f = font((int)std::lround(size * 0.45), QFont::Black);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(r, Qt::AlignCenter, letter);
	}

	void fade_top(qreal height, qreal alpha)
	{
		QLinearGradient g(0, 0, 0, height);
		g.setColorAt(0, QColor(0, 0, 0, (int)(alpha * 255)));
		g.setColorAt(1, QColor(0, 0, 0, 0));
		p.fillRect(QRectF(0, 0, 1080, height), g);
	}

	/* Chat rows stacked up from the bottom of the area, fading out near its top. */
	template<typename Row>
	void stack(const QRectF &area, qreal gap, qreal fade, const std::vector<qreal> &heights, Row row)
	{
		qreal total = 0;
		for (qreal h : heights)
			total += h;
		total += gap * (std::max)(0, (int)heights.size() - 1);
		qreal y = area.bottom() - total;
		p.save();
		p.setClipRect(area);
		for (size_t i = 0; i < heights.size(); i++) {
			const qreal into = (y + heights[i] / 2 - area.top()) / (area.height() * fade);
			const qreal o = std::clamp(into, 0.0, 1.0);
			if (o > 0.02) {
				p.setOpacity(o);
				row(i, y);
			}
			y += heights[i] + gap;
		}
		p.setOpacity(1);
		p.restore();
	}

	QPainter &p;
	qreal dpr;
	qreal s;
};

void tiktok(Sheet &k)
{
	/* TikTok on Fit, its choice for gaming: only the ranking row and the comments are over the stream. */
	k.glass(QRectF(37, 33, 385, 66), "trophy", QColor(0xffb020), 28, "Daily Ranking", 30);
	k.glass(QRectF(475, 33, 260, 66), nullptr, QColor(), 0, "Top viewers", 30);
	k.glass(QRectF(752, 33, 288, 66), nullptr, QColor(), 0, "LIVE Fest", 30);
	const QFont nf = Sheet::font(33, QFont::DemiBold), mf = Sheet::font(37, QFont::DemiBold);
	const qreal rowH = (std::max)(76.0, 33 * 1.25 + 37 * 1.25);
	std::vector<qreal> hs(chat().size(), rowH);
	k.stack(QRectF(37, 1250, 773, 650), 26, 0.18, hs, [&](size_t i, qreal y) {
		const Chat &c = chat()[i];
		k.avatar(37, y, 76, c.hue, QString(QChar(c.name[0])).toUpper());
		k.text(QPointF(37 + 76 + 18, y), nf, c.name, QColor(0xffe3ec));
		k.text(QPointF(37 + 76 + 18, y + 33 * 1.25), mf, c.msg, Qt::white);
	});
}

void youtube(Sheet &k)
{
	/* The stream ends at the top of YouTube's app bar, so the app bar is not over it. */
	k.fade_top(300, 0.45);
	k.icon("arrow-left", 106, 176, 54, Qt::white, true);
	k.avatar(199, 163, 84, QColor(0xff4d6d), "S");
	k.text(QPointF(303, 166), Sheet::font(34, QFont::DemiBold), "@yourname", Qt::white);
	const QFont sf = Sheet::font(29, QFont::Normal);
	const qreal mid = 166 + 34 * 1.25 + 29 * 0.62;
	k.round(QRectF(303, mid - 8, 16, 16), QColor(0xff0000));
	k.text_mid(303 + 24, mid, sf, "Live", Qt::white);
	const qreal ux = 303 + 24 + Sheet::width_of(sf, "Live") + 18;
	k.icon("users", ux, mid - 13.5, 27, Qt::white, true);
	k.text_mid(ux + 35, mid, sf, "213", Qt::white);
	k.round(QRectF(697, 178, 182, 58), Qt::white);
	const QFont bf = Sheet::font(30, QFont::DemiBold);
	k.text_mid(697 + (182 - Sheet::width_of(bf, "Subscribe")) / 2, 178 + 29, bf, "Subscribe", QColor(0x0f0f0f), false);
	k.icon("dots", 925, 180, 52, Qt::white, true);

	const QFont nf = Sheet::font(30, QFont::DemiBold), mf = Sheet::font(31, QFont::Normal);
	std::vector<qreal> hs(chat().size(), 60);
	k.stack(QRectF(104, 1300, 700, 470), 26, 0.2, hs, [&](size_t i, qreal y) {
		const Chat &c = chat()[i];
		k.avatar(104, y, 60, c.hue, QString(QChar(c.name[0])).toUpper());
		k.text_mid(104 + 80, y + 30, nf, c.name, QColor(0xe2e9ff));
		k.text_mid(104 + 80 + Sheet::width_of(nf, c.name) + 20, y + 30, mf, c.msg, Qt::white);
	});

	k.round(QRectF(104, 1806, 740, 76), QColor(255, 255, 255, 46));
	k.text_mid(104 + 34, 1806 + 38, Sheet::font(32, QFont::Normal), "Chat..", QColor(0xf1f4ff), false);
	k.icon("smile", 104 + 740 - 34 - 44, 1806 + 16, 44, QColor(0xf1f4ff));
	k.round(QRectF(880, 1806, 96, 76), QColor(255, 255, 255, 46));
	k.icon("dollar", 880 + 27, 1806 + 17, 42, Qt::white);
}

void instagram(Sheet &k)
{
	k.fade_top(220, 0.35);
	QLinearGradient ring(QPointF(45, 35 + 88), QPointF(45 + 88, 35));
	ring.setColorAt(0, QColor(0xfeda75));
	ring.setColorAt(0.35, QColor(0xfa7e1e));
	ring.setColorAt(0.7, QColor(0xd62976));
	ring.setColorAt(1, QColor(0x962fbf));
	k.p.setPen(Qt::NoPen);
	k.p.setBrush(ring);
	k.p.drawEllipse(QRectF(45, 35, 88, 88));
	k.avatar(49, 39, 80, QColor(0xff4d6d), "S");
	const QFont nf = Sheet::font(34, QFont::DemiBold);
	k.text_mid(170, 58 + 21, nf, "yourname", Qt::white);
	k.icon("chevron", 170 + Sheet::width_of(nf, "yourname") + 10, 58 + 4, 34, Qt::white, true);
	k.round(QRectF(549, 33, 104, 90), QColor(0xff0069), 14);
	const QFont lf = Sheet::font(30, QFont::Bold);
	k.text_mid(549 + (104 - Sheet::width_of(lf, "LIVE")) / 2, 33 + 45, lf, "LIVE", Qt::white, false);
	k.round(QRectF(664, 33, 161, 90), QColor(0, 0, 0, 89), 14);
	const QFont vf = Sheet::font(30, QFont::DemiBold);
	const qreal vw = 34 + 10 + Sheet::width_of(vf, "7,968");
	k.icon("eye", 664 + (161 - vw) / 2, 33 + 28, 34, Qt::white);
	k.text_mid(664 + (161 - vw) / 2 + 44, 33 + 45, vf, "7,968", Qt::white, false);
	k.icon("dots", 862, 50, 56, Qt::white, true);
	k.icon("x", 960, 46, 64, Qt::white, true);

	const QFont cn = Sheet::font(30, QFont::DemiBold), cm = Sheet::font(31, QFont::Normal);
	std::vector<qreal> hs(chat().size(), 80);
	k.stack(QRectF(52, 1226, 880, 680), 30, 0.18, hs, [&](size_t i, qreal y) {
		const Chat &c = chat()[i];
		k.avatar(52, y, 80, c.hue, QString(QChar(c.name[0])).toUpper());
		const qreal top = y + (80 - (30 + 31) * 1.25) / 2;
		k.text(QPointF(52 + 104, top), cn, c.name, Qt::white);
		k.text(QPointF(52 + 104, top + 30 * 1.25), cm, c.msg, Qt::white);
	});
	const qreal ys[] = {1290, 1480, 1560, 1690, 1820};
	for (int i = 0; i < 5; i++) {
		k.p.setOpacity(0.45 + i * 0.12);
		k.icon("heart-fill", 980, ys[i], 54, QColor(0xff3d6e), true);
	}
	k.p.setOpacity(1);
}

void twitch(Sheet &k)
{
	k.avatar(46, 34, 100, QColor(0x9147ff), "S");
	const QFont ff = Sheet::font(32, QFont::DemiBold);
	const qreal fw = 34 + 34 + 12 + Sheet::width_of(ff, "Follow") + 34;
	k.round(QRectF(700, 37, fw, 85), Qt::white);
	k.icon("heart", 700 + 34, 37 + 25.5, 34, QColor(0x0e0e10));
	k.text_mid(700 + 34 + 34 + 12, 37 + 42.5, ff, "Follow", QColor(0x0e0e10), false);
	k.round(QRectF(940, 37, 85, 85), QColor(0, 0, 0, 102));
	k.icon("star", 940 + 22.5, 37 + 22.5, 40, Qt::white);

	QLinearGradient g(0, 1170, 0, 1920);
	g.setColorAt(0, QColor(0, 0, 0, 0));
	g.setColorAt(0.25, QColor(0, 0, 0, 140));
	g.setColorAt(1, QColor(0, 0, 0, 178));
	k.p.fillRect(QRectF(0, 1170, 1080, 750), g);

	/* Enough lines to fill the chat area the way a busy chat does. */
	std::vector<Chat> lines{{"arcadeowl", "that rotation was clean", QColor(0xff69b4)},
				{"frostbyte_", "is this ranked?", QColor(0x1e90ff)},
				{"mintyfresh", "hello from Brazil", QColor(0x9acd32)},
				{"gravelpit", "what crosshair is that", QColor(0xdaa520)},
				{"sunnyside_up", "lurking but loving it", QColor(0xff7f50)},
				{"echo_rift", "just got here, what did I miss?", QColor(0x00c8af)},
				{"brightsideAJ", "that last round was so close", QColor(0xb8a1ff)},
				{"tomato_tactics", "LETS GOOO", QColor(0xff4500)}};
	for (const Chat &c : chat())
		lines.push_back(c);
	lines.push_back({"stormcaller", "how long have you been playing this?", QColor(0xff7f50)});
	lines.push_back({"quietkoala", "the vertical layout looks so clean", QColor(0x2e8b57)});
	const QFont nb = Sheet::font(33, QFont::Bold), mt = Sheet::font(33, QFont::Normal);
	std::vector<qreal> hs(lines.size(), 33 * 1.3);
	k.stack(QRectF(30, 1245, 1020, 585), 12, 0.1, hs, [&](size_t i, qreal y) {
		const Chat &c = lines[i];
		k.text(QPointF(30, y), nb, c.name, c.hue);
		k.text(QPointF(30 + Sheet::width_of(nb, c.name), y), mt, QString(": ") + c.msg, Qt::white);
	});
	k.round(QRectF(30, 1842, 1020, 72), QColor(0x772ce8), 18);
	k.icon("pin", 30 + 24, 1842 + 20, 32, Qt::white);
	k.text_mid(30 + 24 + 32 + 16, 1842 + 36, Sheet::font(29, QFont::DemiBold), "Giveaway at 500 followers, type !join",
		   Qt::white, false);
}

void draw(QPainter &p, const QRectF &pic, qreal dpr, Zones app)
{
	p.save();
	Sheet k(p, pic, dpr);
	switch (app) {
	case Zones::TikTok:
		tiktok(k);
		break;
	case Zones::YouTube:
		youtube(k);
		break;
	case Zones::Instagram:
		instagram(k);
		break;
	case Zones::Twitch:
		twitch(k);
		break;
	default:
		break;
	}
	p.restore();
}

} // namespace realview

/* One app's guides drawn as a small phone, with the app's logo and short name under it. */
class SvGuidePhone : public QAbstractButton {
public:
	SvGuidePhone(Zones z, const std::string &logoName, const QString &shortName, QWidget *parent = nullptr)
		: QAbstractButton(parent),
		  zone(z),
		  logoName(logoName),
		  shortName(shortName)
	{
		setCursor(Qt::PointingHandCursor);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		setToolTip(QString(T("GuidesAppTip")).arg(T(zone_set(z).name)));
		setAccessibleName(toolTip());
		svl::set_tip_colour(this, zone_set(z).colour);
	}
	/* Lit is the app showing now; dimmed are the others while one shows. */
	void setState(bool isLit, bool isDim)
	{
		lit = isLit;
		dim = isDim;
		update();
	}
	QSize sizeHint() const override { return QSize(86, 150); }
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const ZoneSet &zs = zone_set(zone);
		const QColor c = zs.colour;
		if (dim && !hover)
			p.setOpacity(0.42);
		const QRectF ph((width() - 58) / 2.0, 10, 58, 103);
		const qreal rad = 9;
		QPainterPath path;
		path.addRoundedRect(ph, rad, rad);
		if (lit || hover)
			svl::shape_shadow(p, path, QPointF(0, 0), 16, svl::alpha(c, lit ? 0.55 : 0.3));
		svl::shape_shadow(p, path, QPointF(3, 3), 8, QColor(0, 0, 0, 205));
		QLinearGradient g(ph.topLeft(), ph.bottomLeft());
		g.setColorAt(0, svl::mix(c, QColor(0x14, 0x14, 0x1e), lit ? 0.14 : 0.06));
		g.setColorAt(0.5, QColor(0x14, 0x14, 0x1e));
		g.setColorAt(1, QColor(0x10, 0x10, 0x19));
		p.fillPath(path, g);

		/* The app's own buttons and chat, where they sit on a real phone. */
		p.save();
		p.setClipPath(path);
		const qreal sx = ph.width() / 1080.0, sy = ph.height() / 1920.0;
		for (const ZoneBox &b : zs.boxes) {
			const QRectF r(ph.left() + b.x * sx, ph.top() + b.y * sy, b.w * sx, b.h * sy);
			const QColor k = b.side ? svl::AMBER : c;
			p.fillRect(r, svl::alpha(k, b.side ? 0.32 : 0.3));
			p.setPen(QPen(svl::alpha(k, 0.8), 1));
			p.setBrush(Qt::NoBrush);
			p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
		}
		p.restore();
		p.setPen(QPen(svl::alpha(c, lit ? 0.9 : 0.35), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(ph.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);

		const qreal dpr = devicePixelRatioF();
		const QFont nf = svl::body(12.5, QFont::Bold);
		const QFontMetricsF fm(nf);
		const int lg = 16;
		const qreal tw = fm.horizontalAdvance(shortName);
		const qreal x0 = (width() - (lg + 5 + tw)) / 2.0, y0 = ph.bottom() + 10;
		p.drawPixmap(QPointF(x0, y0 + (fm.height() - lg) / 2.0), svl::logo(logoName, lg, dpr));
		p.setFont(nf);
		svl::glow_text(p, QRectF(x0 + lg + 5, y0, tw + 4, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, shortName,
			       lit ? QColor(Qt::white) : svl::mix(c, svl::INK, 0.45), c, lit ? 0.8 : 0.0);
	}

private:
	Zones zone;
	std::string logoName;
	QString shortName;
	bool lit = false, dim = false, hover = false;
};

/* The app guides picker: each app's guides as a small phone. Picking one shows it; Hide turns them off.
   It opens just above the key and closes itself on a pick or a click anywhere else. */
class SvGuidePicker : public QFrame {
public:
	SvGuidePicker(QWidget *parent, bool on, Zones chosen, bool real, std::function<void(Zones)> pick,
		      std::function<void()> hide, std::function<void(bool)> setReal)
		: QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
	{
		setAttribute(Qt::WA_DeleteOnClose);
		svl::mark_tips(this);
		setStyleSheet(svl::sheet());
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(10, 10, 10, 10);
		auto *card = new svl::Card(T("GuidesHeading"), "eye");
		card->setAccent(on ? zone_set(chosen).colour : svl::LIME);
		card->setValue(on ? QString(T(zone_set(chosen).name)) : QString(T("ZonesOff")));
		v->addWidget(card);
		auto *row = new QHBoxLayout();
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);
		const char *logos[] = {"tiktok", "youtube", "instagram", "twitch"};
		const char *shorts[] = {"GuidesShortTikTok", "GuidesShortYouTube", "GuidesShortInstagram", "GuidesShortTwitch"};
		for (int i = 0; i < 4; i++) {
			const Zones z = (Zones)(i + 1);
			auto *phone = new SvGuidePhone(z, logos[i], T(shorts[i]));
			phone->setState(on && chosen == z, on && chosen != z);
			connect(phone, &QAbstractButton::clicked, this, [this, pick, z] {
				close();
				pick(z);
			});
			row->addWidget(phone);
		}
		card->body()->addLayout(row);
		auto *note = new svl::Words();
		note->setText(T("GuidesOnlyYou"), svl::INK2);
		card->body()->addWidget(note);
		/* Show as plain lines, or as the app really looks on a viewer's phone. */
		const QColor tint = on ? zone_set(chosen).colour : svl::LIME;
		auto *linesKey = key(T("GuidesLines"), "layout", tint);
		auto *realKey = key(T("GuidesReal"), "smartphone", tint);
		linesKey->setToolTip(T("GuidesLinesTip"));
		realKey->setToolTip(T("GuidesRealTip"));
		linesKey->setUnchosen(real);
		realKey->setUnchosen(!real);
		auto choose = [linesKey, realKey, setReal](bool wantReal) {
			linesKey->setUnchosen(wantReal);
			realKey->setUnchosen(!wantReal);
			setReal(wantReal);
		};
		connect(linesKey, &QPushButton::clicked, this, [choose] { choose(false); });
		connect(realKey, &QPushButton::clicked, this, [choose] { choose(true); });
		auto *keyRow = new QHBoxLayout();
		keyRow->setContentsMargins(0, 2, 0, 0);
		keyRow->setSpacing(6);
		keyRow->addStretch(1);
		keyRow->addWidget(linesKey);
		keyRow->addWidget(realKey);
		if (on) {
			auto *hideKey = key(T("GuidesHideKey"), "eye-off", zone_set(chosen).colour);
			hideKey->setToolTip(T("GuidesHide"));
			connect(hideKey, &QPushButton::clicked, this, [this, hide] {
				close();
				hide();
			});
			keyRow->addSpacing(10);
			keyRow->addWidget(hideKey);
		}
		keyRow->addStretch(1);
		card->body()->addLayout(keyRow);
	}

	/* Opens just above the key, its right edge lined up with the key's, kept on the screen. */
	void open_above(QWidget *anchor)
	{
		adjustSize();
		const QSize s = sizeHint().expandedTo(size());
		const QPoint keyTop = anchor->mapToGlobal(QPoint(0, 0));
		QPoint at(keyTop.x() + anchor->width() - s.width(), keyTop.y() - s.height() - 6);
		if (QScreen *screen = QGuiApplication::screenAt(keyTop)) {
			const QRect area = screen->availableGeometry();
			if (at.y() < area.top())
				at.setY(keyTop.y() + anchor->height() + 6);
			at.setX(std::clamp(at.x(), area.left(), (std::max)(area.left(), area.right() - s.width())));
		}
		resize(s);
		move(at);
		show();
		raise();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		svl::paint_backdrop(p, rect(), true);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(svl::alpha(svl::BLUE, 0.45), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
	}
};

class SvPreview : public QWidget {
public:
	explicit SvPreview(QWidget *backdropHost, QWidget *parent = nullptr) : QWidget(parent), host(backdropHost)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(130, 150);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMouseTracking(true);
		setFocusPolicy(Qt::ClickFocus);
		/* Guides on or off, and which app they show, are kept apart so turning
		   them off and on again brings back the same app. Older saves kept
		   both in one number, 0 meaning off. */
		const int old = std::clamp(sv_store_int("preview_zones", 0), 0, 4);
		app = (Zones)std::clamp(sv_store_int("guides_app", old > 0 ? old : 1), 1, 4);
		guidesOn = sv_store_int("guides_on", old > 0 ? 1 : 0) != 0;
		guidesLook = sv_store_int("guides_real", 0) != 0;
	}

	~SvPreview() override { shut(); }

	void shut()
	{
		sv_canvas_set_user(SV_CANVAS_PREVIEW, false, sv_canvas_view());
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
		if (tex) {
			obs_enter_graphics();
			gs_texture_destroy(tex);
			obs_leave_graphics();
			tex = nullptr;
		}
		closed = true;
	}

	bool guides() const { return guidesOn; }
	Zones guidesApp() const { return app; }
	bool guidesReal() const { return guidesLook; }
	void setGuidesReal(bool real)
	{
		guidesLook = real;
		sv_store_set_int("guides_real", real ? 1 : 0);
		rebuild();
	}
	void setGuides(bool on)
	{
		guidesOn = on;
		sv_store_set_int("guides_on", on ? 1 : 0);
		rebuild();
	}
	void setGuidesApp(Zones z)
	{
		if (z == Zones::Off)
			return;
		app = z;
		sv_store_set_int("guides_app", (int)z);
		rebuild();
	}

	/* The shortest the picture may get before the cards below start to scroll. */
	static constexpr int MIN_H = 260;
	/* How tall the widget is when the picture fills a given width. */
	static int height_for(int w)
	{
		const qreal pw = std::max<qreal>(1, w - 2 * MARGIN);
		return (int)std::ceil(pw * 16.0 / 9.0 + 2 * MARGIN);
	}
	/* How wide the widget is when the picture fills a given height. */
	static int width_for(int h)
	{
		const qreal ph = std::max<qreal>(1, h - 2 * MARGIN);
		return (int)std::ceil(ph * 9.0 / 16.0 + 2 * MARGIN);
	}

	QSize sizeHint() const override { return QSize(width() > 0 ? width() : 260, MIN_H); }
	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Editing: the dock answers these. */
	std::function<void(QPoint)> onMenu;   /* right-click, at a screen point */
	std::function<void()> onSelection;    /* the selection or a layer's place changed */
	std::function<void()> onDelete;       /* the Delete key */
	SvEdit &editor() { return edit; }

protected:
	void paintEvent(QPaintEvent *) override { create(); }

	/* Canvas pixels under a point on the widget, and how many canvas pixels
	   one screen pixel covers. */
	vec2 to_canvas(const QPointF &p, float *perPx = nullptr) const
	{
		const QRectF pic = picture();
		const float s = (float)(1080.0 / std::max<qreal>(1.0, pic.width()));
		if (perPx)
			*perPx = s;
		vec2 v;
		vec2_set(&v, (float)(p.x() - pic.left()) * s, (float)(p.y() - pic.top()) * s);
		return v;
	}

	static SvEditMods mods_of(Qt::KeyboardModifiers m)
	{
		SvEditMods out;
		out.alt = m & Qt::AltModifier;
		out.shift = m & Qt::ShiftModifier;
		out.ctrl = m & Qt::ControlModifier;
		return out;
	}

	void show_cursor(SvCursor c)
	{
		switch (c) {
		case SvCursor::Move:
			setCursor(Qt::SizeAllCursor);
			break;
		case SvCursor::SizeH:
			setCursor(Qt::SizeHorCursor);
			break;
		case SvCursor::SizeV:
			setCursor(Qt::SizeVerCursor);
			break;
		case SvCursor::SizeFDiag:
			setCursor(Qt::SizeFDiagCursor);
			break;
		case SvCursor::SizeBDiag:
			setCursor(Qt::SizeBDiagCursor);
			break;
		case SvCursor::Rotate:
			setCursor(Qt::OpenHandCursor);
			break;
		case SvCursor::Rotating:
			setCursor(Qt::ClosedHandCursor);
			break;
		default:
			unsetCursor();
			break;
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		float per = 1.0f;
		const vec2 pos = to_canvas(e->position(), &per);
		if (e->button() == Qt::LeftButton) {
			show_cursor(edit.press(scene, pos, per, mods_of(e->modifiers())));
		} else if (e->button() == Qt::RightButton && scene) {
			/* Right-click picks the layer under the pointer first, like OBS. */
			obs_sceneitem_t *under = SvEdit::item_at(scene, pos);
			if (under && !obs_sceneitem_selected(under))
				SvEdit::select_only(scene, under);
		}
		obs_scene_release(scene);
		if (onSelection)
			onSelection();
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		float per = 1.0f;
		const vec2 pos = to_canvas(e->position(), &per);
		if (edit.dragging())
			show_cursor(edit.drag(scene, pos, mods_of(e->modifiers())));
		else
			show_cursor(edit.hover(scene, pos, per));
		obs_scene_release(scene);
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::RightButton) {
			if (onMenu)
				onMenu(e->globalPosition().toPoint());
			return;
		}
		if (e->button() != Qt::LeftButton)
			return;
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		const vec2 pos = to_canvas(e->position());
		edit.release(scene, pos, mods_of(e->modifiers()));
		float per = 1.0f;
		show_cursor(edit.hover(scene, to_canvas(e->position(), &per), per));
		obs_scene_release(scene);
		if (onSelection)
			onSelection();
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		obs_sceneitem_t *under = scene ? SvEdit::item_at(scene, to_canvas(e->position())) : nullptr;
		if (under)
			obs_frontend_open_source_properties(obs_sceneitem_get_source(under));
		obs_scene_release(scene);
	}

	void leaveEvent(QEvent *e) override
	{
		QWidget::leaveEvent(e);
		edit.leave();
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		bool used = true;
		const float step = (e->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
		switch (e->key()) {
		case Qt::Key_Left:
		case Qt::Key_Right:
		case Qt::Key_Up:
		case Qt::Key_Down:
			if (!e->isAutoRepeat())
				edit.remember(scene);
			SvEdit::nudge(scene, e->key() == Qt::Key_Left ? -step : e->key() == Qt::Key_Right ? step : 0.0f,
				      e->key() == Qt::Key_Up ? -step : e->key() == Qt::Key_Down ? step : 0.0f);
			break;
		case Qt::Key_Delete:
		case Qt::Key_Backspace:
			if (onDelete)
				onDelete();
			break;
		case Qt::Key_Z:
			if (e->modifiers() & Qt::ControlModifier)
				edit.undo();
			else
				used = false;
			break;
		default:
			used = false;
			break;
		}
		obs_scene_release(scene);
		if (used) {
			if (onSelection)
				onSelection();
			return;
		}
		QWidget::keyPressEvent(e);
	}

	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		if (!closed)
			sv_canvas_set_user(SV_CANVAS_PREVIEW, true, sv_canvas_view());
		if (display)
			obs_display_set_enabled(display, true);
		rebuild();
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		sv_canvas_set_user(SV_CANVAS_PREVIEW, false, sv_canvas_view());
		if (display)
			obs_display_set_enabled(display, false);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		create();
		if (isVisible() && display) {
			const QSize s = pixels();
			obs_display_resize(display, s.width(), s.height());
		}
		rebuild();
	}

	void moveEvent(QMoveEvent *e) override
	{
		QWidget::moveEvent(e);
		rebuild();
	}

private:
	static constexpr qreal MARGIN = 18;
	static constexpr qreal RADIUS = 0;

	QSize pixels() const { return size() * devicePixelRatioF(); }

	/* As big as the space allows, centred, never stretched. */
	QRectF picture() const
	{
		const qreal aw = width() - 2 * MARGIN, ah = height() - 2 * MARGIN;
		const qreal pw = std::max<qreal>(1, std::min(aw, ah * 9.0 / 16.0));
		const qreal ph = pw * 16.0 / 9.0;
		return QRectF((width() - pw) / 2.0, (height() - ph) / 2.0, pw, ph);
	}

	void create()
	{
		if (display || closed || !windowHandle() || !windowHandle()->isExposed())
			return;
		const QSize s = pixels();
		gs_init_data info = {};
		info.cx = (uint32_t)s.width();
		info.cy = (uint32_t)s.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		info.window.hwnd = (void *)winId();
		display = obs_display_create(&info, 0xFF130A0A);
		if (display)
			obs_display_add_draw_callback(display, draw, this);
	}

	/* The frame, the phone app areas, the label and the phone key, drawn once
	   into a picture that lies over the live picture. Only redrawn when one of
	   them changes, never every frame. */
	void rebuild()
	{
		if (closed || width() < 20 || height() < 20)
			return;
		const qreal dpr = devicePixelRatioF();
		const QSize px = pixels();
		QImage img(px, QImage::Format_ARGB32_Premultiplied);
		img.setDevicePixelRatio(dpr);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);

		const QRectF pic = picture();
		QPainterPath round;
		round.addRoundedRect(pic, RADIUS, RADIUS);
		QPainterPath all;
		all.addRect(QRectF(0, 0, width(), height()));
		const QPainterPath outside = all.subtracted(round);

		/* The panel background around the picture, lined up with the panel's own. */
		p.save();
		p.setClipPath(outside);
		const QPoint off = host ? mapTo(host, QPoint(0, 0)) : QPoint();
		p.translate(-off);
		svl::paint_backdrop(p, QRect(QPoint(0, 0), host ? host->size() : size()), true,
				    host && host->property("svLampY").isValid() ? host->property("svLampY").toInt() : -1);
		p.restore();

		/* Depth, like the cards and keys: a drop shadow so the canvas lifts off the panel.
		   A wide one falling below and a tight one where it sits. Clipped to outside the
		   picture, so it never touches the canvas. */
		p.save();
		p.setClipPath(outside);
		svl::shape_shadow(p, round, QPointF(0, 10), 22, QColor(0, 0, 0, 220));
		svl::shape_shadow(p, round, QPointF(0, 3), 7, QColor(0, 0, 0, 180));
		p.restore();

		/* Like OBS's own preview: the picture, a hairline round it, nothing written on it.
		   Only the guides go over it, and only when the member turns them on. */
		p.save();
		p.setClipRect(pic);
		draw_zones(p, pic);
		p.restore();

		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(255, 255, 255, 40), 1));
		p.drawRect(pic.adjusted(-0.5, -0.5, 0.5, 0.5));
		p.end();

		const QRect picPx((int)std::lround(pic.x() * dpr), (int)std::lround(pic.y() * dpr),
				  (int)std::lround(pic.width() * dpr), (int)std::lround(pic.height() * dpr));
		std::lock_guard<std::mutex> lock(m);
		pending = std::move(img);
		pendingPic = picPx;
		pendingFor = px;
		dirty = true;
	}

	void draw_zones(QPainter &p, const QRectF &pic)
	{
		if (!guidesOn)
			return;
		if (guidesLook) {
			realview::draw(p, pic, devicePixelRatioF(), app);
			return;
		}
		const ZoneSet &set = zone_set(app);
		const qreal s = pic.width() / 1080.0;
		QFont lf = svl::body_exact(9.5, QFont::ExtraBold);
		p.setFont(lf);
		const QFontMetricsF fm(lf);
		for (const ZoneBox &b : set.boxes) {
			const QColor c = b.side ? svl::AMBER : set.colour;
			const QRectF r(pic.left() + b.x * s, pic.top() + b.y * s, b.w * s, b.h * s);
			p.fillRect(r, QBrush(stripes()));
			p.fillRect(r, svl::alpha(c, 0.10));
			p.setPen(QPen(svl::alpha(c, 0.7), 1));
			p.setBrush(Qt::NoBrush);
			p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
			if (!b.label)
				continue;
			const QString text = T(b.label);
			const QColor ink = svl::mix(c, Qt::white, 0.3);
			if (b.side) {
				p.save();
				p.translate(r.left() + r.width() / 2 + fm.height() / 2 - 1, r.bottom() - 10);
				p.rotate(-90);
				svl::glow_text(p, QRectF(0, -2, fm.horizontalAdvance(text) + 4, fm.height()),
					       Qt::AlignLeft | Qt::AlignVCenter, text, ink, c, 0.8);
				p.restore();
			} else if (r.height() >= fm.height() + 6) {
				/* Top labels sit just inside the top edge, centred. */
				const bool top = b.y == 0;
				const QRectF lr(r.left() + 6, top ? pic.top() + 8 : r.top() + 4, r.width() - 12,
						top ? 16 : fm.height());
				svl::glow_text(p, lr, Qt::AlignHCenter | Qt::AlignVCenter, text, ink, c, 0.8);
			}
		}
	}

	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<SvPreview *>(data);
		QImage img;
		bool fresh = false;
		QRect pic;
		QSize made;
		{
			std::lock_guard<std::mutex> lock(self->m);
			if (self->dirty) {
				img = std::move(self->pending);
				self->dirty = false;
				fresh = true;
				self->drawPic = self->pendingPic;
				self->drawFor = self->pendingFor;
			}
			pic = self->drawPic;
			made = self->drawFor;
		}
		if (fresh && !img.isNull()) {
			const uint32_t w = (uint32_t)img.width(), h = (uint32_t)img.height();
			if (!self->tex || gs_texture_get_width(self->tex) != w || gs_texture_get_height(self->tex) != h) {
				gs_texture_destroy(self->tex);
				const uint8_t *bits = img.constBits();
				self->tex = gs_texture_create(w, h, GS_BGRA, 1, &bits, GS_DYNAMIC);
			} else {
				gs_texture_set_image(self->tex, img.constBits(), (uint32_t)img.bytesPerLine(), false);
			}
		}
		if (made != QSize((int)cx, (int)cy) || pic.isEmpty()) {
			/* Mid resize: fit the picture the plain way until the frame catches up. */
			const float scale = (std::min)((float)cx / 1080.0f, (float)cy / 1920.0f);
			const int w = (int)(1080.0f * scale), h = (int)(1920.0f * scale);
			pic = QRect(((int)cx - w) / 2, ((int)cy - h) / 2, w, h);
		}

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, 1080.0f, 0.0f, 1920.0f, -100.0f, 100.0f);
		gs_set_viewport(pic.x(), pic.y(), pic.width(), pic.height());
		sv_canvas_render(sv_canvas_view());

		if (self->tex) {
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			gs_set_viewport(0, 0, (int)cx, (int)cy);
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), self->tex);
			const bool srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(false);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(self->tex, 0, cx, cy);
			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(srgb);
		}

		/* The selection boxes and handles go on top of everything. */
		/* Drawn over the whole widget, not just the picture, so handles on the
		   picture's edge are not cut in half. */
		if (obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view())) {
			const float per = 1080.0f / (float)std::max(1, pic.width());
			gs_ortho(-pic.x() * per, ((float)cx - pic.x()) * per, -pic.y() * per, ((float)cy - pic.y()) * per,
				 -100.0f, 100.0f);
			gs_set_viewport(0, 0, (int)cx, (int)cy);
			self->edit.draw(scene, per);
			obs_scene_release(scene);
		}
		gs_projection_pop();
		gs_viewport_pop();
	}

	QWidget *host = nullptr;
	obs_display_t *display = nullptr;
	bool closed = false;
	Zones app = Zones::TikTok;
	bool guidesOn = false;
	bool guidesLook = false; /* true: Real view, the app as it looks; false: lines */
	SvEdit edit;

	std::mutex m;
	QImage pending;
	QRect pendingPic;
	QSize pendingFor;
	bool dirty = false;
	/* graphics thread only */
	gs_texture_t *tex = nullptr;
	QRect drawPic;
	QSize drawFor;
};

/* ---------- adding a platform ---------- */

struct SvPlatform {
	const char *id;
	const char *label;
	const char *server;
	const char *help;
	/* The longer steps, behind an (i) beside the one help line, so the box never opens on a wall of text. */
	const char *more = nullptr;
};

const SvPlatform PLATFORMS[] = {
	{"youtube", "PlatformYouTube", "rtmp://a.rtmp.youtube.com/live2", "HelpYouTube", "HelpYouTubeMore"},
	{"twitch", "PlatformTwitch", "rtmp://live.twitch.tv/app", "HelpTwitch", "HelpTwitchMore"},
	{"tiktok", "PlatformTikTok", "", "HelpTikTok"},
	{"kick", "PlatformKick", "", "HelpKick"},
	{"instagram", "PlatformInstagram", "", "HelpInstagram"},
	{"facebook", "PlatformFacebook", "", "HelpFacebook"},
	{"custom", "PlatformCustom", "", "HelpCustom"},
};
constexpr int PLATFORM_COUNT = (int)(sizeof(PLATFORMS) / sizeof(PLATFORMS[0]));

/* Where each app shows the stream key (checked 2026-11-19), the same pages as the
   setup guide on the website. Kick has no direct page, so its own help article. */
const char *key_page_url(const char *platform)
{
	static const struct {
		const char *id, *url;
	} pages[] = {
		{"tiktok", "https://livecenter.tiktok.com/producer"},
		{"youtube", "https://studio.youtube.com/channel/UC/livestreaming"},
		{"instagram", "https://www.instagram.com/"},
		{"twitch", "https://dashboard.twitch.tv/settings/stream"},
		{"kick", "https://help.kick.com/en/articles/7066931-how-to-stream-on-kick-com"},
		{"facebook", "https://www.facebook.com/live/producer"},
	};
	for (const auto &p : pages)
		if (std::strcmp(p.id, platform) == 0)
			return p.url;
	return nullptr;
}

/* ---------- the "Go live?" and "Stop your stream?" boxes ---------- */

class SvAskDialog : public SvWindow {
public:
	SvAskDialog(QWidget *parent, bool start, const QString &text)
		: SvWindow(parent, T(start ? "AskLiveTitle" : "AskStopTitle"), start ? "radio" : "stop")
	{
		auto *words = new svl::Words(nullptr, 14);
		words->setText(text, svl::INK2);
		v->addWidget(words);
		QHBoxLayout *buttons = button_row();
		buttons->addStretch(1);
		auto *no = key(T(start ? "DestCancel" : "AskStopKeep"), start ? "x" : "radio");
		auto *yes = key(T(start ? "AskLiveGo" : "AskStopGo"), start ? "radio" : "stop", start ? svl::BLUE : svl::RED);
		/* Enter and Escape both pick the safe answer, so a stray key press changes nothing. */
		no->setDefault(true);
		yes->setAutoDefault(false);
		buttons->addWidget(no);
		buttons->addWidget(yes);
		connect(no, &QPushButton::clicked, this, &QDialog::reject);
		connect(yes, &QPushButton::clicked, this, &QDialog::accept);
		setMinimumWidth(400);
		QTimer::singleShot(0, no, [no] { no->setFocus(); });
	}
};

/* ---------- Twitch Dual Format: what changes in OBS's own Stream settings ---------- */

class SvDualDialog : public SvWindow {
public:
	SvDualDialog(QWidget *parent, const QString &canvasName) : SvWindow(parent, T("DualTitle"), "radio")
	{
		const QColor twitch = svl::platform_colour("twitch");
		auto *intro = new svl::Words(nullptr, 14);
		intro->setText(T("DualIntro"), svl::INK2);
		v->addWidget(intro);

		const SvObsStream now = sv_output_obs_stream();
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(18);
		grid->setVerticalSpacing(8);
		grid->addWidget(svl::field(T("DualNow"), svl::BLUE), 0, 1);
		grid->addWidget(svl::field(T("DualBecomes"), twitch), 0, 2);
		int row = 1;
		/* Each cell keeps to one line; the box widens instead. */
		auto cell = [](const QString &text, const QColor &c) {
			auto *q = new QLabel(text);
			q->setStyleSheet(QString("color: %1; font-size: 16px;").arg(c.name()));
			return q;
		};
		auto add = [&](const char *label, const QString &before, const QString &after) {
			QLabel *l = cell(T(label), svl::INK);
			QLabel *b = cell(before, svl::INK2);
			/* What actually changes is lit in Twitch's colour. */
			QLabel *a = cell(after, before == after ? svl::INK2 : svl::tone(twitch, 0.25));
			grid->addWidget(l, row, 0);
			grid->addWidget(b, row, 1);
			grid->addWidget(a, row, 2);
			row++;
		};
		add("DualRowService", now.service.empty() ? QString(T("DualNone")) : qs(now.service), "Twitch");
		add("DualRowKey", now.has_key ? QString(T("DualKeyNow")) : QString(T("DualNone")), T("DualKeyBecomes"));
		add("DualRowEnhanced", T(now.enhanced ? "DualOn" : "DualOff"), T("DualOn"));
		add("DualRowCanvas", now.extra_canvas.empty() ? QString(T("DualNone")) : qs(now.extra_canvas), canvasName);
		grid->setColumnStretch(1, 1);
		grid->setColumnStretch(2, 1);
		v->addSpacing(4);
		v->addLayout(grid);
		v->addSpacing(4);

		auto *after = new svl::Words(nullptr, 14);
		after->setText(T("DualAfter"), svl::INK2);
		v->addWidget(after);

		QHBoxLayout *buttons = button_row();
		buttons->addStretch(1);
		auto *no = key(T("DestCancel"), "x");
		auto *yes = key(T("DualGo"), "check", twitch);
		no->setDefault(true);
		yes->setAutoDefault(false);
		buttons->addWidget(no);
		buttons->addWidget(yes);
		connect(no, &QPushButton::clicked, this, &QDialog::reject);
		connect(yes, &QPushButton::clicked, this, &QDialog::accept);
		setMinimumWidth(520);
		QTimer::singleShot(0, no, [no] { no->setFocus(); });
	}
};

class SvDestDialog : public SvWindow {
public:
	SvDestDialog(QWidget *parent, const SvDest &existing)
		: SvWindow(parent, T(existing.id.empty() ? "DestTitleAdd" : "DestTitle"), "radio"),
		  dest(existing)
	{
		setMinimumWidth(470);
		/* The tab rows say their smallest is every tab stacked, which would keep this box far too
		   tall, so the box works out its own height in fit(). */
		v->setSizeConstraint(QLayout::SetNoConstraint);

		v->addLayout(field_row(T("DestPlatform")));
		auto *grid = new QGridLayout();
		grid->setSpacing(0);
		for (int i = 0; i < PLATFORM_COUNT; i++) {
			const SvPlatform &p = PLATFORMS[i];
			auto *t = new svl::Tile(T(p.label), "server");
			t->setAccessibleName(T(p.label));
			if (std::strcmp(p.id, "custom") != 0)
				t->setLogo(p.id);
			t->setAccent(svl::platform_colour(p.id));
			grid->addWidget(t, i / 4, i % 4);
			tiles.push_back(t);
			connect(t, &QAbstractButton::clicked, this, [this, i] { choose(i); });
		}
		v->addLayout(grid);
		auto *helpRow = new QHBoxLayout();
		helpRow->setSpacing(6);
		help = say(QString());
		helpRow->addWidget(help, 1);
		helpMore = new svl::Info(QString());
		helpRow->addWidget(helpMore, 0, Qt::AlignTop);
		v->addLayout(helpRow);

		v->addSpacing(2);
		v->addLayout(field_row(T("DestName")));
		name = new QLineEdit();
		v->addWidget(name);

		v->addLayout(field_row(T("DestServer")));
		server = new QLineEdit();
		v->addWidget(server);

		v->addLayout(field_row(T("DestKey"), T("DestKeyTip")));
		auto *keyRow = new QHBoxLayout();
		keyRow->setSpacing(8);
		keyEdit = new QLineEdit();
		keyEdit->setEchoMode(QLineEdit::Password);
		keyEdit->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
		if (dest.has_key)
			keyEdit->setPlaceholderText(T("DestKeySaved"));
		keyRow->addWidget(keyEdit, 1);
		auto *show = key(T("DestShowKey"), "eye");
		show->setCheckable(true);
		keyRow->addWidget(show);
		/* Fetches the key with the login the member made on the dashboard. Twitch only for
		   now: Kick's needs the Kick developer settings changed first. */
		v->addLayout(keyRow);
		/* On its own line under the key box, so the box keeps the room for its hint. */
		getKeyBox = new QWidget();
		auto *getKeyRow = new QHBoxLayout(getKeyBox);
		getKeyRow->setContentsMargins(0, 0, 0, 0);
		getKey = key(T("DestGetKey"), "link");
		getKeyRow->addWidget(getKey);
		/* Every app but Other: the page where that app shows the key, in the browser. */
		keyPage = key(T("DestKeyPage"), "external");
		getKeyRow->addWidget(keyPage);
		getKeyRow->addStretch(1);
		v->addWidget(getKeyBox);
		connect(keyPage, &QPushButton::clicked, this, [this] {
			const char *url = key_page_url(current().id);
			if (url)
				QDesktopServices::openUrl(QUrl(QString::fromUtf8(url)));
		});
		keyNote = say(QString(), svl::INK2);
		keyNote->hide();
		v->addWidget(keyNote);
		connect(getKey, &QPushButton::clicked, this, [this] { get_key(); });

		/* Twitch Dual Format: Twitch takes OBS's wide picture and a vertical in one stream,
		   through OBS's own stream. Only one platform can have it, as OBS has one stream. */
		dualBox = new QWidget();
		auto *dualCol = new QVBoxLayout(dualBox);
		dualCol->setContentsMargins(0, 2, 0, 0);
		dualCol->setSpacing(4);
		auto *dualTop = new QHBoxLayout();
		dualTop->setSpacing(6);
		dual = new svl::Switch(T("DestDual"));
		dual->setChecked(dest.dual);
		dualTop->addWidget(dual);
		dualTop->addWidget(new svl::Info(T("DestDualTip")));
		dualTop->addStretch(1);
		dualCol->addLayout(dualTop);
		dualNote = say(QString(), svl::INK2);
		dualCol->addWidget(dualNote);
		v->addWidget(dualBox);
		for (const SvDest &d : sv_output_dests())
			if (d.dual && d.id != dest.id)
				dualTaken = true;
		if (dualTaken) {
			dual->setChecked(false);
			dual->setEnabled(false);
		}
		connect(dual, &QAbstractButton::toggled, this, [this](bool) {
			show_dual();
			fit();
		});

		/* Which vertical canvas this platform shows. A new one is only made on Save. */
		if (sv_canvas_running()) {
			canvases = sv_canvases();
			v->addSpacing(2);
			v->addLayout(field_row(T("DestCanvas"), T("DestCanvasTip")));
			auto *canvasRow = new QHBoxLayout();
			canvasRow->setSpacing(8);
			canvasTabs = new svl::TabBar();
			int canvasAt = 0;
			for (size_t i = 0; i < canvases.size(); i++) {
				const int at = canvasTabs->addTab(
					canvases[i].main ? QString(T("DestCanvasMain")) : qs(canvases[i].name), QString());
				canvasTabs->setTabHue(at, canvas_hue(canvases[i], (int)i), true);
				if (!canvases[i].main && canvases[i].uuid == dest.canvas)
					canvasAt = (int)i;
			}
			/* OBS's own wide picture, the main scene as OBS shows it, for a platform that
			   wants the usual stream instead of a vertical one. */
			wideAt = canvasTabs->addTab(T("DestCanvasWide"), QString());
			canvasTabs->setTabHue(wideAt, svl::YELLOW, true);
			if (dest.canvas == SV_WIDE)
				canvasAt = wideAt;
			canvasTabs->setCurrent(canvasAt);
			canvasTabs->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
			canvasRow->addWidget(canvasTabs);
			newCanvas = key(T("DestCanvasNew"), "plus");
			newCanvas->setVisible((int)canvases.size() < SV_CANVAS_MAX);
			canvasRow->addWidget(newCanvas);
			canvasRow->addStretch(1);
			v->addLayout(canvasRow);
			canvasNote = say(QString(), svl::INK2);
			v->addWidget(canvasNote);
			canvasTabs->onPick = [this](int i) {
				canvasTabs->setCurrent(i);
				show_canvas_note();
				show_size_names();
				show_own_note();
				/* The note comes and goes, so the box grows or shrinks with it. */
				fit();
			};
			connect(newCanvas, &QPushButton::clicked, this, [this] {
				/* Named after the platform, made as a copy of the main canvas when saved. */
				pendingCanvas = sv_canvas_free_canvas_name(name->text().trimmed().isEmpty()
										   ? std::string(T(current().label))
										   : name->text().trimmed().toStdString());
				pendingAt = canvasTabs->addTab(qs(pendingCanvas), QString());
				SvVCanvas toBe;
				toBe.name = pendingCanvas;
				canvasTabs->setTabHue(pendingAt, canvas_hue(toBe, (int)canvases.size()), true);
				canvasTabs->setCurrent(pendingAt);
				newCanvas->hide();
				show_canvas_note();
				fit();
			});
			show_canvas_note();
		}

		/* Its own video: a bitrate, size and frame rate just for this platform. */
		usual = sv_output_settings();
		v->addSpacing(2);
		auto *ownRow = new QHBoxLayout();
		ownRow->setSpacing(6);
		own = new svl::Switch(T("DestOwnVideo"));
		own->setChecked(dest.own_video);
		ownRow->addWidget(own);
		ownInfo = new svl::Info(T("DestOwnVideoTip"));
		ownRow->addWidget(ownInfo);
		ownRow->addStretch(1);
		v->addLayout(ownRow);
		ownBox = new QWidget();
		auto *ob = new QGridLayout(ownBox);
		ob->setContentsMargins(0, 0, 0, 0);
		ob->setHorizontalSpacing(14);
		ob->setVerticalSpacing(6);
		ownBitrate = new svl::NumberBox(1000, 20000, 500, dest.own_video ? dest.bitrate : usual.bitrate,
						T("UnitKbps"));
		ob->addLayout(field_row(T("SettingsBitrate")), 0, 0);
		ob->addWidget(ownBitrate, 1, 0, Qt::AlignLeft);
		ownSize = new svl::TabBar();
		ownSize->addTab(T("SettingsSize1080"), QString());
		ownSize->addTab(T("SettingsSize900"), QString());
		ownSize->addTab(T("SettingsSize720"), QString());
		ownSize->setCurrent(std::clamp(dest.own_video ? dest.size : usual.size, 0, 2));
		ownSize->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		show_size_names();
		ob->addLayout(field_row(T("SettingsSize")), 2, 0);
		ob->addWidget(ownSize, 3, 0, Qt::AlignLeft);
		/* The usual rate first, then OBS's own rate and its even shares, like Settings offers.
		   The usual rate is not listed twice. */
		const int whole = sv_output_obs_fps();
		usualFps = usual.fps > 0 && usual.fps < whole ? usual.fps : whole;
		fpsChoices.push_back(0);
		for (int d = 1; d <= 12; d++)
			if (whole % d == 0 && whole / d >= 10 && whole / d != usualFps)
				fpsChoices.push_back(whole / d);
		if (dest.own_video && dest.fps > 0 && dest.fps != usualFps &&
		    std::find(fpsChoices.begin(), fpsChoices.end(), dest.fps) == fpsChoices.end())
			fpsChoices.push_back(dest.fps);
		ownFps = new svl::TabBar();
		int fpsAt = 0;
		for (size_t i = 0; i < fpsChoices.size(); i++) {
			ownFps->addTab(fpsChoices[i] == 0 ? QString(T("DestFpsUsual")).arg(usualFps)
							  : QString::number(fpsChoices[i]),
				       QString());
			if (dest.own_video && fpsChoices[i] == dest.fps)
				fpsAt = (int)i;
		}
		ownFps->setCurrent(fpsAt);
		ownFps->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		show_size_names();
		ob->addLayout(field_row(T("SettingsFps")), 4, 0);
		ob->addWidget(ownFps, 5, 0, Qt::AlignLeft);
		ownNote = say(QString(), svl::INK2);
		ob->addWidget(ownNote, 6, 0);
		v->addWidget(ownBox);
		if (sv_output_info(dest.id).state != SvRunState::Off && !dest.id.empty()) {
			auto *later = say(T("DestOwnVideoLater"), svl::AMBER);
			v->addWidget(later);
		}
		ownBox->setVisible(dest.own_video);
		connect(own, &QAbstractButton::toggled, this, [this](bool on) {
			/* Turned on for the first time: start from numbers that suit this platform. */
			if (on && !dest.own_video && !touchedOwn)
				suggest_own();
			ownBox->setVisible(on);
			show_own_note();
			fit();
		});
		ownBitrate->onChange = [this](int) {
			touchedOwn = true;
			show_own_note();
		};
		ownSize->onPick = [this](int) {
			touchedOwn = true;
			show_own_note();
		};
		ownFps->onPick = [this](int i) {
			touchedOwn = true;
			ownFps->setCurrent(i);
			show_own_note();
		};
		show_own_note();

		problem = say(QString(), PROBLEM);
		problem->hide();
		v->addWidget(problem);
		/* Any spare height goes here, above the buttons, so no heading drifts away from its box. */
		v->addStretch(1);

		QHBoxLayout *buttons = button_row();
		auto *remove = key(T("DestRemove"), "trash", svl::RED);
		remove->setVisible(!dest.id.empty());
		buttons->addWidget(remove);
		buttons->addStretch(1);
		auto *cancel = key(T("DestCancel"), "x");
		auto *save = key(T("DestSave"), "check");
		save->setDefault(true);
		buttons->addWidget(cancel);
		buttons->addWidget(save);

		connect(show, &QPushButton::toggled, this, [this, show](bool on) {
			keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
			show->setText(on ? T("DestHideKey") : T("DestShowKey"));
		});
		connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
		connect(save, &QPushButton::clicked, this, [this] { save_clicked(); });
		connect(remove, &QPushButton::clicked, this, [this] { remove_clicked(); });

		int at = 0;
		for (int i = 0; i < PLATFORM_COUNT; i++) {
			if (!dest.platform.empty() && dest.platform == PLATFORMS[i].id)
				at = i;
		}
		if (!dest.id.empty()) {
			/* Opening a saved platform shows exactly what is saved, never the
			   platform's default address. */
			name->setText(qs(dest.name));
			server->setText(qs(dest.server));
			choose(at, false);
		} else {
			choose(at, true);
		}
	}

private:
	const SvPlatform &current() const { return PLATFORMS[chosen]; }

	/* "Get my key": asks the website on a thread of its own, so OBS never stalls, and puts
	   the key in the box like a pasted one. Nothing is saved until Save. */
	void get_key()
	{
		if (fetching)
			return;
		fetching = true;
		const std::string platform = current().id;
		getKey->setEnabled(false);
		keyNote->setText(T(platform == "twitch" ? "KeyAsking" : "KeyAskingKick"), svl::INK2);
		keyNote->show();
		fit();
		QPointer<SvDestDialog> self(this);
		std::thread([self, platform] {
			const SvStreamKey got = sv_link_stream_key(platform);
			QMetaObject::invokeMethod(
				qApp,
				[self, got, platform] {
					if (!self)
						return;
					self->fetching = false;
					self->getKey->setEnabled(true);
					/* The member picked another platform while it was asking. */
					if (platform != self->current().id) {
						self->keyNote->hide();
						self->fit();
						return;
					}
					if (got.ok) {
						self->keyEdit->setText(qs(got.key));
						if (!got.server.empty())
							self->server->setText(qs(got.server));
						self->keyNote->setText(T("KeyGot"), svl::tone(svl::GREEN, 0.3));
					} else {
						self->keyNote->setText(T(got.problem.c_str()), svl::AMBER);
					}
					self->keyNote->show();
					self->fit();
				},
				Qt::QueuedConnection);
		}).detach();
	}

	void choose(int i, bool fill = true)
	{
		chosen = std::clamp(i, 0, PLATFORM_COUNT - 1);
		for (int t = 0; t < (int)tiles.size(); t++)
			tiles[(size_t)t]->setOn(t == chosen);
		const SvPlatform &p = current();
		help->setText(T(p.help), svl::INK2);
		helpMore->setVisible(p.more != nullptr);
		helpMore->setTip(p.more ? T(p.more) : QString());
		/* Twitch and Kick hand over the key to an app the member approved; the others do not. */
		const bool twitch = strcmp(p.id, "twitch") == 0;
		const bool fetches = twitch || strcmp(p.id, "kick") == 0;
		getKey->setVisible(fetches && !fetching);
		keyPage->setVisible(key_page_url(p.id) != nullptr);
		keyPage->setToolTip(QString(T("DestKeyPageTip")).arg(T(p.label)));
		getKeyBox->setVisible(getKey->isVisibleTo(getKeyBox) || keyPage->isVisibleTo(getKeyBox));
		getKey->setToolTip(T(twitch ? "DestGetKeyTip" : "DestGetKeyTipKick"));
		keyNote->hide();
		/* Picking another platform while its own video is on and untouched moves to that platform's numbers. */
		if (own && own->isChecked() && !dest.own_video && !touchedOwn)
			suggest_own();
		/* Only replace what we filled in ourselves, never what the member typed or saved. */
		if (fill) {
			if (server->text().isEmpty() || (!lastFilled.isNull() && server->text() == lastFilled))
				server->setText(p.server);
			if (name->text().isEmpty() || (!lastName.isNull() && name->text() == lastName))
				name->setText(T(p.label));
		}
		lastFilled = p.server;
		lastName = T(p.label);
		if (own)
			show_dual();
		/* Help lines differ in length, so the box shrinks or grows to the new one instead of
		   keeping a gap above Remove, Cancel and Save. */
		if (isVisible())
			fit();
	}

	/* Numbers that suit each platform, from its own published guidance. Other keeps the usual ones. */
	void suggest_own()
	{
		const std::string id = current().id;
		int kbps = usual.bitrate, size = usual.size;
		if (id == "youtube")
			kbps = 9000;
		else if (id == "twitch" || id == "facebook")
			kbps = 6000;
		else if (id == "kick")
			kbps = 8000;
		else if (id == "tiktok")
			kbps = 4000;
		else if (id == "instagram") {
			kbps = 4000;
			size = SV_SIZE_720;
		}
		ownBitrate->setValue(kbps);
		ownSize->setCurrent(size);
		/* Instagram shows 30 frames a second, so sending more only costs internet. */
		int fpsAt = 0;
		if (id == "instagram")
			for (size_t i = 1; i < fpsChoices.size(); i++)
				if (fpsChoices[i] == 30 && usualFps > 30)
					fpsAt = (int)i;
		ownFps->setCurrent(fpsAt);
		touchedOwn = false;
		show_own_note();
	}

	/* Says whether this platform shares the usual video or has one made just for it. */
	void show_own_note()
	{
		if (!ownNote)
			return;
		if (wide_picked()) {
			/* The wide picture is always a video of its own, so there is nothing to share. */
			ownNote->setText(T("DestOwnWide"), svl::tone(svl::YELLOW, 0.35));
			return;
		}
		const int f = fpsChoices[(size_t)std::clamp(ownFps->current(), 0, (int)fpsChoices.size() - 1)];
		const bool same = ownBitrate->value() == usual.bitrate && ownSize->current() == usual.size &&
				  sv_output_fps_div(f > 0 ? f : usual.fps) == usual.fps_div;
		ownNote->setText(same ? T("DestOwnSame") : T("DestOwnExtra"), same ? svl::INK2 : svl::tone(svl::BLUE, 0.35));
	}

	/* Says what picking a canvas other than the main one means. */
	void show_canvas_note()
	{
		if (!canvasTabs || !canvasNote)
			return;
		const int at = canvasTabs->current();
		if (at == wideAt) {
			canvasNote->setVisible(true);
			canvasNote->setText(T("DestCanvasWideNote"), svl::tone(svl::YELLOW, 0.35));
			return;
		}
		const bool mainOne = at != pendingAt && (at < 0 || at >= (int)canvases.size() || canvases[(size_t)at].main);
		canvasNote->setVisible(!mainOne);
		canvasNote->setText(at == pendingAt ? T("DestCanvasNewNote") : T("DestCanvasOwnNote"),
				    svl::tone(svl::BLUE, 0.35));
	}

	bool wide_picked() const { return canvasTabs && wideAt >= 0 && canvasTabs->current() == wideAt; }

	bool dual_on() const
	{
		return dual && dual->isChecked() && std::strcmp(current().id, "twitch") == 0 && sv_output_dual_possible();
	}

	/* With Twitch Dual Format, Twitch picks the quality and the wide picture is always there,
	   so the platform's own video and the OBS wide tab step aside. */
	void show_dual()
	{
		if (!dualBox || !own)
			return;
		dualBox->setVisible(std::strcmp(current().id, "twitch") == 0 && sv_output_dual_possible());
		const bool on = dual_on();
		if (dualTaken) {
			dualNote->setText(T("DestDualTaken"), svl::AMBER);
			dualNote->setVisible(true);
		} else {
			dualNote->setText(T("DestDualNote"), svl::tone(svl::platform_colour("twitch"), 0.3));
			dualNote->setVisible(on);
		}
		own->setVisible(!on);
		ownInfo->setVisible(!on);
		ownBox->setVisible(!on && own->isChecked());
		if (canvasTabs && wideAt >= 0) {
			canvasTabs->setTabVisible(wideAt, !on);
			if (on && canvasTabs->current() == wideAt) {
				canvasTabs->setCurrent(0);
				show_canvas_note();
				show_size_names();
			}
		}
		show_own_note();
	}

	/* The canvas's name as OBS's Stream settings list it. */
	QString canvas_obs_name(int at) const
	{
		if (at == pendingAt)
			return qs(pendingCanvas);
		if (at >= 0 && at < (int)canvases.size())
			return qs(canvases[(size_t)at].name);
		return qs(SV_CANVAS_NAME);
	}

	/* The picture sizes read the wide way round for OBS's wide picture. */
	void show_size_names()
	{
		if (!ownSize)
			return;
		const bool w = wide_picked();
		ownSize->setTabText(0, T(w ? "SettingsWide1080" : "SettingsSize1080"));
		ownSize->setTabText(1, T(w ? "SettingsWide900" : "SettingsSize900"));
		ownSize->setTabText(2, T(w ? "SettingsWide720" : "SettingsSize720"));
		/* "Usual" on the wide picture is OBS's own frame rate. */
		if (ownFps)
			ownFps->setTabText(0, QString(T("DestFpsUsual")).arg(w ? sv_output_obs_fps() : usualFps));
	}

	void fail(const char *text)
	{
		problem->setText(T(text), PROBLEM);
		problem->show();
		fit();
	}

	/* Wrapped help lines make Qt guess the box taller than it is, so measure at the real width.
	   It never gets shorter than what it holds, and shrinks to fit when asked. */
	void fit(bool shrink = true)
	{
		v->activate();
		const int w = width();
		const int h = v->hasHeightForWidth() ? v->totalHeightForWidth(w) : sizeHint().height();
		if (h <= 0)
			return;
		setMinimumHeight(h);
		if (shrink || height() < h)
			resize(w, h);
	}

	void showEvent(QShowEvent *e) override
	{
		SvWindow::showEvent(e);
		QTimer::singleShot(0, this, [this] { fit(); });
	}

	/* A narrower box wraps more lines, so its smallest height is worked out again. */
	void resizeEvent(QResizeEvent *e) override
	{
		SvWindow::resizeEvent(e);
		if (e->size().width() != e->oldSize().width())
			QTimer::singleShot(0, this, [this] { fit(false); });
	}

	void save_clicked()
	{
		const QString addr = server->text().trimmed();
		const QString k = keyEdit->text().trimmed();
		const bool wellFormed = addr.startsWith("rtmp://", Qt::CaseInsensitive) ||
					addr.startsWith("rtmps://", Qt::CaseInsensitive);
		/* Two addresses run together, from pasting into a filled box. */
		if (!wellFormed || addr.indexOf("://") != addr.lastIndexOf("://")) {
			fail("DestNeedServer");
			return;
		}
		if (k.isEmpty() && !dest.has_key) {
			fail("DestNeedKey");
			return;
		}
		/* The server address pasted into the key box, or two things run together. */
		if (k.contains(QRegularExpression("\\s")) || k.startsWith("rtmp", Qt::CaseInsensitive)) {
			fail("DestKeyLooksWrong");
			return;
		}
		SvDest out = dest;
		out.platform = current().id;
		out.name = name->text().trimmed().toStdString();
		if (out.name.empty())
			out.name = T(current().label);
		out.server = addr.toStdString();
		out.key = k.toStdString();
		out.dual = dual_on();
		out.own_video = own->isChecked() && !out.dual;

		/* Twitch Dual Format changes OBS's own Stream settings, so the member sees exactly
		   what changes first. Asked before a new canvas is made, so Cancel leaves nothing behind. */
		bool setUpDual = false;
		if (out.dual) {
			const int at = canvasTabs ? canvasTabs->current() : 0;
			std::string wantCanvas = dest.canvas;
			if (canvasTabs && at >= 0 && at < (int)canvases.size())
				wantCanvas = canvases[(size_t)at].main ? std::string() : canvases[(size_t)at].uuid;
			const bool same = dest.dual && k.isEmpty() && at != pendingAt && wantCanvas == dest.canvas &&
					  sv_output_dual_ready(dest.id);
			if (!same) {
				if (obs_frontend_streaming_active()) {
					fail("DualObsLive");
					return;
				}
				SvDualDialog ask(this, canvas_obs_name(at));
				if (ask.exec() != QDialog::Accepted)
					return;
				setUpDual = true;
			}
		}
		if (out.own_video) {
			out.bitrate = ownBitrate->value();
			out.size = std::clamp(ownSize->current(), 0, 2);
			out.fps = fpsChoices[(size_t)std::clamp(ownFps->current(), 0, (int)fpsChoices.size() - 1)];
		}
		if (canvasTabs) {
			const int at = canvasTabs->current();
			if (at == wideAt) {
				out.canvas = SV_WIDE;
			} else if (at == pendingAt) {
				out.canvas = sv_canvas_add(pendingCanvas);
				if (out.canvas.empty()) {
					fail("DestCanvasFailed");
					return;
				}
			} else if (at >= 0 && at < (int)canvases.size()) {
				out.canvas = canvases[(size_t)at].main ? std::string() : canvases[(size_t)at].uuid;
			}
		}
		if (out.dual && out.canvas == SV_WIDE)
			out.canvas.clear();
		const std::string savedId = sv_output_save_dest(out);
		keyEdit->clear();
		if (setUpDual) {
			bool restart = false;
			std::string why;
			if (sv_output_setup_dual(savedId, restart, why))
				sv_output_set_notice(T(restart ? "DualRestart" : "DualDone"), false);
			else
				sv_output_set_notice(why, true);
		}
		accept();
	}

	void remove_clicked()
	{
		if (!sv_output_remove_dest(dest.id)) {
			fail("DestStopFirst");
			return;
		}
		accept();
	}

	SvDest dest;
	std::vector<svl::Tile *> tiles;
	std::vector<SvVCanvas> canvases;
	svl::TabBar *canvasTabs = nullptr;
	QPushButton *newCanvas = nullptr;
	svl::Words *canvasNote = nullptr;
	std::string pendingCanvas;
	int pendingAt = -1;
	int wideAt = -1;
	int chosen = 0;
	svl::Words *help = nullptr;
	svl::Info *helpMore = nullptr;
	QLineEdit *name = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *keyEdit = nullptr;
	svl::Key *getKey = nullptr;
	svl::Key *keyPage = nullptr;
	QWidget *getKeyBox = nullptr;
	svl::Words *keyNote = nullptr;
	bool fetching = false;
	svl::Words *problem = nullptr;
	QString lastFilled;
	QString lastName;
	SvOutSettings usual;
	int usualFps = 30;
	std::vector<int> fpsChoices; /* 0 first: the usual rate */
	svl::Switch *own = nullptr;
	svl::Info *ownInfo = nullptr;
	QWidget *dualBox = nullptr;
	svl::Switch *dual = nullptr;
	svl::Words *dualNote = nullptr;
	bool dualTaken = false;
	QWidget *ownBox = nullptr;
	svl::NumberBox *ownBitrate = nullptr;
	svl::TabBar *ownSize = nullptr;
	svl::TabBar *ownFps = nullptr;
	svl::Words *ownNote = nullptr;
	bool touchedOwn = false;
};

/* ---------- settings ---------- */

/* Under a quality level box: where the number sits between sharp and small, with the usual
   stops marked, so the number means something. */
class SvLevelScale : public QWidget {
public:
	explicit SvLevelScale(const QColor &hue) : hue(hue) { setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); }
	void setLevel(int v)
	{
		level = v;
		update();
	}
	QSize sizeHint() const override { return QSize(540, 46); }
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const qreal left = 16, right = width() - 16, y = 13;
		const int lo = 14, hi = 30;
		auto xOf = [&](int v) { return left + (right - left) * (std::clamp(v, lo, hi) - lo) / (qreal)(hi - lo); };
		/* Bright on the sharp side, dimmer towards small files. */
		QLinearGradient g(left, 0, right, 0);
		g.setColorAt(0, svl::alpha(hue, 0.95));
		g.setColorAt(1, svl::alpha(hue, 0.3));
		p.setPen(QPen(QBrush(g), 3, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(left, y), QPointF(right, y));
		struct Stop {
			int v;
			const char *words;
		};
		const Stop stops[] = {{16, "SettingsLevel16"}, {20, "SettingsLevel20"}, {23, "SettingsLevel23"},
				      {28, "SettingsLevel28"}};
		p.setFont(svl::body(13));
		for (const Stop &st : stops) {
			const qreal x = xOf(st.v);
			const bool here = st.v == level;
			p.setPen(QPen(svl::alpha(hue, here ? 1.0 : 0.65), 1.5));
			p.drawLine(QPointF(x, y - 5), QPointF(x, y + 5));
			p.setPen(here ? svl::mix(hue, svl::INK, 0.55) : svl::tone(hue, 0.3));
			p.drawText(QRectF(x - 90, y + 9, 180, 22), Qt::AlignHCenter | Qt::AlignTop,
				   QString("%1 %2").arg(st.v).arg(QString(T(st.words))));
		}
		/* The number typed above: a lit pointer on the line. */
		const qreal x = xOf(level);
		QPainterPath tri;
		tri.moveTo(x, y + 3);
		tri.lineTo(x - 6.5, y - 9);
		tri.lineTo(x + 6.5, y - 9);
		tri.closeSubpath();
		svl::shape_shadow(p, tri, QPointF(0, 0), 6, svl::alpha(hue, 0.85));
		p.fillPath(tri, svl::mix(hue, svl::INK, 0.45));
	}

private:
	QColor hue;
	int level = 20;
};

/* The sound routing table on the Recording tab, laid out like a mixer's routing matrix: a row
   per sound with a level bar, a column per OBS track, lines running across and down like cables,
   a lit diamond where a sound is sent to a track, and along the bottom a key per track for whether
   it goes in the file. Nothing here changes OBS until the settings window saves. */
class SvRouteTable : public QWidget {
public:
	explicit SvRouteTable(const QColor &hue) : hue(hue)
	{
		setMouseTracking(true);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		auto *timer = new QTimer(this);
		timer->setInterval(50);
		connect(timer, &QTimer::timeout, this, [this] { tick_meters(); });
		timer->start();
	}
	~SvRouteTable() override
	{
		for (auto &r : rows) {
			if (!r->meter)
				continue;
			obs_volmeter_remove_callback(r->meter, on_meter, r.get());
			obs_volmeter_detach_source(r->meter);
			obs_volmeter_destroy(r->meter);
		}
	}
	void addRow(const std::string &uuid, const QString &name)
	{
		auto r = std::make_unique<Row>();
		r->uuid = uuid;
		r->name = name;
		/* Its own meter, so the bar moves whichever way the vertical hears its sound. */
		if (obs_source_t *src = obs_get_source_by_uuid(uuid.c_str())) {
			r->meter = obs_volmeter_create(OBS_FADER_LOG);
			if (r->meter) {
				obs_volmeter_attach_source(r->meter, src);
				obs_volmeter_add_callback(r->meter, on_meter, r.get());
			}
			obs_source_release(src);
		}
		rows.push_back(std::move(r));
		setFixedSize(sizeHint());
	}
	int rowCount() const { return (int)rows.size(); }
	const std::string &uuidAt(int i) const { return rows[(size_t)i]->uuid; }
	uint32_t mask(int i) const { return rows[(size_t)i]->mask; }
	void setMask(int i, uint32_t m)
	{
		rows[(size_t)i]->mask = m & 63u;
		update();
	}
	uint32_t inFile() const { return file; }
	void setInFile(uint32_t m)
	{
		file = m & 63u;
		own = file;
		update();
	}
	void setHead(int t, const QString &text)
	{
		heads[t - 1] = text;
		update();
	}
	/* The vertical's own track is always in the file. */
	void setMain(int t)
	{
		const int old = mainCol;
		/* The vertical's own track was only on because it had to be: when it moves,
		   that track goes back to how you set it, which is off unless you turned it on. */
		if (!mainSet && t >= 1 && t <= 6)
			own &= ~(1u << (t - 1));
		else if (old != t && old >= 1 && old <= 6)
			file = (file & ~(1u << (old - 1))) | (own & (1u << (old - 1)));
		mainSet = true;
		mainCol = t;
		if (t >= 1 && t <= 6)
			file |= 1u << (t - 1);
		update();
	}
	/* The own mix's track (0 when the mix is off) belongs to the mix alone. */
	void setDesk(int t)
	{
		deskCol = t;
		update();
	}
	void setLocked(bool on)
	{
		locked = on;
		update();
	}
	QSize sizeHint() const override
	{
		return QSize(NAME_W + 6 * COL_W + 8, HEAD_H + ROW_H * (std::max)((int)rows.size(), 1) + FOOT_H);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *e) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const int n = (int)rows.size();
		const qreal bodyTop = HEAD_H, bodyBottom = HEAD_H + ROW_H * (std::max)(n, 1);
		const qreal fade = locked ? 0.45 : 1.0;
		const uint32_t deskBit = deskCol ? 1u << (deskCol - 1) : 0u;

		p.setPen(QPen(svl::alpha(hue, 0.2), 1));
		p.setBrush(QColor(0, 0, 0, 110));
		p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

		/* Level bars: the timer repaints only these, so the rest is skipped then. */
		for (int i = 0; i < n; i++) {
			const QRectF bar(13, row_y(i) - 10, 6, 20);
			p.setPen(Qt::NoPen);
			p.setBrush(svl::alpha(hue, 0.26));
			p.drawRoundedRect(bar, 1.5, 1.5);
			const qreal h = bar.height() * rows[(size_t)i]->shown;
			if (h > 0.5) {
				p.setBrush(hue);
				p.drawRoundedRect(QRectF(bar.left(), bar.bottom() - h, bar.width(), h), 1.5, 1.5);
			}
		}
		if (!e->rect().intersects(QRect(26, 0, width(), height())))
			return;

		/* Track numbers and names. */
		for (int t = 1; t <= 6; t++) {
			const QColor c = col_hue(t);
			const qreal x0 = col_x(t) - COL_W / 2.0;
			const bool hot = hotCol == t;
			p.setFont(svl::head(17));
			svl::glow_text(p, QRectF(x0, 8, COL_W, 26), Qt::AlignCenter, QString::number(t),
				       svl::mix(c, svl::INK, hot ? 0.7 : 0.88), c, hot ? 0.9 : 0.55);
			QString name = heads[t - 1];
			QColor ink = svl::tone(hue, 0.3);
			if (t == deskCol)
				ink = svl::mix(svl::LIME, svl::INK, 0.8);
			else if (t == mainCol)
				ink = svl::mix(hue, svl::INK, 0.5);
			if (name.isEmpty())
				name = QString(T("SoundTrackN")).arg(t);
			p.setFont(svl::body(11.5));
			const QString shown = QFontMetricsF(p.font()).elidedText(name, Qt::ElideRight, COL_W - 6);
			svl::glow_text(p, QRectF(x0, 36, COL_W, 20), Qt::AlignCenter, shown, ink, c,
				       t == deskCol ? 0.6 : 0.0);
		}

		/* The buses running down and the sounds running across. */
		for (int t = 1; t <= 6; t++) {
			p.setPen(QPen(svl::alpha(col_hue(t), (hotCol == t ? 0.5 : 0.18) * fade), 1));
			p.drawLine(QPointF(col_x(t), bodyTop + 2), QPointF(col_x(t), bodyBottom));
		}
		const qreal lineFrom = NAME_W - 8, lineTo = col_x(6) + COL_W / 2.0 - 6;
		for (int i = 0; i < n; i++) {
			const qreal y = row_y(i);
			p.setPen(QPen(svl::alpha(hue, (hotRow == i ? 0.42 : 0.13) * fade), 1));
			p.drawLine(QPointF(lineFrom, y), QPointF(lineTo, y));
			/* The signal lit from the sound to the last track it is sent to. */
			const uint32_t m = rows[(size_t)i]->mask & ~deskBit;
			int last = 0;
			for (int t = 1; t <= 6; t++)
				if (m & (1u << (t - 1)))
					last = t;
			if (last) {
				p.setPen(QPen(svl::alpha(hue, 0.2 * fade), 6, Qt::SolidLine, Qt::RoundCap));
				p.drawLine(QPointF(lineFrom, y), QPointF(col_x(last), y));
				p.setPen(QPen(svl::alpha(hue, 0.95 * fade), 2, Qt::SolidLine,
					      Qt::RoundCap));
				p.drawLine(QPointF(lineFrom, y), QPointF(col_x(last), y));
			}
		}

		/* Sound names. */
		p.setFont(svl::body(13));
		const QFontMetricsF fm(p.font());
		for (int i = 0; i < n; i++) {
			const QString nm = fm.elidedText(rows[(size_t)i]->name, Qt::ElideRight, NAME_W - 42);
			p.setPen(hotRow == i ? svl::INK : svl::INK2);
			p.drawText(QRectF(28, row_y(i) - ROW_H / 2.0, NAME_W - 34, ROW_H), Qt::AlignLeft | Qt::AlignVCenter,
				   nm);
		}
		if (n == 0) {
			p.setPen(svl::tone(hue, 0.3));
			p.drawText(QRectF(28, bodyTop, width() - 40, ROW_H), Qt::AlignLeft | Qt::AlignVCenter,
				   QString(T("SoundNoSources")));
		}

		/* The crossings. */
		for (int i = 0; i < n; i++) {
			for (int t = 1; t <= 6; t++) {
				const QPointF c(col_x(t), row_y(i));
				QPainterPath d;
				d.moveTo(c.x(), c.y() - 8);
				d.lineTo(c.x() + 8, c.y());
				d.lineTo(c.x(), c.y() + 8);
				d.lineTo(c.x() - 8, c.y());
				d.closeSubpath();
				if (t == deskCol) {
					p.setPen(QPen(svl::alpha(svl::LIME, 0.35), 1));
					p.setBrush(QColor(0, 0, 0, 170));
					p.drawPath(d);
					continue;
				}
				const bool on = rows[(size_t)i]->mask & (1u << (t - 1));
				const bool hot = hotRow == i && hotCol == t && !locked;
				if (on) {
					svl::shape_shadow(p, d, QPointF(0, 0), 9, svl::alpha(hue, 0.95 * fade));
					p.setPen(QPen(svl::alpha(svl::mix(hue, svl::INK, 0.6), fade), 1));
					p.setBrush(svl::alpha(svl::mix(hue, svl::INK, hot ? 0.7 : 1.0), fade));
				} else {
					p.setPen(QPen(svl::alpha(hue, (hot ? 0.8 : 0.36) * fade), 1.2));
					p.setBrush(hot ? svl::alpha(hue, 0.18) : QColor(0, 0, 0, 170));
				}
				p.drawPath(d);
			}
		}

		/* In the file. */
		p.setPen(QPen(svl::alpha(hue, 0.3), 1));
		p.drawLine(QPointF(12, bodyBottom + 4), QPointF(width() - 12, bodyBottom + 4));
		const qreal fy = bodyBottom + 4 + (FOOT_H - 4) / 2.0;
		p.setFont(svl::body(13, QFont::Medium));
		p.setPen(svl::mix(hue, svl::INK, 0.45));
		p.drawText(QRectF(14, fy - 14, NAME_W - 20, 28), Qt::AlignLeft | Qt::AlignVCenter, QString(T("SettingsTrackInFile")));
		for (int t = 1; t <= 6; t++) {
			const QRectF k(col_x(t) - 18, fy - 12, 36, 24);
			QPainterPath kp;
			kp.addRoundedRect(k, 6, 6);
			const bool on = file & (1u << (t - 1));
			const bool hot = hotFoot && hotCol == t && !locked && t != mainCol;
			const QColor c = col_hue(t);
			if (on) {
				svl::shape_shadow(p, kp, QPointF(0, 0), 8, svl::alpha(c, 0.75 * fade));
				p.setPen(QPen(svl::alpha(svl::mix(c, svl::INK, 0.6), fade), 1));
				p.setBrush(svl::alpha(svl::mix(c, svl::INK, hot ? 0.7 : 1.0), fade));
				p.drawPath(kp);
				const QPixmap pm = svl::icon(t == mainCol ? "lock" : "check", QColor(28, 14, 4), 16,
							     devicePixelRatioF());
				p.drawPixmap(QPointF(k.center().x() - 8, k.center().y() - 8), pm);
			} else {
				p.setPen(QPen(svl::alpha(c, (hot ? 0.8 : 0.36) * fade), 1.2));
				p.setBrush(hot ? svl::alpha(c, 0.18) : QColor(0, 0, 0, 160));
				p.drawPath(kp);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		const Hit h = hit(e->position());
		if (h.row != hotRow || h.col != hotCol || h.foot != hotFoot) {
			hotRow = h.row;
			hotCol = h.col;
			hotFoot = h.foot;
			update();
		}
		setCursor(clickable(h) ? Qt::PointingHandCursor : Qt::ArrowCursor);
	}
	void leaveEvent(QEvent *) override
	{
		hotRow = -1;
		hotCol = 0;
		hotFoot = false;
		update();
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		const Hit h = hit(e->position());
		if (!clickable(h))
			return;
		const uint32_t bit = 1u << (h.col - 1);
		if (h.foot) {
			file ^= bit;
			own = (own & ~bit) | (file & bit);
		} else {
			rows[(size_t)h.row]->mask ^= bit;
		}
		update();
	}
	bool event(QEvent *e) override
	{
		if (e->type() == QEvent::ToolTip) {
			auto *he = static_cast<QHelpEvent *>(e);
			const Hit h = hit(he->pos());
			QString tip;
			if (h.col && (h.row >= 0 || h.foot) && locked)
				tip = T("SettingsLockedWhy");
			else if (h.col && h.col == deskCol && h.row >= 0)
				tip = T("SettingsTrackDeskTip");
			else if (h.foot && h.col && h.col == mainCol)
				tip = T("SettingsTrackMainTip");
			else if (h.row >= 0 && !h.col)
				tip = rows[(size_t)h.row]->name;
			if (tip.isEmpty())
				QToolTip::hideText();
			else
				QToolTip::showText(he->globalPos(), tip, this);
			return true;
		}
		return QWidget::event(e);
	}

private:
	static constexpr int NAME_W = 214, COL_W = 92, HEAD_H = 62, ROW_H = 34, FOOT_H = 50;
	struct Row {
		std::string uuid;
		QString name;
		uint32_t mask = 0;
		obs_volmeter_t *meter = nullptr;
		std::atomic<float> peak{-100.0f};
		float shown = 0.0f;
	};
	struct Hit {
		int row = -1;
		int col = 0;
		bool foot = false;
	};

	static void on_meter(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			     const float[MAX_AUDIO_CHANNELS])
	{
		auto *r = static_cast<Row *>(param);
		float top = -100.0f;
		for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
			if (std::isfinite(peak[i]))
				top = (std::max)(top, peak[i]);
		r->peak.store(top);
	}
	void tick_meters()
	{
		if (!isVisible())
			return;
		for (int i = 0; i < (int)rows.size(); i++) {
			Row &r = *rows[(size_t)i];
			const float db = r.peak.load();
			const float level = db <= -60.0f ? 0.0f : (std::min)(1.0f, (db + 60.0f) / 60.0f);
			const float next = level >= r.shown ? level : (std::max)(level, r.shown - 0.05f);
			if (std::fabs(next - r.shown) > 0.004f) {
				r.shown = next;
				update(QRect(11, (int)row_y(i) - 12, 10, 24));
			}
		}
	}
	qreal col_x(int t) const { return NAME_W + (t - 1) * COL_W + COL_W / 2.0; }
	qreal row_y(int i) const { return HEAD_H + i * ROW_H + ROW_H / 2.0; }
	QColor col_hue(int t) const { return t == deskCol ? svl::LIME : hue; }
	Hit hit(QPointF pos) const
	{
		Hit h;
		const qreal x = pos.x(), y = pos.y();
		const int n = (int)rows.size();
		if (x >= NAME_W && x < NAME_W + 6 * COL_W)
			h.col = (int)((x - NAME_W) / COL_W) + 1;
		const qreal bodyBottom = HEAD_H + ROW_H * (std::max)(n, 1);
		if (y >= HEAD_H && y < HEAD_H + ROW_H * n)
			h.row = (int)((y - HEAD_H) / ROW_H);
		else if (y >= bodyBottom + 4 && y < bodyBottom + FOOT_H)
			h.foot = true;
		return h;
	}
	bool clickable(const Hit &h) const
	{
		if (locked || !h.col)
			return false;
		if (h.foot)
			return h.col != mainCol;
		return h.row >= 0 && h.col != deskCol;
	}

	QColor hue;
	std::vector<std::unique_ptr<Row>> rows;
	QString heads[6];
	uint32_t file = 1;
	uint32_t own = 1; /* the In the file keys as you set them, the vertical's own track left out */
	bool mainSet = false;
	int mainCol = 1;
	int deskCol = 0;
	bool locked = false;
	int hotRow = -1;
	int hotCol = 0;
	bool hotFoot = false;
};

/* The recordings folder on one line. A long path keeps its end in view (the folder's own
   name) and shows the whole path on hover. */
class SvPathLine : public QWidget {
public:
	SvPathLine() { setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); }
	void setPath(const QString &t)
	{
		text = t;
		setToolTip(t);
		update();
	}
	QSize sizeHint() const override
	{
		return QSize(120, (int)std::ceil(QFontMetricsF(svl::body(14, QFont::Medium)).height()) + 4);
	}
	QSize minimumSizeHint() const override { return QSize(40, sizeHint().height()); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::TextAntialiasing);
		const QFont f = svl::body(14, QFont::Medium);
		p.setFont(f);
		const QString shown = QFontMetricsF(f).elidedText(text, Qt::ElideLeft, width());
		p.setPen(QColor(0, 0, 0, 215));
		p.drawText(QRectF(rect()).translated(0, 1), Qt::AlignLeft | Qt::AlignVCenter, shown);
		p.setPen(svl::INK2);
		p.drawText(QRectF(rect()), Qt::AlignLeft | Qt::AlignVCenter, shown);
	}

private:
	QString text;
};

/* "Track 2: Desktop Audio, Mic/Aux and 2 more", so the choice says what you will hear. */
static QString track_label(int track)
{
	const SvSoundDesk desk = sv_sound_desk();
	if (desk.own && desk.track == track)
		return QString(T("SettingsTrackDeskItem")).arg(track);
	struct Ctx {
		uint32_t bit;
		QStringList names;
	} ctx{1u << (track - 1), {}};
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			auto *c = static_cast<Ctx *>(param);
			if ((obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) && (obs_source_get_audio_mixers(s) & c->bit))
				c->names << QString::fromUtf8(obs_source_get_name(s));
			return true;
		},
		&ctx);
	ctx.names.sort(Qt::CaseInsensitive);
	QString what;
	if (ctx.names.isEmpty())
		what = T("SettingsTrackEmpty");
	else if (ctx.names.size() <= 3)
		what = ctx.names.join(", ");
	else
		what = QString(T("SettingsTrackMore")).arg(ctx.names.mid(0, 2).join(", ")).arg(ctx.names.size() - 2);
	return QString(T("SettingsTrackItem")).arg(track).arg(what);
}

/* The Settings tabs. The window sets their height itself (fit_pages), so the
   pages' own guesses, made as if the window were very narrow, are left out. */
class SvPages : public QStackedWidget {
public:
	bool hasHeightForWidth() const override { return false; }
	QSize minimumSizeHint() const override { return QSize(QStackedWidget::minimumSizeHint().width(), 0); }
};

class SvOutSettingsDialog : public SvWindow {
public:
	explicit SvOutSettingsDialog(QWidget *parent) : SvWindow(parent, T("SettingsTitle"), "settings")
	{
		setMinimumWidth(580);
		now = sv_output_settings();
		const bool busy = sv_output_encoders_in_use();

		if (busy) {
			auto *row = new QHBoxLayout();
			row->setSpacing(6);
			row->addWidget(icon_label("info", svl::AMBER, 14));
			auto *w = say(T("SettingsLocked"), AMBER_INK);
			row->addWidget(w, 1);
			row->addWidget(new svl::Info(T("SettingsLockedWhy")));
			v->addLayout(row);
		}

		/* Seven calm tabs, one page each. Exact numbers are typed, never slid. */
		auto *tabs = new svl::TabBar();
		tabs->addTab(T("SettingsTabStream"), "radio");
		tabs->addTab(T("SettingsTabPicture"), "canvas");
		tabs->addTab(T("SettingsTabSound"), "volume");
		tabs->addTab(T("SettingsTabRecording"), "video");
		tabs->addTab(T("SettingsTabBacktrack"), "history");
		tabs->addTab(T("SettingsTabReplay"), "replay");
		tabs->addTab(T("SettingsTabSwitch"), "move");
		/* Each tab has its own colour so the seven read apart at a glance. Replay is blue,
		   the colour its frame takes when the layout has none. */
		const QColor hues[] = {svl::VIOLET, svl::CYAN, svl::LIME, svl::ORANGE, svl::EMERALD, svl::BLUE, svl::PINK};
		for (int i = 0; i < 7; i++)
			tabs->setTabHue(i, hues[i]);
		/* The seven tabs stay on one row. Left to itself the bar asks for room to
		   stack every tab in one column, which made the window far too tall. */
		tabs->setMinimumSize(tabs->sizeHint());
		v->addWidget(tabs);
		v->addSpacing(4);
		auto *stack = new SvPages();
		pages = stack;
		/* fit_pages() sets the height; the pages' own guess is far too tall. */
		stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
		v->addWidget(stack);
		tabs->onPick = [stack](int i) { stack->setCurrentIndex(i); };

		/* ----- Stream ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[0]);
			rowHue = hues[0];
			auto *pv = page_layout(page);
			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);
			bitrate = new svl::NumberBox(1000, 20000, 500, now.bitrate, T("UnitKbps"));
			grid->addLayout(field_row(T("SettingsBitrate"), T("SettingsBitrateNote")), 0, 0);
			grid->addWidget(bitrate, 1, 0, Qt::AlignLeft);
			delay = new svl::NumberBox(0, 600, 5, now.delay_seconds, T("UnitSeconds"), T("OffWord"));
			delay->setUnitOne(T("UnitSecond"));
			grid->addLayout(field_row(T("SettingsDelay"), T("SettingsDelayNote")), 0, 1);
			grid->addWidget(delay, 1, 1, Qt::AlignLeft);
			tries = new svl::NumberBox(0, 100, 1, now.reconnect_tries, T("UnitTimes"), T("OffWord"));
			tries->setUnitOne(T("UnitTime"));
			grid->addLayout(field_row(T("SettingsTries"), T("SettingsTriesNote")), 2, 0);
			grid->addWidget(tries, 3, 0, Qt::AlignLeft);
			wait = new svl::NumberBox(1, 60, 1, now.reconnect_wait, T("UnitSeconds"));
			wait->setUnitOne(T("UnitSecond"));
			grid->addLayout(field_row(T("SettingsWait")), 2, 1);
			grid->addWidget(wait, 3, 1, Qt::AlignLeft);
			grid->setColumnStretch(0, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);

			pv->addSpacing(4);
			auto *followRow = new QHBoxLayout();
			followRow->setSpacing(6);
			follow = new svl::Switch(T("SettingsFollowObs"));
			follow->setChecked(now.follow_obs);
			followRow->addWidget(follow);
			followRow->addWidget(new svl::Info(T("SettingsFollowObsNote")));
			followRow->addStretch(1);
			pv->addLayout(followRow);
			auto *askRow = new QHBoxLayout();
			askRow->setSpacing(6);
			askLive = new svl::Switch(T("SettingsAskLive"));
			askLive->setChecked(sv_store_int("ask_go_live", 1) != 0);
			askRow->addWidget(askLive);
			askRow->addWidget(new svl::Info(T("SettingsAskLiveNote")));
			askRow->addSpacing(18);
			askStop = new svl::Switch(T("SettingsAskStop"));
			askStop->setChecked(sv_store_int("ask_stop_live", 1) != 0);
			askRow->addWidget(askStop);
			askRow->addWidget(new svl::Info(T("SettingsAskStopNote")));
			askRow->addStretch(1);
			pv->addLayout(askRow);
			pv->addStretch(1);
			stack->addWidget(page);
		}

		/* ----- Picture ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[1]);
			rowHue = hues[1];
			auto *pv = page_layout(page);
			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);

			size = choice({T("SettingsSize1080"), T("SettingsSize900"), T("SettingsSize720")},
				      std::clamp(now.size, 0, 2));
			grid->addLayout(field_row(T("SettingsSize"), T("SettingsSizeNote")), 0, 0);
			grid->addWidget(size, 1, 0, Qt::AlignLeft);
			sharpBox = new QWidget();
			auto *sharpCol = new QVBoxLayout(sharpBox);
			sharpCol->setContentsMargins(0, 0, 0, 0);
			sharpCol->setSpacing(6);
			sharp = choice({T("SettingsSharp"), T("SettingsSmooth")}, now.sharp_scale ? 0 : 1);
			sharpCol->addLayout(field_row(T("SettingsShrink"), T("SettingsShrinkNote")));
			sharpCol->addWidget(sharp, 0, Qt::AlignLeft);
			grid->addWidget(sharpBox, 0, 1, 2, 1, Qt::AlignTop | Qt::AlignLeft);
			sharpBox->setVisible(now.size != SV_SIZE_1080);
			size->onPick = [this](int i) {
				sharpBox->setVisible(i != SV_SIZE_1080);
			};

			/* 60 is always offered. The vertical draws on OBS's clock, so OBS's own rate or an even
			   share of it; above OBS's rate it needs OBS raised first, which the key next to it does. */
			fpsHue = hues[1];
			const int whole = sv_output_obs_fps();
			for (int d = 1; d <= 12; d++)
				if (whole % d == 0 && whole / d >= 10)
					fpsChoices.push_back(whole / d);
			fpsChoices.push_back(60);
			const int nowFps = now.fps > 0 ? now.fps : whole;
			fpsChoices.push_back(nowFps);
			std::sort(fpsChoices.begin(), fpsChoices.end(), std::greater<int>());
			fpsChoices.erase(std::unique(fpsChoices.begin(), fpsChoices.end()), fpsChoices.end());
			QStringList fpsNames;
			int fpsAt = 0;
			for (size_t i = 0; i < fpsChoices.size(); i++) {
				fpsNames << QString::number(fpsChoices[i]);
				if (fpsChoices[i] == nowFps)
					fpsAt = (int)i;
			}
			fps = choice(fpsNames, fpsAt);
			grid->addLayout(field_row(T("SettingsFps"), T("SettingsFpsNote")), 2, 0, 1, 2);
			auto *fpsRow = new QHBoxLayout();
			fpsRow->setSpacing(10);
			fpsRow->addWidget(fps);
			setObsFps = key(QString(), "settings", hues[1]);
			fpsRow->addWidget(setObsFps);
			fpsRow->addStretch(1);
			grid->addLayout(fpsRow, 3, 0, 1, 2);
			fpsWarn = say(QString(), svl::AMBER, 14);
			grid->addWidget(fpsWarn, 4, 0, 1, 2);
			fps->onPick = [this](int) { show_fps(); };
			connect(setObsFps, &QPushButton::clicked, this, [this] { set_obs_fps(); });
			grid->setColumnStretch(0, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);

			/* Automatic says which encoder it is on this PC. */
			encoder = new QComboBox();
			hue_combo(encoder, hues[1]);
			const std::vector<SvEncoderChoice> encoders = sv_output_encoders();
			const std::string autoId = sv_output_auto_encoder();
			QString autoName;
			for (const SvEncoderChoice &c : encoders)
				if (c.id == autoId)
					autoName = qs(c.name);
			encoder->addItem(autoName.isEmpty() ? QString(T("SettingsEncoderAuto"))
							    : QString(T("SettingsEncoderAutoIs")).arg(autoName),
					 QString());
			for (const SvEncoderChoice &c : encoders)
				encoder->addItem(qs(c.name), qs(c.id));
			const int at = encoder->findData(qs(now.encoder));
			encoder->setCurrentIndex(at >= 0 ? at : 0);
			pv->addLayout(field_row(T("SettingsEncoder")));
			pv->addWidget(encoder);

			/* The encoder's own presets, by the names OBS uses, lightest first. */
			presetBox = new QWidget();
			auto *presetCol = new QVBoxLayout(presetBox);
			presetCol->setContentsMargins(0, 0, 0, 0);
			presetCol->setSpacing(6);
			preset = new QComboBox();
			hue_combo(preset, hues[1]);
			presetCol->addLayout(field_row(T("SettingsPreset"), T("SettingsPresetNote")));
			presetCol->addWidget(preset);
			pv->addWidget(presetBox);
			fill_presets(sv_output_preset_for(chosen_encoder(), now));
			connect(encoder, &QComboBox::currentIndexChanged, this, [this] {
				fill_presets(preset->currentData().toString().toStdString());
				if (recMode && clipQ)
					show_quality();
			});

			auto *kfRow = new QHBoxLayout();
			kfRow->setSpacing(8);
			auto *kfCol = new QVBoxLayout();
			kfCol->setSpacing(6);
			keyframe = new svl::NumberBox(1, 10, 1, now.keyframe_sec, T("UnitSeconds"));
			keyframe->setUnitOne(T("UnitSecond"));
			kfCol->addLayout(field_row(T("SettingsKeyframe"), T("SettingsKeyframeNote")));
			kfCol->addWidget(keyframe, 0, Qt::AlignLeft);
			kfRow->addLayout(kfCol);
			kfRow->addSpacing(20);
			auto *copyCol = new QVBoxLayout();
			copyCol->setSpacing(6);
			copyCol->addLayout(field_row(T("SettingsCopyTitle"), T("SettingsCopyNote")));
			copyMain = key(T("SettingsCopyMain"), "copy", hues[1]);
			copyCol->addWidget(copyMain, 0, Qt::AlignLeft);
			kfRow->addLayout(copyCol);
			kfRow->addStretch(1);
			pv->addLayout(kfRow);
			copied = say(QString(), svl::tone(hues[1], 0.3));
			copied->setVisible(false);
			pv->addWidget(copied);
			pv->addStretch(1);
			stack->addWidget(page);
		}

		/* ----- Sound: what the vertical hears ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[2]);
			rowHue = hues[2];
			soundHue = hues[2];
			auto *pv = page_layout(page);
			desk = sv_sound_desk();
			deskBefore = sv_sound_snapshot();

			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(18);
			grid->setVerticalSpacing(6);
			soundMode = choice({T("SoundSameTrack"), T("SoundOwnMix")}, desk.own ? 1 : 0);
			grid->addLayout(field_row(T("SoundWhat"), T("SoundWhatNote")), 0, 0);
			grid->addWidget(soundMode, 1, 0, Qt::AlignLeft);
			const int rates[] = {128, 160, 192, 320};
			QStringList rateNames;
			int rateAt = 1;
			for (int i = 0; i < 4; i++) {
				rateNames << QString("%1 %2").arg(rates[i]).arg(T("UnitKbps"));
				if (rates[i] == now.audio_bitrate)
					rateAt = i;
			}
			soundQ = choice(rateNames, rateAt);
			grid->addLayout(field_row(T("SettingsSoundQuality"), T("SettingsSoundQualityNote")), 0, 1);
			grid->addWidget(soundQ, 1, 1, Qt::AlignLeft);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);

			/* A main OBS track, as before. */
			trackBox = new QWidget();
			auto *tv = new QVBoxLayout(trackBox);
			tv->setContentsMargins(0, 4, 0, 0);
			tv->setSpacing(6);
			audio = new QComboBox();
			hue_combo(audio, hues[2]);
			for (int i = 1; i <= 6; i++)
				audio->addItem(track_label(i), i);
			audio->setCurrentIndex(std::clamp(now.audio_track, 1, 6) - 1);
			tv->addLayout(field_row(T("SettingsAudio"), T("SettingsAudioNote")));
			tv->addWidget(audio);
			pv->addWidget(trackBox);

			/* Its own mix: a volume for every sound, heard on the vertical only. */
			deskBox = new QWidget();
			auto *dv = new QVBoxLayout(deskBox);
			dv->setContentsMargins(0, 4, 0, 0);
			dv->setSpacing(6);
			dv->addWidget(say(T("SoundOwnWhat"), svl::tone(hues[2], 0.3), 14));
			dv->addSpacing(6);
			auto *dgrid = new QGridLayout();
			dgrid->setHorizontalSpacing(18);
			dgrid->setVerticalSpacing(6);
			deskTrack = new QComboBox();
			hue_combo(deskTrack, hues[2]);
			for (int i = 1; i <= 6; i++)
				deskTrack->addItem(QString(T("SoundTrackN")).arg(i), i);
			deskTrack->setCurrentIndex(std::clamp(desk.track, 1, 6) - 1);
			dgrid->addLayout(field_row(T("SoundDeskTrack"), T("SoundDeskTrackNote")), 0, 0);
			dgrid->addWidget(deskTrack, 1, 0, Qt::AlignLeft);
			master = new svl::NumberBox(0, 200, 5, desk.master, "%");
			dgrid->addLayout(field_row(T("SoundMaster"), T("SoundMasterNote")), 0, 1);
			dgrid->addWidget(master, 1, 1, Qt::AlignLeft);
			auto *muteRow = new QHBoxLayout();
			muteRow->setSpacing(6);
			followMute = new svl::Switch(T("SoundFollowMute"));
			followMute->setChecked(desk.follow_mute);
			muteRow->addWidget(followMute);
			muteRow->addWidget(new svl::Info(T("SoundFollowMuteNote")));
			muteRow->addStretch(1);
			dgrid->addLayout(muteRow, 1, 2, Qt::AlignLeft | Qt::AlignVCenter);
			dgrid->setColumnStretch(2, 1);
			dv->addLayout(dgrid);
			deskWarn = say(QString(), svl::AMBER, 14);
			deskWarn->setVisible(false);
			dv->addWidget(deskWarn);
			dv->addLayout(field_row(T("SoundSources"), T("SoundSourcesNote")));
			auto *area = new QScrollArea();
			area->setWidgetResizable(true);
			area->setFrameShape(QFrame::NoFrame);
			area->viewport()->setAutoFillBackground(false);
			area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			/* At least five rows show; the list takes any room the window has to spare. */
			area->setMinimumHeight(176);
			rowsHost = new QWidget();
			rowsHost->setAutoFillBackground(false);
			rowsGrid = new QGridLayout(rowsHost);
			rowsGrid->setContentsMargins(0, 0, 10, 0);
			rowsGrid->setHorizontalSpacing(10);
			rowsGrid->setVerticalSpacing(4);
			area->setWidget(rowsHost);
			dv->addWidget(area, 1);
			pv->addWidget(deskBox, 100);
			pv->addStretch(1);
			stack->addWidget(page);

			trackBox->setVisible(!desk.own);
			deskBox->setVisible(desk.own);
			build_rows();
			show_desk_warning();
			soundMode->onPick = [this](int i) {
				desk.own = i == 1;
				sv_sound_set_desk(desk);
				trackBox->setVisible(!desk.own);
				deskBox->setVisible(desk.own);
				build_rows();
				after_desk_change();
				/* The window grows to fit the mix when it shows. */
				fit_pages();
			};
			connect(deskTrack, &QComboBox::currentIndexChanged, this, [this] {
				desk.track = deskTrack->currentData().toInt();
				sv_sound_set_desk(desk);
				after_desk_change();
			});
			master->onChange = [this](int v) {
				desk.master = v;
				sv_sound_set_desk(desk);
			};
			connect(followMute, &QAbstractButton::toggled, this, [this](bool on) {
				desk.follow_mute = on;
				sv_sound_set_desk(desk);
			});
			/* The meters move with the sound; the list follows sources added or removed in OBS. */
			auto *meterTimer = new QTimer(this);
			meterTimer->setInterval(50);
			connect(meterTimer, &QTimer::timeout, this, [this] {
				if (!deskBox->isVisible())
					return;
				for (const SoundRow &r : rows)
					r.meter->setLevel(sv_sound_level(r.uuid));
			});
			meterTimer->start();
			auto *listTimer = new QTimer(this);
			listTimer->setInterval(1000);
			connect(listTimer, &QTimer::timeout, this, [this] {
				if (!deskBox->isVisible())
					return;
				std::vector<std::string> ids;
				for (const SvSoundSource &src : sv_sound_sources())
					ids.push_back(src.uuid + "|" + src.name);
				if (ids != rowIds)
					build_rows();
			});
			listTimer->start();
		}

		/* ----- Recording ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[3]);
			rowHue = hues[3];
			auto *pv = page_layout(page);
			folder = qs(now.folder);
			pv->addLayout(field_row(T("SettingsFolder")));
			auto *folderRow = new QHBoxLayout();
			folderRow->setSpacing(8);
			folderText = new SvPathLine();
			folderRow->addWidget(folderText, 1);
			browse = key(T("SettingsBrowse"), "folder", hues[3]);
			reset = key(T("SettingsReset"), "refresh", hues[3]);
			folderRow->addWidget(browse);
			folderRow->addWidget(reset);
			pv->addLayout(folderRow);
			show_folder();
			record_choice(pv, now);

			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);
			fileType = choice({"MP4", "MKV", "MOV"}, std::clamp(now.rec_format, 0, 2));
			grid->addLayout(field_row(T("SettingsFileType"), T("SettingsFileTypeNote")), 0, 0);
			grid->addWidget(fileType, 1, 0, Qt::AlignLeft);
			split = choice({T("SettingsSplitNever"), T("SettingsSplitTime"), T("SettingsSplitSize")},
				       std::clamp(now.split_mode, 0, 2));
			splitMin = new svl::NumberBox(1, 600, 5, now.split_minutes, T("UnitMinutes"));
			splitMin->setUnitOne(T("UnitMinute"));
			splitMb = new svl::NumberBox(100, 100000, 100, now.split_mb, T("UnitMB"));
			grid->addLayout(field_row(T("SettingsSplit"), T("SettingsSplitNote")), 0, 1);
			auto *splitRow = new QHBoxLayout();
			splitRow->setSpacing(10);
			splitRow->addWidget(split);
			splitRow->addWidget(splitMin);
			splitRow->addWidget(splitMb);
			splitRow->addStretch(1);
			grid->addLayout(splitRow, 1, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);
			splitMin->setVisible(now.split_mode == SV_SPLIT_TIME);
			splitMb->setVisible(now.split_mode == SV_SPLIT_SIZE);
			split->onPick = [this](int i) {
				splitMin->setVisible(i == SV_SPLIT_TIME);
				splitMb->setVisible(i == SV_SPLIT_SIZE);
			};

			/* HEVC and AV1: a sharper file at the same size, on newer graphics cards. */
			fileHue = hues[3];
			codec = choice({"H.264", "HEVC", "AV1"}, std::clamp(now.rec_codec, 0, 2));
			pv->addLayout(field_row(T("SettingsCodec"), T("SettingsCodecNote")));
			pv->addWidget(codec, 0, Qt::AlignLeft);
			codecHint = say(QString(), svl::tone(hues[3], 0.3), 14);
			pv->addWidget(codecHint);
			/* Its own box, so the (i) hides with the switch. */
			codecYouTubeBox = new QWidget();
			auto *ytCol = new QVBoxLayout(codecYouTubeBox);
			ytCol->setContentsMargins(0, 0, 0, 0);
			codecYouTube = switch_row(ytCol, T("SettingsCodecYouTube"), T("SettingsCodecYouTubeNote"),
						  now.codec_youtube);
			pv->addWidget(codecYouTubeBox);
			codec->onPick = [this](int) { show_codec(); };
			connect(encoder, &QComboBox::currentIndexChanged, this, [this] { show_codec(); });
			show_codec();

			recMode = choice({T("SettingsRecSame"), T("SettingsCbr"), T("SettingsVbr"), T("SettingsCqp")},
					 ui_of(REC_UI, now.rec_quality));
			recLevel = new svl::NumberBox(1, 51, 1, now.rec_level, QString());
			recBitrate = new svl::NumberBox(1000, 50000, 500, now.record_bitrate, T("UnitKbps"));
			pv->addLayout(field_row(T("SettingsRecQuality"), T("SettingsRecQualityNote")));
			auto *recRow = new QHBoxLayout();
			recRow->setSpacing(10);
			recRow->addWidget(recMode);
			recRow->addWidget(recLevel);
			recRow->addWidget(recBitrate);
			recRow->addStretch(1);
			pv->addLayout(recRow);
			recScale = new SvLevelScale(hues[3]);
			pv->addWidget(recScale);
			recHint = say(QString(), svl::tone(hues[3], 0.3), 14);
			pv->addWidget(recHint);
			recMode->onPick = [this](int) { show_quality(); };
			recLevel->onChange = [this](int) { show_quality(); };
			recBitrate->onChange = [this](int) { show_quality(); };

			pv->addSpacing(6);
			pv->addLayout(field_row(T("SettingsTracks"), T("SettingsTracksNote")));
			build_track_grid(pv, hues[3]);

			pv->addSpacing(4);
			recGame = switch_row(pv, T("SettingsRecGame"), T("SettingsGameNote"), now.rec_game_name);
			keepAll = switch_row(pv, T("SettingsRecordLive"), T("SettingsRecordLiveNote"), now.record_with_live);
			pv->addStretch(1);
			stack->addWidget(page);
		}

		/* ----- Backtrack ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[4]);
			rowHue = hues[4];
			auto *pv = page_layout(page);
			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);
			clipCanvas = canvas_choice(pv, T("SettingsClipCanvas"), T("SettingsClipCanvasNote"), now.clip_canvas);
			backtrack = new svl::NumberBox(10, 600, 10, now.backtrack_seconds, T("UnitSeconds"));
			grid->addLayout(field_row(T("SettingsBacktrack"), T("SettingsBacktrackNote")), 0, 0);
			grid->addWidget(backtrack, 1, 0, Qt::AlignLeft);
			after = new svl::NumberBox(0, 30, 1, now.clip_after, T("UnitSeconds"), T("OffWord"));
			after->setUnitOne(T("UnitSecond"));
			grid->addLayout(field_row(T("SettingsAfter"), T("SettingsAfterNote")), 0, 1);
			grid->addWidget(after, 1, 1, Qt::AlignLeft);
			memLimit = new svl::NumberBox(0, 64000, 100, now.clip_max_mb, T("UnitMB"), T("NoLimitWord"));
			grid->addLayout(field_row(T("SettingsMemory"), T("SettingsMemoryNote")), 2, 0);
			grid->addWidget(memLimit, 3, 0, Qt::AlignLeft);
			grid->setColumnStretch(0, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);

			autoBack = choice({T("SettingsAutoNever"), T("SettingsAutoOpen"), T("SettingsAutoLive")},
					  std::clamp(now.backtrack_auto, 0, 2));
			pv->addLayout(field_row(T("SettingsAutoBacktrack"), T("SettingsAutoBacktrackNote")));
			pv->addWidget(autoBack, 0, Qt::AlignLeft);

			clipQ = choice({T("SettingsClipAsRec"), T("SettingsRecSame"), T("SettingsCbr"), T("SettingsVbr"),
					T("SettingsCqp")},
				       ui_of(CLIP_UI, now.clip_quality));
			clipLevel = new svl::NumberBox(1, 51, 1, now.clip_level, QString());
			clipBitrate = new svl::NumberBox(1000, 50000, 500, now.clip_bitrate, T("UnitKbps"));
			pv->addLayout(field_row(T("SettingsClipQuality"), T("SettingsClipQualityNote")));
			auto *clipRow = new QHBoxLayout();
			clipRow->setSpacing(10);
			clipRow->addWidget(clipQ);
			clipRow->addWidget(clipLevel);
			clipRow->addWidget(clipBitrate);
			clipRow->addStretch(1);
			pv->addLayout(clipRow);
			clipScale = new SvLevelScale(hues[4]);
			pv->addWidget(clipScale);
			clipHint = say(QString(), svl::tone(hues[4], 0.3), 14);
			pv->addWidget(clipHint);
			clipQ->onPick = [this](int) { show_quality(); };
			clipLevel->onChange = [this](int) { show_quality(); };
			clipBitrate->onChange = [this](int) { show_quality(); };

			clipFolder = qs(now.clip_folder);
			pv->addLayout(field_row(T("SettingsClipFolder")));
			auto *folderRow = new QHBoxLayout();
			folderRow->setSpacing(8);
			clipFolderText = new SvPathLine();
			folderRow->addWidget(clipFolderText, 1);
			clipBrowse = key(T("SettingsBrowse"), "folder", hues[4]);
			clipReset = key(T("SettingsClipReset"), "refresh", hues[4]);
			folderRow->addWidget(clipBrowse);
			folderRow->addWidget(clipReset);
			pv->addLayout(folderRow);
			show_folder();

			pv->addSpacing(4);
			clipGame = switch_row(pv, T("SettingsClipGame"), T("SettingsGameNote"), now.clip_game_name);
			pv->addStretch(1);
			stack->addWidget(page);
		}

		/* ----- Replay: how Play last clip shows a clip on the vertical ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[5]);
			rowHue = hues[5];
			auto *pv = page_layout(page);
			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);
			const int slowAt = now.replay_slow >= 100 ? 0 : now.replay_slow >= 75 ? 1 : now.replay_slow >= 50 ? 2 : 3;
			replaySlow = choice({T("ReplaySlowOff"), T("ReplaySlow75"), T("ReplaySlow50"), T("ReplaySlow25")}, slowAt);
			grid->addLayout(field_row(T("SettingsReplaySlow"), T("SettingsReplaySlowNote")), 0, 0);
			grid->addWidget(replaySlow, 1, 0, Qt::AlignLeft);
			replaySlowWhat = choice({T("ReplaySlowMoment"), T("ReplaySlowWhole"), T("ReplaySlowPress")},
						 now.replay_slow_press ? 2 : now.replay_slow_whole ? 1 : 0);
			grid->addLayout(field_row(T("SettingsReplaySlowWhat"), T("SettingsReplaySlowWhatNote")), 0, 1);
			grid->addWidget(replaySlowWhat, 1, 1, Qt::AlignLeft);
			replaySlowLen = new svl::NumberBox(1, 30, 1, now.replay_slow_seconds, T("UnitSeconds"));
			replaySlowLen->setUnitOne(T("UnitSecond"));
			grid->addLayout(field_row(T("SettingsReplaySlowLength"), T("SettingsReplaySlowLengthNote")), 2, 0);
			grid->addWidget(replaySlowLen, 3, 0, Qt::AlignLeft);
			replayLen = new svl::NumberBox(0, 60, 5, now.replay_seconds, T("UnitSeconds"), T("ReplayWholeClip"));
			grid->addLayout(field_row(T("SettingsReplayLength"), T("SettingsReplayLengthNote")), 2, 1);
			grid->addWidget(replayLen, 3, 1, Qt::AlignLeft);
			replayTrans = choice({T("ReplayTransWipe"), T("ReplayTransSlide"), T("ReplayTransFade"), T("ReplayTransCine")},
					     std::clamp(now.replay_transition, 0, 3));
			grid->addLayout(field_row(T("SettingsReplayTrans"), T("SettingsReplayTransNote")), 4, 0);
			grid->addWidget(replayTrans, 5, 0, Qt::AlignLeft);
			replayZoom = new svl::NumberBox(0, 25, 5, now.replay_zoom, T("UnitPercent"), T("ReplayZoomOff"));
			grid->addLayout(field_row(T("SettingsReplayZoom"), T("SettingsReplayZoomNote")), 4, 1);
			grid->addWidget(replayZoom, 5, 1, Qt::AlignLeft);
			grid->setColumnStretch(0, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);

			pv->addSpacing(4);
			replaySound = switch_row(pv, T("SettingsReplaySound"), T("SettingsReplaySoundNote"), now.replay_sound);
			replayTag = switch_row(pv, T("SettingsReplayTag"), T("SettingsReplayTagNote"), now.replay_tag);
			replayLine = switch_row(pv, T("SettingsReplayLine"), T("SettingsReplayLineNote"), now.replay_line);
			replayCamLive = switch_row(pv, T("SettingsReplayCamLive"), T("SettingsReplayCamLiveNote"),
						   now.replay_cam_live);
			replayAuto = switch_row(pv, T("SettingsReplayAuto"), T("SettingsReplayAutoNote"), now.replay_auto);
			pv->addStretch(1);
			stack->addWidget(page);

			/* What slows down and for how long only matter with slow motion on, and the
			   slow part's length only when it is the moment, or the key press, that slows. */
			const auto showSlow = [this] {
				const bool slow = replaySlow->current() != 0;
				replaySlowWhat->setEnabled(slow);
				replaySlowLen->setEnabled(slow && replaySlowWhat->current() != 1);
			};
			replaySlow->onPick = [showSlow](int) { showSlow(); };
			replayLine->setEnabled(replayTag->isChecked());
			connect(replayTag, &QAbstractButton::toggled, replayLine, &QWidget::setEnabled);
			replaySlowWhat->onPick = [showSlow](int) { showSlow(); };
			showSlow();
		}

		/* ----- Layout transitions: how the boxes move when the layout does ----- */
		{
			auto *page = new QWidget();
			svl::set_hue(page, hues[6]);
			rowHue = hues[6];
			auto *pv = page_layout(page);
			pv->addWidget(say(T("SwitchWhat"), svl::tone(hues[6], 0.3), 15));
			pv->addSpacing(2);
			pv->addLayout(field_row(T("SwitchTitle"), T("SwitchNote")));
			auto *modes = new QHBoxLayout();
			modes->setSpacing(0);
			const char *names[] = {"SwitchCut", "SwitchFade", "SwitchSlide", "SwitchGlide"};
			const char *subs[] = {"SwitchCutSub", "SwitchFadeSub", "SwitchSlideSub", "SwitchGlideSub"};
			for (int i = 0; i < 4; i++) {
				auto *t = new svl::Tile(T(names[i]), QString());
				t->setAccent(hues[6]);
				t->setToolTip(T(subs[i]));
				modes->addWidget(t);
				modeTiles.push_back(t);
				connect(t, &QAbstractButton::clicked, this, [this, i] { pick_mode((SvSwitch)i); });
			}
			modes->addStretch(1);
			pv->addLayout(modes);

			auto *grid = new QGridLayout();
			grid->setHorizontalSpacing(14);
			grid->setVerticalSpacing(6);
			feelBox = new QWidget();
			auto *feelCol = new QVBoxLayout(feelBox);
			feelCol->setContentsMargins(0, 0, 0, 0);
			feelCol->setSpacing(6);
			feel = choice({T("SwitchSmooth"), T("SwitchSnappy"), T("SwitchBouncy")}, sv_source_switch_feel());
			feelCol->addLayout(field_row(T("SwitchFeel"), T("SwitchFeelNote")));
			feelCol->addWidget(feel, 0, Qt::AlignLeft);
			grid->addWidget(feelBox, 0, 0, Qt::AlignTop | Qt::AlignLeft);
			lengthBox = new QWidget();
			auto *lengthCol = new QVBoxLayout(lengthBox);
			lengthCol->setContentsMargins(0, 0, 0, 0);
			lengthCol->setSpacing(6);
			length = new svl::NumberBox(100, 2000, 50, sv_source_switch_ms(), T("UnitMs"));
			lengthCol->addLayout(field_row(T("SwitchLength"), T("SwitchLengthNote")));
			lengthCol->addWidget(length, 0, Qt::AlignLeft);
			grid->addWidget(lengthBox, 0, 1, Qt::AlignTop | Qt::AlignLeft);
			dirBox = new QWidget();
			auto *dirCol = new QVBoxLayout(dirBox);
			dirCol->setContentsMargins(0, 0, 0, 0);
			dirCol->setSpacing(6);
			direction = choice({T("SwitchLeft"), T("SwitchRight"), T("SwitchUp"), T("SwitchDown")},
					   sv_source_switch_dir());
			dirCol->addLayout(field_row(T("SwitchDirection"), T("SwitchDirectionNote")));
			dirCol->addWidget(direction, 0, Qt::AlignLeft);
			grid->addWidget(dirBox, 1, 0, 1, 2, Qt::AlignTop | Qt::AlignLeft);
			grid->setColumnStretch(0, 1);
			grid->setColumnStretch(1, 1);
			pv->addLayout(grid);
			pv->addStretch(1);
			stack->addWidget(page);
			feel->onPick = [this](int) { show_motion(); };
			direction->onPick = [this](int) { show_motion(); };
			length->onChange = [this](int) { show_motion(); };
			pick_mode(sv_source_switch_mode());
		}

		show_quality();

		/* What the running stream uses stays as it is until everything stops. Volumes on the
		   vertical's own mix stay free, so they can be fixed while live. */
		for (QWidget *w : std::initializer_list<QWidget *>{
			     bitrate, delay, tries, wait, audio, soundQ, soundMode, deskTrack, size, sharp, fps, encoder, preset,
			     keyframe, copyMain, browse, reset, fileType, split, splitMin, splitMb, recMode, recLevel, recBitrate,
			     codec, codecYouTube, backtrack, after, memLimit, clipQ, clipLevel, clipBitrate, clipBrowse,
			     clipReset}) {
			w->setEnabled(!busy);
			if (busy)
				w->setToolTip(T("SettingsLockedWhy"));
		}
		for (svl::TabBar *c : {clipCanvas}) {
			if (c && busy) {
				c->setEnabled(false);
				c->setToolTip(T("SettingsLockedWhy"));
			}
		}
		for (auto &pick : recPicks) {
			if (busy) {
				pick.second->setEnabled(false);
				pick.second->setToolTip(T("SettingsLockedWhy"));
			}
		}
		locked = busy;
		lock_main_track();
		show_fps();
		connect(audio, &QComboBox::currentIndexChanged, this, [this] { lock_main_track(); });
		connect(deskTrack, &QComboBox::currentIndexChanged, this, [this] { lock_main_track(); });

		QHBoxLayout *buttons = button_row();
		buttons->addStretch(1);
		auto *cancel = key(T("DestCancel"), "x");
		auto *save = key(T("DestSave"), "check");
		save->setDefault(true);
		buttons->addWidget(cancel);
		buttons->addWidget(save);
		fit_pages();

		connect(browse, &QPushButton::clicked, this, [this] {
			const QString start = folder.isEmpty() ? qs(sv_output_record_folder()) : folder;
			const QString picked = QFileDialog::getExistingDirectory(this, T("SettingsFolder"), start);
			if (!picked.isEmpty()) {
				folder = picked;
				show_folder();
			}
		});
		connect(reset, &QPushButton::clicked, this, [this] {
			folder.clear();
			show_folder();
		});
		connect(clipBrowse, &QPushButton::clicked, this, [this] {
			const QString start = !clipFolder.isEmpty() ? clipFolder
					      : !folder.isEmpty()    ? folder
								     : qs(sv_output_record_folder());
			const QString picked = QFileDialog::getExistingDirectory(this, T("SettingsClipFolder"), start);
			if (!picked.isEmpty()) {
				clipFolder = picked;
				show_folder();
			}
		});
		connect(clipReset, &QPushButton::clicked, this, [this] {
			clipFolder.clear();
			show_folder();
		});
		connect(copyMain, &QPushButton::clicked, this, [this] { copy_main(); });
		connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
		connect(save, &QPushButton::clicked, this, [this, busy] {
			SvOutSettings s = now;
			/* Something may have started while the window was open (a hotkey, Start with OBS).
			   What it uses then stays as it is, and the dock says so. */
			const bool startedSince = !busy && sv_output_encoders_in_use();
			if (startedSince)
				sv_output_set_notice(T("SettingsStartedWhileOpen"), true);
			sv_store_batch_begin();
			if (!busy && !startedSince) {
				const int rates[] = {128, 160, 192, 320};
				s.bitrate = bitrate->value();
				s.delay_seconds = delay->value();
				s.reconnect_tries = tries->value();
				s.reconnect_wait = wait->value();
				s.audio_track = audio->currentData().toInt();
				s.audio_bitrate = rates[std::clamp(soundQ->current(), 0, 3)];
				s.size = size->current();
				s.sharp_scale = sharp->current() == 0;
				/* OBS's own rate, left as it was, keeps following OBS. */
				const int pickedFps = picked_fps();
				s.fps = now.fps == 0 && pickedFps == sv_output_obs_fps() ? 0 : pickedFps;
				s.fps_div = sv_output_fps_div(s.fps);
				s.encoder = encoder->currentData().toString().toStdString();
				s.preset = preset->currentData().toString().toStdString();
				s.keyframe_sec = keyframe->value();
				s.folder = folder.toStdString();
				s.rec_format = fileType->current();
				s.split_mode = split->current();
				s.split_minutes = splitMin->value();
				s.split_mb = splitMb->value();
				s.rec_quality = REC_UI[(size_t)std::clamp(recMode->current(), 0, 3)];
				s.rec_level = recLevel->value();
				s.record_bitrate = recBitrate->value();
				s.rec_tracks = route ? route->inFile() : now.rec_tracks;
				s.rec_codec = codec->current();
				s.codec_youtube = codecYouTube->isChecked();
				s.backtrack_seconds = backtrack->value();
				s.clip_after = after->value();
				s.clip_max_mb = memLimit->value();
				s.clip_quality = CLIP_UI[(size_t)std::clamp(clipQ->current(), 0, 4)];
				s.clip_level = clipLevel->value();
				s.clip_bitrate = clipBitrate->value();
				s.clip_folder = clipFolder.toStdString();
				if (!recPicks.empty()) {
					std::vector<std::string> list;
					for (const auto &pick : recPicks)
						if (pick.second->isChecked())
							list.push_back(pick.first);
					if (list.empty())
						list.push_back(std::string());
					s.rec_canvases = list;
					s.rec_canvas = list.front();
				}
				if (clipCanvas)
					s.clip_canvas = picked_canvas(clipCanvas);
				apply_ticks();
			}
			s.follow_obs = follow->isChecked();
			sv_store_set_int("ask_go_live", askLive->isChecked() ? 1 : 0);
			sv_store_set_int("ask_stop_live", askStop->isChecked() ? 1 : 0);
			s.record_with_live = keepAll->isChecked();
			s.rec_game_name = recGame->isChecked();
			s.clip_game_name = clipGame->isChecked();
			static const int SLOW_UI[] = {100, 75, 50, 25};
			s.replay_slow = SLOW_UI[std::clamp(replaySlow->current(), 0, 3)];
			s.replay_slow_whole = replaySlowWhat->current() == 1;
			s.replay_slow_press = replaySlowWhat->current() == 2;
			s.replay_slow_seconds = replaySlowLen->value();
			s.replay_sound = replaySound->isChecked();
			s.replay_seconds = replayLen->value();
			s.replay_tag = replayTag->isChecked();
			s.replay_auto = replayAuto->isChecked();
			s.replay_transition = replayTrans->current();
			s.replay_zoom = replayZoom->value();
			s.replay_line = replayLine->isChecked();
			s.replay_cam_live = replayCamLive->isChecked();
			s.backtrack_auto = autoBack->current();
			sv_output_set_settings(s);
			sv_source_set_switch(mode, length->value());
			sv_source_set_switch_style(feel->current(), direction->current());
			sv_store_batch_end();
			/* The sound mix is already live; Save keeps it. */
			soundSaved = true;
			accept();
		});
	}

	/* Cancel, Escape or the window's X: the sound mix goes back to how it was. */
	void reject() override
	{
		if (!soundSaved)
			sv_sound_restore(deskBefore);
		QDialog::reject();
	}

private:
	/* The window is as tall as its tallest tab, measured at the width it
	   really has. Small print that wraps reports a huge height when it is
	   measured narrow, which used to make the window taller than the screen. */
	void fit_pages()
	{
		if (!pages)
			return;
		const int across = (std::max)(width(), sizeHint().width()) - 40;
		int tall = 0;
		for (int i = 0; i < pages->count(); i++) {
			QWidget *pg = pages->widget(i);
			const int h = pg->hasHeightForWidth() ? pg->heightForWidth(across) : pg->sizeHint().height();
			tall = (std::max)(tall, h);
		}
		pages->setMinimumHeight(tall);
		/* An open window grows by itself when its smallest size grows. */
		if (!isVisible())
			adjustSize();
	}

	QStackedWidget *pages = nullptr;

	static QVBoxLayout *page_layout(QWidget *page)
	{
		auto *pv = new QVBoxLayout(page);
		pv->setContentsMargins(0, 4, 0, 0);
		pv->setSpacing(6);
		return pv;
	}

	/* A switch with an (i) beside it, on a row of its own. */
	static svl::Switch *switch_row(QVBoxLayout *pv, const QString &text, const QString &note, bool on)
	{
		auto *row = new QHBoxLayout();
		row->setSpacing(6);
		auto *sw = new svl::Switch(text);
		sw->setChecked(on);
		row->addWidget(sw);
		row->addWidget(new svl::Info(note));
		row->addStretch(1);
		pv->addLayout(row);
		return sw;
	}

	/* A short row of choices: the picked one raised and lit, the rest carved in. */
	static svl::TabBar *choice(const QStringList &names, int current)
	{
		auto *c = new svl::TabBar();
		for (const QString &n : names)
			c->addTab(n, QString());
		c->setCurrent(current);
		c->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		return c;
	}

	/* Which vertical canvas a recording or Backtrack keeps. Shown only when there is more
	   than one; a canvas that has gone shows as the main one. */
	svl::TabBar *canvas_choice(QVBoxLayout *pv, const QString &title, const QString &note, const std::string &saved)
	{
		if (!sv_canvas_running())
			return nullptr;
		if (fileCanvases.empty())
			fileCanvases = sv_canvases();
		if (fileCanvases.size() < 2)
			return nullptr;
		int at = 0;
		QStringList names;
		for (size_t i = 0; i < fileCanvases.size(); i++) {
			names << (fileCanvases[i].main ? QString(T("DestCanvasMain")) : qs(fileCanvases[i].name));
			if (!fileCanvases[i].main && fileCanvases[i].uuid == saved)
				at = (int)i;
		}
		pv->addLayout(field_row(title, note));
		svl::TabBar *c = choice(names, at);
		for (size_t i = 0; i < fileCanvases.size(); i++)
			c->setTabHue((int)i, canvas_hue(fileCanvases[i], (int)i), true);
		c->onPick = [c](int i) { c->setCurrent(i); };
		pv->addWidget(c, 0, Qt::AlignLeft);
		return c;
	}

	/* What Record records: a switch for each vertical canvas in its own colour, then OBS's own
	   horizontal picture in gold. Each one ticked becomes a file of its own. At least one stays on. */
	void record_choice(QVBoxLayout *pv, const SvOutSettings &now)
	{
		if (!sv_canvas_running())
			return;
		if (fileCanvases.empty())
			fileCanvases = sv_canvases();
		const std::vector<std::string> ticked = sv_output_record_canvases(now);
		const auto is_ticked = [&](const std::string &id) {
			return std::find(ticked.begin(), ticked.end(), id) != ticked.end();
		};
		pv->addLayout(field_row(T("SettingsRecCanvases"), T("SettingsRecCanvasesNote")));
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(18);
		grid->setVerticalSpacing(4);
		const auto add = [&](const std::string &id, const QString &name, const QColor &hue) {
			auto *sw = new svl::Switch(name);
			svl::set_hue(sw, hue);
			sw->setProperty("svStrongText", true);
			sw->setChecked(is_ticked(id));
			const int at = (int)recPicks.size();
			grid->addWidget(sw, at / 3, at % 3, Qt::AlignLeft);
			recPicks.emplace_back(id, sw);
			connect(sw, &QAbstractButton::toggled, this, [this, sw](bool on) {
				if (on)
					return;
				for (const auto &pick : recPicks)
					if (pick.second->isChecked())
						return;
				sw->setChecked(true); /* something always records */
			});
		};
		for (size_t i = 0; i < fileCanvases.size(); i++) {
			const SvVCanvas &c = fileCanvases[i];
			add(c.main ? std::string() : c.uuid, c.main ? QString(T("DestCanvasMain")) : qs(c.name),
			    canvas_hue(c, (int)i));
		}
		add(SV_WIDE, T("DestCanvasWide"), svl::YELLOW);
		grid->setColumnStretch(3, 1);
		pv->addLayout(grid);
	}

	std::string picked_canvas(svl::TabBar *c) const
	{
		const int at = c->current();
		if (at < 0 || at >= (int)fileCanvases.size() || fileCanvases[(size_t)at].main)
			return std::string();
		return fileCanvases[(size_t)at].uuid;
	}

	/* The quality choices in the order shown, as the numbers the settings keep. */
	static constexpr int REC_UI[] = {SV_Q_SAME, SV_Q_BITRATE, SV_Q_VBR, SV_Q_LEVEL};
	static constexpr int CLIP_UI[] = {SV_CLIP_AS_RECORDING, SV_CLIP_AS_STREAM, SV_CLIP_BITRATE, SV_CLIP_VBR,
					  SV_CLIP_LEVEL};
	template<size_t N> static int ui_of(const int (&order)[N], int kept)
	{
		for (size_t i = 0; i < N; i++)
			if (order[i] == kept)
				return (int)i;
		return 0;
	}

	int picked_fps() const
	{
		return fpsChoices[(size_t)std::clamp(fps->current(), 0, (int)fpsChoices.size() - 1)];
	}

	/* An amber line when the picked rate is more than OBS gives, with a key to raise OBS. */
	void show_fps()
	{
		const int whole = sv_output_obs_fps();
		const int want = picked_fps();
		const int out = std::max(1, whole / sv_output_fps_div(want));
		const bool above = want > whole;
		const bool uneven = !above && whole % want != 0;
		if (above)
			fpsWarn->setText(QString(T("SettingsFpsAbove")).arg(whole).arg(want), svl::AMBER, false);
		else if (uneven)
			fpsWarn->setText(QString(T("SettingsFpsUneven")).arg(want).arg(whole).arg(out), svl::AMBER, false);
		fpsWarn->setVisible(above || uneven);
		setObsFps->setText(QString(T("SettingsFpsSetObs")).arg(want));
		setObsFps->setVisible(above && !locked);
	}

	/* Raises OBS's own frame rate (OBS Settings, Video), only while nothing at all is running. */
	void set_obs_fps()
	{
		const int want = picked_fps();
		if (obs_video_active() || sv_output_any_running() || obs_frontend_streaming_active() ||
		    obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
		    obs_frontend_virtualcam_active()) {
			fpsWarn->setText(T("SettingsFpsBusy"), svl::AMBER, false);
			fpsWarn->setVisible(true);
			return;
		}
		config_t *cfg = obs_frontend_get_profile_config();
		if (!cfg)
			return;
		const int common[] = {10, 20, 30, 48, 60};
		if (std::find(std::begin(common), std::end(common), want) != std::end(common)) {
			config_set_uint(cfg, "Video", "FPSType", 0);
			config_set_string(cfg, "Video", "FPSCommon", std::to_string(want).c_str());
		} else {
			config_set_uint(cfg, "Video", "FPSType", 1);
			config_set_uint(cfg, "Video", "FPSInt", (uint64_t)want);
		}
		config_save_safe(cfg, "tmp", nullptr);
		obs_frontend_reset_video();
		show_fps();
		if (sv_output_obs_fps() == want) {
			fpsWarn->setText(QString(T("SettingsFpsDone")).arg(want), svl::tone(fpsHue, 0.3), false);
			fpsWarn->setVisible(true);
		}
	}

	/* The sound tracks as a routing table: a column per OBS track, a row per sound with a lit
	   crossing on each track it is on, and a key per track for whether it goes in the file.
	   Crossings change OBS's own track routing (the same as Edit, Advanced Audio Properties)
	   when Save is pressed. */
	void build_track_grid(QVBoxLayout *pv, const QColor &hue)
	{
		trackHue = hue;
		route = new SvRouteTable(hue);
		route->setInFile(now.rec_tracks);
		for (const SvSoundSource &src : sv_sound_sources())
			route->addRow(src.uuid, qs(src.name));
		route->setFixedSize(route->sizeHint());
		auto *area = new QScrollArea();
		area->setWidgetResizable(false);
		area->setFrameShape(QFrame::NoFrame);
		area->viewport()->setAutoFillBackground(false);
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		area->setWidget(route);
		/* Every sound shows when there are up to eleven; more scroll. */
		const int tall = route->height();
		const int cap = 490;
		area->setFixedHeight((std::min)(tall, cap));
		area->setFixedWidth(route->width() + (tall > cap ? 14 : 0));
		pv->addWidget(area);
		read_ticks();
	}

	/* Each crossing as OBS has it right now. */
	void read_ticks()
	{
		if (!route)
			return;
		routeBefore.assign((size_t)route->rowCount(), 0u);
		for (int i = 0; i < route->rowCount(); i++) {
			obs_source_t *src = obs_get_source_by_uuid(route->uuidAt(i).c_str());
			routeBefore[(size_t)i] = src ? obs_source_get_audio_mixers(src) : 0;
			obs_source_release(src);
			route->setMask(i, routeBefore[(size_t)i]);
		}
	}

	/* Column names and which crossings can be changed. The own mix's track belongs to it alone. */
	void show_track_heads()
	{
		if (!route)
			return;
		const SvSoundDesk d = sv_sound_desk();
		const int main = main_track();
		config_t *cfg = obs_frontend_get_profile_config();
		route->setDesk(d.own ? d.track : 0);
		route->setLocked(locked);
		for (int t = 1; t <= 6; t++) {
			QString text;
			if (d.own && d.track == t) {
				text = T("SettingsTrackHeadDesk");
			} else if (t == main) {
				text = T("SettingsTrackHeadMain");
			} else if (cfg) {
				const std::string keyName = "Track" + std::to_string(t) + "Name";
				const char *n = config_get_string(cfg, "AdvOut", keyName.c_str());
				if (n && *n)
					text = QString::fromUtf8(n);
			}
			route->setHead(t, text);
		}
	}

	/* On Save: every sound whose crossings changed goes on exactly those tracks. The own mix's
	   track is left as it is. */
	void apply_ticks()
	{
		if (!route)
			return;
		const SvSoundDesk d = sv_sound_desk();
		const uint32_t deskBit = d.own ? 1u << (d.track - 1) : 0u;
		for (int i = 0; i < route->rowCount() && i < (int)routeBefore.size(); i++) {
			const uint32_t want = route->mask(i);
			if ((want & ~deskBit) == (routeBefore[(size_t)i] & ~deskBit))
				continue;
			obs_source_t *src = obs_get_source_by_uuid(route->uuidAt(i).c_str());
			if (!src)
				continue;
			const uint32_t cur = obs_source_get_audio_mixers(src);
			const uint32_t next = (want & ~deskBit) | (cur & deskBit);
			if (next != cur)
				obs_source_set_audio_mixers(src, next);
			obs_source_release(src);
		}
	}

	void show_folder()
	{
		folderText->setPath(folder.isEmpty() ? QString(T("SettingsFolderDefault")) : QDir::toNativeSeparators(folder));
		if (clipFolderText)
			clipFolderText->setPath(clipFolder.isEmpty() ? QString(T("SettingsClipFolderDefault"))
								     : QDir::toNativeSeparators(clipFolder));
	}

	/* Fills the Picture and Stream boxes from OBS's own stream. Nothing is kept until Save. */
	void copy_main()
	{
		const SvMainEncoder m = sv_output_main_encoder();
		if (!m.found) {
			copied->setText(T("SettingsCopyNone"), svl::AMBER, false);
			copied->setVisible(true);
			return;
		}
		QStringList what;
		if (!m.encoder.empty()) {
			const int at = encoder->findData(qs(m.encoder));
			if (at >= 0) {
				encoder->setCurrentIndex(at);
				what << encoder->currentText();
			}
		}
		if (!m.preset.empty()) {
			const int at = preset->findData(qs(m.preset));
			if (at >= 0) {
				preset->setCurrentIndex(at);
				what << QString(T("SettingsCopyPreset")).arg(preset->currentText().section(':', 0, 0));
			}
		}
		keyframe->setValue(std::clamp(m.keyframe_sec, 1, 10));
		what << QString(T(m.keyframe_auto ? "SettingsCopyKeyframeAuto" : "SettingsCopyKeyframe")).arg(keyframe->value());
		if (m.bitrate >= 1000) {
			bitrate->setValue(std::clamp(m.bitrate, 1000, 20000));
			what << QString(T("SettingsCopyBitrate")).arg(bitrate->value());
		}
		copied->setText(QString(T("SettingsCopied")).arg(what.join(", ")), svl::tone(svl::CYAN, 0.3), false);
		copied->setVisible(true);
	}

	void pick_mode(SvSwitch m)
	{
		mode = m;
		for (int i = 0; i < (int)modeTiles.size(); i++)
			modeTiles[(size_t)i]->setOn(i == (int)m);
		/* None has no length or feel, and only Slide has a direction. */
		const bool moves = m != SvSwitch::Cut;
		const bool slides = m == SvSwitch::Slide;
		bool changed = false;
		/* isHidden, not isVisibleTo: the page itself is hidden while another tab is open. */
		if (lengthBox->isHidden() == moves) {
			lengthBox->setVisible(moves);
			feelBox->setVisible(moves);
			changed = true;
		}
		if (dirBox->isHidden() == slides) {
			dirBox->setVisible(slides);
			changed = true;
		}
		if (changed && isVisible())
			grow_to_fit();
		show_motion();
	}

	/* The stream's own sound is always in the file, so its switch stays on. */
	/* The track the vertical's sound comes from: the own mix's track when that is picked. */
	int main_track() const
	{
		if (soundMode && soundMode->current() == 1 && deskTrack)
			return deskTrack->currentData().toInt();
		return audio ? audio->currentData().toInt() : 1;
	}

	void lock_main_track()
	{
		if (route)
			route->setMain(main_track());
		show_track_heads();
	}

	/* The encoder that will really be used: Automatic means the first one this PC has. */
	std::string chosen_encoder() const
	{
		const std::string id = encoder->currentData().toString().toStdString();
		return id.empty() ? sv_output_auto_encoder() : id;
	}

	/* Says what the picked video format means on this PC, and offers YouTube only for HEVC or AV1. */
	void show_codec()
	{
		const int c = codec->current();
		const bool can = !sv_output_codec_encoder(chosen_encoder(), c).empty();
		if (c == SV_CODEC_H264)
			codecHint->setText(T("SettingsCodecH264"), svl::tone(fileHue, 0.3), false);
		else if (can)
			codecHint->setText(c == SV_CODEC_HEVC ? T("SettingsCodecHevc") : T("SettingsCodecAv1"),
					   svl::tone(fileHue, 0.3), false);
		else
			codecHint->setText(QString(T("SettingsCodecCannot")).arg(c == SV_CODEC_HEVC ? "HEVC" : "AV1"),
					   svl::AMBER, false);
		codecYouTubeBox->setVisible(c != SV_CODEC_H264 && can);
	}

	/* Lists the chosen encoder's presets, keeping `want` if it has it, else OBS's usual one. */
	void fill_presets(const std::string &want)
	{
		std::string usual;
		const std::vector<SvPreset> list = sv_output_presets(chosen_encoder(), &usual);
		const QSignalBlocker block(preset);
		preset->clear();
		for (const SvPreset &p : list)
			preset->addItem(qs(p.name), qs(p.id));
		int at = preset->findData(qs(want));
		if (at < 0)
			at = preset->findData(qs(usual));
		preset->setCurrentIndex(std::max(at, 0));
		presetBox->setVisible(!list.empty());
	}

	/* Shows the box for the picked quality, and a line saying what the number means. */
	void show_quality()
	{
		const QString levelName = chosen_encoder() == "obs_x264" ? T("SettingsCrf") : T("SettingsCqp");
		recMode->setTabText(3, levelName);
		clipQ->setTabText(4, levelName);
		const int rates[] = {128, 160, 192, 320};
		const int audioKbps = soundQ ? rates[std::clamp(soundQ->current(), 0, 3)] : 160;
		/* kind: 0 nothing of its own, 1 CBR, 2 VBR, 3 a quality level */
		auto one = [&](int kind, svl::NumberBox *level, svl::NumberBox *rate, SvLevelScale *scale,
			       svl::Words *hint) {
			level->setVisible(kind == 3);
			scale->setVisible(kind == 3);
			rate->setVisible(kind == 1 || kind == 2);
			const QColor ink = svl::tone(svl::hue_of(hint), 0.3);
			const QString gb = QString::number((rate->value() + audioKbps) * 3600.0 / 8.0 / 1000000.0, 'f', 1);
			if (kind == 3) {
				scale->setLevel(level->value());
				hint->setText(T("SettingsLevelHint"), ink, false);
			} else if (kind == 1) {
				hint->setText(QString(T("SettingsCbrHint")).arg(gb), ink, false);
			} else if (kind == 2) {
				hint->setText(QString(T("SettingsVbrHint")).arg(gb).arg(QLocale().toString(rate->value() * 3 / 2)), ink,
					      false);
			}
			hint->setVisible(kind != 0);
		};
		const int r = REC_UI[(size_t)std::clamp(recMode->current(), 0, 3)];
		one(r == SV_Q_BITRATE ? 1 : r == SV_Q_VBR ? 2 : r == SV_Q_LEVEL ? 3 : 0, recLevel, recBitrate, recScale,
		    recHint);
		const int c = CLIP_UI[(size_t)std::clamp(clipQ->current(), 0, 4)];
		one(c == SV_CLIP_BITRATE ? 1 : c == SV_CLIP_VBR ? 2 : c == SV_CLIP_LEVEL ? 3 : 0, clipLevel, clipBitrate,
		    clipScale, clipHint);
	}

	/* One line per sound source: its name, a mute key, its vertical volume and a meter. */
	void build_rows()
	{
		while (QLayoutItem *item = rowsGrid->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		rows.clear();
		rowIds.clear();
		const std::vector<SvSoundSource> list = sv_sound_sources();
		int r = 0;
		for (const SvSoundSource &src : list) {
			rowIds.push_back(src.uuid + "|" + src.name);
			SoundRow row;
			row.uuid = src.uuid;
			row.muted = src.muted;
			const QString name = qs(src.name);
			const QFontMetrics fm(svl::body(14));
			auto *label = say(fm.elidedText(name, Qt::ElideRight, 170), svl::INK2, 14);
			label->setFixedWidth(176);
			label->setToolTip(name);
			row.mute = key(QString(), src.muted ? "volume-x" : "volume", src.muted ? svl::RED : soundHue);
			row.mute->setToolTip(src.muted ? T("SoundUnmuteTip") : T("SoundMuteTip"));
			row.volume = new svl::NumberBox(0, 200, 5, src.volume, "%");
			row.meter = new svl::Meter();
			row.meter->setAccent(soundHue);
			rowsGrid->addWidget(label, r, 0, Qt::AlignVCenter);
			rowsGrid->addWidget(row.mute, r, 1, Qt::AlignVCenter);
			rowsGrid->addWidget(row.volume, r, 2, Qt::AlignVCenter);
			rowsGrid->addWidget(row.meter, r, 3, Qt::AlignVCenter);
			const size_t at = rows.size();
			connect(row.mute, &QPushButton::clicked, this, [this, at] {
				if (at >= rows.size())
					return;
				SoundRow &x = rows[at];
				x.muted = !x.muted;
				x.mute->setIconName(x.muted ? "volume-x" : "volume");
				x.mute->setAccent(x.muted ? svl::RED : soundHue);
				x.mute->setToolTip(x.muted ? T("SoundUnmuteTip") : T("SoundMuteTip"));
				sv_sound_set_source(x.uuid, x.volume->value(), x.muted);
			});
			row.volume->onChange = [this, at](int v) {
				if (at < rows.size())
					sv_sound_set_source(rows[at].uuid, v, rows[at].muted);
			};
			rows.push_back(row);
			r++;
		}
		if (list.empty())
			rowsGrid->addWidget(say(T("SoundNoSources"), svl::tone(soundHue, 0.3), 14), 0, 0, 1, 4);
		rowsGrid->setColumnStretch(3, 1);
		rowsGrid->setRowStretch(r + 1, 1);
	}

	/* After the mix is turned on or off or moves track: the track names and the warning. */
	void after_desk_change()
	{
		/* Turning the mix on or moving it changes which tracks the sounds are on. */
		read_ticks();
		const QSignalBlocker block(audio);
		const int keep = audio->currentIndex();
		for (int i = 1; i <= 6; i++)
			audio->setItemText(i - 1, track_label(i));
		audio->setCurrentIndex(keep);
		show_desk_warning();
		lock_main_track();
		if (isVisible())
			grow_to_fit();
	}

	/* Taller when something new shows, never wider: the buttons stay where they were. */
	void grow_to_fit()
	{
		const int want = sizeHint().height();
		if (want > height())
			resize(width(), want);
	}

	/* The own mix must not sit on a track OBS's own stream or recording uses. */
	void show_desk_warning()
	{
		const int t = deskTrack->currentData().toInt();
		const bool clash = soundMode->current() == 1 && (sv_sound_main_tracks() & (1u << (t - 1)));
		if (clash)
			deskWarn->setText(QString(T("SoundTrackClash")).arg(t), svl::AMBER, false);
		deskWarn->setVisible(clash);
	}

	/* Every canvas moves with the feel, direction and length picked right now. */
	void show_motion()
	{
		for (int i = 0; i < (int)modeTiles.size(); i++)
			modeTiles[(size_t)i]->setMotion(i, feel->current(), direction->current(), length->value());
	}

	SvOutSettings now;
	svl::NumberBox *bitrate = nullptr;
	QComboBox *audio = nullptr;
	svl::TabBar *soundQ = nullptr;
	svl::NumberBox *tries = nullptr;
	svl::NumberBox *wait = nullptr;
	svl::TabBar *size = nullptr;
	QWidget *sharpBox = nullptr;
	svl::TabBar *sharp = nullptr;
	svl::TabBar *fps = nullptr;
	std::vector<int> fpsChoices;
	svl::Key *setObsFps = nullptr;
	svl::Words *fpsWarn = nullptr;
	QColor fpsHue = svl::BLUE;
	QWidget *presetBox = nullptr;
	QComboBox *preset = nullptr;
	svl::NumberBox *keyframe = nullptr;
	svl::Key *copyMain = nullptr;
	svl::Words *copied = nullptr;
	svl::TabBar *fileType = nullptr;
	svl::TabBar *split = nullptr;
	svl::NumberBox *splitMin = nullptr;
	svl::NumberBox *splitMb = nullptr;
	svl::TabBar *recMode = nullptr;
	std::vector<SvVCanvas> fileCanvases;
	std::vector<std::pair<std::string, svl::Switch *>> recPicks;
	svl::TabBar *clipCanvas = nullptr;
	svl::NumberBox *recLevel = nullptr;
	svl::NumberBox *recBitrate = nullptr;
	svl::Words *recHint = nullptr;
	svl::TabBar *codec = nullptr;
	svl::Words *codecHint = nullptr;
	svl::Switch *codecYouTube = nullptr;
	QWidget *codecYouTubeBox = nullptr;
	QColor fileHue = svl::BLUE;
	SvLevelScale *recScale = nullptr;
	svl::NumberBox *clipLevel = nullptr;
	svl::NumberBox *clipBitrate = nullptr;
	svl::Words *clipHint = nullptr;
	SvLevelScale *clipScale = nullptr;
	/* Recording: sound tracks */
	SvRouteTable *route = nullptr;
	std::vector<uint32_t> routeBefore;
	QColor trackHue = svl::BLUE;
	/* Sound */
	struct SoundRow {
		std::string uuid;
		bool muted = false;
		svl::Key *mute = nullptr;
		svl::NumberBox *volume = nullptr;
		svl::Meter *meter = nullptr;
	};
	SvSoundDesk desk;
	std::string deskBefore;
	bool soundSaved = false;
	QColor soundHue = svl::LIME;
	svl::TabBar *soundMode = nullptr;
	QWidget *trackBox = nullptr;
	QWidget *deskBox = nullptr;
	QComboBox *deskTrack = nullptr;
	svl::NumberBox *master = nullptr;
	svl::Switch *followMute = nullptr;
	svl::Words *deskWarn = nullptr;
	QWidget *rowsHost = nullptr;
	QGridLayout *rowsGrid = nullptr;
	std::vector<SoundRow> rows;
	std::vector<std::string> rowIds;
	bool locked = false;
	svl::Switch *recGame = nullptr;
	svl::TabBar *autoBack = nullptr;
	QComboBox *encoder = nullptr;
	SvPathLine *folderText = nullptr;
	svl::Key *browse = nullptr;
	svl::Key *reset = nullptr;
	svl::NumberBox *backtrack = nullptr;
	svl::NumberBox *after = nullptr;
	svl::NumberBox *memLimit = nullptr;
	svl::TabBar *clipQ = nullptr;
	QString clipFolder;
	SvPathLine *clipFolderText = nullptr;
	svl::Key *clipBrowse = nullptr;
	svl::Key *clipReset = nullptr;
	svl::Switch *clipGame = nullptr;
	svl::TabBar *replaySlow = nullptr;
	svl::TabBar *replaySlowWhat = nullptr;
	svl::NumberBox *replaySlowLen = nullptr;
	svl::Switch *replaySound = nullptr;
	svl::NumberBox *replayLen = nullptr;
	svl::TabBar *replayTrans = nullptr;
	svl::NumberBox *replayZoom = nullptr;
	svl::Switch *replayLine = nullptr;
	svl::Switch *replayCamLive = nullptr;
	svl::Switch *replayTag = nullptr;
	svl::Switch *replayAuto = nullptr;
	svl::NumberBox *delay = nullptr;
	QString folder;
	std::vector<svl::Tile *> modeTiles;
	QWidget *lengthBox = nullptr;
	QWidget *feelBox = nullptr;
	QWidget *dirBox = nullptr;
	svl::NumberBox *length = nullptr;
	svl::TabBar *feel = nullptr;
	svl::TabBar *direction = nullptr;
	SvSwitch mode = SvSwitch::Glide;
	svl::Switch *follow = nullptr;
	svl::Switch *askLive = nullptr;
	svl::Switch *askStop = nullptr;
	svl::Switch *keepAll = nullptr;
};

/* ---------- scene links ---------- */

class SvSceneLinksDialog : public SvWindow {
public:
	explicit SvSceneLinksDialog(QWidget *parent) : SvWindow(parent, T("ScenesTitle"), "link")
	{
		/* With another app running the vertical, our own vertical scenes are not used, so
		   only layouts are offered. Links to them are kept for if the member comes back. */
		own = sv_store_route() == SV_ROUTE_OWN;
		v->addWidget(say(T(own ? "ScenesNote" : "ScenesNoteOther")));

		/* Each canvas has its own routes, so a platform's canvas can change by itself. */
		running = own && sv_canvas_running();
		if (running)
			canvases = sv_canvases();
		if (canvases.empty())
			canvases.push_back({std::string(), std::string(), true});
		multi = canvases.size() > 1;
		setMinimumSize(multi ? 960 : 800, 380);

		const std::string collection = current_collection();
		layouts = sv_link_presets();
		for (const SvVCanvas &c : canvases)
			vscenes.push_back(running ? sv_canvas_scenes(c.main ? std::string() : c.uuid) : std::vector<SvVScene>());

		if (obs_source_t *cur = obs_frontend_get_current_scene()) {
			onNow = obs_source_get_uuid(cur);
			obs_source_release(cur);
		}
		obs_frontend_source_list all = {};
		obs_frontend_get_scenes(&all);
		for (size_t i = 0; i < all.sources.num; i++)
			scenes.push_back({obs_source_get_uuid(all.sources.array[i]), obs_source_get_name(all.sources.array[i])});
		obs_frontend_source_list_free(&all);

		/* The column titles line up with the drop-downs in each route. */
		header = new QWidget();
		auto *head = new QHBoxLayout(header);
		head->setContentsMargins(10, 4, 10, 0);
		head->setSpacing(8);
		head->addWidget(sized(svl::field(T("ScenesWhen")), SCENE_W));
		head->addWidget(sized(new QWidget(), ARROW_W));
		if (multi) {
			auto *cw = new QWidget();
			auto *ch = new QHBoxLayout(cw);
			ch->setContentsMargins(0, 0, 0, 0);
			ch->setSpacing(6);
			ch->addWidget(svl::field(T("DestCanvas")));
			ch->addWidget(new svl::Info(T("ScenesCanvasTip")));
			ch->addStretch(1);
			head->addWidget(sized(cw, CANVAS_W));
			head->addWidget(sized(new QWidget(), ARROW_W));
		}
		head->addWidget(svl::field(T("RoutesTo")), 1);
		head->addWidget(sized(new QWidget(), NOW_W));
		head->addWidget(sized(new QWidget(), REMOVE_W));
		v->addWidget(header);

		area = new QScrollArea();
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		area->viewport()->setAutoFillBackground(false);
		auto *host = new QWidget();
		host->setAutoFillBackground(false);
		list = new QVBoxLayout(host);
		list->setContentsMargins(0, 0, 0, 0);
		list->setSpacing(4);
		empty = say(T("RoutesEmpty"), svl::INK3, 15);
		list->addWidget(empty);
		list->addStretch(1);
		area->setWidget(host);
		v->addWidget(area, 1);

		/* The saved links become routes, in the order of the OBS scene list. */
		std::vector<std::vector<std::string>> layoutLinks;
		for (const SvVCanvas &c : canvases)
			layoutLinks.push_back(c.main ? sv_store_list(collection, SV_STORE_SCENE_LINKS)
						     : sv_store_list(collection, sv_store_canvas_links(c.uuid).c_str()));
		const std::vector<std::string> sceneLinks = sv_store_list(collection, SV_STORE_VSCENE_LINKS);
		for (const Scene &sc : scenes) {
			const std::string prefix = sc.uuid + "|";
			for (int ci = 0; ci < (int)canvases.size(); ci++) {
				for (const std::string &link : layoutLinks[ci]) {
					if (link.rfind(prefix, 0) != 0)
						continue;
					/* A layout deleted on the website stays as a marked route, so the member sees it. */
					const std::string value = link.substr(prefix.size());
					add_route(sc.uuid, ci, qs("L:" + value), !known_layout(ci, value));
				}
				for (const std::string &link : sceneLinks) {
					if (link.rfind(prefix, 0) != 0)
						continue;
					const std::string value = link.substr(prefix.size());
					for (const SvVScene &vs : vscenes[ci])
						if (vs.uuid == value)
							add_route(sc.uuid, ci, qs("V:" + value));
				}
			}
			/* A vertical scene deleted since: which canvas it was on is gone with it, so it shows under Main. */
			if (running) {
				for (const std::string &link : sceneLinks) {
					if (link.rfind(prefix, 0) != 0)
						continue;
					const std::string value = link.substr(prefix.size());
					bool found = false;
					for (const std::vector<SvVScene> &on : vscenes)
						for (const SvVScene &vs : on)
							found = found || vs.uuid == value;
					if (!found)
						add_route(sc.uuid, 0, qs("V:" + value), true);
				}
			}
		}
		refresh();

		QHBoxLayout *buttons = button_row();
		auto *add = key(T("RoutesAdd"), "plus");
		add->setAutoDefault(false);
		buttons->addWidget(add);
		buttons->addStretch(1);
		auto *cancel = key(T("DestCancel"), "x");
		auto *save = key(T("DestSave"), "check");
		save->setDefault(true);
		buttons->addWidget(cancel);
		buttons->addWidget(save);

		/* A new route starts on the OBS scene showing now, since that is usually the one being set up. */
		connect(add, &QPushButton::clicked, this, [this] {
			if (scenes.empty())
				return;
			add_route(onNow.empty() ? scenes.front().uuid : onNow, 0, QString());
			refresh();
		});
		connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
		connect(save, &QPushButton::clicked, this, [this] {
			/* Two routes for the same scene, canvas and kind: the lower one wins. */
			std::vector<std::string> seen, mainLayouts, scenesOut;
			std::vector<std::vector<std::string>> extra(canvases.size());
			for (auto it = routes.rbegin(); it != routes.rend(); ++it) {
				const Route &r = **it;
				const std::string sc = r.scene->currentData().toString().toStdString();
				const std::string data = r.target->currentData().toString().toStdString();
				const int ci = r.canvas->currentData().toInt();
				if (sc.empty() || data.size() < 3 || ci < 0 || ci >= (int)canvases.size())
					continue;
				const std::string k = sc + "|" + std::to_string(ci) + "|" + data[0];
				if (std::find(seen.begin(), seen.end(), k) != seen.end())
					continue;
				seen.push_back(k);
				const std::string link = sc + "|" + data.substr(2);
				if (data[0] == 'V')
					scenesOut.push_back(link);
				else if (canvases[ci].main)
					mainLayouts.push_back(link);
				else
					extra[ci].push_back(link);
			}
			std::reverse(mainLayouts.begin(), mainLayouts.end());
			std::reverse(scenesOut.begin(), scenesOut.end());
			const std::string collection = current_collection();
			sv_store_set_list(collection, SV_STORE_SCENE_LINKS, mainLayouts);
			for (int ci = 0; ci < (int)canvases.size(); ci++) {
				if (canvases[ci].main)
					continue;
				std::reverse(extra[ci].begin(), extra[ci].end());
				sv_store_set_list(collection, sv_store_canvas_links(canvases[ci].uuid).c_str(), extra[ci]);
			}
			/* Only while the vertical runs are its scenes listed, so only then are their links saved. */
			if (running)
				sv_store_set_list(collection, SV_STORE_VSCENE_LINKS, scenesOut);
			sv_action_follow_scene();
			accept();
		});
	}

private:
	static constexpr int SCENE_W = 250, CANVAS_W = 160, ARROW_W = 18, NOW_W = 72, REMOVE_W = 38;

	struct Scene {
		std::string uuid;
		std::string name;
	};
	struct Route {
		QFrame *row = nullptr;
		QComboBox *scene = nullptr;
		QComboBox *canvas = nullptr; /* holds the canvas's place in the list */
		QComboBox *target = nullptr; /* "L:layout id" or "V:vertical scene id" */
		QLabel *now = nullptr;
		QString gone; /* a saved target that no longer exists, kept so the member can see and fix it */
	};

	static QWidget *sized(QWidget *w, int px)
	{
		w->setFixedWidth(px);
		return w;
	}

	bool known_layout(int ci, const std::string &value) const
	{
		if (value == (canvases[ci].main ? SV_SCENE_LINK_SITE : SV_SCENE_LINK_MAIN))
			return true;
		for (const SvLayoutChoice &c : layouts)
			if (c.id == value)
				return true;
		return false;
	}

	/* What a route can switch to depends on its canvas: that canvas's own vertical scenes,
	   and the layouts, with "Same as main" for every canvas but the main one. */
	void fill_target(Route *r, const QString &keep)
	{
		int ci = r->canvas->currentData().toInt();
		if (ci < 0 || ci >= (int)canvases.size())
			ci = 0;
		const qreal dpr = qApp->devicePixelRatio();
		const QIcon layoutIcon(svl::icon("layout", svl::BLUE, 16, dpr));
		const QIcon phoneIcon(svl::icon("canvas", svl::BLUE, 16, dpr));
		QSignalBlocker hold(r->target);
		r->target->clear();
		if (canvases[ci].main)
			r->target->addItem(layoutIcon, T("LayoutWebsite"), QString("L:") + SV_SCENE_LINK_SITE);
		else
			r->target->addItem(layoutIcon, T("LayoutSameAsMain"), QString("L:") + SV_SCENE_LINK_MAIN);
		/* A deleted target stays first, marked, until the member picks something else. */
		if (!r->gone.isEmpty() && keep == r->gone) {
			const bool layout = r->gone.startsWith("L:");
			r->target->insertItem(0, QIcon(svl::icon("alert", svl::AMBER, 16, dpr)),
					      T(layout ? "RoutesLayoutGone" : "RoutesSceneGone"), r->gone);
		}
		for (const SvLayoutChoice &c : layouts)
			r->target->addItem(layoutIcon, qs(c.name), qs("L:" + c.id));
		if (!vscenes[ci].empty()) {
			r->target->insertSeparator(r->target->count());
			for (const SvVScene &vs : vscenes[ci])
				r->target->addItem(phoneIcon, qs(vs.name), qs("V:" + vs.uuid));
		}
		int at = keep.isEmpty() ? -1 : r->target->findData(keep);
		if (at < 0)
			at = layouts.empty() ? 0 : 1;
		r->target->setCurrentIndex(at);
		mark(r);
	}

	/* A route whose target was deleted reads amber and says why it does nothing. */
	void mark(Route *r)
	{
		const QString data = r->target->currentData().toString();
		const bool gone = !r->gone.isEmpty() && data == r->gone;
		r->target->setStyleSheet(gone ? QString("QComboBox { color: rgb(245,158,11); }") : QString());
		r->target->setToolTip(gone ? QString(T(data.startsWith("L:") ? "RoutesLayoutGoneTip" : "RoutesSceneGoneTip"))
					   : QString());
	}

	/* The route for the OBS scene showing now is lit. */
	void light(Route *r)
	{
		const bool live = r->scene->currentData().toString().toStdString() == onNow;
		r->row->setStyleSheet(
			live ? QString("QFrame#svRoute { background: rgba(77,124,255,0.12); border: 0; border-radius: 10px; }")
			     : QString("QFrame#svRoute { background: transparent; border: 0; border-radius: 0;"
				       " border-top: 1px solid rgba(77,124,255,0.16); }"));
		r->now->setText(live ? QString(T("ScenesOnNow")) : QString());
	}

	void add_route(const std::string &sceneUuid, int canvasIndex, const QString &target, bool gone = false)
	{
		auto route = std::make_unique<Route>();
		Route *r = route.get();
		if (gone)
			r->gone = target;
		r->row = new QFrame();
		r->row->setObjectName("svRoute");
		auto *h = new QHBoxLayout(r->row);
		h->setContentsMargins(10, 6, 10, 6);
		h->setSpacing(8);

		r->scene = new QComboBox();
		for (const Scene &sc : scenes)
			r->scene->addItem(QString::fromUtf8(sc.name.c_str()), qs(sc.uuid));
		r->scene->setCurrentIndex(std::max(0, r->scene->findData(qs(sceneUuid))));
		r->scene->setFixedWidth(SCENE_W);

		r->canvas = new QComboBox();
		for (int i = 0; i < (int)canvases.size(); i++)
			r->canvas->addItem(canvases[i].main ? QString(T("DestCanvasMain")) : qs(canvases[i].name), i);
		r->canvas->setCurrentIndex(std::max(0, r->canvas->findData(canvasIndex)));
		r->canvas->setFixedWidth(CANVAS_W);

		r->target = new QComboBox();
		r->target->setIconSize(QSize(16, 16));
		r->target->setMinimumWidth(240);
		r->target->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		r->now = new QLabel();
		r->now->setFixedWidth(NOW_W);
		r->now->setStyleSheet(QString("QLabel { color: rgb(77,124,255); font-family: '%1'; font-size: 15px; font-weight: 700; }")
					      .arg(svl::body(15).family()));

		auto *remove = key(QString(), "trash");
		remove->setAutoDefault(false);
		remove->setFixedSize(REMOVE_W, 36);
		remove->setToolTip(T("RoutesRemove"));

		h->addWidget(r->scene);
		h->addWidget(icon_label("arrow-right", svl::BLUE, ARROW_W));
		if (multi) {
			h->addWidget(r->canvas);
			h->addWidget(icon_label("arrow-right", svl::BLUE, ARROW_W));
		} else {
			r->canvas->setParent(r->row);
			r->canvas->hide();
		}
		h->addWidget(r->target, 1);
		h->addWidget(r->now);
		h->addWidget(remove);

		fill_target(r, target);
		light(r);
		connect(r->canvas, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this, r] { fill_target(r, r->target->currentData().toString()); });
		connect(r->scene, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, r] { light(r); });
		connect(r->target, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, r] { mark(r); });
		connect(remove, &QPushButton::clicked, this, [this, r] {
			for (auto it = routes.begin(); it != routes.end(); ++it) {
				if (it->get() == r) {
					r->row->deleteLater();
					routes.erase(it);
					break;
				}
			}
			refresh();
		});

		list->insertWidget((int)routes.size(), r->row);
		routes.push_back(std::move(route));
	}

	/* Column titles only over routes; with none, a line says what routes are for. The
	   window opens tall enough for every route, so it only scrolls with a long list. */
	void refresh()
	{
		header->setVisible(!routes.empty());
		empty->setVisible(routes.empty());
		area->setMinimumHeight(std::clamp(56 * (int)routes.size() + 12, 120, 520));
		/* An open window grows with a new route instead of sliding it under the buttons. */
		if (isVisible()) {
			v->invalidate();
			setMinimumHeight(std::max(380, v->totalMinimumSize().height()));
			if (height() < minimumHeight())
				resize(width(), minimumHeight());
		}
	}

	bool own = true;
	bool running = false;
	bool multi = false;
	std::vector<SvVCanvas> canvases;
	std::vector<std::vector<SvVScene>> vscenes;
	std::vector<SvLayoutChoice> layouts;
	std::vector<Scene> scenes;
	std::string onNow;
	std::vector<std::unique_ptr<Route>> routes;
	QWidget *header = nullptr;
	QWidget *empty = nullptr;
	QScrollArea *area = nullptr;
	QVBoxLayout *list = nullptr;
};

/* The setup window's screen showing now, empty while it is closed (SvWelcome sets it). */
std::string g_setupStep;

/* Tells the website's setup guide how far the OBS setup has got, so it ticks itself
   off (sv_link_report_setup only sends a change). `liveNow`: a platform is live now,
   remembered for good so "Go live" stays ticked. */
void report_setup(bool liveNow)
{
	if (liveNow && sv_store_int("went_live", 0) == 0)
		sv_store_set_int("went_live", 1);
	const SvPicks picks = sv_store_current_picks();
	SvSetupProgress p;
	p.game = !picks.game.empty();
	p.camera = !picks.cam.empty();
	p.finished = sv_store_welcome_done();
	p.platforms = (int)sv_output_dests().size();
	p.otherApp = sv_store_route() == SV_ROUTE_OTHER;
	p.wentLive = sv_store_int("went_live", 0) != 0;
	p.step = g_setupStep;
	sv_link_report_setup(p);
}

/* ---------- one platform in the Go live card ---------- */

class DestRow : public QWidget {
public:
	DestRow(const SvDest &d, bool firstRow) : id(d.id), first(firstRow)
	{
		auto *g = new QGridLayout(this);
		g->setContentsMargins(0, 8, 0, 8);
		g->setHorizontalSpacing(9);
		g->setVerticalSpacing(5);
		well = new svl::Well(d.platform);
		auto *col = new QVBoxLayout();
		col->setSpacing(1);
		col->setContentsMargins(0, 0, 0, 0);
		name = new svl::Words(nullptr, 13);
		name->setSegs({{qs(d.name), Qt::white, false, true}});
		state = new svl::Words(nullptr, 12);
		col->addWidget(name);
		col->addWidget(state);
		auto *keys = new QHBoxLayout();
		keys->setSpacing(5);
		keys->setContentsMargins(0, 0, 0, 0);
		action = key(T("GoLive"));
		edit = key(QString(), "pencil");
		edit->setToolTip(T("Edit"));
		/* On: Go live on all takes this platform along. Off: only its own key starts it. */
		onSwitch = new svl::Switch(QString());
		onSwitch->setChecked(d.on);
		onSwitch->setToolTip(T("DestOnTip"));
		on = d.on;
		keys->addWidget(onSwitch);
		keys->addWidget(action);
		keys->addWidget(edit);
		spark = new svl::Spark();
		spark->hide();
		g->addWidget(well, 0, 0, Qt::AlignVCenter);
		g->addLayout(col, 0, 1);
		g->addLayout(keys, 0, 2, Qt::AlignVCenter);
		g->addWidget(spark, 1, 1, 1, 2);
		g->setColumnStretch(1, 1);
	}

	void show_info(const SvRunInfo &info, int delay)
	{
		QColor washNow;
		const QString clock = clock_text(info.seconds);
		switch (info.state) {
		case SvRunState::Off:
			if (!info.problem.empty())
				state->setText(qs(info.problem), PROBLEM);
			else if (!on)
				state->setText(T("StateSwitchedOff"), svl::tone(svl::BLUE, 0.45));
			else
				state->setText(T("StateReady"), svl::INK3);
			action->setText(T("GoLive"));
			action->setIconName(QString());
			action->setAccent(svl::BLUE);
			spark->hide();
			break;
		case SvRunState::Starting:
			state->setText(delay > 0 ? QString(T("StateDelayed")).arg(delay) : QString(T("StateStarting")),
				       svl::tone(svl::BLUE, 0.35), true);
			action->setText(T("Stop"));
			action->setAccent(svl::RED);
			spark->hide();
			break;
		case SvRunState::Live: {
			std::vector<svl::Seg> segs;
			segs.push_back({QString(T("StateLiveClock")).arg(clock), RED_INK, true, true});
			QColor line = svl::GREEN;
			if (info.health == SvHealth::Steady) {
				segs.push_back({T("HealthSteady"), GREEN_INK, true, false});
			} else if (info.health == SvHealth::Shaky) {
				const QString text = info.low_bitrate && info.dropped == 0
							     ? QString(T("HealthSlow"))
							     : QString(T("HealthShaky")).arg(info.dropped);
				segs.push_back({text, AMBER_INK, true, false});
				line = svl::AMBER;
				washNow = svl::AMBER;
			} else {
				segs.push_back({QString(T("HealthDropping")).arg(info.dropped), PROBLEM, true, false});
				line = svl::RED;
				washNow = svl::RED;
			}
			if (info.kbps > 0)
				segs.push_back({QString(T("Mbps")).arg(info.kbps / 1000.0, 0, 'f', 1), svl::INK3, false, false});
			state->setSegs(segs);
			spark->setValues(info.history, line);
			spark->setVisible(info.history.size() >= 2);
			action->setText(T("Stop"));
			action->setAccent(svl::RED);
			break;
		}
		case SvRunState::Reconnecting:
			state->setText(info.reconnect_left > 0 ? QString(T("StateReconnectingIn")).arg(info.reconnect_left)
							       : QString(T("StateReconnecting")),
				       AMBER_INK, true);
			action->setText(T("DestCancel"));
			action->setAccent(svl::AMBER);
			spark->setVisible(info.history.size() >= 2);
			spark->setValues(info.history, svl::AMBER);
			washNow = svl::AMBER;
			break;
		case SvRunState::Stopping:
			state->setText(T("StateStopping"), svl::INK3);
			action->setText(T("Stop"));
			action->setAccent(svl::RED);
			break;
		}
		action->setEnabled(info.state != SvRunState::Stopping);
		edit->setVisible(info.state == SvRunState::Off);
		edit->setEnabled(info.state == SvRunState::Off);
		action->setToolTip(info.state == SvRunState::Live ? QString(T("HealthTip")) : QString());
		if (washNow != wash) {
			wash = washNow;
			update();
		}
	}

	/* With more than one canvas, the row says which canvas the platform shows, in that
	   canvas's colour, with a line of the same colour down its left edge. */
	void show_canvas(const QString &platformName, const DestCanvas &c, bool several)
	{
		std::vector<svl::Seg> segs = {{platformName, Qt::white, false, true}};
		if (several)
			segs.push_back({"  " + c.name, c.hue, false, true}); /* plain coloured words: a glow here read as a badge */
		name->setSegs(segs);
		const QColor hueNow = several ? c.hue : QColor();
		if (hueNow != hue) {
			hue = hueNow;
			auto *g = static_cast<QGridLayout *>(layout());
			g->setContentsMargins(several ? 10 : 0, 8, 0, 8);
			update();
		}
	}

	std::string id;
	svl::Key *action = nullptr;
	svl::Key *edit = nullptr;
	svl::Switch *onSwitch = nullptr;
	bool on = true;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (hue.isValid()) {
			/* The canvas line: soft glow behind, the solid colour on top. */
			const QRectF bar(1, 9, 3, height() - 18);
			p.setPen(Qt::NoPen);
			p.setBrush(svl::alpha(hue, 0.22));
			p.drawRoundedRect(bar.adjusted(-1.5, -1.5, 1.5, 1.5), 3, 3);
			p.setBrush(hue);
			p.drawRoundedRect(bar, 1.5, 1.5);
		}
		if (wash.isValid()) {
			/* The row warms up when a stream struggles. */
			QLinearGradient g(QPointF(0, 0), QPointF(width(), 0));
			g.setColorAt(0, svl::alpha(wash, 0.13));
			g.setColorAt(0.7, svl::alpha(wash, 0.0));
			p.fillRect(rect().adjusted(-4, 1, 0, -1), g);
		}
		if (!first) {
			p.setPen(QPen(svl::alpha(svl::BLUE, 0.10), 1));
			p.drawLine(QPointF(0, 0.5), QPointF(width(), 0.5));
		}
	}

private:
	bool first;
	QColor wash;
	QColor hue;
	svl::Well *well = nullptr;
	svl::Words *name = nullptr;
	svl::Words *state = nullptr;
	svl::Spark *spark = nullptr;
};

/* ---------- the panel ---------- */

void show_welcome();
void reveal_dock();

/* Scrolls up and down only, and keeps the panel at least as wide as its widest
   row, so nothing is ever cut off at the right edge. */
/* ---------- editing the vertical scene ---------- */

bool name_free(const std::string &name)
{
	obs_source_t *clash = obs_get_source_by_name(name.c_str());
	obs_source_release(clash);
	return !name.empty() && !clash;
}

std::string unique_source_name(const std::string &base)
{
	if (name_free(base))
		return base;
	for (int n = 2; n < 1000; n++) {
		const std::string name = base + " " + std::to_string(n);
		if (name_free(name))
			return name;
	}
	return base;
}

/* Puts a source on the vertical scene that is up now, selected, top layer. */
void add_to_vertical(obs_source_t *src)
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	if (!scene || !src) {
		obs_scene_release(scene);
		return;
	}
	obs_sceneitem_t *item = obs_scene_add(scene, src);
	if (item)
		SvEdit::select_only(scene, item);
	obs_scene_release(scene);
}

/* Where a replay plays: on the whole canvas, or inside a Replay Window the member places
   like any source. The window is our own source (sv-replay.cpp), found in groups too. */
constexpr const char *REPLAY_WINDOW_ID = "simply_vertical_replay_window";

static bool scan_replay_windows(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *found = static_cast<std::vector<obs_sceneitem_t *> *>(param);
	if (obs_sceneitem_is_group(item)) {
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), scan_replay_windows, param);
		return true;
	}
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && std::strcmp(obs_source_get_id(src), REPLAY_WINDOW_ID) == 0)
		found->push_back(item);
	return true;
}

/* True when a replay on the vertical scene that is up now plays in a box. */
bool replay_in_box()
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	if (!scene)
		return false;
	std::vector<obs_sceneitem_t *> found;
	obs_scene_enum_items(scene, scan_replay_windows, &found);
	bool on = false;
	for (obs_sceneitem_t *item : found)
		on = on || obs_sceneitem_visible(item);
	obs_scene_release(scene);
	return on;
}

/* In a box: shows the Replay Window (adding one, centred, the first time) and selects it so
   its handles are on the canvas, ready to move and resize. Whole canvas: hides it, keeping
   its place for next time. */
void set_replay_in_box(bool on)
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	if (!scene)
		return;
	std::vector<obs_sceneitem_t *> found;
	obs_scene_enum_items(scene, scan_replay_windows, &found);
	if (!on) {
		for (obs_sceneitem_t *item : found)
			obs_sceneitem_set_visible(item, false);
	} else if (!found.empty()) {
		obs_sceneitem_set_visible(found.back(), true);
		SvEdit::select_only(scene, found.back());
	} else if (obs_source_t *src = obs_source_create(REPLAY_WINDOW_ID, unique_source_name(T("ReplayWindowName")).c_str(),
							 nullptr, nullptr)) {
		if (obs_sceneitem_t *item = obs_scene_add(scene, src)) {
			obs_source_t *sceneSrc = obs_scene_get_source(scene);
			const float cw = obs_source_get_width(sceneSrc) ? (float)obs_source_get_width(sceneSrc) : 1080.0f;
			const float ch = obs_source_get_height(sceneSrc) ? (float)obs_source_get_height(sceneSrc) : 1920.0f;
			const float sw = obs_source_get_width(src) ? (float)obs_source_get_width(src) : 1080.0f;
			const float sh = obs_source_get_height(src) ? (float)obs_source_get_height(src) : 1080.0f;
			/* Three quarters of the canvas's width, so it reads as a box to place. */
			const float k = std::min(cw / sw, ch / sh) * 0.75f;
			vec2 scale, pos;
			vec2_set(&scale, k, k);
			vec2_set(&pos, (cw - sw * k) / 2, (ch - sh * k) / 2);
			obs_sceneitem_set_scale(item, &scale);
			obs_sceneitem_set_pos(item, &pos);
			SvEdit::select_only(scene, item);
		}
		obs_source_release(src);
	}
	obs_scene_release(scene);
}

/* The Add menu: every kind of source OBS offers, then sources and scenes the
   member already has, so the same webcam or alert box can be reused. */
/* Our menus pop up over OBS, which gives them its own small words and a flat box. Give them
   ours: our words on a lit card. */
static void menu_look(QMenu *menu, const QColor &accent = svl::BLUE)
{
	menu->setFont(svl::body(14.5, QFont::Medium));
	svl::lit_menu(menu, accent);
}

void fill_add_menu(QMenu *menu, QWidget *parent)
{
	struct Kind {
		QString name;
		std::string id;
	};
	std::vector<Kind> kinds;
	const char *id = nullptr;
	const char *unversioned = nullptr;
	for (size_t i = 0; obs_enum_input_types2(i, &id, &unversioned); i++) {
		const uint32_t flags = obs_get_source_output_flags(id);
		/* Sound-only sources do nothing on the vertical picture, so they are left out. */
		if (flags & (OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DEPRECATED) || !(flags & OBS_SOURCE_VIDEO))
			continue;
		const char *name = obs_source_get_display_name(id);
		if (!name)
			continue;
		kinds.push_back({QString::fromUtf8(name), id});
	}
	std::sort(kinds.begin(), kinds.end(),
		  [](const Kind &a, const Kind &b) { return a.name.localeAwareCompare(b.name) < 0; });

	for (const Kind &k : kinds) {
		const std::string kindId = k.id;
		const QString kindName = k.name;
		menu->addAction(kindName, parent, [parent, kindId, kindName] {
			bool ok = false;
			const QString picked = QInputDialog::getText(
				parent, T("AddSourceTitle"), T("AddSourceName"), QLineEdit::Normal,
				QString::fromStdString(unique_source_name(kindName.toStdString())), &ok);
			if (!ok)
				return;
			const std::string name = picked.trimmed().toStdString();
			if (!name_free(name)) {
				QMessageBox::information(parent, T("AddSourceTitle"), T("NameTaken"));
				return;
			}
			obs_source_t *src = obs_source_create(kindId.c_str(), name.c_str(), nullptr, nullptr);
			if (!src)
				return;
			add_to_vertical(src);
			obs_frontend_open_source_properties(src);
			obs_source_release(src);
		});
	}

	menu->addSeparator();
	QMenu *existing = menu->addMenu(T("AddExisting"));
	std::vector<obs_source_t *> have;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO)
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(s));
			return true;
		},
		&have);
	std::sort(have.begin(), have.end(), [](obs_source_t *a, obs_source_t *b) {
		return QString::fromUtf8(obs_source_get_name(a)).localeAwareCompare(obs_source_get_name(b)) < 0;
	});
	for (obs_source_t *s : have) {
		const std::string uuid = obs_source_get_uuid(s);
		existing->addAction(QString::fromUtf8(obs_source_get_name(s)), parent, [uuid] {
			obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
			add_to_vertical(src);
			obs_source_release(src);
		});
		obs_source_release(s);
	}
	if (have.empty())
		existing->setEnabled(false);

	QMenu *scenes = menu->addMenu(T("AddMainScene"));
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *s = list.sources.array[i];
		const std::string uuid = obs_source_get_uuid(s);
		scenes->addAction(QString::fromUtf8(obs_source_get_name(s)), parent, [uuid] {
			obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
			add_to_vertical(src);
			obs_source_release(src);
		});
	}
	if (list.sources.num == 0)
		scenes->setEnabled(false);
	obs_frontend_source_list_free(&list);
}

/* Keeps a layer's middle where it was while its size or turn changes. */
void keep_centre(obs_sceneitem_t *item, const std::function<void()> &change)
{
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	vec3 before;
	vec3_set(&before, 0.5f, 0.5f, 0.0f);
	vec3_transform(&before, &before, &box);
	change();
	obs_sceneitem_get_box_transform(item, &box);
	vec3 after;
	vec3_set(&after, 0.5f, 0.5f, 0.0f);
	vec3_transform(&after, &after, &box);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += before.x - after.x;
	pos.y += before.y - after.y;
	obs_sceneitem_set_pos(item, &pos);
}

enum class Fix { Reset, Fit, Stretch, Centre, FlipH, FlipV, RotRight, RotLeft };

void fix_selected(SvEdit &edit, Fix how)
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	if (!scene)
		return;
	edit.remember(scene);
	for (obs_sceneitem_t *item : SvEdit::selected(scene)) {
		if (obs_sceneitem_locked(item)) {
			obs_sceneitem_release(item);
			continue;
		}
		obs_sceneitem_defer_update_begin(item);
		switch (how) {
		case Fix::Reset:
		case Fix::Fit:
		case Fix::Stretch: {
			obs_transform_info info = {};
			vec2_set(&info.scale, 1.0f, 1.0f);
			info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
			info.bounds_type = how == Fix::Fit       ? OBS_BOUNDS_SCALE_INNER
					   : how == Fix::Stretch ? OBS_BOUNDS_STRETCH
								 : OBS_BOUNDS_NONE;
			info.bounds_alignment = OBS_ALIGN_CENTER;
			if (how != Fix::Reset)
				vec2_set(&info.bounds, 1080.0f, 1920.0f);
			info.crop_to_bounds = false;
			obs_sceneitem_set_info2(item, &info);
			if (how == Fix::Reset) {
				obs_sceneitem_crop none = {};
				obs_sceneitem_set_crop(item, &none);
			}
			break;
		}
		case Fix::Centre: {
			matrix4 box;
			obs_sceneitem_get_box_transform(item, &box);
			vec3 mid;
			vec3_set(&mid, 0.5f, 0.5f, 0.0f);
			vec3_transform(&mid, &mid, &box);
			vec2 pos;
			obs_sceneitem_get_pos(item, &pos);
			pos.x += 540.0f - mid.x;
			pos.y += 960.0f - mid.y;
			obs_sceneitem_set_pos(item, &pos);
			break;
		}
		case Fix::FlipH:
		case Fix::FlipV:
			keep_centre(item, [item, how] {
				vec2 sc;
				obs_sceneitem_get_scale(item, &sc);
				if (how == Fix::FlipH)
					sc.x = -sc.x;
				else
					sc.y = -sc.y;
				obs_sceneitem_set_scale(item, &sc);
			});
			break;
		case Fix::RotRight:
		case Fix::RotLeft:
			keep_centre(item, [item, how] {
				float r = obs_sceneitem_get_rot(item) + (how == Fix::RotRight ? 90.0f : -90.0f);
				while (r >= 360.0f)
					r -= 360.0f;
				while (r < 0.0f)
					r += 360.0f;
				obs_sceneitem_set_rot(item, r);
			});
			break;
		}
		obs_sceneitem_defer_update_end(item);
		obs_sceneitem_release(item);
	}
	obs_scene_release(scene);
}

void order_selected(obs_order_movement how)
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	std::vector<obs_sceneitem_t *> items = SvEdit::selected(scene);
	/* Moving down goes bottom first, so a group of layers keeps its own order. */
	if (how == OBS_ORDER_MOVE_DOWN || how == OBS_ORDER_MOVE_BOTTOM)
		std::reverse(items.begin(), items.end());
	for (obs_sceneitem_t *item : items) {
		obs_sceneitem_set_order(item, how);
		obs_sceneitem_release(item);
	}
	obs_scene_release(scene);
}

void remove_selected(QWidget *parent)
{
	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	std::vector<obs_sceneitem_t *> items = SvEdit::selected(scene);
	obs_scene_release(scene);
	if (items.empty())
		return;
	const QString what = items.size() == 1
				     ? QString(T("RemoveOne")).arg(obs_source_get_name(obs_sceneitem_get_source(items[0])))
				     : QString(T("RemoveMany")).arg(items.size());
	if (QMessageBox::question(parent, T("RemoveTitle"), what) == QMessageBox::Yes) {
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_remove(item);
	}
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
}

void rename_source(QWidget *parent, obs_source_t *src)
{
	if (!src)
		return;
	bool ok = false;
	const QString picked = QInputDialog::getText(parent, T("RenameTitle"), T("AddSourceName"), QLineEdit::Normal,
						     QString::fromUtf8(obs_source_get_name(src)), &ok);
	const std::string name = picked.trimmed().toStdString();
	if (!ok || name == obs_source_get_name(src))
		return;
	if (!name_free(name)) {
		QMessageBox::information(parent, T("RenameTitle"), T("NameTaken"));
		return;
	}
	obs_source_set_name(src, name.c_str());
}

/* The right-click menu on the picture and on the Sources list, like OBS's. */
void fill_item_menu(QMenu *menu, QWidget *parent, SvEdit &edit)
{
	const qreal dpr = parent->devicePixelRatioF();
	QMenu *add = menu->addMenu(QIcon(svl::icon("plus", svl::BLUE, 16, dpr)), T("AddSource"));
	fill_add_menu(add, parent);

	obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
	std::vector<obs_sceneitem_t *> items = SvEdit::selected(scene);
	obs_scene_release(scene);
	if (items.empty())
		return;
	obs_sceneitem_t *first = items[0];
	obs_source_t *src = obs_sceneitem_get_source(first);
	const std::string uuid = obs_source_get_uuid(src);
	const bool one = items.size() == 1;
	const bool visible = obs_sceneitem_visible(first);
	const bool locked = obs_sceneitem_locked(first);
	const int64_t itemId = obs_sceneitem_get_id(first);
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);

	auto with_source = [uuid](void (*fn)(obs_source_t *)) {
		return [uuid, fn] {
			obs_source_t *s = obs_get_source_by_uuid(uuid.c_str());
			if (s)
				fn(s);
			obs_source_release(s);
		};
	};

	menu->addSeparator();
	if (one) {
		menu->addAction(QIcon(svl::icon("settings", svl::BLUE, 16, dpr)), T("Properties"), parent,
				with_source(obs_frontend_open_source_properties));
		menu->addAction(QIcon(svl::icon("filter", svl::BLUE, 16, dpr)), T("Filters"), parent,
				with_source(obs_frontend_open_source_filters));
		menu->addAction(QIcon(svl::icon("pencil", svl::BLUE, 16, dpr)), T("Rename"), parent, [parent, uuid] {
			obs_source_t *s = obs_get_source_by_uuid(uuid.c_str());
			rename_source(parent, s);
			obs_source_release(s);
		});
	}

	QMenu *tf = menu->addMenu(T("Transform"));
	if (one) {
		tf->addAction(T("EditTransform"), parent, [itemId] {
			obs_scene_t *s = sv_canvas_scene_ref(sv_canvas_view());
			obs_sceneitem_t *it = s ? obs_scene_find_sceneitem_by_id(s, itemId) : nullptr;
			if (it)
				obs_frontend_open_sceneitem_edit_transform(it);
			obs_scene_release(s);
		});
		tf->addSeparator();
	}
	tf->addAction(T("FitScreen"), parent, [&edit] { fix_selected(edit, Fix::Fit); });
	tf->addAction(T("StretchScreen"), parent, [&edit] { fix_selected(edit, Fix::Stretch); });
	tf->addAction(T("CentreScreen"), parent, [&edit] { fix_selected(edit, Fix::Centre); });
	tf->addSeparator();
	tf->addAction(T("FlipH"), parent, [&edit] { fix_selected(edit, Fix::FlipH); });
	tf->addAction(T("FlipV"), parent, [&edit] { fix_selected(edit, Fix::FlipV); });
	tf->addAction(T("RotRight"), parent, [&edit] { fix_selected(edit, Fix::RotRight); });
	tf->addAction(T("RotLeft"), parent, [&edit] { fix_selected(edit, Fix::RotLeft); });
	tf->addSeparator();
	tf->addAction(T("ResetTransform"), parent, [&edit] { fix_selected(edit, Fix::Reset); });

	QMenu *order = menu->addMenu(T("Order"));
	order->addAction(QIcon(svl::icon("arrow-up", svl::BLUE, 16, dpr)), T("MoveUp"), parent,
			 [] { order_selected(OBS_ORDER_MOVE_UP); });
	order->addAction(QIcon(svl::icon("arrow-down", svl::BLUE, 16, dpr)), T("MoveDown"), parent,
			 [] { order_selected(OBS_ORDER_MOVE_DOWN); });
	order->addAction(T("MoveTop"), parent, [] { order_selected(OBS_ORDER_MOVE_TOP); });
	order->addAction(T("MoveBottom"), parent, [] { order_selected(OBS_ORDER_MOVE_BOTTOM); });

	menu->addSeparator();
	menu->addAction(QIcon(svl::icon(visible ? "eye-off" : "eye", svl::BLUE, 16, dpr)), visible ? T("Hide") : T("Show"),
			parent, [visible] {
				obs_scene_t *s = sv_canvas_scene_ref(sv_canvas_view());
				for (obs_sceneitem_t *it : SvEdit::selected(s)) {
					obs_sceneitem_set_visible(it, !visible);
					obs_sceneitem_release(it);
				}
				obs_scene_release(s);
			});
	menu->addAction(QIcon(svl::icon(locked ? "unlock" : "lock", svl::BLUE, 16, dpr)), locked ? T("Unlock") : T("Lock"),
			parent, [locked] {
				obs_scene_t *s = sv_canvas_scene_ref(sv_canvas_view());
				for (obs_sceneitem_t *it : SvEdit::selected(s)) {
					obs_sceneitem_set_locked(it, !locked);
					obs_sceneitem_release(it);
				}
				obs_scene_release(s);
			});
	menu->addSeparator();
	menu->addAction(QIcon(svl::icon("trash", svl::AMBER, 16, dpr)), T("Remove"), parent,
			[parent] { remove_selected(parent); });
}

/* A small flat icon button for list rows. */
QToolButton *row_button(const QString &iconName, const QString &tip)
{
	auto *b = new QToolButton();
	b->setAutoRaise(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setToolTip(tip);
	b->setIcon(QIcon(svl::icon(iconName, svl::BLUE, 17, qApp->devicePixelRatio())));
	b->setIconSize(QSize(17, 17));
	b->setFixedSize(28, 26);
	b->setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 5px; }"
			 "QToolButton:hover { background: rgba(77,124,255,0.18); }");
	return b;
}

/* A small row of icon keys that sits at the right of a heading. Each key says what it does on hover. */
class Toolbar : public QWidget {
public:
	Toolbar()
	{
		h = new QHBoxLayout(this);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(0);
	}
	svl::Key *add(const char *iconName, const QString &tip, const QColor &accent = svl::BLUE)
	{
		svl::Key *k = key(QString(), QString::fromUtf8(iconName), accent);
		k->setToolTip(tip);
		h->addWidget(k);
		return k;
	}
	void add(svl::Key *k) { h->addWidget(k); }

private:
	QHBoxLayout *h = nullptr;
};

/* A heading inside a tab with its tools at the right. */
QWidget *heading_row(const QString &text, QWidget *tools)
{
	auto *row = new QWidget();
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(2, 0, 0, 0);
	h->setSpacing(6);
	h->addWidget(new Title(text, 14), 1, Qt::AlignVCenter);
	h->addWidget(tools, 0, Qt::AlignVCenter);
	return row;
}

/* The icon for a kind of source, so a row says what it is at a glance. */
static const char *source_kind_icon(const std::string &kind)
{
	if (kind == "game_capture")
		return "gamepad";
	if (kind == "window_capture" || kind == "monitor_capture" || kind == "display_capture")
		return "monitor";
	if (kind == "dshow_input" || kind == "av_capture_input" || kind == "v4l2_input")
		return "webcam";
	if (kind == "browser_source")
		return "globe";
	if (kind == "image_source" || kind == "slideshow")
		return "image";
	if (kind.rfind("text_", 0) == 0)
		return "type";
	if (kind == "ffmpeg_source" || kind == "vlc_source")
		return "play";
	if (kind.find("wasapi") != std::string::npos || kind.find("audio") != std::string::npos)
		return "volume";
	if (kind.rfind("simply_vertical", 0) == 0)
		return "canvas";
	return "layers";
}

/* Each kind of source lights in its own colour, far apart on the wheel. */
static QColor source_kind_colour(const std::string &kind)
{
	const std::string i = source_kind_icon(kind);
	if (i == "gamepad")
		return svl::VIOLET;
	if (i == "monitor")
		return svl::YELLOW;
	if (i == "webcam")
		return svl::PINK;
	if (i == "globe")
		return svl::CYAN;
	if (i == "image")
		return svl::ORANGE;
	if (i == "type")
		return svl::LIME;
	if (i == "play")
		return svl::AMBER;
	if (i == "volume")
		return svl::EMERALD;
	if (i == "canvas")
		return svl::BLUE;
	return svl::INK2;
}

/* A small coloured word naming one row of the scenes area: Canvas, Scene or Sources.
   All three are as wide as the widest, so the keys after them line up. */
/* A thin glowing line that splits the Canvas, Scene and Sources rows into clear sections:
   bright under the row word, fading out to the right, like the lamp line under the header. */
class RowRule : public QWidget {
public:
	/* tall: the room the line sits in the middle of, so the gaps above and below it match. */
	explicit RowRule(const QColor &c, int tall = 15) : colour(c)
	{
		setFixedHeight(tall);
		setAttribute(Qt::WA_TranslucentBackground);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		/* A touch above the middle: the keys' shadows make the row above look further away.
		   A quiet 1px line, brightest under the row word and fading out to the right, so it
		   splits the rows without shouting over them (Nick, 2026-11-21: the glowing bars were a mess). */
		const qreal y = std::floor(height() / 2.0 - 2) + 0.5;
		QLinearGradient g(QPointF(0, y), QPointF(width(), y));
		g.setColorAt(0, svl::alpha(colour, 0.0));
		g.setColorAt(0.04, svl::alpha(colour, 0.45));
		g.setColorAt(0.5, svl::alpha(colour, 0.14));
		g.setColorAt(1, svl::alpha(colour, 0.0));
		p.fillRect(QRectF(0, y - 0.5, width(), 1), g);
	}

private:
	QColor colour;
};

static void colour_row_label(QLabel *l, const QColor &colour)
{
	/* The size goes in the style too, or the panel's own label style wins over setFont. */
	const QFont f = svl::head(16);
	l->setStyleSheet(QString("color: %1; background: transparent; font-family: '%2'; font-size: %3px; font-weight: 900;")
				 .arg(svl::mix(colour, QColor(Qt::white), 0.82).name(), f.family())
				 .arg(f.pixelSize()));
	auto *glow = dynamic_cast<QGraphicsDropShadowEffect *>(l->graphicsEffect());
	if (!glow) {
		glow = new QGraphicsDropShadowEffect(l);
		glow->setOffset(0, 0);
		glow->setBlurRadius(14);
		l->setGraphicsEffect(glow);
	}
	glow->setColor(svl::alpha(colour, 0.9));
}

/* The arrow at the right end of a row that folds it to its word. */
static QToolButton *fold_button()
{
	QToolButton *b = row_button("chevron", QString());
	b->setFixedSize(28, 36);
	return b;
}

static QLabel *row_label(const QString &text, const QColor &colour)
{
	auto *l = new QLabel(text);
	l->setFont(svl::head(16));
	colour_row_label(l, colour);
	const QFontMetrics fm(l->font());
	int w = 0;
	for (const char *k : {"RowCanvas", "RowScene", "RowSources"})
		w = (std::max)(w, fm.horizontalAdvance(QString(T(k))));
	l->setFixedSize(w + 16, 36);
	l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	return l;
}

/* One layer in the Sources list, drawn as a slab in its kind's colour: raised while it shows,
   carved in while hidden, glowing when picked. A handle to drag it, what kind it is, its name,
   then show/hide and lock. */
class SourceRow : public QWidget {
public:
	SourceRow(const QString &name, bool visible, bool locked, const std::string &kind)
		: name(name),
		  iconName(QString::fromUtf8(source_kind_icon(kind))),
		  colour(source_kind_colour(kind)),
		  on(visible)
	{
		auto *h = new QHBoxLayout(this);
		h->setContentsMargins(64, 0, 9, 2);
		h->setSpacing(2);
		h->addStretch(1);
		eye = row_button(visible ? "eye" : "eye-off", T(visible ? "Hide" : "Show"));
		lock = row_button(locked ? "lock" : "unlock", T(locked ? "Unlock" : "Lock"));
		h->addWidget(eye);
		h->addWidget(lock);
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_Hover);
		setToolTip(T("SourceDragTip"));
	}
	void setPicked(bool p)
	{
		if (picked != p) {
			picked = p;
			update();
		}
	}
	QToolButton *eye = nullptr;
	QToolButton *lock = nullptr;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		svl::paint_list_row(p, QRectF(rect()).adjusted(3, 3, -5, -5), colour, iconName, name,
				    on ? QString() : QString(T("SourceHiddenWord")), on, picked, hover, eye->x() - 6);
	}
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

private:
	QString name, iconName;
	QColor colour;
	bool on = true, picked = false, hover = false;
};

/* The layers of the vertical scene that is up, top layer first, like OBS's Sources dock. */
/* The glowing word over a part of Edit scene, SCENES or SOURCES, as wide as its own word. */
static QLabel *part_label(const QString &text, const QColor &colour)
{
	auto *l = new QLabel(text.toUpper());
	l->setFont(svl::head(16));
	colour_row_label(l, colour);
	const QFontMetrics fm(l->font());
	l->setFixedSize(fm.horizontalAdvance(l->text()) + 14, 38);
	l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	return l;
}

/* One canvas as a tab across the top of Scenes: its word in the canvas colour, with a glowing
   line under the one being shown. Red with the radio icon while a platform shows it live.
   An action tab (Add canvas) stays bright and only lines up on hover. */
class CanvasTab : public QAbstractButton {
public:
	CanvasTab(const QString &text, const QString &iconName, const QColor &colour, bool chosen)
		: iconName(iconName),
		  colour(colour),
		  chosen(chosen)
	{
		setText(text);
		setCursor(Qt::PointingHandCursor);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	}
	void setAction(bool on)
	{
		action = on;
		update();
	}
	QSize sizeHint() const override
	{
		const QFontMetricsF fm(tab_font());
		qreal w = 24 + fm.horizontalAdvance(text().toUpper());
		if (!iconName.isEmpty())
			w += 22;
		return QSize((int)std::ceil(w), 38);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void enterEvent(QEnterEvent *e) override
	{
		hover = true;
		update();
		QAbstractButton::enterEvent(e);
	}
	void leaveEvent(QEvent *e) override
	{
		hover = false;
		update();
		QAbstractButton::leaveEvent(e);
	}
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const qreal dpr = devicePixelRatioF();
		const QString t = text().toUpper();
		const QFont f = tab_font();
		p.setFont(f);
		const QFontMetricsF fm(f);
		const bool lit = chosen || isDown();
		/* The others rest in their own full colour, only a touch softer and with no glow or line.
		   Washed with white and faded, they read grey (Twitch did). */
		p.setOpacity(lit || action || hover ? 1.0 : 0.85);
		const qreal textH = height() - 7;
		qreal x = 12;
		if (!iconName.isEmpty()) {
			p.drawPixmap(QPointF(x, std::round((textH - 16) / 2.0)), svl::icon(iconName, colour, 16, dpr));
			x += 22;
		}
		const QColor ink = lit ? svl::mix(colour, QColor(Qt::white), 0.55) : colour;
		svl::glow_text(p, QRectF(x, 0, fm.horizontalAdvance(t) + 2, textH), Qt::AlignLeft | Qt::AlignVCenter, t, ink,
			       colour, lit ? 0.9 : 0.0);
		if (!lit && !hover)
			return;
		p.setOpacity(1.0);
		p.setPen(Qt::NoPen);
		const QRectF line(6, height() - 5, width() - 12, 2);
		if (lit) {
			/* A soft glow round the line, then the line itself. */
			for (int i = 3; i >= 1; i--) {
				p.setBrush(svl::alpha(colour, 0.12));
				p.drawRoundedRect(line.adjusted(-i, -i, i, i), 1 + i, 1 + i);
			}
			p.setBrush(svl::mix(colour, QColor(Qt::white), 0.25));
		} else {
			p.setBrush(svl::alpha(colour, 0.45));
		}
		p.drawRoundedRect(line, 1, 1);
	}

private:
	static QFont tab_font()
	{
		QFont f = svl::body(13, QFont::Black);
		f.setLetterSpacing(QFont::PercentageSpacing, 104);
		return f;
	}
	QString iconName;
	QColor colour;
	bool chosen = false;
	bool action = false;
	bool hover = false;
};

/* The row of canvas tabs, with a quiet line along the bottom for the lit tab to sit on. */
class TabStrip : public Flow {
public:
	explicit TabStrip(int gap) : Flow(gap) {}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		QLinearGradient g(QPointF(0, 0), QPointF(width(), 0));
		g.setColorAt(0, svl::alpha(svl::BLUE, 0.32));
		g.setColorAt(1, svl::alpha(svl::BLUE, 0.06));
		p.fillRect(QRectF(0, height() - 4, width(), 1), g);
	}
};

/* The scenes as equal tiles: as many columns as fit (two at dock width), every tile one width
   whatever the number of scenes, so one scene never stretches across the whole row. */
class TileGrid : public QWidget {
public:
	TileGrid(int gap, int minTile) : gap(gap), minTile(minTile)
	{
		QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
	}
	void add(QWidget *w)
	{
		w->setParent(this);
		items.push_back(w);
		w->show();
		updateGeometry();
		place(width(), true);
	}
	void clear()
	{
		/* Later, not now: a tile can clear the grid it sits in from its own click. */
		for (QWidget *w : items) {
			w->hide();
			w->deleteLater();
		}
		items.clear();
		updateGeometry();
	}
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return place(w, false); }
	QSize sizeHint() const override
	{
		const int w = width() > 0 ? width() : 300;
		return QSize(w, place(w, false));
	}
	QSize minimumSizeHint() const override { return QSize(minTile, place((std::max)(minTile, width()), false)); }

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		place(width(), true);
	}

private:
	int place(int w, bool apply) const
	{
		if (items.empty() || w <= 0)
			return 0;
		const int cols = std::clamp((w + gap) / (minTile + gap), 1, 6);
		const int tileW = (w - gap * (cols - 1)) / cols;
		int tileH = 0;
		for (QWidget *i : items)
			tileH = (std::max)(tileH, i->sizeHint().height());
		const int count = (int)items.size();
		const int rows = (count + cols - 1) / cols;
		if (apply) {
			for (int n = 0; n < count; n++)
				items[(size_t)n]->setGeometry((n % cols) * (tileW + gap), (n / cols) * (tileH + gap), tileW, tileH);
		}
		return rows * tileH + (rows - 1) * gap;
	}

	std::vector<QWidget *> items;
	int gap;
	int minTile;
};

class SvSourcesCard : public svl::Card {
public:
	explicit SvSourcesCard(SvEdit &e) : svl::Card(T("SourcesTitle"), "layers"), edit(e)
	{
		list = new QListWidget();
		list->setSelectionMode(QAbstractItemView::ExtendedSelection);
		list->setDragDropMode(QAbstractItemView::InternalMove);
		list->setDefaultDropAction(Qt::MoveAction);
		list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		list->setUniformItemSizes(true);
		/* The rows refit whenever the list's width changes. Without it they kept the width the list
		   had when they were made, and ran off the right edge of the Edit scene pop out. */
		list->setResizeMode(QListView::Adjust);
		/* No box: the rows sit straight on the panel, under the scenes. */
		list->setFrameShape(QFrame::NoFrame);
		/* Each row draws its own slab (SourceRow), so the list itself draws nothing. */
		list->setStyleSheet(
			"QListWidget { background: transparent; border: none; padding: 0px; outline: none; }"
			"QListWidget::item, QListWidget::item:hover, QListWidget::item:selected"
			" { background: transparent; border: none; }");
		list->viewport()->setAutoFillBackground(false);
		body()->addWidget(list);

		/* SOURCES and its keys, each with its word: Add and Pop out. On a narrow panel the keys
		   wrap under the word. The keys that change a source sit on a line of their own under it
		   and show once a source is picked, so Add and Pop out never move about. */
		head = new Flow(6);
		head->add(part_label(T("SourcesTitle"), svl::CYAN));
		auto *addKey = key(T("AddShort"), "plus", svl::EMERALD);
		addKey->setToolTip(T("AddSource"));
		head->add(addKey);
		outKey = key(T("PopOutShort"), "external", svl::PINK);
		head->add(outKey);
		pickKeys = new Flow(6);
		propsKey = key(T("Properties"), "settings", svl::BLUE);
		filtersKey = key(T("Filters"), "filter", svl::VIOLET);
		upKey = key(T("UpShort"), "arrow-up", svl::LIME);
		upKey->setToolTip(T("MoveUp"));
		downKey = key(T("DownShort"), "arrow-down", svl::LIME);
		downKey->setToolTip(T("MoveDown"));
		removeKey = key(T("Remove"), "trash", svl::AMBER);
		removeKey->setToolTip(T("RemoveSource"));
		for (svl::Key *k : {propsKey, filtersKey, upKey, downKey, removeKey})
			pickKeys->add(k);
		pickKeys->hide();
		body()->insertWidget(0, head);
		body()->insertWidget(1, pickKeys);

		connect(addKey, &QPushButton::clicked, this, [this, addKey] {
			QMenu menu(this);
			menu_look(&menu);
			fill_add_menu(&menu, this);
			menu.exec(addKey->mapToGlobal(QPoint(0, addKey->height())));
			changed();
		});
		connect(propsKey, &QPushButton::clicked, this, [this] { on_first(obs_frontend_open_source_properties); });
		connect(filtersKey, &QPushButton::clicked, this, [this] { on_first(obs_frontend_open_source_filters); });
		connect(upKey, &QPushButton::clicked, this, [this] {
			order_selected(OBS_ORDER_MOVE_UP);
			changed();
		});
		connect(downKey, &QPushButton::clicked, this, [this] {
			order_selected(OBS_ORDER_MOVE_DOWN);
			changed();
		});
		connect(removeKey, &QPushButton::clicked, this, [this] {
			remove_selected(this);
			changed();
		});
		connect(list, &QListWidget::itemSelectionChanged, this, [this] {
			light_picked();
			push_selection();
		});
		connect(list, &QListWidget::itemDoubleClicked, this,
			[this](QListWidgetItem *) { on_first(obs_frontend_open_source_properties); });
		connect(list, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
			QMenu menu(this);
			menu_look(&menu);
			fill_item_menu(&menu, this, edit);
			menu.exec(list->viewport()->mapToGlobal(at));
			changed();
		});
		connect(list->model(), &QAbstractItemModel::rowsMoved, this, [this] { apply_order(); });
	}

	/* Called after anything changes the layers: rebuilds the rows only when they differ. */
	void refresh()
	{
		struct Row {
			int64_t id;
			std::string name;
			bool visible, locked, selected;
			std::string kind;
		};
		std::vector<Row> rows;
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		if (scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					static_cast<std::vector<Row> *>(param)->push_back(
						{obs_sceneitem_get_id(item), obs_source_get_name(obs_sceneitem_get_source(item)),
						 obs_sceneitem_visible(item), obs_sceneitem_locked(item),
						 obs_sceneitem_selected(item),
						 obs_source_get_unversioned_id(obs_sceneitem_get_source(item))});
					return true;
				},
				&rows);
		}
		obs_scene_release(scene);
		std::reverse(rows.begin(), rows.end());

		std::string signature;
		for (const Row &r : rows)
			signature += std::to_string(r.id) + "|" + r.name + "|" + r.kind + "|" + (r.visible ? "v" : "h") +
				     (r.locked ? "l" : "u") + "\n";

		syncing = true;
		if (signature != lastSignature) {
			lastSignature = signature;
			list->clear();
			for (const Row &r : rows) {
				auto *item = new QListWidgetItem(list);
				item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(r.id));
				item->setSizeHint(QSize(10, 44));
				auto *w = new SourceRow(QString::fromStdString(r.name), r.visible, r.locked, r.kind);
				list->setItemWidget(item, w);
				const int64_t id = r.id;
				connect(w->eye, &QToolButton::clicked, this, [this, id] { toggle(id, true); });
				connect(w->lock, &QToolButton::clicked, this, [this, id] { toggle(id, false); });
			}
			const int shown = std::clamp((int)rows.size(), 1, maxRows);
			const int rowH = std::max(44, list->count() > 0 ? list->sizeHintForRow(0) : 44);
			list->setFixedHeight(shown * rowH + 2 * list->frameWidth() + 2);
		}
		bool any = false;
		for (int i = 0; i < list->count() && i < (int)rows.size(); i++) {
			list->item(i)->setSelected(rows[(size_t)i].selected);
			any = any || rows[(size_t)i].selected;
		}
		syncing = false;
		light_picked();
		pickKeys->setVisible(any);
		setValue(rows.size() == 1 ? QString(T("LayersOne")) : QString(T("LayersMany")).arg(rows.size()));
	}

	std::function<void()> onChanged;
	/* Pop out, on the SOURCES heading; the panel wires it up. */
	svl::Key *outKey = nullptr;

	/* How many rows show before the list scrolls: few in the canvas panel, more in a panel of its own. */
	void setMaxRows(int rows)
	{
		if (maxRows == rows)
			return;
		maxRows = rows;
		lastSignature.clear();
		refresh();
	}

private:
	void changed()
	{
		refresh();
		if (onChanged)
			onChanged();
	}

	/* The picked rows glow. */
	void light_picked()
	{
		for (int i = 0; i < list->count(); i++) {
			if (auto *w = dynamic_cast<SourceRow *>(list->itemWidget(list->item(i))))
				w->setPicked(list->item(i)->isSelected());
		}
	}

	obs_sceneitem_t *item_for(obs_scene_t *scene, QListWidgetItem *row) const
	{
		return scene && row ? obs_scene_find_sceneitem_by_id(scene, row->data(Qt::UserRole).toLongLong())
				    : nullptr;
	}

	void push_selection()
	{
		if (syncing)
			return;
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		for (int i = 0; i < list->count(); i++) {
			if (obs_sceneitem_t *it = item_for(scene, list->item(i)))
				obs_sceneitem_select(it, list->item(i)->isSelected());
		}
		obs_scene_release(scene);
		changed();
	}

	void toggle(int64_t id, bool visibility)
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		obs_sceneitem_t *it = scene ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
		if (it) {
			if (visibility)
				obs_sceneitem_set_visible(it, !obs_sceneitem_visible(it));
			else
				obs_sceneitem_set_locked(it, !obs_sceneitem_locked(it));
		}
		obs_scene_release(scene);
		changed();
	}

	void on_first(void (*fn)(obs_source_t *))
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		std::vector<obs_sceneitem_t *> items = SvEdit::selected(scene);
		obs_scene_release(scene);
		if (!items.empty())
			fn(obs_sceneitem_get_source(items[0]));
		for (obs_sceneitem_t *it : items)
			obs_sceneitem_release(it);
	}

	/* Dragging rows reorders the layers: the list is top first, OBS counts from the bottom. */
	void apply_order()
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		if (!scene)
			return;
		std::vector<obs_sceneitem_t *> bottomFirst;
		for (int i = list->count() - 1; i >= 0; i--) {
			if (obs_sceneitem_t *it = item_for(scene, list->item(i)))
				bottomFirst.push_back(it);
		}
		if (!bottomFirst.empty())
			obs_scene_reorder_items(scene, bottomFirst.data(), bottomFirst.size());
		obs_scene_release(scene);
		lastSignature.clear();
		QTimer::singleShot(0, this, [this] { changed(); });
	}

	SvEdit &edit;
	QListWidget *list = nullptr;
	Flow *head = nullptr;
	Flow *pickKeys = nullptr;
	svl::Key *propsKey = nullptr;
	svl::Key *filtersKey = nullptr;
	svl::Key *upKey = nullptr;
	svl::Key *downKey = nullptr;
	svl::Key *removeKey = nullptr;
	std::string lastSignature;
	int maxRows = 8;
	bool syncing = false;
};

/* The colour of the canvas on the picture; its scene keys take it too. */
static QColor view_hue()
{
	if (!sv_canvas_running())
		return svl::BLUE;
	const std::vector<SvVCanvas> canvases = sv_canvases();
	const std::string view = sv_canvas_view();
	for (int i = 0; i < (int)canvases.size(); i++) {
		const std::string id = canvases[i].main ? std::string() : canvases[i].uuid;
		if (id == view)
			return canvas_hue(canvases[i], i);
	}
	return svl::BLUE;
}

/* The vertical scenes: canvas tabs across the top, the SCENES heading with its keys, then a
   tile for each scene. The lit tile is on the canvas, clicking another switches to it. */
class SvScenesCard : public svl::Card {
public:
	SvScenesCard() : svl::Card(T("VScenesTitle"), "scenes")
	{
		/* The canvases as tabs (Main first, then the ones you named, each in its own colour),
		   then Add canvas. Shown even with only Main, so adding one is always in sight. */
		canvasTabs = new TabStrip(2);
		canvasTabs->setToolTip(T("VCanvasStripTip"));
		body()->addWidget(canvasTabs);
		canvasTabs->hide();

		/* SCENES and its keys, each with its word: Add, Edit (rename, copy or remove the scene,
		   and the canvas options), Link and Pop out (the panel wires those two). On a narrow
		   panel the keys wrap under the word. */
		auto *sceneHead = new Flow(6);
		sceneHead->add(part_label(T("VScenesTitle"), svl::YELLOW));
		auto *addKey = key(T("AddShort"), "plus", svl::EMERALD);
		addKey->setToolTip(T("VSceneAdd"));
		sceneHead->add(addKey);
		auto *editKey = key(T("Edit"), "pencil", svl::ORANGE);
		editKey->setToolTip(T("VSceneEditTip"));
		sceneHead->add(editKey);
		linkKey = key(T("ScenesShort"), "link");
		linkKey->setToolTip(T("ScenesLinkTip"));
		sceneHead->add(linkKey);
		outKey = key(T("PopOutShort"), "external", svl::PINK);
		sceneHead->add(outKey);
		body()->addWidget(sceneHead);

		/* A tile for each scene under the heading. */
		sceneGrid = new TileGrid(8, 140);
		body()->addWidget(sceneGrid);

		connect(addKey, &QPushButton::clicked, this, [this] {
			bool ok = false;
			const QString name = QInputDialog::getText(
				this, T("VSceneAdd"), T("VSceneName"), QLineEdit::Normal,
				QString::fromStdString(sv_canvas_free_name(T("VSceneBase"), sv_canvas_view())), &ok);
			if (ok && sv_canvas_add_scene(name.trimmed().toStdString(), sv_canvas_view()).empty())
				QMessageBox::information(this, T("VSceneAdd"), T("NameTaken"));
		});
		connect(editKey, &QPushButton::clicked, this, [this, editKey] {
			QMenu menu(this);
			menu_look(&menu);
			const qreal dpr = devicePixelRatioF();
			menu.addAction(QIcon(svl::icon("pencil", svl::BLUE, 16, dpr)), T("VSceneRename"),
				       [this] { rename_current(); });
			menu.addAction(QIcon(svl::icon("copy", svl::BLUE, 16, dpr)), T("VSceneCopy"),
				       [this] { copy_current(); });
			/* The last scene cannot go. */
			if (sv_canvas_scenes(sv_canvas_view()).size() > 1) {
				menu.addSeparator();
				menu.addAction(QIcon(svl::icon("trash", svl::AMBER, 16, dpr)), T("VSceneRemove"),
					       [this] { remove_current(); });
			}
			/* Canvases: each platform can show its own, picked in Where to go live. */
			if (sv_canvas_running()) {
				menu.addSeparator();
				if ((int)sv_canvases().size() < SV_CANVAS_MAX)
					menu.addAction(QIcon(svl::icon("layers", svl::BLUE, 16, dpr)), T("VCanvasAdd"),
						       [this] { add_canvas(); });
				if (!sv_canvas_view().empty()) {
					menu.addAction(QIcon(svl::icon("pencil", svl::BLUE, 16, dpr)), T("VCanvasRename"),
						       [this] { rename_canvas(); });
					menu.addAction(QIcon(svl::icon("trash", svl::AMBER, 16, dpr)), T("VCanvasRemove"),
						       [this] { remove_canvas(); });
				}
			}
			menu.exec(editKey->mapToGlobal(QPoint(0, editKey->height())));
		});
	}

	/* A new canvas, a copy of the main one, shown straight away so it can be changed. */
	void add_canvas()
	{
		bool ok = false;
		const QString name = QInputDialog::getText(
			this, T("VCanvasAdd"), T("VCanvasName"), QLineEdit::Normal,
			QString::fromStdString(sv_canvas_free_canvas_name(T("VCanvasBase"))), &ok);
		if (!ok || name.trimmed().isEmpty())
			return;
		const std::string id = sv_canvas_add(name.trimmed().toStdString());
		if (id.empty()) {
			QMessageBox::information(this, T("VCanvasAdd"), T("DestCanvasFailed"));
			return;
		}
		sv_canvas_set_view(id);
	}

	void rename_canvas()
	{
		const std::string view = sv_canvas_view();
		if (view.empty())
			return;
		const QString was = canvas_name(view);
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, T("VCanvasRename"), T("VCanvasName"), QLineEdit::Normal, was, &ok);
		if (!ok || name.trimmed().isEmpty() || name.trimmed() == was)
			return;
		if (!sv_canvas_rename(view, name.trimmed().toStdString()))
			QMessageBox::information(this, T("VCanvasRename"), T("NameTaken"));
		refresh(true);
	}

	void remove_canvas()
	{
		const std::string view = sv_canvas_view();
		if (view.empty())
			return;
		if (sv_output_canvas_in_use(view)) {
			QMessageBox::information(this, T("VCanvasRemove"), T("VCanvasStopFirst"));
			return;
		}
		if (QMessageBox::question(this, T("RemoveTitle"), QString(T("VCanvasRemoveAsk")).arg(canvas_name(view))) !=
		    QMessageBox::Yes)
			return;
		sv_output_forget_canvas(view);
		if (!sv_canvas_remove(view))
			QMessageBox::information(this, T("VCanvasRemove"), T("VCanvasStopFirst"));
		refresh(true);
	}

	void copy_current()
	{
		{
			const std::string from = sv_canvas_current(sv_canvas_view());
			std::string base;
			for (const SvVScene &s : sv_canvas_scenes(sv_canvas_view()))
				if (s.uuid == from)
					base = s.name;
			bool ok = false;
			const QString name = QInputDialog::getText(this, T("VSceneCopy"), T("VSceneName"),
								   QLineEdit::Normal,
								   QString::fromStdString(sv_canvas_free_name(base, sv_canvas_view())), &ok);
			if (ok && sv_canvas_duplicate_scene(from, name.trimmed().toStdString()).empty())
				QMessageBox::information(this, T("VSceneCopy"), T("NameTaken"));
		}
	}

	void rename_current()
	{
		const std::string uuid = sv_canvas_current(sv_canvas_view());
		const QString was = current_name();
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, T("VSceneRename"), T("VSceneName"), QLineEdit::Normal, was, &ok);
		if (!ok || name.trimmed().isEmpty() || name.trimmed() == was)
			return;
		if (!sv_canvas_rename_scene(uuid, name.trimmed().toStdString()))
			QMessageBox::information(this, T("Rename"), T("NameTaken"));
		refresh(true);
	}

	void remove_current()
	{
		const std::string uuid = sv_canvas_current(sv_canvas_view());
		if (uuid.empty())
			return;
		if (QMessageBox::question(this, T("RemoveTitle"), QString(T("RemoveSceneAsk")).arg(current_name())) ==
		    QMessageBox::Yes)
			sv_canvas_remove_scene(uuid);
	}

	void refresh(bool force = false)
	{
		refresh_canvases(force);
		const std::vector<SvVScene> scenes = sv_canvas_scenes(sv_canvas_view());
		const std::string current = sv_canvas_current(sv_canvas_view());
		const QColor hue = view_hue();
		std::string signature = sv_canvas_view() + "\n" + current + "\n" + hue.name().toStdString() + "\n";
		for (const SvVScene &s : scenes)
			signature += s.uuid + "|" + s.name + "\n";
		if (force || signature != lastSignature) {
			lastSignature = signature;
			sceneGrid->clear();
			for (const SvVScene &s : scenes) {
				const QString name = QString::fromStdString(s.name);
				svl::Key *k = key(name, QString(), hue);
				k->setUnchosen(s.uuid != current);
				/* A long name is cut short on its tile; hover shows all of it. */
				k->setToolTip(name);
				const std::string uuid = s.uuid;
				connect(k, &QPushButton::clicked, this, [uuid] { sv_canvas_switch(uuid); });
				sceneGrid->add(k);
			}
		}
		setValue(current_name());
	}

	/* Link and Pop out, on the SCENES heading; the panel wires them up. */
	svl::Key *linkKey = nullptr;
	svl::Key *outKey = nullptr;

private:
	void refresh_canvases(bool force)
	{
		const std::vector<SvVCanvas> canvases = sv_canvas_running() ? sv_canvases() : std::vector<SvVCanvas>();
		const std::string view = sv_canvas_view();
		std::string signature = view + "\n";
		for (const SvVCanvas &c : canvases) {
			const std::string id = c.main ? std::string() : c.uuid;
			signature += c.uuid + "|" + c.name + "|" + (sv_output_canvas_streaming(id) ? "1" : "0") +
				     (sv_output_canvas_in_use(id) ? "1" : "0") + "\n";
		}
		if (!force && signature == canvasSignature)
			return;
		canvasSignature = signature;
		canvasTabs->clear();
		for (int i = 0; i < (int)canvases.size(); i++) {
			const SvVCanvas &c = canvases[i];
			const std::string id = c.main ? std::string() : c.uuid;
			/* Red and "Live now" only for a real stream; Backtrack or a recording on this canvas is not live. */
			const bool live = sv_output_canvas_streaming(id);
			const QString name = c.main ? QString(T("DestCanvasMain")) : QString::fromStdString(c.name);
			auto *tab = new CanvasTab(name, live ? QString("radio") : QString(), live ? svl::RED : canvas_hue(c, i),
						  id == view);
			tab->setToolTip(live ? QString(T("VCanvasLiveTip"))
					     : sv_output_canvas_in_use(id) ? QString(T("VCanvasBusyTip")) : QString(T("VCanvasKeyTip")));
			connect(tab, &QAbstractButton::clicked, this, [id] { sv_canvas_set_view(id); });
			canvasTabs->add(tab);
		}
		/* Add canvas at the end, gone once there are as many as allowed. It opens its box a
		   moment later, so the tabs can be rebuilt under it while the box is open. */
		if (!canvases.empty() && (int)canvases.size() < SV_CANVAS_MAX) {
			auto *add = new CanvasTab(T("VCanvasAddShort"), "plus", svl::EMERALD, false);
			add->setAction(true);
			add->setToolTip(T("VCanvasAdd"));
			connect(add, &QAbstractButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { add_canvas(); }); });
			canvasTabs->add(add);
		}
		/* Always shown while the vertical canvas is on, even with only Main, so adding another
		   canvas is always in sight (Nick, 2026-11-21: hidden, it looked like extra canvases had gone). */
		canvasTabs->setVisible(!canvases.empty());
	}


	static QString canvas_name(const std::string &id)
	{
		for (const SvVCanvas &c : sv_canvases())
			if (!c.main && c.uuid == id)
				return QString::fromStdString(c.name);
		return QString();
	}

	QString current_name() const
	{
		const std::string current = sv_canvas_current(sv_canvas_view());
		for (const SvVScene &s : sv_canvas_scenes(sv_canvas_view()))
			if (s.uuid == current)
				return QString::fromStdString(s.name);
		return QString();
	}

	TileGrid *sceneGrid = nullptr;
	TabStrip *canvasTabs = nullptr;

public:
	/* Set by the panel while the scenes sit in an OBS panel of their own. */
	bool poppedOut = false;
	std::string lastSignature;
	std::string canvasSignature;
};

class SvScroll : public QScrollArea {
public:
	QSize minimumSizeHint() const override
	{
		QSize s = QScrollArea::minimumSizeHint();
		if (widget())
			s.setWidth(widget()->minimumSizeHint().width() + verticalScrollBar()->sizeHint().width() +
				   2 * frameWidth());
		return s;
	}
};

/* A panel that opens from a key in the control row and closes when the member clicks anywhere else. */
class SvPopup : public QFrame {
public:
	explicit SvPopup(QWidget *parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		svl::mark_tips(this);
		/* Only the cards show: the window round them is see-through, with no box or edge. */
		setAttribute(Qt::WA_TranslucentBackground);
		setStyleSheet(svl::sheet());
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(10, 10, 10, 10);
		v->setSpacing(0);
		scroll = new SvScroll();
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setAutoFillBackground(false);
		page = new QWidget();
		page->setAutoFillBackground(false);
		pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(0, 0, 4, 0);
		pageLayout->setSpacing(8);
		pageLayout->addStretch(1);
		scroll->setWidget(page);
		v->addWidget(scroll);
	}

	/* Puts a widget at the top of the popup, taking it out of wherever it was. */
	void hold(QWidget *w)
	{
		if (w->parentWidget() == page)
			return;
		if (QWidget *old = w->parentWidget())
			if (old->layout())
				old->layout()->removeWidget(w);
		pageLayout->insertWidget(0, w);
		w->show();
	}

	/* Opens just above the row the key sits in, as wide as the panel, as tall as it
	   needs up to most of the panel. The window's clear 10 px edge may hang over the
	   row, the panel's sides or the screen's edge: only the card counts, and it runs
	   4 px in from the panel's sides and ends 6 px above the row, so no key under it
	   peeks out half covered at its bottom or ends. */
	void open_over(QWidget *panel, QWidget *row)
	{
		const int w = (std::max)(260, panel->width() + 12);
		const int inner = w - 20 - 4;
		const int need = (page->hasHeightForWidth() ? page->heightForWidth(inner) : page->sizeHint().height()) + 22;
		const int h = std::clamp(need, 80, (std::max)(200, panel->height() * 4 / 5));
		const QPoint rowTop = row->mapToGlobal(QPoint(0, 0));
		QPoint at(panel->mapToGlobal(QPoint(0, 0)).x() - 6, rowTop.y() - h + 10 - 6);
		if (QScreen *screen = QGuiApplication::screenAt(rowTop)) {
			const QRect area = screen->availableGeometry();
			if (at.y() < area.top())
				at.setY(rowTop.y() + row->height() + 6 - 10);
			at.setX(std::clamp(at.x(), area.left() - 10, (std::max)(area.left() - 10, area.right() + 1 + 10 - w)));
		}
		resize(w, h);
		move(at);
		show();
		raise();
	}

	/* For a row at the top of the panel: opens just under it instead, over the canvas. */
	void open_under(QWidget *panel, QWidget *row)
	{
		const int w = (std::max)(260, panel->width() + 12);
		const int inner = w - 20 - 4;
		const int need = (page->hasHeightForWidth() ? page->heightForWidth(inner) : page->sizeHint().height()) + 22;
		const int h = std::clamp(need, 80, (std::max)(200, panel->height() * 4 / 5));
		const QPoint rowTop = row->mapToGlobal(QPoint(0, 0));
		QPoint at(panel->mapToGlobal(QPoint(0, 0)).x() - 6, rowTop.y() + row->height() + 6 - 10);
		if (QScreen *screen = QGuiApplication::screenAt(rowTop)) {
			const QRect area = screen->availableGeometry();
			if (at.y() + h > area.bottom())
				at.setY((std::max)(area.top(), area.bottom() - h));
			at.setX(std::clamp(at.x(), area.left() - 10, (std::max)(area.left() - 10, area.right() + 1 + 10 - w)));
		}
		resize(w, h);
		move(at);
		show();
		raise();
	}

	/* Opens just under a key, only as wide as what it holds, its right edge lined up with the
	   key's: the gear menu, a short column of keys, with no empty card running off to the side. */
	void open_under_key(QWidget *keyWidget)
	{
		page->adjustSize();
		const QSize content = page->sizeHint();
		const int w = content.width() + 24;
		const int h = content.height() + 22;
		const QPoint keyTop = keyWidget->mapToGlobal(QPoint(0, 0));
		QPoint at(keyTop.x() + keyWidget->width() - w + 10, keyTop.y() + keyWidget->height() + 6 - 10);
		if (QScreen *screen = QGuiApplication::screenAt(keyTop)) {
			const QRect area = screen->availableGeometry();
			if (at.y() + h > area.bottom())
				at.setY((std::max)(area.top(), area.bottom() - h));
			at.setX(std::clamp(at.x(), area.left() - 10, (std::max)(area.left() - 10, area.right() + 1 + 10 - w)));
		}
		resize(w, h);
		move(at);
		show();
		raise();
	}

	QWidget *page = nullptr;

private:
	SvScroll *scroll = nullptr;
	QVBoxLayout *pageLayout = nullptr;
};

/* An OBS panel of its own for the Scenes or the Sources, when the member pops one out of the
   canvas panel. Empty while that part sits in the canvas panel, which is where it starts. */
/* ---------- The live stats, opened out ----------
   Clicking the stats in the header opens this card under them: the vertical's own numbers,
   OBS's, anything going out, and one plain line saying whether anything needs doing.
   It updates every second while open and closes on a click anywhere else. */
class SvStatsCard : public QWidget {
public:
	SvStatsCard() : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		cpu = os_cpu_usage_info_start();
		tick.setInterval(1000);
		QObject::connect(&tick, &QTimer::timeout, this, [this] { refresh(); });
	}
	~SvStatsCard() override
	{
		if (cpu)
			os_cpu_usage_info_destroy(cpu);
	}

	/* Opens under `anchor`. A click on the stats while open closes it (the click
	   outside closes the card first, so it is not opened again straight away). */
	void toggle(QWidget *anchor)
	{
		if (isVisible() || os_gettime_ns() - closedNs < 300000000ULL)
			return;
		refresh();
		QPoint at = anchor->mapToGlobal(QPoint(-(int)GLOW - 4, anchor->height() + 2));
		if (QScreen *sc = anchor->screen()) {
			const QRect a = sc->availableGeometry();
			at.setX(std::clamp(at.x(), a.left(), std::max(a.left(), a.right() - width())));
			at.setY(std::clamp(at.y(), a.top(), std::max(a.top(), a.bottom() - height())));
		}
		move(at);
		show();
		tick.start();
	}

protected:
	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		tick.stop();
		closedNs = os_gettime_ns();
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const QRectF r = QRectF(rect()).adjusted(GLOW, GLOW, -GLOW, -GLOW);
		QPainterPath path;
		path.addRoundedRect(r, 12, 12);
		svl::shape_shadow(p, path, QPointF(0, 8), 16, QColor(0, 0, 0, 190));
		svl::shape_shadow(p, path, QPointF(0, 0), 10, svl::alpha(svl::BLUE, 0.28));
		p.fillPath(path, QColor(0x0f, 0x0f, 0x18));
		QLinearGradient top(r.topLeft(), r.bottomLeft());
		top.setColorAt(0, svl::alpha(svl::BLUE, 0.10));
		top.setColorAt(0.4, svl::alpha(svl::BLUE, 0.0));
		p.fillPath(path, top);
		p.setPen(QPen(svl::alpha(svl::BLUE, 0.55), 1));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		p.setPen(QPen(QColor(255, 255, 255, 16), 1));
		p.drawLine(QPointF(r.left() + 12, r.top() + 1.5), QPointF(r.right() - 12, r.top() + 1.5));

		qreal y = r.top() + PAD;
		const qreal x = r.left() + PAD, w = r.width() - 2 * PAD;
		for (const Row &row : rows) {
			switch (row.kind) {
			case Row::Head:
				p.setFont(svl::body(12.5, QFont::ExtraBold));
				svl::glow_text(p, QRectF(x, y, w, HEAD_H), Qt::AlignLeft | Qt::AlignVCenter, row.label,
					       svl::BLUE, svl::BLUE, 0.4);
				y += HEAD_H;
				break;
			case Row::Line:
				p.setFont(svl::body(13, QFont::Medium));
				p.setPen(svl::INK2);
				p.drawText(QRectF(x, y, w, LINE_H), Qt::AlignLeft | Qt::AlignVCenter, row.label);
				p.setFont(svl::body(13, QFont::Bold));
				p.setPen(row.colour);
				p.drawText(QRectF(x, y, w, LINE_H), Qt::AlignRight | Qt::AlignVCenter, row.value);
				y += LINE_H;
				break;
			case Row::Gap:
				y += GAP_H;
				break;
			case Row::Note: {
				p.setFont(svl::body(13, QFont::Medium));
				const QRectF nr(x, y, w, row.height);
				p.setPen(QColor(0, 0, 0, 215));
				p.drawText(nr.translated(0, 1), Qt::TextWordWrap | Qt::AlignLeft, row.label);
				p.setPen(row.colour);
				p.drawText(nr, Qt::TextWordWrap | Qt::AlignLeft, row.label);
				y += row.height;
				break;
			}
			}
		}
	}

private:
	static constexpr qreal GLOW = 18, PAD = 16, HEAD_H = 24, LINE_H = 22, GAP_H = 10, CARD_W = 340;

	struct Row {
		enum Kind { Head, Line, Gap, Note } kind;
		QString label, value;
		QColor colour = svl::INK;
		qreal height = 0;
	};

	static QString count(uint64_t n) { return QLocale().toString((qulonglong)n); }

	void head(const QString &t) { rows.push_back({Row::Head, t, {}, svl::BLUE}); }
	void line(const QString &l, const QString &v, const QColor &c = svl::INK)
	{
		rows.push_back({Row::Line, l, v, c});
	}
	void gap() { rows.push_back({Row::Gap, {}, {}}); }

	void refresh()
	{
		rows.clear();
		const uint64_t now = os_gettime_ns();

		/* The vertical. */
		head(T("StatsVertical"));
		const float ms = sv_source_cost_ms();
		line(T("StatsFrameTime"), ms < 0       ? QString(T("StatMeasuring"))
					  : ms < 0.1f ? QString(T("StatTiny"))
						      : QString(T("StatCost")).arg((double)ms, 0, 'f', 1));
		if (video_t *video = sv_canvas_video()) {
			if (const video_output_info *vi = video_output_get_info(video)) {
				const double fps = vi->fps_den ? (double)vi->fps_num / vi->fps_den : 0;
				line(T("StatsSize"), QString(T("StatsSizeValue"))
							     .arg(vi->width)
							     .arg(vi->height)
							     .arg(QLocale().toString(fps, 'g', 4)));
			}
			const uint64_t missed = video_output_get_skipped_frames(video);
			const uint64_t total = video_output_get_total_frames(video);
			if (lastMissed != UINT64_MAX && missed > lastMissed)
				badNs = now;
			lastMissed = missed;
			line(T("StatsMissed"), total ? QString(T("StatsOf")).arg(count(missed), count(total)) : count(missed),
			     missed ? AMBER_INK : svl::INK);
		}
		gap();

		/* OBS as a whole. */
		head(T("StatsObs"));
		obs_video_info ovi = {};
		const bool haveVi = obs_get_video_info(&ovi);
		const double target = haveVi && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 0;
		line(T("StatsObsFps"),
		     QString(T("StatsFpsValue")).arg(QLocale().toString(obs_get_active_fps(), 'f', 1)));
		const double frameMs = obs_get_average_frame_time_ns() / 1e6;
		const double budget = target > 0 ? 1000.0 / target : 0;
		line(T("StatsObsFrame"),
		     budget > 0 ? QString(T("StatsMsOf"))
					  .arg(QLocale().toString(frameMs, 'f', 1), QLocale().toString(budget, 'f', 1))
				: QString(T("StatCost")).arg(frameMs, 0, 'f', 1),
		     budget > 0 && frameMs > budget ? AMBER_INK : svl::INK);
		const uint64_t lagged = obs_get_lagged_frames();
		if (lastLagged != UINT64_MAX && lagged > lastLagged)
			badNs = now;
		lastLagged = lagged;
		line(T("StatsLate"), QString(T("StatsOf")).arg(count(lagged), count(obs_get_total_frames())),
		     lagged ? AMBER_INK : svl::INK);
		if (cpu)
			line(T("StatsCpu"),
			     QString("%1%").arg(QLocale().toString(os_cpu_usage_info_query(cpu), 'f', 1)));
		line(T("StatsMemory"),
		     QString(T("StatsMemValue")).arg(count(os_get_proc_resident_size() / (1024 * 1024))));

		/* Anything going out from the vertical right now. */
		bool anyOut = false;
		auto out_head = [&] {
			if (anyOut)
				return;
			anyOut = true;
			gap();
			head(T("StatsOut"));
		};
		for (const SvDest &d : sv_output_dests()) {
			const SvRunInfo info = sv_output_info(d.id);
			if (info.state != SvRunState::Live && info.state != SvRunState::Reconnecting)
				continue;
			out_head();
			line(QString::fromStdString(d.name),
			     QString(T("StatsOutValue"))
				     .arg(count((uint64_t)info.kbps), QLocale().toString(info.dropped_pct, 'f', 1)),
			     info.health == SvHealth::Steady ? svl::INK : AMBER_INK);
		}
		const SvRunInfo rec = sv_output_info(SV_RECORD_ID);
		if (rec.state == SvRunState::Live) {
			out_head();
			line(T("StatsRecording"), QString(T("StatsRecValue")).arg(count(rec.dropped)),
			     rec.dropped ? AMBER_INK : svl::INK);
		}

		/* What to do, in one line. */
		gap();
		const bool behind = badNs && now - badNs < 15ULL * 1000000000ULL;
		Row note{Row::Note, behind ? QString(T("StatsBehind")) : QString(T("StatsAllGood")), {},
			 behind ? AMBER_INK : svl::INK};
		const QFontMetricsF fm(svl::body(13, QFont::Medium));
		note.height = std::ceil(fm.boundingRect(QRectF(0, 0, CARD_W - 2 * PAD, 10000),
							Qt::TextWordWrap | Qt::AlignLeft, note.label)
						.height());
		rows.push_back(note);

		qreal h = 2 * PAD;
		for (const Row &row : rows)
			h += row.kind == Row::Head   ? HEAD_H
			     : row.kind == Row::Line ? LINE_H
			     : row.kind == Row::Gap  ? GAP_H
						     : row.height;
		resize((int)(CARD_W + 2 * GLOW), (int)std::ceil(h + 2 * GLOW));
		update();
	}

	std::vector<Row> rows;
	QTimer tick;
	os_cpu_usage_info_t *cpu = nullptr;
	uint64_t closedNs = 0, badNs = 0;
	uint64_t lastMissed = UINT64_MAX, lastLagged = UINT64_MAX;
};

class SvSidePanel : public QWidget {
public:
	explicit SvSidePanel(const QString &noteText)
	{
		svl::load_fonts();
		svl::mark_tips(this);
		setStyleSheet(svl::sheet());
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(12, 12, 12, 8);
		v->setSpacing(6);
		body = v;
		note = say(noteText, AMBER_INK);
		v->addWidget(note);
		v->addStretch(1);
		note->hide();
	}

	/* Takes a part into this panel, at the top. */
	void hold(QWidget *part)
	{
		if (part->parentWidget() == this)
			return;
		if (QWidget *old = part->parentWidget())
			if (old->layout())
				old->layout()->removeWidget(part);
		body->insertWidget(0, part);
		part->show();
	}

	void setOwn(bool own) { note->setVisible(!own); }

protected:
	/* The website's lamp along the top, its light falling over the part. */
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		svl::paint_backdrop(p, rect(), true, 5);
	}

private:
	QVBoxLayout *body = nullptr;
	svl::Words *note = nullptr;
};

void show_part_dock(bool scenes);

/* False for the first seconds after OBS opens. SE.Live builds its vertical a few
   seconds after OBS has loaded, so until then a missing vertical is not a sign that
   it is switched off, and our layout must not be added a second time. */
bool g_settled = false;

/* ---------- the panel tour ----------
   Nick, 2026-11-19: after the setup, show the member around the panel. One key at a
   time: the rest of the panel dims, that key gets a lit ring, and a card beside it says
   what it does in a line or two, with Back, Next and Skip tour. It is a window of its
   own laid over the panel, because the canvas picture is drawn by OBS in a window of its
   own that nothing inside the panel can cover. Clicks on the dimmed panel do nothing
   while it is up. It follows the panel if OBS is moved or resized, and closes if the
   panel is hidden. Shown once after the first setup, and any time from the menu. */
struct SvTourStep {
	QPointer<QWidget> target;
	QString title;
	QString text;
};

class SvTour : public QWidget {
public:
	SvTour(QWidget *panel, std::vector<SvTourStep> list)
		: QWidget(panel, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
		  host(panel),
		  steps(std::move(list))
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_DeleteOnClose);
		setStyleSheet(svl::sheet());
		card = new QWidget(this);
		auto *col = new QVBoxLayout(card);
		col->setContentsMargins(16, 14, 16, 14);
		col->setSpacing(6);
		count = say(QString(), svl::INK3, 13);
		title = new Title(QString(), 18);
		text = say(QString(), svl::INK, 14.5);
		col->addWidget(count);
		col->addWidget(title);
		col->addWidget(text);
		auto *row = new QHBoxLayout();
		row->setSpacing(8);
		skip = key(T("TourSkip"), "x");
		back = key(T("WizardBack"), "chevron-left");
		next = key(T("WizardNext"), "chevron-right");
		row->addWidget(skip);
		row->addStretch(1);
		row->addWidget(back);
		row->addWidget(next);
		col->addSpacing(4);
		col->addLayout(row);
		connect(skip, &QPushButton::clicked, this, [this] { close(); });
		connect(back, &QPushButton::clicked, this, [this] { show_step(prev(at)); });
		connect(next, &QPushButton::clicked, this, [this] {
			const int to = following(at);
			if (to < 0)
				close();
			else
				show_step(to);
		});
		host->installEventFilter(this);
		if (QWidget *w = host->window(); w && w != host)
			w->installEventFilter(this);
		show_step(following(-1));
	}

	~SvTour() override
	{
		if (host) {
			host->removeEventFilter(this);
			if (QWidget *w = host->window(); w && w != host)
				w->removeEventFilter(this);
		}
	}

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		switch (e->type()) {
		case QEvent::Move:
		case QEvent::Resize:
		case QEvent::WindowStateChange:
			QTimer::singleShot(0, this, [this] { place(); });
			break;
		case QEvent::Hide:
			if (o == host)
				QTimer::singleShot(0, this, [this] { close(); });
			break;
		default:
			break;
		}
		return QWidget::eventFilter(o, e);
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		if (e->key() == Qt::Key_Escape)
			close();
		else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
			next->click();
		else if (e->key() == Qt::Key_Left && back->isVisible())
			back->click();
		else
			QWidget::keyPressEvent(e);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		/* Everything but the key dims. */
		QPainterPath all;
		all.addRect(QRectF(rect()));
		QPainterPath ring;
		ring.addRoundedRect(QRectF(hole), 12, 12);
		p.fillPath(all.subtracted(ring), QColor(3, 5, 14, 185));
		/* The lit ring round the key, soft light falling outwards. */
		p.setBrush(Qt::NoBrush);
		for (int i = 4; i >= 1; i--) {
			p.setPen(QPen(svl::alpha(svl::BLUE, 0.07 * (5 - i)), 2.0 + i * 3.0));
			p.drawRoundedRect(QRectF(hole), 12, 12);
		}
		p.setPen(QPen(svl::mix(svl::BLUE, Qt::white, 0.35), 2));
		p.drawRoundedRect(QRectF(hole).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
		/* The card: dark, a lit blue edge, lifted off the panel. */
		const QRectF c = QRectF(card->geometry());
		QPainterPath box;
		box.addRoundedRect(c, 14, 14);
		svl::shape_shadow(p, box, QPointF(0, 8), 18, QColor(0, 0, 0, 200));
		p.fillPath(box, QColor(13, 15, 28));
		p.setPen(QPen(svl::alpha(svl::BLUE, 0.75), 1.2));
		p.drawPath(box);
	}

private:
	/* The next step with its key on screen, or -1 when there is none. */
	int following(int from) const
	{
		for (int i = from + 1; i < (int)steps.size(); i++)
			if (steps[i].target && steps[i].target->isVisible())
				return i;
		return -1;
	}

	int prev(int from) const
	{
		for (int i = from - 1; i >= 0; i--)
			if (steps[i].target && steps[i].target->isVisible())
				return i;
		return from;
	}

	void show_step(int i)
	{
		if (i < 0) {
			close();
			return;
		}
		at = i;
		int shownCount = 0, number = 0;
		for (int k = 0; k < (int)steps.size(); k++) {
			if (!steps[k].target || !steps[k].target->isVisible())
				continue;
			shownCount++;
			if (k == i)
				number = shownCount;
		}
		count->setText(QString(T("TourCount")).arg(number).arg(shownCount), svl::INK3);
		title->setText(steps[i].title);
		text->setText(steps[i].text, svl::INK);
		const bool last = following(i) < 0;
		next->setText(T(last ? "TourDone" : "WizardNext"));
		next->setIconName(last ? "check" : "chevron-right");
		back->setVisible(prev(i) != i);
		place();
		setFocus();
	}

	/* Over the panel, the ring round this step's key, and the card above it when there is
	   room, else below, kept inside the panel. */
	void place()
	{
		if (!host || at < 0 || !steps[at].target) {
			close();
			return;
		}
		setGeometry(QRect(host->mapToGlobal(QPoint(0, 0)), host->size()));
		QWidget *t = steps[at].target;
		hole = QRect(t->mapTo(host, QPoint(0, 0)), t->size()).adjusted(-6, -6, 6, 6);
		const int w = std::max(220, std::min(width() - 24, 360));
		card->setFixedWidth(w);
		QLayout *l = card->layout();
		l->activate();
		const int h = l->hasHeightForWidth() ? l->totalHeightForWidth(w) : l->totalSizeHint().height();
		card->setFixedHeight(h);
		const int x = std::clamp(hole.center().x() - w / 2, 12, std::max(12, width() - w - 12));
		int y = hole.top() - h - 14;
		if (y < 8)
			y = hole.bottom() + 14;
		if (y + h > height() - 8)
			y = std::max(8, std::min(hole.top() + 12, height() - h - 8));
		card->move(x, y);
		if (!isVisible()) {
			show();
			raise();
			activateWindow();
		}
		update();
	}

	QPointer<QWidget> host;
	std::vector<SvTourStep> steps;
	int at = -1;
	QRect hole;
	QWidget *card = nullptr;
	svl::Words *count = nullptr;
	Title *title = nullptr;
	svl::Words *text = nullptr;
	svl::Key *skip = nullptr;
	svl::Key *back = nullptr;
	svl::Key *next = nullptr;
};

class SvDock : public QWidget {
public:
	SvDock()
	{
		svl::load_fonts();
		svl::mark_tips(this);
		setStyleSheet(svl::sheet());
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(12, 12, 12, 8);
		outer->setSpacing(8);

		/* The header is one line: the logo, which account this PC is linked to (or the link
		   code while approving, or frames OBS missed, the only stat worth a glance), the
		   link (i) that turns into a warning when something is wrong, and the settings key,
		   which opens the menu with Settings first. */
		auto *top = new QHBoxLayout();
		top->setSpacing(8);
		top->addWidget(new svl::Logo(), 0, Qt::AlignVCenter);
		auto *acctRow = new QHBoxLayout();
		acctRow->setSpacing(5);
		acct = new svl::Words(nullptr, 13);
		acctInfo = new svl::Info(QString());
		statsIcon = icon_label("gauge", svl::AMBER, 14);
		statsIcon->setToolTip(T("CostTip"));
		statsIcon->setCursor(Qt::PointingHandCursor);
		statsIcon->installEventFilter(this);
		/* Frames OBS missed, amber, only while there are some; a click opens the live stats. */
		missedLine = new svl::Words(nullptr, 13);
		missedLine->setToolTip(T("CostTip"));
		missedLine->setCursor(Qt::PointingHandCursor);
		missedLine->installEventFilter(this);
		missedLine->hide();
		statsIcon->hide();
		linkIcon = icon_label("link", svl::GREEN, 14);
		linkIcon->hide();
		/* A newer plugin on the website: a lit line that opens the page with the download. */
		updateLine = new svl::Words(nullptr, 13);
		updateLine->setCursor(Qt::PointingHandCursor);
		updateLine->installEventFilter(this);
		updateLine->hide();
		acctRow->addStretch(1);
		acctRow->addWidget(updateLine, 0, Qt::AlignVCenter);
		acctRow->addWidget(statsIcon, 0, Qt::AlignVCenter);
		acctRow->addWidget(missedLine, 0, Qt::AlignVCenter);
		acctRow->addSpacing(6);
		acctRow->addWidget(linkIcon, 0, Qt::AlignVCenter);
		acctRow->addWidget(acct, 0, Qt::AlignVCenter);
		acctRow->addWidget(acctInfo, 0, Qt::AlignVCenter);
		top->addLayout(acctRow, 1);
		settingsKey = key(QString(), "settings", svl::VIOLET);
		settingsKey->setToolTip(T("MenuTip"));
		top->addWidget(settingsKey, 0, Qt::AlignVCenter);
		outer->addLayout(top);
		/* The lamp sits in this gap, under the header, clear of the status keys. */
		outer->addSpacing(12);
		headerRow = top;

		linkKey = key(T("LinkButton"), "link");
		linkKey->setBig(true);
		outer->addWidget(linkKey);

		/* Laid out around what a streamer glances at mid-stream: how live, recording and
		   Backtrack are doing on the three keys at the top, the canvas taking every bit of room
		   that is left, the layout under it, then the clip keys. Setup (the scene, guides, the
		   virtual camera) sits in a small row at the bottom, and everything else opens from a
		   key only when wanted, so nothing ever pushes the canvas about or sits over it. */

		/* Scenes, then Sources straight under them, opened from Edit scene. Either one pops
		   out into an OBS panel of its own and comes back with one click. */
		foldCard = new QWidget();
		foldBody = new QVBoxLayout(foldCard);
		foldBody->setContentsMargins(0, 0, 0, 0);
		foldBody->setSpacing(4);

		/* The vertical's scenes: canvas tabs, the SCENES heading (Add, Edit, Link to OBS's
		   scenes, Pop out), then a tile for each scene. */
		scenesCard = new SvScenesCard();
		scenesCard->setBare(true);
		scenesKey = scenesCard->linkKey;
		scenesOutKey = scenesCard->outKey;
		scenesPart = new QWidget();
		auto *ss = new QHBoxLayout(scenesPart);
		ss->setContentsMargins(0, 0, 0, 0);
		ss->setSpacing(6);
		ss->addWidget(scenesCard, 1);
		foldBody->addWidget(scenesPart);
		/* Splits the scenes from the sources while both sit in this panel. */
		partsRule = new RowRule(svl::CYAN, 17);
		foldBody->addWidget(partsRule);


		/* Go live, Record and Backtrack across the top: each key also says how it is doing
		   (the live clock and stream health, the record clock, how much Backtrack holds).
		   Filled in with the other keys further down. */
		statusRow = new QWidget();
		statusLayout = new QHBoxLayout(statusRow);
		statusLayout->setContentsMargins(0, 0, 0, 0);
		statusLayout->setSpacing(8);
		outer->addWidget(statusRow);

		/* The canvas fills whatever the panel has left, always 9:16, centred. Nothing ever
		   sits over it: the layout has its own row under it. */
		preview = new SvPreview(this, this);
		outer->addWidget(preview, 1);

		/* The layout showing, with the one before and after a press away. The name opens
		   every layout, Follow my game and Edit on website. */
		layoutRow = new QWidget();
		auto *lw = new QHBoxLayout(layoutRow);
		lw->setContentsMargins(0, 0, 0, 0);
		lw->setSpacing(6);
		layoutPrevKey = key(QString(), "chevron-left", svl::PINK);
		layoutPrevKey->setToolTip(T("LayoutPrevTip"));
		layoutNameKey = key(QString(), "canvas", svl::PINK);
		layoutNameKey->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		layoutNextKey = key(QString(), "chevron-right", svl::PINK);
		layoutNextKey->setToolTip(T("LayoutNextTip"));
		lw->addWidget(layoutPrevKey);
		lw->addWidget(layoutNameKey, 1);
		lw->addWidget(layoutNextKey);
		outer->addWidget(layoutRow);

		/* Not run by the plugin: a note, and the layouts sit right in the panel. */
		noteRow = new QWidget();
		auto *nr = new QVBoxLayout(noteRow);
		nr->setContentsMargins(0, 0, 0, 0);
		nr->setSpacing(8);
		note = say(QString());
		nr->addWidget(note);
		setupKey = key(T("SetupButton"), "tools");
		nr->addWidget(setupKey, 0, Qt::AlignLeft);
		seSetupKey = key(T("SeSetupButton"), "plus");
		nr->addWidget(seSetupKey, 0, Qt::AlignLeft);
		outer->addWidget(noteRow);
		inlineScroll = new SvScroll();
		inlineScroll->setWidgetResizable(true);
		inlineScroll->setFrameShape(QFrame::NoFrame);
		inlineScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		inlineScroll->viewport()->setAutoFillBackground(false);
		inlinePage = new QWidget();
		inlinePage->setAutoFillBackground(false);
		inlineLayout = new QVBoxLayout(inlinePage);
		inlineLayout->setContentsMargins(0, 0, 4, 0);
		inlineLayout->addStretch(1);
		inlineScroll->setWidget(inlinePage);
		outer->addWidget(inlineScroll, 1);

		/* The vertical scene's layers, under the scenes in the same box, with the SOURCES
		   heading and its keys on top. Pop out gives it an OBS panel of its own. */
		sourcesCard = new SvSourcesCard(preview->editor());
		sourcesCard->setBare(true);
		sourcesCard->setMaxRows(IN_ROWS);
		sourcesOutKey = sourcesCard->outKey;
		/* Nothing folds any more; the arrow is kept only so the old fold code has something to hide. */
		sourcesFoldKey = fold_button();
		sourcesFoldKey->setToolTip(T("SourcesFold"));
		sourcesPart = new QWidget();
		auto *sp = new QVBoxLayout(sourcesPart);
		sp->setContentsMargins(0, 0, 0, 0);
		sp->setSpacing(4);
		sp->addWidget(sourcesCard);
		foldBody->addWidget(sourcesPart);
		scenesSide = new SvSidePanel(T("ScenesNotSetUp"));
		sourcesSide = new SvSidePanel(T("SourcesNotSetUp"));
		show_part_keys();
		apply_sources_fold();

		/* layout card */
		layoutCard = new svl::Card(T("LayoutHeading"), "canvas");
		tilesFlow = new Flow(0);
		/* The tiles share the row evenly: all one width, as wide as the longest name needs,
		   but never so wide that fewer fit across than the row has room for. */
		tilesFlow->beforePlace = [this](int across) {
			const int n = (int)layoutTiles.size();
			if (n == 0)
				return;
			int wanted = 84;
			for (auto &t : layoutTiles)
				wanted = (std::max)(wanted, t.first->wantedWidth());
			const int cols = (std::max)(1, (std::min)(n, across / 84));
			const int w = (std::min)(wanted, across / cols);
			for (auto &t : layoutTiles)
				t.first->setTileWidth(w);
		};
		layoutCard->body()->addWidget(tilesFlow);
		layoutEmpty = say(T("LayoutNoneSaved"), svl::INK2, 13.5);
		layoutCard->body()->addWidget(layoutEmpty);
		layoutKeys = new Flow(6);
		editKey = key(T("EditOnWebsite"), "external");
		layoutInfo = new svl::Info(QString());
		layoutKeys->add(editKey);
		layoutKeys->add(layoutInfo);
		layoutCard->body()->addWidget(layoutKeys);
		/* Follow my game: the layout made for the game in front, by itself. */
		auto *gameRow = new QWidget();
		auto *gr = new QHBoxLayout(gameRow);
		gr->setContentsMargins(0, 0, 0, 0);
		gr->setSpacing(6);
		followGame = new QCheckBox(T("FollowGame"));
		followGame->setChecked(sv_follow_on());
		gr->addWidget(followGame);
		gr->addWidget(new svl::Info(T("FollowGameNote")));
		gr->addStretch(1);
		layoutCard->body()->addWidget(gameRow);
		connect(followGame, &QCheckBox::toggled, this, [](bool on) { sv_follow_set(on); });
		/* The game Follow my game sees right now, so the member knows it is working. */
		followSeen = say(QString(), svl::GREEN, 13.5);
		followSeen->hide();
		layoutCard->body()->addWidget(followSeen);
		/* Hold changes. In the plugin's own panel it lives on a key; here it sits with the layouts. */
		holdRow = new QWidget();
		auto *hr = new QHBoxLayout(holdRow);
		hr->setContentsMargins(0, 0, 0, 0);
		hr->setSpacing(6);
		holdBox = new QCheckBox(T("HoldChanges"));
		holdBox->setChecked(sv_link_holding());
		hr->addWidget(holdBox);
		hr->addWidget(new svl::Info(T("HoldChangesNote")));
		hr->addStretch(1);
		holdRow->hide();
		layoutCard->body()->addWidget(holdRow);
		connect(holdBox, &QCheckBox::toggled, this, [](bool on) { sv_action_set_hold(on); });

		/* Only with another app running the vertical: zoom, game and camera, scene links. */
		otherBox = new QWidget();
		otherBox->setAutoFillBackground(false);
		auto *ob = new QVBoxLayout(otherBox);
		ob->setContentsMargins(0, 0, 0, 0);
		ob->setSpacing(8);

		zoomCard = new svl::Card(T("ZoomHeading"), "zoom");
		zoomCard->setAccent(svl::VIOLET);
		zoomFlow = new Flow(6);
		zoomCard->body()->addWidget(zoomFlow);
		zoomEmpty = say(T("ZoomNone"), svl::INK2, 13.5);
		zoomCard->body()->addWidget(zoomEmpty);
		ob->addWidget(zoomCard);

		picksCard = new svl::Card(T("PicksHeading"), "webcam");
		picksCard->setAccent(svl::EMERALD);
		auto *pg = new QGridLayout();
		pg->setHorizontalSpacing(8);
		pg->setVerticalSpacing(6);
		pg->addWidget(svl::field(T("GamePick"), svl::EMERALD), 0, 0);
		pg->addWidget(svl::field(T("CamPick"), svl::EMERALD), 0, 1);
		gameBox = new QComboBox();
		camBox = new QComboBox();
		gameBox->setMinimumContentsLength(8);
		camBox->setMinimumContentsLength(8);
		gameBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		camBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		pg->addWidget(gameBox, 1, 0);
		pg->addWidget(camBox, 1, 1);
		pg->setColumnStretch(0, 1);
		pg->setColumnStretch(1, 1);
		picksCard->body()->addLayout(pg);
		ob->addWidget(picksCard);
		connect(gameBox, &QComboBox::activated, this, [this](int) { save_picks(); });
		connect(camBox, &QComboBox::activated, this, [this](int) { save_picks(); });

		linksCard = new svl::Card(T("LinksHeading"), "link");
		linksCard->setAccent(svl::ORANGE);
		linksHost = new QWidget();
		linksGrid = new QGridLayout(linksHost);
		linksGrid->setContentsMargins(0, 0, 0, 0);
		linksGrid->setHorizontalSpacing(8);
		linksGrid->setVerticalSpacing(4);
		linksGrid->setColumnStretch(0, 1);
		linksGrid->setColumnStretch(2, 1);
		linksCard->body()->addWidget(linksHost);
		linksEmpty = say(T("LinksNone"), svl::INK2, 13.5);
		linksCard->body()->addWidget(linksEmpty);
		linksKey = key(T("LinksEdit"), "link", svl::ORANGE);
		linksCard->body()->addWidget(linksKey, 0, Qt::AlignLeft);
		ob->addWidget(linksCard);
		connect(linksKey, &QPushButton::clicked, this, [this] {
			SvSceneLinksDialog dialog(this);
			dialog.exec();
			refresh();
		});

		auto *remoteRow = new QHBoxLayout();
		remoteRow->setSpacing(6);
		remoteRow->addWidget(say(T("RemoteLine"), svl::INK2, 13.5), 1);
		remoteRow->addWidget(new svl::Info(T("RemoteNote")), 0, Qt::AlignTop);
		ob->addLayout(remoteRow);
		otherBox->hide();
		inlineLayout->insertWidget(0, otherBox);

		/* A zoom can also be pressed from a key, the Stream Deck or by itself coming back,
		   so while zoom keys show, their lit state is checked twice a second. Nothing leaves the PC. */
		zoomTimer = new QTimer(this);
		zoomTimer->setInterval(500);
		connect(zoomTimer, &QTimer::timeout, this, [this] { refresh_zoom(); });


		/* go live card */
		liveCard = new svl::Card(T("LiveTitle"), "radio");
		goAll = key(T("GoLiveAll"), "radio");
		goAll->setBig(true);
		liveCard->body()->addWidget(goAll);
		emptyRow = new QWidget();
		auto *er = new QHBoxLayout(emptyRow);
		er->setContentsMargins(0, 0, 0, 0);
		er->setSpacing(6);
		er->addWidget(say(T("LiveEmptyShort")), 1);
		er->addWidget(new svl::Info(T("LiveEmpty")), 0, Qt::AlignTop);
		liveCard->body()->addWidget(emptyRow);
		rowsHost = new QWidget();
		rowsLayout = new QVBoxLayout(rowsHost);
		rowsLayout->setContentsMargins(0, 0, 0, 0);
		rowsLayout->setSpacing(0);
		liveCard->body()->addWidget(rowsHost);
		addKey = key(T("AddDestination"), "plus");
		liveCard->body()->addWidget(addKey, 0, Qt::AlignLeft);


		/* The go live list and the layouts open over the panel from the control row. */
		livePop = new SvPopup(this);
		livePop->hold(liveCard);
		layoutPop = new SvPopup(this);

		/* The recent clips: tick some and play them back to back, or play one. */
		clipsCard = new svl::Card(T("ClipsTitle"), "history");
		clipsCard->setAccent(svl::EMERALD);
		clipsEmpty = say(T("ClipsEmpty"));
		clipsCard->body()->addWidget(clipsEmpty);
		clipsHost = new QWidget();
		clipsList = new QVBoxLayout(clipsHost);
		clipsList->setContentsMargins(0, 0, 0, 0);
		clipsList->setSpacing(4);
		clipsCard->body()->addWidget(clipsHost);
		clipsRowKey = key(T("ClipsPlayRow"), "play", svl::EMERALD);
		clipsRowKey->setToolTip(T("ClipsPlayRowTip"));
		clipsCard->body()->addWidget(clipsRowKey, 0, Qt::AlignLeft);
		/* Where replays play: the whole canvas, or a box the member places on it. */
		auto *placeRow = new QWidget();
		auto *pr = new QHBoxLayout(placeRow);
		pr->setContentsMargins(0, 6, 0, 0);
		pr->setSpacing(6);
		pr->addWidget(say(T("ReplayPlaysOn"), svl::EMERALD), 0, Qt::AlignVCenter);
		replayWholeKey = key(T("ReplayWhole"), "canvas", svl::EMERALD);
		replayWholeKey->setToolTip(T("ReplayWholeTip"));
		replayBoxKey = key(T("ReplayBox"), "pip", svl::EMERALD);
		replayBoxKey->setToolTip(T("ReplayBoxTip"));
		pr->addWidget(replayWholeKey);
		pr->addWidget(replayBoxKey);
		pr->addStretch(1);
		clipsCard->body()->addWidget(placeRow);
		connect(replayWholeKey, &QPushButton::clicked, this, [this] {
			set_replay_in_box(false);
			refresh_replay_place();
		});
		connect(replayBoxKey, &QPushButton::clicked, this, [this] {
			set_replay_in_box(true);
			refresh_replay_place();
			/* Closes so the box and its handles are in view on the canvas. */
			clipsPop->hide();
		});
		clipsPop = new SvPopup(this);
		clipsPop->hold(clipsCard);

		clipCard = new svl::Card(T("ClipTitle"), "check");
		clipCard->setAccent(svl::GREEN);
		clipName = say(QString());
		clipCard->body()->addWidget(clipName);
		auto *clipKeys = new Flow(6);
		auto *clipOpen = key(T("ClipOpen"), "play", svl::GREEN);
		auto *clipShow = key(T("ClipShow"), "folder", svl::GREEN);
		auto *clipClose = key(QString(), "x", svl::GREEN);
		clipClose->setToolTip(T("ClipClose"));
		clipKeys->add(clipOpen);
		clipKeys->add(clipShow);
		clipKeys->add(clipClose);
		clipCard->body()->addWidget(clipKeys);
		clipCard->hide();
		outer->addWidget(clipCard);

		notice = say(QString());
		notice->hide();
		outer->addWidget(notice);

		/* The keys. Go live, Record and Backtrack sit in the status row over the canvas;
		   under the layout row come the clip keys, the replay controls while a replay
		   plays, and a small setup row (Edit scene, guides, virtual camera, game HUD). */
		ctrlRow = new QWidget();
		auto *cr = new QVBoxLayout(ctrlRow);
		cr->setContentsMargins(0, 0, 0, 0);
		cr->setSpacing(6);
		goKey = key(QString(), "radio");
		goMore = key(QString(), "chevron");
		goMore->setToolTip(T("LiveMoreTip"));
		recKey = key(QString(), "video", svl::ORANGE);
		/* Pause sits beside Record while it records: a pause sign, then a play sign to carry on. */
		pauseKey = key(QString(), "pause", svl::ORANGE);
		pauseKey->setToolTip(T("RecordPauseTip"));
		pauseKey->hide();
		btKey = key(QString(), "history", svl::VIOLET);
		/* The three status keys share the row evenly; the arrow and Pause keep their own size. */
		for (const std::vector<svl::Key *> &keys :
		     std::vector<std::vector<svl::Key *>>{{goKey, goMore}, {recKey, pauseKey}, {btKey}}) {
			auto *place = new QWidget();
			place->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			auto *pl = new QHBoxLayout(place);
			pl->setContentsMargins(0, 0, 0, 0);
			pl->setSpacing(2);
			keys[0]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			pl->addWidget(keys[0], 1);
			/* The arrow and Pause stand as tall as the keys with a status line. */
			for (size_t i = 1; i < keys.size(); i++) {
				keys[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
				pl->addWidget(keys[i]);
			}
			/* Go live gives up room to its arrow, so its share is bigger to keep its key as wide as the others. */
			statusLayout->addWidget(place, keys[0] == goKey ? 7 : 6);
		}

		/* The clip keys: Save clip (dimmed until Backtrack runs), save and replay, and Replay
		   with the arrow that opens the recent clips. */
		clipsRow = new QWidget();
		auto *cl = new QHBoxLayout(clipsRow);
		cl->setContentsMargins(0, 0, 0, 0);
		cl->setSpacing(6);
		clipKey = key(QString(), "scissors", svl::VIOLET);
		clipKey->setToolTip(T("BacktrackSave"));
		clipKey->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		/* Save and replay: one press saves the clip and plays it as soon as it is written. */
		clipPlayKey = key(T("CtrlSavePlay"), "clapperboard", svl::AMBER);
		clipPlayKey->setToolTip(T("SaveReplayTip"));
		clipPlayKey->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		replayKey = key(QString(), "replay", svl::EMERALD);
		replayKey->setToolTip(T("ReplayTip"));
		replayKey->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		/* The arrow on Replay opens the recent clips. */
		replayMore = key(QString(), "chevron", svl::EMERALD);
		replayMore->setToolTip(T("ClipsMoreTip"));
		cl->addWidget(clipKey, 1);
		cl->addWidget(clipPlayKey, 1);
		auto *replayPlace = new QHBoxLayout();
		replayPlace->setSpacing(2);
		replayPlace->addWidget(replayKey, 1);
		replayPlace->addWidget(replayMore);
		cl->addLayout(replayPlace, 1);
		cr->addWidget(clipsRow);

		/* The small setup row at the bottom. */
		toolsRow = new QWidget();
		auto *tl = new QHBoxLayout(toolsRow);
		tl->setContentsMargins(0, 0, 0, 0);
		tl->setSpacing(6);
		editSceneKey = key(QString(), "layers", svl::CYAN);
		editSceneKey->setToolTip(T("EditSceneTip"));
		guidesKey = key(QString(), "guides", svl::LIME);
		vcKey = key(QString(), "webcam", svl::BLUE);
		/* Hide game HUD shows while the layout has small game layers, and stays, red,
		   while they are hidden, so there is always a way back. */
		hudKey = key(T("CtrlHud"), "eye-off", svl::YELLOW);
		hudKey->setToolTip(T("HudHideTip"));
		hudKey->hide();
		for (svl::Key *k : {editSceneKey, guidesKey, vcKey}) {
			k->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			tl->addWidget(k, 1);
		}
		tl->addWidget(hudKey);
		/* Edit scene opens the scenes and sources over the panel, on a card of their own
		   like every other popup, since the popup window itself is see-through. */
		auto *editCard = new svl::Card(T("CtrlEditScene"), "layers");
		editCard->setAccent(svl::CYAN);
		editCard->body()->addWidget(foldCard);
		editPop = new SvPopup(this);
		editPop->hold(editCard);

		/* While a replay plays, its controls sit under the clip keys: pause, back 5 seconds,
		   from the start again, and the slow motion speed, with Slow motion first when a
		   press slows the replay. Stop is the Replay key itself, red while it plays. */
		replayRow = new QWidget();
		auto *rl = new QHBoxLayout(replayRow);
		rl->setContentsMargins(0, 0, 0, 0);
		rl->setSpacing(8);
		replayPauseKey = key(T("ReplayPause"), "pause", svl::EMERALD);
		replayBackKey = key(T("ReplayBack"), "rotate-ccw", svl::EMERALD);
		replayBackKey->setToolTip(T("ReplayBackTip"));
		replayRestartKey = key(T("ReplayRestart"), "skip-back", svl::EMERALD);
		replayRestartKey->setToolTip(T("ReplayRestartTip"));
		replaySpeedKey = key(QStringLiteral("50%"), "gauge", svl::EMERALD);
		replaySlowKey = key(T("ReplaySlowKey"), "snail", svl::EMERALD);
		replaySlowKey->setToolTip(T("ReplaySlowKeyTip"));
		replaySlowKey->hide();
		rl->addStretch(1);
		for (svl::Key *k : {replaySlowKey, replayPauseKey, replayBackKey, replayRestartKey, replaySpeedKey})
			rl->addWidget(k);
		rl->addStretch(1);
		replayRow->hide();
		cr->addWidget(replayRow);
		cr->addWidget(toolsRow);
		outer->addWidget(ctrlRow);
		ctrlRow->installEventFilter(this);

		/* wiring */
		connect(scenesOutKey, &QPushButton::clicked, this, [this] { toggle_part(true); });
		connect(sourcesOutKey, &QPushButton::clicked, this, [this] { toggle_part(false); });
		connect(sourcesFoldKey, &QToolButton::clicked, this, [this] {
			sv_store_set_int("sources_folded", sv_store_int("sources_folded", 0) != 0 ? 0 : 1);
			apply_sources_fold();
		});
		connect(linkKey, &QPushButton::clicked, this, [] { sv_link_request(); });
		connect(setupKey, &QPushButton::clicked, this, [] { show_welcome(); });
		connect(seSetupKey, &QPushButton::clicked, this, [this] { open_se_setup(); });
		connect(settingsKey, &QPushButton::clicked, this, [this] { show_menu(); });
		connect(layoutNameKey, &QPushButton::clicked, this, [this] { layoutPop->open_over(this, layoutRow); });
		connect(layoutPrevKey, &QPushButton::clicked, this, [this] {
			sv_action_step_layout(-1, sv_canvas_view());
			refresh();
		});
		connect(layoutNextKey, &QPushButton::clicked, this, [this] {
			sv_action_step_layout(1, sv_canvas_view());
			refresh();
		});
		connect(editSceneKey, &QPushButton::clicked, this, [this] {
			refresh_editing();
			editPop->open_over(this, toolsRow);
		});
		connect(editKey, &QPushButton::clicked, this, [this] {
			layoutPop->hide();
			open_site();
		});
		connect(scenesKey, &QPushButton::clicked, this, [this] {
			SvSceneLinksDialog dialog(this);
			dialog.exec();
			refresh();
		});
		/* Editing on the picture keeps the Sources list in step, and the other way round. */
		preview->onSelection = [this] { refresh_sources(); };
		preview->onMenu = [this](QPoint at) {
			QMenu menu(this);
			menu_look(&menu);
			fill_item_menu(&menu, this, preview->editor());
			menu.exec(at);
			refresh_sources();
		};
		preview->onDelete = [this] {
			remove_selected(this);
			refresh_sources();
		};
		connect(goAll, &QPushButton::clicked, this, [this] { toggle_all_asked(); });
		connect(addKey, &QPushButton::clicked, this, [this] {
			livePop->hide();
			edit_dest(SvDest());
		});
		/* With no platform added yet, Go live opens the list so one can be added. */
		connect(goKey, &QPushButton::clicked, this, [this] {
			if (sv_output_dests().empty())
				livePop->open_under(this, statusRow);
			else
				toggle_all_asked();
		});
		connect(goMore, &QPushButton::clicked, this, [this] { livePop->open_under(this, statusRow); });
		connect(recKey, &QPushButton::clicked, this, [this] { act([] { sv_action_toggle(SV_RECORD_ID); }); });
		connect(pauseKey, &QPushButton::clicked, this, [this] { act([] { sv_action_pause_record(-1); }); });
		connect(btKey, &QPushButton::clicked, this, [this] { act([] { sv_action_toggle(SV_BACKTRACK_ID); }); });
		connect(vcKey, &QPushButton::clicked, this, [this] { act([] { sv_action_toggle(SV_VCAM_ID); }); });
		connect(clipKey, &QPushButton::clicked, this, [this] { act([] { sv_action_save_backtrack(); }); });
		connect(replayKey, &QPushButton::clicked, this, [this] { act([] { sv_action_toggle_replay(); }); });
		connect(clipPlayKey, &QPushButton::clicked, this, [this] { act([] { sv_replay_save_and_play(); }); });
		connect(replayMore, &QPushButton::clicked, this, [this] {
			fill_clips();
			clipsPop->open_over(this, clipsRow);
		});
		connect(clipsRowKey, &QPushButton::clicked, this, [this] {
			/* The list is newest first; a row plays in the order they were saved. */
			std::vector<SvClip> picked;
			for (auto it = clipChecks.rbegin(); it != clipChecks.rend(); ++it)
				if (it->first->isChecked())
					picked.push_back(it->second);
			if (picked.size() < 2)
				return;
			clipsPop->hide();
			act([picked] { sv_replay_play_row(picked); });
		});
		connect(replayPauseKey, &QPushButton::clicked, this, [this] { steer_replay([] { sv_replay_pause_toggle(); }); });
		connect(replayBackKey, &QPushButton::clicked, this, [this] { steer_replay([] { sv_replay_back(); }); });
		connect(replayRestartKey, &QPushButton::clicked, this, [this] { steer_replay([] { sv_replay_restart(); }); });
		connect(replaySpeedKey, &QPushButton::clicked, this, [this] { steer_replay([] { sv_replay_speed_next(); }); });
		connect(replaySlowKey, &QPushButton::clicked, this, [this] { steer_replay([] { sv_replay_slow_press(); }); });
		connect(hudKey, &QPushButton::clicked, this, [this] { act([] { sv_action_toggle_hud(); }); });
		connect(guidesKey, &QPushButton::clicked, this, [this] { show_guides_menu(); });
		connect(clipOpen, &QPushButton::clicked, this, [this] {
			if (!clipPath.isEmpty())
				QDesktopServices::openUrl(QUrl::fromLocalFile(clipPath));
		});
		connect(clipShow, &QPushButton::clicked, this, [this] {
			if (clipPath.isEmpty())
				return;
			QProcess explorer;
			explorer.setProgram("explorer.exe");
			explorer.setNativeArguments("/select,\"" + QDir::toNativeSeparators(clipPath) + "\"");
			explorer.startDetached();
		});
		connect(clipClose, &QPushButton::clicked, this, [this] { clipCard->hide(); });

		clipTimer = new QTimer(this);
		clipTimer->setSingleShot(true);
		clipTimer->setInterval(30000);
		connect(clipTimer, &QTimer::timeout, this, [this] { clipCard->hide(); });

		/* Counts the clocks and samples stream health only while something runs or
		   the panel is on screen. Nothing here goes over the network. */
		tick = new QTimer(this);
		tick->setInterval(1000);
		connect(tick, &QTimer::timeout, this, [this] {
			if (sv_output_any_running())
				sv_output_sample();
			refresh_running();
		});

		soon = new QTimer(this);
		soon->setSingleShot(true);
		soon->setInterval(120);
		connect(soon, &QTimer::timeout, this, [this] { refresh(); });

		seenClip = sv_output_last_clip().seq;

		QPointer<SvDock> self(this);
		auto later = [self] {
			QMetaObject::invokeMethod(
				qApp,
				[self] {
					if (self)
						self->soon->start();
				},
				Qt::QueuedConnection);
		};
		SvLinkListener l;
		l.onStatus = later;
		l.onLayout = [later](const std::string &) { later(); };
		listener = sv_link_listen(std::move(l), nullptr);
		sv_follow_set_listener(later);
		sv_source_set_picks_listener([self] {
			QMetaObject::invokeMethod(
				qApp,
				[self] {
					if (self)
						self->adopt_source_picks();
				},
				Qt::QueuedConnection);
		});
		sv_output_set_listener([self] {
			QMetaObject::invokeMethod(
				qApp,
				[self] {
					if (self) {
						self->refresh_running();
						self->refresh_editing();
					}
				},
				Qt::QueuedConnection);
		});

		/* The vertical's scenes. The scene being watched changes straight away,
		   because OBS may be about to remove the old one; the lists catch up a
		   moment later. */
		sv_canvas_set_listener([self] {
			if (!self)
				return;
			self->watch_scene();
			QMetaObject::invokeMethod(
				qApp,
				[self] {
					if (self) {
						self->refresh_editing();
						self->refresh_layouts();
					}
				},
				Qt::QueuedConnection);
		});
		signal_handler_connect(obs_get_signal_handler(), "source_rename", on_scene_signal, this);
		watch_scene();

		refresh();
	}

	~SvDock() override
	{
		unlisten();
		unwatch();
		/* The Scenes and Sources go with this panel even while they sit in panels of their own. */
		delete statsCard;
		delete scenesPart.data();
		delete sourcesPart.data();
	}

	/* The panels the Scenes and the Sources pop out into, handed to OBS as panels of their own. */
	QWidget *scenes_panel() const { return scenesSide; }
	QWidget *sources_panel() const { return sourcesSide; }

	QDockWidget *part_dock(bool scenes) const
	{
		QWidget *side = scenes ? static_cast<QWidget *>(scenesSide) : static_cast<QWidget *>(sourcesSide);
		return side ? qobject_cast<QDockWidget *>(side->parentWidget()) : nullptr;
	}

	/* A popped out part's OBS panel is the truth: open means out, closed (by its X, the
	   Docks menu or Put back) means back in this panel, so a part can never go missing. */
	void watch_part_docks()
	{
		for (bool scenes : {true, false}) {
			if (QDockWidget *d = part_dock(scenes)) {
				d->removeEventFilter(this);
				d->installEventFilter(this);
				set_part_out(scenes, !d->isHidden());
			}
		}
	}

	/* Puts both parts back in this panel and closes their own panels. */
	void put_back_both()
	{
		for (bool scenes : {true, false}) {
			if (QDockWidget *d = part_dock(scenes))
				d->hide();
			set_part_out(scenes, false);
		}
	}

	void shut()
	{
		unlisten();
		unwatch();
		tick->stop();
		sv_follow_set_listener(nullptr);
		sv_source_set_picks_listener(nullptr);
		sv_output_set_listener(nullptr);
		sv_source_set_measuring(false);
		preview->shut();
	}

	/* The panel tour (SvTour): the canvas, then the keys under it, then the menu. A key that
	   is not on screen right now (folded away, or another app runs the vertical) is skipped. */
	void start_tour()
	{
		sv_store_set_int("tour_done", 1);
		reveal_dock();
		std::vector<SvTourStep> list = {
			{preview, T("TourPhoneHead"), T("TourPhone")},
			{goKey, T("TourGoHead"), T("TourGo")},
			{goMore, T("TourMoreHead"), T("TourMore")},
			{recKey, T("TourRecHead"), T("TourRec")},
			{btKey, T("TourBtHead"), T("TourBt")},
			{layoutNameKey, T("TourLayoutsHead"), T("TourLayouts")},
			{clipKey, T("TourClipHead"), T("TourClip")},
			{replayKey, T("TourReplayHead"), T("TourReplay")},
			{editSceneKey, T("TourSceneHead"), T("TourScene")},
			{guidesKey, T("TourGuidesHead"), T("TourGuides")},
			{settingsKey, T("TourMenuHead"), T("TourMenuText")},
		};
		if (tour)
			tour->close();
		tour = new SvTour(this, std::move(list));
	}

	void refresh()
	{
		refresh_link();
		refresh_route();
		refresh_layouts();
		refresh_running();
		refresh_guides();
		refresh_editing();
	}

	void refresh_editing()
	{
		if (scenesPart)
			scenesCard->refresh();
		refresh_sources();
	}

	void refresh_sources()
	{
		if (sourcesCard)
			sourcesCard->refresh();
	}

protected:
	/* The website's lamp under the header, its light falling over the scenes and the canvas. */
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const int lampY = headerRow ? headerRow->geometry().bottom() + 11 : -1;
		/* The canvas's surround reads this, so the lamp's light lines up behind it too. */
		setProperty("svLampY", lampY);
		svl::paint_backdrop(p, rect(), true, lampY);
	}

	/* The control row changed width, so the button names are fitted again. A part's own
	   panel opened or closed, so the part moves there or comes back here. */
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (o == ctrlRow && e->type() == QEvent::Resize) {
			fit_tools();
			fit_replay_row();
		}
		if (o == updateLine && e->type() == QEvent::MouseButtonRelease &&
		    static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
			open_help();
			return true;
		}
		/* A click on the missed frames opens the live stats. */
		if ((o == statsIcon || o == missedLine) && e->type() == QEvent::MouseButtonRelease &&
		    static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton && statsIcon && statsIcon->isVisible()) {
			open_stats(statsIcon);
			return true;
		}
		if (e->type() == QEvent::Show || e->type() == QEvent::Hide) {
			for (bool scenes : {true, false}) {
				QDockWidget *d = part_dock(scenes);
				if (!d || o != d)
					continue;
				if (e->type() == QEvent::Show && !d->isHidden())
					set_part_out(scenes, true);
				else if (e->type() == QEvent::Hide && d->isHidden())
					set_part_out(scenes, false);
			}
		}
		return QWidget::eventFilter(o, e);
	}

	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		sv_source_set_measuring(true);
		refresh_running();
		if (!own)
			refresh_zoom();
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		sv_source_set_measuring(false);
		refresh_running();
		zoomTimer->stop();
	}

private:
	/* Anything that changes the layers of the scene on the picture. */
	static constexpr const char *SCENE_SIGNALS[] = {"item_add",      "item_remove",  "reorder",
							"refresh",       "item_select",  "item_deselect",
							"item_visible",  "item_locked"};

	static void on_scene_signal(void *data, calldata_t *)
	{
		QPointer<SvDock> self(static_cast<SvDock *>(data));
		QMetaObject::invokeMethod(
			qApp,
			[self] {
				if (self)
					self->refresh_sources();
			},
			Qt::QueuedConnection);
	}

	void watch_scene()
	{
		obs_scene_t *scene = sv_canvas_scene_ref(sv_canvas_view());
		obs_source_t *now = scene ? obs_scene_get_source(scene) : nullptr;
		if (now != watched) {
			if (watched) {
				signal_handler_t *sh = obs_source_get_signal_handler(watched);
				for (const char *sig : SCENE_SIGNALS)
					signal_handler_disconnect(sh, sig, on_scene_signal, this);
				obs_source_release(watched);
				watched = nullptr;
			}
			if (now) {
				watched = obs_source_get_ref(now);
				signal_handler_t *sh = obs_source_get_signal_handler(watched);
				for (const char *sig : SCENE_SIGNALS)
					signal_handler_connect(sh, sig, on_scene_signal, this);
			}
		}
		obs_scene_release(scene);
	}

	void unwatch()
	{
		sv_canvas_set_listener(nullptr);
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_scene_signal, this);
		if (watched) {
			signal_handler_t *sh = obs_source_get_signal_handler(watched);
			for (const char *sig : SCENE_SIGNALS)
				signal_handler_disconnect(sh, sig, on_scene_signal, this);
			obs_source_release(watched);
			watched = nullptr;
		}
	}

	void unlisten()
	{
		if (listener >= 0)
			sv_link_unlisten(listener);
		listener = -1;
	}

	template<typename F> void act(F action)
	{
		action();
		refresh_running();
	}

	/* Reads the clip folder again each time the list opens, so it is always current. */
	void fill_clips()
	{
		/* The old rows leave the list now, not later: rows still waiting to be deleted were
		   counted when the popup measured itself, so it grew taller on every open. */
		for (QWidget *w : clipsHost->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
			clipsList->removeWidget(w);
			w->hide();
			w->deleteLater();
		}
		clipChecks.clear();
		const std::vector<SvClipInfo> list = sv_output_recent_clips(10);
		const std::string last = sv_replay_last_played();
		const QDate today = QDate::currentDate();
		for (const SvClipInfo &c : list) {
			const QDateTime at = QDateTime::fromSecsSinceEpoch(c.savedAt);
			const QString when = at.date() == today ? QLocale().toString(at.time(), QLocale::ShortFormat)
								: QLocale().toString(at, QLocale::ShortFormat);
			const QString text = c.game.empty() ? QString(T("ClipsRow")).arg(when).arg(c.clip.seconds)
							    : QString(T("ClipsRowGame")).arg(when).arg(c.clip.seconds).arg(qs(c.game));
			auto *row = new QWidget(clipsHost);
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(6);
			auto *tick = new QCheckBox(text, row);
			tick->setToolTip(T("ClipsTickTip"));
			h->addWidget(tick, 1);
			if (c.clip.path == last)
				h->addWidget(say(T("ClipsLastPlayed"), svl::EMERALD), 0, Qt::AlignVCenter);
			auto *play = key(QString(), "play", svl::EMERALD);
			play->setToolTip(T("ClipsPlayOneTip"));
			h->addWidget(play);
			clipsList->addWidget(row);
			clipChecks.push_back({tick, c.clip});
			const SvClip clip = c.clip;
			connect(play, &QPushButton::clicked, this, [this, clip] {
				clipsPop->hide();
				act([clip] { sv_replay_play_row({clip}); });
			});
			connect(tick, &QCheckBox::toggled, this, [this] { refresh_clips_row_key(); });
		}
		clipsEmpty->setVisible(list.empty());
		clipsHost->setVisible(!list.empty());
		clipsRowKey->setVisible(list.size() >= 2);
		clipsCard->setValue(list.empty() ? QString() : QString::number((int)list.size()));
		refresh_clips_row_key();
		refresh_replay_place();
	}

	void refresh_replay_place()
	{
		const bool box = replay_in_box();
		replayWholeKey->setUnchosen(box);
		replayBoxKey->setUnchosen(!box);
	}

	void refresh_clips_row_key()
	{
		int ticked = 0;
		for (const auto &c : clipChecks)
			ticked += c.first->isChecked() ? 1 : 0;
		clipsRowKey->setEnabled(ticked >= 2);
		clipsRowKey->setText(ticked >= 2 ? QString(T("ClipsPlayRowCount")).arg(ticked) : QString(T("ClipsPlayRow")));
	}

	/* A replay control. A speed change takes a moment to land, so the keys look again
	   once it has. */
	template<typename F> void steer_replay(F action)
	{
		action();
		refresh_replay_row();
		QTimer::singleShot(500, this, [this] { refresh_replay_row(); });
	}

	void refresh_replay_row()
	{
		if (!replayRow)
			return;
		const bool replaying = sv_replay_playing();
		replayRow->setVisible(replaying && own);
		if (!replaying)
			return;
		const bool paused = sv_replay_paused();
		replayPauseKey->setText(paused ? T("ReplayResume") : T("ReplayPause"));
		replayPauseKey->setIconName(paused ? "play" : "pause");
		replayPauseKey->setToolTip(paused ? T("ReplayResumeTip") : T("ReplayPauseTip"));
		replaySpeedKey->setText(QStringLiteral("%1%").arg(sv_replay_speed()));
		const bool press = sv_replay_press_mode();
		replaySpeedKey->setToolTip(press ? T("ReplaySpeedPressTip") : T("ReplaySpeedTip"));
		/* Slow motion: only when What slows down is When I press. While slow it says the
		   next press goes back to normal speed. */
		const bool slowed = press && sv_replay_press_slowed();
		replaySlowKey->setVisible(press);
		replaySlowKey->setIconName(slowed ? "play" : "snail");
		replaySlowKey->setToolTip(slowed ? T("ReplaySlowKeyBackTip") : T("ReplaySlowKeyTip"));
		fit_replay_row();
	}

	/* The names go when the dock is too narrow for them all, leaving the icons (and the
	   speed, which is the key's whole point). Slow motion keeps its name longest. */
	void fit_replay_row()
	{
		if (!replayRow || !ctrlRow)
			return;
		const bool paused = sv_replay_paused();
		const bool slowShown = !replaySlowKey->isHidden();
		const bool slowed = slowShown && sv_replay_press_slowed();
		const QString names[4] = {slowed ? T("ReplaySlowKeyBack") : T("ReplaySlowKey"),
					  paused ? T("ReplayResume") : T("ReplayPause"), T("ReplayBack"), T("ReplayRestart")};
		svl::Key *keys[4] = {replaySlowKey, replayPauseKey, replayBackKey, replayRestartKey};
		const QString icons[5] = {slowed ? QStringLiteral("play") : QStringLiteral("snail"), paused ? QStringLiteral("play") : QStringLiteral("pause"),
					  QStringLiteral("rotate-ccw"), QStringLiteral("skip-back"), QStringLiteral("gauge")};
		svl::Key *all[5] = {replaySlowKey, replayPauseKey, replayBackKey, replayRestartKey, replaySpeedKey};
		for (int i = 0; i < 4; i++)
			keys[i]->setText(names[i]);
		for (int i = 0; i < 5; i++)
			all[i]->setIconName(icons[i]);
		auto need = [&] {
			int w = 0, n = 0;
			for (svl::Key *k : {replaySlowKey, replayPauseKey, replayBackKey, replayRestartKey, replaySpeedKey})
				if (!k->isHidden()) {
					w += k->sizeHint().width();
					n++;
				}
			return w + (n - 1) * 8;
		};
		/* Nick's rule: a key never shows a symbol on its own; the word always stays. Too
		   narrow: the icons go first, then Slow motion takes its short name. */
		const int room = ctrlRow->width();
		if (need() > room)
			for (int i = 0; i < 5; i++)
				all[i]->setIconName(QString());
		if (need() > room && slowShown)
			replaySlowKey->setText(slowed ? T("ReplaySlowKeyBackShort") : T("ReplaySlowKeyShort"));
	}

	/* Going live and stopping from the panel ask first (each can be switched off in Settings,
	   Stream tab), so a stray click never starts or ends a stream. Hotkeys, Stream Deck keys,
	   the web addresses and Start with OBS never ask: pressing those is already on purpose.
	   Enter and Escape both pick the safe answer. */
	bool ask_live(const std::vector<std::string> &ids, bool start)
	{
		if (ids.empty() || sv_store_int(start ? "ask_go_live" : "ask_stop_live", 1) == 0)
			return true;
		QStringList names;
		for (const SvDest &d : sv_output_dests())
			if (std::find(ids.begin(), ids.end(), d.id) != ids.end())
				names << qs(d.name);
		QString list = names.join(", ");
		if (names.size() > 1)
			list = QStringList(names.mid(0, names.size() - 1)).join(", ") + " " + T("AndWord") + " " +
			       names.last();
		SvAskDialog box(this, start, QString(T(start ? "AskLiveText" : "AskStopText")).arg(list));
		return box.exec() == QDialog::Accepted;
	}

	/* Go live and Go live on all: stops everything if anything is live, otherwise starts
	   everything, asking first. */
	void toggle_all_asked()
	{
		std::vector<std::string> live, off;
		for (const SvDest &d : sv_output_dests()) {
			if (sv_output_info(d.id).state != SvRunState::Off)
				live.push_back(d.id);
			else if (d.on)
				off.push_back(d.id); /* switched off platforms stay off */
		}
		const bool stopping = !live.empty();
		if (!ask_live(stopping ? live : off, !stopping))
			return;
		act([stopping] {
			if (stopping)
				sv_action_stop_all();
			else
				sv_action_go_live_all();
		});
	}

	/* The gear's menu, built like every other pop out in the panel (Go live, Recent clips): a lit
	   violet card under the gear, holding raised keys, each with its word and its own colour.
	   The panel's own things first, then setup and help on a second line. Made fresh each time,
	   since Link and Put back change. */
	void show_menu()
	{
		auto *pop = new SvPopup(this);
		pop->setAttribute(Qt::WA_DeleteOnClose);
		auto *card = new svl::Card(T("MenuTitle"), "settings");
		card->setAccent(svl::VIOLET);
		/* One column, top to bottom, every key as wide as the widest word, so it reads like a
		   menu (Nick, 2026-11-21: not a wide box of keys side by side). Each key closes the card
		   first, then does its job. */
		std::vector<svl::Key *> keys;
		auto item = [pop, &keys](QVBoxLayout *col, const QString &word, const char *iconName, const QColor &colour,
					 const QString &tip, std::function<void()> job) {
			svl::Key *k = key(word, QString::fromUtf8(iconName), colour);
			if (!tip.isEmpty())
				k->setToolTip(tip);
			QObject::connect(k, &QPushButton::clicked, pop, [pop, job] {
				pop->close();
				QTimer::singleShot(0, job);
			});
			col->addWidget(k, 0, Qt::AlignHCenter);
			keys.push_back(k);
		};
		auto *panelCol = new QVBoxLayout();
		panelCol->setContentsMargins(0, 0, 0, 0);
		panelCol->setSpacing(6);
		item(panelCol, T("MenuSettings"), "settings", svl::VIOLET, QString(), [this] {
			SvOutSettingsDialog dialog(this);
			dialog.exec();
			refresh();
		});
		if (own)
			item(panelCol, T("MenuStats"), "gauge", svl::CYAN, QString(), [this] { open_stats(settingsKey); });
		if (own && (scenesOut || sourcesOut))
			item(panelCol, T("MenuPutBackShort"), "putback", svl::ORANGE, T("MenuPutBack"),
			     [this] { put_back_both(); });
		card->body()->addLayout(panelCol);
		card->body()->addWidget(new RowRule(svl::VIOLET));
		auto *helpCol = new QVBoxLayout();
		helpCol->setContentsMargins(0, 0, 0, 0);
		helpCol->setSpacing(6);
		item(helpCol, T("SetupButton"), "tools", svl::EMERALD, QString(), [] { show_welcome(); });
		item(helpCol, T("TourMenu"), "canvas", svl::PINK, QString(), [this] { start_tour(); });
		if (sv_link_state().kind == SvLinkKind::NotLinked)
			item(helpCol, T("LinkButton"), "link", svl::BLUE, QString(), [] { sv_link_request(); });
		else
			item(helpCol, T("UnlinkButton"), "unlink", svl::AMBER, QString(), [this] {
				if (QMessageBox::question(this, T("UnlinkButton"), T("UnlinkConfirm")) == QMessageBox::Yes)
					sv_link_unlink();
			});
		item(helpCol, T("MenuHelp"), "help", svl::YELLOW, QString(), [] { open_help(); });
		card->body()->addLayout(helpCol);
		int widest = 0;
		for (svl::Key *k : keys)
			widest = (std::max)(widest, k->sizeHint().width());
		for (svl::Key *k : keys)
			k->setFixedWidth(widest);
		pop->hold(card);
		pop->open_under_key(settingsKey);
	}

	/* The app guides: each app's guides as a small phone to pick from, or hide them. */
	void show_guides_menu()
	{
		auto *picker = new SvGuidePicker(
			this, preview->guides(), preview->guidesApp(), preview->guidesReal(),
			[this](Zones z) {
				preview->setGuidesApp(z);
				preview->setGuides(true);
				refresh_guides();
			},
			[this] {
				preview->setGuides(false);
				refresh_guides();
			},
			[this](bool real) {
				preview->setGuidesReal(real);
				/* Picking a look shows the guides, so the choice can be seen straight away. */
				if (!preview->guides()) {
					preview->setGuides(true);
					refresh_guides();
				}
			});
		picker->open_above(guidesKey);
	}

	void refresh_guides()
	{
		const bool on = preview->guides();
		const Zones chosen = preview->guidesApp();
		guidesKey->setAccent(on ? zone_set(chosen).colour : svl::LIME);
		guidesKey->setToolTip(on ? QString(T("GuidesShowing")).arg(T(zone_set(chosen).name)) : QString(T("GuidesTip")));
	}

	/* Moves the layouts to where they belong: in the panel when the plugin does not run the
	   vertical, behind the Layouts key when it does. */
	void place_layouts()
	{
		if (own) {
			layoutPop->hold(layoutCard);
			return;
		}
		if (layoutCard->parentWidget() != inlinePage) {
			if (QWidget *old = layoutCard->parentWidget())
				if (old->layout())
					old->layout()->removeWidget(layoutCard);
			inlineLayout->insertWidget(0, layoutCard);
			layoutCard->show();
		}
	}

	void refresh_link()
	{
		const SvLinkState s = sv_link_state();
		/* Only a problem shows: offline or not linked turn the (i) into a warning.
		   The hover always says the whole story. */
		const bool good = s.kind == SvLinkKind::Linked && !s.offline;
		acctInfo->setWarn(!good && s.kind != SvLinkKind::Waiting);
		acctInfo->setTip(qs(sv_link_status()));
		linkCode = s.kind == SvLinkKind::Waiting ? QString(T("AcctWaiting")).arg(qs(s.code)) : QString();
		linkKey->setVisible(s.kind == SvLinkKind::NotLinked);
		const bool named = s.kind == SvLinkKind::Linked && !s.account.empty();
		linkedName = named ? qs(s.account) : QString();
		linkIcon->setToolTip(named ? QString(T("DockLinkedTo")).arg(linkedName) : QString());
		acct->setToolTip(linkIcon->toolTip());
		const QString newer = qs(sv_link_update_version());
		if (!newer.isEmpty()) {
			updateLine->setText(T("UpdateReady"), svl::BLUE, true);
			updateLine->setToolTip(QString(T("UpdateReadyTip")).arg(newer));
			/* Always one line: as wide as the words need, so it never wraps in a narrow panel. */
			const int one = updateLine->heightForWidth(2000);
			int w = 40;
			while (w < 600 && updateLine->heightForWidth(w) > one)
				w += 2;
			updateLine->setFixedWidth(w);
		}
		updateLine->setVisible(!newer.isEmpty());
		paint_header();
	}

	/* The words in the header: the link code while approving, otherwise the linked
	   channel with its link icon, and the frames OBS missed only while there are some. */
	void paint_header()
	{
		const bool missed = linkCode.isEmpty() && !statSegs.empty();
		statsIcon->setVisible(missed);
		missedLine->setVisible(missed);
		if (missed)
			missedLine->setSegs(statSegs);
		std::vector<svl::Seg> segs;
		if (!linkCode.isEmpty())
			segs.push_back({linkCode, AMBER_INK, true, true});
		else if (!linkedName.isEmpty())
			segs.push_back({linkedName, svl::INK, false, true});
		linkIcon->setVisible(linkCode.isEmpty() && !linkedName.isEmpty());
		acct->setSegs(segs);
		acct->setVisible(!segs.empty());
	}

	/* The live stats card, opened from the missed frames or the menu. */
	void open_stats(QWidget *from)
	{
		if (!statsCard)
			statsCard = new SvStatsCard();
		statsCard->toggle(from);
	}

	void refresh_route()
	{
		const std::string route = sv_store_route();
		own = route == SV_ROUTE_OWN;
		/* Not run by the plugin: no picture or controls, only a note and the layouts. */
		if (scenesPart)
			scenesPart->setVisible(own);
		if (sourcesPart)
			sourcesPart->setVisible(own);
		refresh_fold_card();
		preview->setVisible(own);
		ctrlRow->setVisible(own);
		statusRow->setVisible(own);
		layoutRow->setVisible(own);
		refresh_replay_row();
		refresh_cost();
		noteRow->setVisible(!own);
		inlineScroll->setVisible(!own);
		if (!own) {
			livePop->hide();
			layoutPop->hide();
			clipsPop->hide();
			editPop->hide();
		}
		place_layouts();
		otherBox->setVisible(!own);
		holdRow->setVisible(!own);
		refresh_other();
		scenesSide->setOwn(own);
		sourcesSide->setOwn(own);
		/* Set to another app that has since been removed: say so and offer setup again. */
		const SvOtherApp app = route == SV_ROUTE_OTHER ? sv_detect_other_app() : SvOtherApp();
		const bool otherHere = route == SV_ROUTE_OTHER && !app.name.empty();
		setupKey->setVisible(!otherHere);
		/* The app is there but its vertical is not (switched off, or never set up):
		   wait for it, and our layout goes in the moment it appears. */
		if (otherHere && app.canvas.empty() && g_settled && sv_store_int("other_pending", 0) == 0)
			sv_store_set_int("other_pending", 1);
		const bool waiting = otherHere && sv_store_int("other_pending", 0) != 0;
		/* SE.Live only has a vertical once the member makes one, so offer to open
		   the place where it is made. */
		const bool seWaiting = waiting && app.name == "SE.Live";
		seSetupKey->setVisible(seWaiting);
		if (!seWaiting)
			seNeedsSignIn = false;
		if (seWaiting) {
			note->setText(T(seNeedsSignIn ? "DockSeSignIn" : "DockSeWaiting"), AMBER_INK);
		} else if (waiting) {
			note->setText(QString(T("DockOtherWaiting")).arg(qs(app.name)), AMBER_INK);
		} else if (otherHere) {
			note->setText(QString(T("DockOtherNote")).arg(qs(app.name)), svl::INK2);
		} else if (route == SV_ROUTE_OTHER) {
			note->setText(T("DockOtherGone"), AMBER_INK);
		} else if (!own) {
			note->setText(T("DockNotSetUp"), AMBER_INK);
		}
		layoutKeys->relayout();
		fit_height();
	}

	/* SE.Live's vertical is made inside its own Multi-Streaming box, a web page we cannot
	   press buttons in. So bring that box into view for the member, and the note says
	   which buttons to press there. No box means they are not signed in to StreamElements:
	   open the StreamElements menu instead, where Login is. */
	void open_se_setup()
	{
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (!main)
			return;
		for (QDockWidget *d : main->findChildren<QDockWidget *>()) {
			const QString id = d->objectName().toLower();
			const bool box = d->windowTitle() == "StreamElements Multi-Streaming" ||
					 (id.contains("multistreaming") && !id.contains("canvas"));
			if (!box)
				continue;
			seNeedsSignIn = false;
			d->show();
			d->raise();
			d->activateWindow();
			refresh_route();
			return;
		}
		seNeedsSignIn = true;
		refresh_route();
		if (QMenuBar *bar = main->menuBar()) {
			for (QAction *a : bar->actions()) {
				if (a->menu() && a->text().remove('&') == "StreamElements") {
					bar->setActiveAction(a);
					return;
				}
			}
		}
	}

	/* With another app running the vertical, the panel takes only the height it needs,
	   so that app's own preview, often just below in the same column, gets the room. */
	void fit_height()
	{
		QPointer<SvDock> self(this);
		QTimer::singleShot(0, this, [self] {
			if (!self)
				return;
			if (self->own) {
				self->setMaximumHeight(QWIDGETSIZE_MAX);
				return;
			}
			self->layout()->activate();
			/* A scroll area reports a small fixed size, not its contents, so swap in
			   the height the layouts actually need at the panel's width. */
			int h = self->layout()->totalSizeHint().height();
			if (QWidget *page = self->inlineScroll->widget()) {
				const int w = self->inlineScroll->viewport()->width();
				const int need = page->hasHeightForWidth() ? page->heightForWidth(w)
									   : page->sizeHint().height();
				h += need - self->inlineScroll->sizeHint().height();
			}
			self->setMaximumHeight(std::max(h, self->layout()->totalMinimumSize().height()));
		});
	}

	void refresh_layouts()
	{
		const std::vector<SvLayoutChoice> list = sv_link_presets();
		/* An extra canvas has its own pick. Its first tile follows the main canvas instead of the website. */
		const std::string view = sv_canvas_view();
		const std::string active = view.empty() ? sv_link_active_preset() : sv_canvas_layout_pick(view);

		/* The pictures are only redrawn when a layout really changed. */
		std::string signature = view.empty() ? "main\n" : "extra\n";
		for (const SvLayoutChoice &c : list)
			signature += c.id + "=" + c.name + "#" + std::to_string(std::hash<std::string>{}(c.drawList)) + "\n";
		if (signature != layoutSignature || layoutTiles.empty()) {
			layoutSignature = signature;
			tilesFlow->clear();
			layoutTiles.clear();
			auto *site = new svl::LayoutTile();
			site->setWebsite(view.empty() ? T("LayoutWebsiteTile") : T("LayoutSameAsMain"));
			add_layout_tile(site, "");
			int n = 1;
			for (const SvLayoutChoice &c : list) {
				SvLayout layout;
				std::string err;
				if (!sv_layout_parse(c.drawList.c_str(), layout, err))
					layout = SvLayout();
				auto *t = new svl::LayoutTile();
				t->setLayout(qs(c.name), layout, n++);
				add_layout_tile(t, c.id);
			}
			tilesFlow->relayout();
		}
		QString activeName;
		for (const SvLayoutChoice &c : list) {
			if (c.id == active)
				activeName = qs(c.name);
		}
		for (auto &t : layoutTiles)
			t.first->setSelected(t.second == active);
		const QString following = view.empty() ? QString(T("LayoutFollowing")) : QString(T("LayoutSameAsMain"));
		layoutCard->setValue(active.empty() || activeName.isEmpty() ? following : activeName);
		if (followGame->isChecked() != sv_follow_on()) {
			const QSignalBlocker quiet(followGame);
			followGame->setChecked(sv_follow_on());
		}
		if (holdBox->isChecked() != sv_link_holding()) {
			const QSignalBlocker quiet(holdBox);
			holdBox->setChecked(sv_link_holding());
		}
		const std::string seen = sv_follow_seen();
		followSeen->setText(QString(T("FollowSeeing")).arg(game_title(seen)), svl::GREEN);
		followSeen->setVisible(!own && !seen.empty());
		/* With nothing saved there is nothing to pick, so say how to get layouts here instead of one lone tile. */
		tilesFlow->setVisible(!list.empty());
		layoutEmpty->setVisible(list.empty());

		const std::string collection = current_collection();
		/* How many main scenes switch something in the vertical. */
		std::vector<std::string> linked;
		std::vector<std::string> linkLists{SV_STORE_SCENE_LINKS, SV_STORE_VSCENE_LINKS};
		if (own && sv_canvas_running())
			for (const SvVCanvas &c : sv_canvases())
				if (!c.main)
					linkLists.push_back(sv_store_canvas_links(c.uuid));
		for (const std::string &listName : linkLists) {
			for (const std::string &link : sv_store_list(collection, listName.c_str())) {
				const std::string scene = link.substr(0, link.find('|'));
				if (std::find(linked.begin(), linked.end(), scene) == linked.end())
					linked.push_back(scene);
			}
		}
		const int scenes = (int)linked.size();
		/* Green while some OBS scene switches the vertical, so the member can see it is set up. */
		scenesKey->setAccent(scenes > 0 ? svl::GREEN : svl::BLUE);
		scenesKey->setToolTip(scenes > 0 ? QString(T("ScenesCount")).arg(scenes) + ". " + T("ScenesLinkTip")
						 : QString(T("ScenesLinkTip")));
		layoutInfo->setTip(!view.empty()       ? T("LayoutCanvasNote")
				   : !active.empty() ? T("LayoutPickedNote")
				   : list.empty()  ? T("LayoutNoneSaved")
						   : T("LayoutTilesNote"));
		layoutKeys->relayout();
		/* The layout row: the name of the layout showing, the arrows step through them all
		   (Follow my game first), so they only show with something to step to. */
		const QString showing = active.empty() || activeName.isEmpty() ? following : activeName;
		layoutNameKey->setText(showing);
		layoutNameKey->setToolTip(QString(T("LayoutsKeyTip")).arg(showing));
		layoutPrevKey->setVisible(!list.empty());
		layoutNextKey->setVisible(!list.empty());
		layoutRow->setVisible(own);
		if (!own)
			fit_height();
	}

	void add_layout_tile(svl::LayoutTile *t, const std::string &id)
	{
		tilesFlow->add(t);
		layoutTiles.push_back({t, id});
		connect(t, &QAbstractButton::clicked, this, [this, id] {
			const std::string view = sv_canvas_view();
			if (view.empty())
				sv_action_pick_layout(id);
			else
				sv_canvas_set_layout_pick(view, id);
			refresh_layouts();
		});
	}

	/* A game's id from the website ("apex-legends") as words ("Apex Legends"). */
	static QString game_title(const std::string &id)
	{
		QString out;
		bool start = true;
		for (const QChar c : qs(id)) {
			if (c == '-' || c == '_') {
				out += ' ';
				start = true;
			} else {
				out += start ? c.toUpper() : c;
				start = false;
			}
		}
		return out;
	}

	/* The parts only shown while another app runs the vertical. */
	void refresh_other()
	{
		if (own) {
			zoomTimer->stop();
			return;
		}
		refresh_zoom();
		refresh_picks();
		refresh_links();
		/* The source notes a new layout's zooms on its own thread, maybe just after this. */
		QPointer<SvDock> self(this);
		QTimer::singleShot(400, this, [self] {
			if (self && !self->own)
				self->refresh_zoom();
		});
	}

	/* A key for every zoom in the layout on the stream. Lit green while zoomed in. */
	void refresh_zoom()
	{
		const std::vector<SvZoomInfo> list = sv_zoom_list();
		std::string signature;
		for (const SvZoomInfo &z : list)
			signature += z.name + (z.hold ? "|h\n" : "\n");
		if (signature != zoomSignature) {
			zoomSignature = signature;
			zoomFlow->clear();
			zoomKeys.clear();
			for (const SvZoomInfo &z : list) {
				const std::string k = sv_zoom_key_for(z.name);
				auto *b = key(qs(z.name), "zoom", svl::VIOLET);
				b->setToolTip(T("ZoomKeyTip"));
				zoomFlow->add(b);
				zoomKeys.push_back({b, k});
				connect(b, &QPushButton::clicked, this, [this, k] {
					sv_zoom_do(k, SvZoomDo::Flip);
					refresh_zoom();
				});
			}
			zoomFlow->setVisible(!list.empty());
			zoomEmpty->setVisible(list.empty());
			zoomLit.assign(list.size(), false);
			fit_height();
		}
		for (size_t i = 0; i < zoomKeys.size() && i < list.size(); i++) {
			if (zoomLit[i] == list[i].zoomed)
				continue;
			zoomLit[i] = list[i].zoomed;
			zoomKeys[i].first->setAccent(list[i].zoomed ? svl::GREEN : svl::VIOLET);
		}
		const bool watch = !own && !list.empty() && isVisible();
		if (watch && !zoomTimer->isActive())
			zoomTimer->start();
		else if (!watch && zoomTimer->isActive())
			zoomTimer->stop();
	}

	static void fill_pick(QComboBox *box, const std::vector<SvFound> &found, const std::string &want,
			      int SvFound::*score)
	{
		std::vector<SvFound> sorted = found;
		std::stable_sort(sorted.begin(), sorted.end(),
				 [score](const SvFound &a, const SvFound &b) { return a.*score > b.*score; });
		box->clear();
		box->addItem(T("NoSource"), QString());
		for (const SvFound &f : sorted)
			box->addItem(qs(f.name), qs(f.uuid));
		const int at = box->findData(qs(want));
		box->setCurrentIndex(at >= 0 ? at : 0);
	}

	/* The game and camera the layout cuts from, changeable here without running setup again. */
	void refresh_picks()
	{
		if (gameBox->view() && gameBox->view()->isVisible())
			return; /* never refill a list the member has open */
		if (camBox->view() && camBox->view()->isVisible())
			return;
		const std::vector<SvFound> found = sv_detect_sources();
		const SvPicks now = sv_store_current_picks();
		std::string signature = now.game + "|" + now.cam + "\n";
		for (const SvFound &f : found)
			signature += f.uuid + "=" + f.name + "\n";
		if (signature == picksSignature)
			return;
		picksSignature = signature;
		fill_pick(gameBox, found, now.game, &SvFound::gameScore);
		fill_pick(camBox, found, now.cam, &SvFound::camScore);
		const bool hasGame = !gameBox->currentData().toString().isEmpty();
		picksCard->setValue(hasGame ? QString() : QString(T("PicksNoGame")));
		picksCard->setValueColour(AMBER_INK);
	}

	void save_picks()
	{
		SvPicks picks;
		picks.game = gameBox->currentData().toString().toStdString();
		picks.cam = camBox->currentData().toString().toStdString();
		sv_store_set_picks(current_collection(), picks);
		sv_store_set_current_picks(picks);
		sv_detect_apply_picks(picks);
		picksSignature.clear();
		refresh_picks();
	}

	/* A source's game or camera was changed in its own Properties: that becomes the
	   pick, so the lists here show what the layout really uses. Other copies are
	   left as they are. */
	void adopt_source_picks()
	{
		SvPicks got;
		if (!sv_detect_source_picks(sv_store_current_picks(), got))
			return;
		sv_store_set_picks(current_collection(), got);
		sv_store_set_current_picks(got);
		picksSignature.clear();
		if (!own)
			refresh_picks();
	}

	/* Which OBS scenes change the layout, as "scene, arrow, layout" lines. */
	void refresh_links()
	{
		const std::string collection = current_collection();
		const std::vector<SvLayoutChoice> layouts = sv_link_presets();
		std::vector<std::pair<QString, QString>> shown;
		for (const std::string &link : sv_store_list(collection, SV_STORE_SCENE_LINKS)) {
			const size_t bar = link.find('|');
			if (bar == std::string::npos)
				continue;
			const std::string value = link.substr(bar + 1);
			QString layoutName;
			if (value == SV_SCENE_LINK_SITE)
				layoutName = T("LayoutWebsite");
			for (const SvLayoutChoice &c : layouts)
				if (c.id == value)
					layoutName = qs(c.name);
			obs_source_t *scene = obs_get_source_by_uuid(link.substr(0, bar).c_str());
			if (!scene || layoutName.isEmpty()) {
				obs_source_release(scene);
				continue;
			}
			shown.push_back({QString::fromUtf8(obs_source_get_name(scene)), layoutName});
			obs_source_release(scene);
		}
		QString signature;
		for (const auto &s : shown)
			signature += s.first + "\x1f" + s.second + "\n";
		linksKey->setText(shown.empty() ? T("LinksAdd") : T("LinksEdit"));
		linksHost->setVisible(!shown.empty());
		linksEmpty->setVisible(shown.empty());
		if (signature == linksSignature && linksGrid->count() == (int)shown.size() * 3)
			return;
		linksSignature = signature;
		while (QLayoutItem *it = linksGrid->takeAt(0)) {
			if (QWidget *w = it->widget())
				w->deleteLater();
			delete it;
		}
		int row = 0;
		for (const auto &s : shown) {
			linksGrid->addWidget(say(s.first, svl::INK, 13.5), row, 0);
			linksGrid->addWidget(icon_label("chevron-right", svl::ORANGE, 14), row, 1, Qt::AlignCenter);
			linksGrid->addWidget(say(s.second, svl::ORANGE, 13.5), row, 2);
			row++;
		}
		fit_height();
	}

	/* Everything that changes while streaming: rows, tools, clip, label and cost. */
	void refresh_running()
	{
		if (!own) {
			paint_header();
			update_tick(false);
			report_setup(false);
			return;
		}
		const std::vector<SvDest> list = sv_output_dests();
		const std::vector<SvVCanvas> canvasList = sv_canvas_running() ? sv_canvases() : std::vector<SvVCanvas>();
		/* A platform's on/off switch is left out on purpose: flipping it only changes that
		   row (below), so the Go live card is never torn down and rebuilt, which made it flicker. */
		std::string signature;
		for (const SvDest &d : list)
			signature += d.id + "|" + d.platform + "|" + d.name + "|" + d.canvas + (d.dual ? "|d" : "|s") + "\n";
		for (const SvVCanvas &c : canvasList)
			signature += "c|" + c.uuid + "|" + c.name + "\n";
		if (signature != destSignature) {
			destSignature = signature;
			rebuild_rows(list);
			/* The canvas names show once there is more than one picture to tell apart. */
			const bool several = canvasList.size() > 1 || std::any_of(list.begin(), list.end(), [](const SvDest &d) {
						     return d.canvas == SV_WIDE || d.dual;
					     });
			for (size_t i = 0; i < rows.size() && i < list.size(); i++) {
				DestCanvas dc = dest_canvas(list[i].canvas, canvasList);
				/* Twitch Dual Format carries OBS's wide picture as well as the vertical. */
				if (list[i].dual)
					dc.name = QString(T("DestCanvasDual")).arg(dc.name);
				rows[i]->show_canvas(qs(list[i].name), dc, several);
			}
		}
		/* A switch changed somewhere else (a Stream Deck key, the setup window): the row
		   follows, quietly, so it does not save the same thing again. */
		for (size_t i = 0; i < rows.size() && i < list.size(); i++) {
			if (rows[i]->on == list[i].on && rows[i]->onSwitch->isChecked() == list[i].on)
				continue;
			rows[i]->on = list[i].on;
			const QSignalBlocker quiet(rows[i]->onSwitch);
			rows[i]->onSwitch->setChecked(list[i].on);
		}

		const int delay = sv_output_settings().delay_seconds;
		int running = 0, live = 0, reconnecting = 0;
		uint64_t longest = 0;
		/* For the Go live key's status line: the worst health of any live platform and
		   the bitrate going out in all. */
		SvRunInfo worst;
		bool anyHealth = false;
		double kbps = 0;
		auto rank = [](const SvRunInfo &i) {
			return i.health == SvHealth::Steady ? 0 : i.health == SvHealth::Shaky ? 1 : 2;
		};
		for (DestRow *row : rows) {
			const SvRunInfo info = sv_output_info(row->id);
			row->show_info(info, delay);
			if (info.state != SvRunState::Off)
				running++;
			if (info.state == SvRunState::Reconnecting)
				reconnecting++;
			if (info.state == SvRunState::Live || info.state == SvRunState::Reconnecting) {
				live++;
				longest = (std::max)(longest, info.seconds);
			}
			if (info.state == SvRunState::Live) {
				kbps += info.kbps;
				if (!anyHealth || rank(info) > rank(worst))
					worst = info;
				anyHealth = true;
			}
		}
		const bool streaming = running > 0;
		const int count = (int)rows.size();
		report_setup(live > 0);
		liveCard->setAccent(streaming ? svl::RED : svl::BLUE);
		liveCard->setTitle(streaming ? T("LiveTitleLive") : T("LiveTitle"));
		liveCard->setValue(streaming    ? QString(T("LiveOf")).arg(live).arg(count)
				   : count == 0 ? QString()
				   : count == 1 ? QString(T("LiveCountOne"))
						: QString(T("LiveCountMany")).arg(count));
		goAll->setVisible(count > 0);
		goAll->setText(streaming ? T("StopAll") : T("GoLiveAll"));
		goAll->setIconName(streaming ? "stop" : "radio");
		goAll->setAccent(streaming ? svl::RED : svl::BLUE);
		emptyRow->setVisible(count == 0);
		rowsHost->setVisible(count > 0);


		/* The keys. Each has its own colour so it is easy to find (Go live blue, Record
		   orange, Backtrack violet, Save clip green, Replay emerald, Virtual cam cyan, Guides
		   lime). A key that is running turns red with a stop square, so stopping is always
		   the red key, and its hover says how to stop it. The three status keys also say,
		   on a second line, how things stand. */
		goKey->setAccent(streaming ? svl::RED : svl::BLUE);
		goKey->setIconName(streaming ? "stop" : "radio");
		if (live > 0) {
			goKey->setText(QString(T("HeaderLive")).arg(clock_text(longest)));
			if (reconnecting > 0 && !anyHealth) {
				goKey->setSub(T("StateReconnecting"), AMBER_INK);
			} else {
				QString health;
				QColor ink = GREEN_INK;
				if (worst.health == SvHealth::Steady) {
					health = T("HealthSteady");
				} else if (worst.health == SvHealth::Shaky) {
					health = worst.low_bitrate && worst.dropped == 0 ? QString(T("HealthSlow"))
											 : QString(T("HealthShaky")).arg(worst.dropped);
					ink = AMBER_INK;
				} else {
					health = QString(T("HealthDropping")).arg(worst.dropped);
					ink = PROBLEM;
				}
				if (kbps > 0)
					health += "  " + QString(T("Mbps")).arg(kbps / 1000.0, 0, 'f', 1);
				goKey->setSub(health, ink);
			}
		} else if (streaming) {
			goKey->setText(T("StateStarting"));
			goKey->setSub(T("KeyPressStop"), RED_INK);
		} else {
			const int onCount = (int)std::count_if(list.begin(), list.end(), [](const SvDest &d) { return d.on; });
			goKey->setText(T("CtrlGoLive"));
			goKey->setSub(count == 0     ? QString(T("KeyAddPlatform"))
				      : onCount == 0 ? QString(T("KeyNoneOn"))
				      : onCount == 1 ? QString(T("KeyOnePlatform"))
						     : QString(T("KeyPlatforms")).arg(onCount),
				      svl::tone(svl::BLUE, 0.45));
		}
		goKey->setToolTip(streaming    ? QString(T("LiveOf")).arg(live).arg(count) + ". " + T("StopAll")
				  : count == 0 ? QString(T("AddDestination"))
					       : QString(T("GoLiveAll")));
		goMore->setAccent(streaming ? svl::RED : svl::BLUE);

		const SvRunInfo rec = sv_output_info(SV_RECORD_ID);
		const SvRunInfo bt = sv_output_info(SV_BACKTRACK_ID);
		const SvRunInfo vc = sv_output_info(SV_VCAM_ID);
		const bool recOn = rec.state != SvRunState::Off;
		const bool btOn = bt.state != SvRunState::Off;
		const bool vcOn = vc.state != SvRunState::Off;
		recKey->setAccent(recOn ? svl::RED : svl::ORANGE);
		recKey->setIconName(recOn ? "stop" : "video");
		recKey->setToolTip(recOn ? tool_sub(rec, QString(T(rec.paused ? "StateRecordPaused" : "StateRecording"))
								 .arg(clock_text(rec.seconds))) +
						   ". " + T("StopRecord")
					 : QString(T("Record")) + ". " + T("RecordTip"));
		/* No Pause key on the processor (x264): pause is a graphics card feature. */
		pauseKey->setVisible(rec.state == SvRunState::Live && (rec.paused || sv_output_can_pause()));
		pauseKey->setIconName(rec.paused ? "play" : "pause");
		pauseKey->setToolTip(rec.paused ? T("RecordResumeTip") : T("RecordPauseTip"));
		btKey->setAccent(btOn ? svl::RED : svl::VIOLET);
		btKey->setIconName(btOn ? "stop" : "history");
		btKey->setToolTip(btOn ? tool_sub(bt, QString(T("BacktrackOn")).arg(sv_output_settings().backtrack_seconds)) +
						 ". " + T("BacktrackStop")
				       : QString(T("BacktrackStart")) + ". " + T("BacktrackTip"));
		vcKey->setAccent(vcOn ? svl::RED : svl::BLUE);
		vcKey->setIconName(vcOn ? "stop" : "webcam");
		vcKey->setToolTip(vcOn ? tool_sub(vc, T("VcamOn")) + ". " + T("VcamStop")
				       : QString(T("VcamStart")) + ". " + T("VcamTip"));
		recKey->setEnabled(rec.state != SvRunState::Stopping);
		btKey->setEnabled(bt.state != SvRunState::Stopping);
		vcKey->setEnabled(vc.state != SvRunState::Stopping);
		/* Record: REC and the clock while it records, how to stop it under that. */
		if (rec.state == SvRunState::Live) {
			recKey->setText(QString(T("HeaderRec")).arg(clock_text(rec.seconds)));
			recKey->setSub(rec.paused ? T("KeyPaused") : T("KeyPressStop"), rec.paused ? AMBER_INK : RED_INK);
		} else if (recOn) {
			recKey->setText(T("CtrlRecord"));
			recKey->setSub(tool_sub(rec, QString()), RED_INK);
		} else {
			recKey->setText(T("CtrlRecord"));
			recKey->setSub(T("KeyPressStart"), svl::tone(svl::ORANGE, 0.45));
		}
		/* Backtrack: how many seconds a clip can go back while it runs. */
		btKey->setText(T("CtrlBacktrack"));
		if (bt.state == SvRunState::Live)
			btKey->setSub(QString(T("KeyBtReady")).arg(sv_output_settings().backtrack_seconds), GREEN_INK);
		else if (btOn)
			btKey->setSub(tool_sub(bt, QString()), RED_INK);
		else
			btKey->setSub(T("KeyPressStart"), svl::tone(svl::VIOLET, 0.45));
		/* Save clip and save and replay always sit in the clips row, dimmed until Backtrack
		   runs, so a streamer always knows where they are. */
		const bool btLive = bt.state == SvRunState::Live;
		clipKey->setEnabled(btLive);
		clipPlayKey->setEnabled(btLive);
		clipKey->setToolTip(btLive ? T("BacktrackSave") : T("ClipNeedsBacktrack"));
		clipPlayKey->setToolTip(btLive ? T("SaveReplayTip") : T("ClipNeedsBacktrack"));
		/* Replay always sits beside them, so a streamer can always find it. With no clip
		   yet it says how to get one, and pressing it says the same in the notice line. */
		const bool replaying = sv_replay_playing();
		const bool hasClip = !sv_output_last_clip().path.empty();
		replayKey->setAccent(replaying ? svl::RED : svl::EMERALD);
		replayKey->setIconName(replaying ? "stop" : "replay");
		replayKey->setToolTip(replaying ? T("ReplayStopTip") : hasClip ? T("ReplayTip") : T("ReplayNoClipTip"));
		refresh_replay_row();
		const bool hudOff = sv_source_hud_hidden();
		hudKey->setVisible(hudOff || sv_source_has_hud());
		hudKey->setAccent(hudOff ? svl::RED : svl::YELLOW);
		hudKey->setIconName(hudOff ? "eye" : "eye-off");
		hudKey->setToolTip(hudOff ? T("HudShowTip") : T("HudHideTip"));
		fit_tools();

		/* the clip card */
		const SvClip clip = sv_output_last_clip();
		if (clip.seq != seenClip && !clip.path.empty()) {
			seenClip = clip.seq;
			clipPath = qs(clip.path);
			clipName->setText(QFileInfo(clipPath).fileName(), svl::INK2);
			clipCard->setValue(QString(T("ClipSeconds")).arg(clip.seconds));
			clipCard->show();
			clipTimer->start();
			if (sv_output_notice().text == T("BacktrackSaving"))
				sv_output_set_notice("", false);
		}

		/* one line for what went wrong */
		QString problemText;
		for (const SvRunInfo *i : {&rec, &bt, &vc}) {
			if (i->state == SvRunState::Off && !i->problem.empty())
				problemText = qs(i->problem);
		}
		const SvNotice n = sv_output_notice();
		if (!n.text.empty())
			notice->setText(qs(n.text), n.problem ? PROBLEM : svl::INK2);
		else if (!problemText.isEmpty())
			notice->setText(problemText, PROBLEM);
		notice->setVisible(!n.text.empty() || !problemText.isEmpty());

		refresh_cost();
		update_tick(streaming || rec.state != SvRunState::Off);
	}

	/* The clip keys always say their names (a narrow key drops its icon first). The setup
	   row always says its names too: a narrow key drops its icon, then the shorter names.
	   Game HUD says "HUD". */
	void fit_tools()
	{
		if (!toolsRow)
			return;
		auto name = [](svl::Key *k, const QString &text) {
			if (k->text() != text)
				k->setText(text);
		};
		name(clipKey, T("CtrlSaveClip"));
		name(clipPlayKey, T("CtrlSavePlay"));
		name(replayKey, T("CtrlReplay"));
		const std::vector<std::pair<svl::Key *, QString>> tools = {
			{editSceneKey, T("CtrlEditScene")}, {guidesKey, T("CtrlGuides")}, {vcKey, T("CtrlCamera")}};
		int need = 0, shown = 0;
		for (const auto &t : tools) {
			if (t.first->isHidden())
				continue;
			name(t.first, t.second);
			need += t.first->sizeHint().width();
			shown++;
		}
		if (!hudKey->isHidden()) {
			need += hudKey->sizeHint().width();
			shown++;
		}
		need += (shown - 1) * 6;
		if (need <= toolsRow->width())
			return;
		/* Nick's rule: a key never shows a symbol on its own; the word always stays. Too
		   narrow for word and icon: the key drops its icon by itself while the word fits.
		   Too narrow for the full word even then: the shorter names. The keys share the row
		   evenly, so the widest word decides. */
		const int hud = hudKey->isHidden() ? 0 : hudKey->sizeHint().width() + 6;
		const int keys = shown - (hudKey->isHidden() ? 0 : 1);
		const int each = keys > 0 ? (toolsRow->width() - hud - (keys - 1) * 6) / keys : 0;
		int widest = 0;
		for (const auto &t : tools)
			if (!t.first->isHidden())
				widest = std::max(widest, t.first->sizeHint().width() - 22); /* without its icon */
		if (widest <= each)
			return;
		name(editSceneKey, T("CtrlEditSceneShort"));
		name(vcKey, T("CtrlCameraShort"));
	}

	static QString tool_sub(const SvRunInfo &info, const QString &onText)
	{
		switch (info.state) {
		case SvRunState::Off:
			return T("ToolOff");
		case SvRunState::Starting:
			return T("ToolStarting");
		case SvRunState::Stopping:
			return T("ToolStopping");
		default:
			return onText;
		}
	}

	/* The header's one stat: frames OBS could not make in time. The rest is in the
	   live stats, from the menu or a click on the missed frames. */
	void refresh_cost()
	{
		/* Only frames OBS missed make the header: everything else is in the live stats. */
		std::vector<svl::Seg> segs;
		video_t *video = own ? sv_canvas_video() : nullptr;
		const uint32_t missed = video ? video_output_get_skipped_frames(video) : 0;
		if (missed > 0)
			segs.push_back({QString(T("StatMissed")).arg(missed), AMBER_INK, true, true});
		const bool changed = segs.size() != statSegs.size() || (!segs.empty() && segs[0].text != statSegs[0].text);
		statSegs = segs;
		if (changed)
			paint_header();
	}

	/* ---------- Scenes and Sources: folded in here, or popped out ---------- */

	static constexpr int IN_ROWS = 4;
	static constexpr int OUT_ROWS = 8;

	void toggle_part(bool scenes)
	{
		const bool out = scenes ? scenesOut : sourcesOut;
		if (out) {
			if (QDockWidget *d = part_dock(scenes))
				d->hide();
			set_part_out(scenes, false);
		} else {
			show_part_dock(scenes);
		}
	}

	/* Moves a part into its own panel or back into the box at the top of this one. */
	void set_part_out(bool scenes, bool out)
	{
		QWidget *part = scenes ? scenesPart.data() : sourcesPart.data();
		SvSidePanel *side = scenes ? scenesSide : sourcesSide;
		if (!part || !side)
			return;
		bool &now = scenes ? scenesOut : sourcesOut;
		const bool placed = out ? part->parentWidget() == side : part->parentWidget() != side;
		now = out;
		if (!placed) {
			if (out) {
				side->hold(part);
			} else {
				if (QWidget *old = part->parentWidget())
					if (old->layout())
						old->layout()->removeWidget(part);
				foldBody->insertWidget(scenes ? 0 : foldBody->count(), part);
			}
			part->setVisible(own);
		}
		if (!scenes && sourcesCard)
			sourcesCard->setMaxRows(out ? OUT_ROWS : IN_ROWS);
		show_part_keys();
		apply_sources_fold();
		refresh_fold_card();
	}

	void show_part_keys()
	{
		scenesOutKey->setIconName(scenesOut ? "putback" : "external");
		scenesOutKey->setText(scenesOut ? T("MenuPutBackShort") : T("PopOutShort"));
		scenesCard->poppedOut = scenesOut;
		scenesOutKey->setToolTip(scenesOut ? T("ScenesPutBack") : T("ScenesPopOut"));
		sourcesOutKey->setText(sourcesOut ? T("MenuPutBackShort") : T("PopOutShort"));
		sourcesOutKey->setIconName(sourcesOut ? "putback" : "external");
		sourcesOutKey->setToolTip(sourcesOut ? T("SourcesPutBack") : T("SourcesPopOut"));
	}

	/* The Sources list is always open: Edit scene is a pop out, so there is nothing to fold
	   (folded, the pop out showed the word Sources with no sources under it). */
	void apply_sources_fold()
	{
		const bool folded = false;
		sourcesFoldKey->setVisible(false);
		sourcesFoldKey->setIcon(QIcon(svl::icon(folded ? "chevron-right" : "chevron", svl::BLUE, 17,
							 qApp->devicePixelRatio())));
		sourcesFoldKey->setToolTip(folded ? T("SourcesUnfold") : T("SourcesFold"));
		/* Folded, only the list goes; the word and the buttons stay. */
		if (sourcesCard)
			sourcesCard->setVisible(!folded);
		if (sourcesLabel)
			sourcesLabel->setVisible(true);
	}

	void refresh_fold_card()
	{
		/* With both parts in panels of their own, Edit scene has nothing left to open. */
		const bool any = own && !(scenesOut && sourcesOut);
		foldCard->setVisible(any);
		if (editSceneKey)
			editSceneKey->setVisible(any);
		if (!any && editPop)
			editPop->hide();
		if (partsRule)
			partsRule->setVisible(own && !scenesOut && !sourcesOut);
	}

	void update_tick(bool running)
	{
		const bool want = running || isVisible();
		if (want && !tick->isActive())
			tick->start();
		else if (!want && tick->isActive())
			tick->stop();
	}

	void rebuild_rows(const std::vector<SvDest> &list)
	{
		while (QLayoutItem *item = rowsLayout->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		rows.clear();
		dests = list;
		for (size_t i = 0; i < dests.size(); i++) {
			auto *row = new DestRow(dests[i], i == 0);
			rowsLayout->addWidget(row);
			rows.push_back(row);
			const std::string id = dests[i].id;
			connect(row->action, &QPushButton::clicked, this, [this, id] {
				const bool stopping = sv_output_info(id).state != SvRunState::Off;
				if (!ask_live({id}, !stopping))
					return;
				act([id, stopping] {
					/* Does what was asked, even if the stream changed while the box was open. */
					const bool off = sv_output_info(id).state == SvRunState::Off;
					if (stopping && !off)
						sv_output_stop(id);
					else if (!stopping && off)
						sv_action_toggle(id);
				});
			});
			connect(row->onSwitch, &QAbstractButton::toggled, this, [this, row, id](bool want) {
				/* Saved straight away. Only this row's words change, a moment later, never
				   while this switch is still busy sending its signal. */
				row->on = want;
				sv_output_set_dest_on(id, want);
				QTimer::singleShot(0, this, [this] { refresh_running(); });
			});
			connect(row->edit, &QPushButton::clicked, this, [this, id] {
				livePop->hide();
				/* The saved list, not the rows' copy: the on/off switch changes without a rebuild. */
				for (const SvDest &one : sv_output_dests()) {
					if (one.id == id) {
						edit_dest(one);
						return;
					}
				}
			});
		}
	}

	void edit_dest(const SvDest &dest)
	{
		SvDestDialog dialog(this, dest);
		if (dialog.exec() == QDialog::Accepted) {
			destSignature.clear();
			refresh_running();
		}
	}

#ifdef SV_DEV_BUILD
public:
	/* Testing builds only: opens a part of the panel the way a click would. */
	void test_open(const std::string &what)
	{
		if (what == "tour") {
			start_tour();
		} else if (what == "live") {
			livePop->open_under(this, statusRow);
		} else if (what == "clips") {
			fill_clips();
			clipsPop->open_over(this, clipsRow);
		} else if (what == "layouts") {
			layoutPop->open_over(this, layoutRow);
		} else if (what == "scene") {
			editPop->open_over(this, toolsRow);
		} else if (what == "menu") {
			show_menu();
		} else if (what == "close") {
			livePop->hide();
			layoutPop->hide();
			clipsPop->hide();
			editPop->hide();
		} else if (what == "settings") {
			SvOutSettingsDialog dialog(this);
			dialog.exec();
			refresh();
		} else if (what == "add") {
			edit_dest(SvDest());
		} else if (what.rfind("click:", 0) == 0) {
			/* Clicks the button with this text in the window in front, the way the member would. */
			QWidget *top = QApplication::activeModalWidget();
			const QString want = QString::fromStdString(what.substr(6));
			if (top)
				for (QAbstractButton *b : top->findChildren<QAbstractButton *>())
					if (b->isVisible() && b->isEnabled() && (b->text() == want || b->accessibleName() == want)) {
						b->click();
						break;
					}
		} else if (what.rfind("tab:", 0) == 0) {
			/* Picks tab N of the top row of tabs in the window in front. */
			QWidget *top = QApplication::activeModalWidget();
			svl::TabBar *bar = nullptr;
			int barY = 1 << 30;
			if (top)
				for (QWidget *w : top->findChildren<QWidget *>())
					if (auto *t = dynamic_cast<svl::TabBar *>(w); t && t->isVisible()) {
						const int y = t->mapTo(top, QPoint(0, 0)).y();
						if (y < barY) {
							barY = y;
							bar = t;
						}
					}
			const int n = std::atoi(what.c_str() + 4);
			if (bar) {
				bar->setCurrent(n);
				if (bar->onPick)
					bar->onPick(n);
			}
		} else if (what == "obsrec:start") {
			obs_frontend_recording_start();
		} else if (what == "obsrec:stop") {
			obs_frontend_recording_stop();
		} else if (what == "failmore") {
			sv_output_dev_fail_more();
		} else if (what.rfind("addpiece:", 0) == 0) {
			/* addpiece:Minimap puts a Simply Vertical piece on the current horizontal scene. */
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, "piece_name", what.substr(9).c_str());
			obs_source_t *src = obs_source_create("simply_vertical_piece", "SV piece test", st, nullptr);
			obs_data_release(st);
			obs_source_t *sceneSrc = obs_frontend_get_current_scene();
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene && src) {
				obs_sceneitem_t *item = obs_scene_add(scene, src);
				struct vec2 pos;
				vec2_set(&pos, 40.0f, 40.0f);
				obs_sceneitem_set_pos(item, &pos);
			}
			obs_source_release(sceneSrc);
			obs_source_release(src);
		} else if (what.rfind("setpiece:", 0) == 0) {
			obs_source_t *src = obs_get_source_by_name("SV piece test");
			if (src) {
				obs_data_t *st = obs_data_create();
				obs_data_set_string(st, "piece_name", what.substr(9).c_str());
				obs_source_update(src, st);
				obs_data_release(st);
				obs_source_release(src);
			}
		} else if (what.rfind("setgrow:", 0) == 0) {
			obs_source_t *src = obs_get_source_by_name("SV piece test");
			if (src) {
				obs_data_t *st = obs_data_create();
				obs_data_set_int(st, "piece_grow", std::atoi(what.c_str() + 8));
				obs_source_update(src, st);
				obs_data_release(st);
				obs_source_release(src);
			}
		} else if (what == "rmpiece") {
			obs_source_t *src = obs_get_source_by_name("SV piece test");
			if (src) {
				obs_source_remove(src);
				obs_source_release(src);
			}
		} else if (what.rfind("seti:", 0) == 0 || what.rfind("sets:", 0) == 0) {
			/* seti:name=5 or sets:name=text changes one saved setting. */
			const size_t eq = what.find('=');
			if (eq != std::string::npos) {
				const std::string name = what.substr(5, eq - 5);
				const std::string value = what.substr(eq + 1);
				if (what[3] == 'i')
					sv_store_set_int(name.c_str(), std::atoi(value.c_str()));
				else
					sv_store_set_string(name.c_str(), value);
			}
		} else if (what == "record") {
			sv_action_toggle(SV_RECORD_ID);
		} else if (what == "split") {
			sv_output_split_record();
		} else if (what == "pause") {
			sv_output_pause_record(-1);
		} else if (what == "chapter") {
			sv_output_add_chapter();
		} else if (what.rfind("key:", 0) == 0) {
			/* Types into the stream key box of the window in front. */
			QWidget *top = QApplication::activeModalWidget();
			if (top)
				for (QLineEdit *e : top->findChildren<QLineEdit *>())
					if (e->echoMode() == QLineEdit::Password) {
						e->setText(QString::fromStdString(what.substr(4)));
						break;
					}
		} else if (what.rfind("dest:", 0) == 0) {
			const size_t n = (size_t)std::atoi(what.c_str() + 5);
			const std::vector<SvDest> list = sv_output_dests();
			if (n < list.size())
				edit_dest(list[n]);
		} else if (what.rfind("switch:", 0) == 0) {
			/* Clicks a row's on/off switch, the way the member would. */
			const size_t n = (size_t)std::atoi(what.c_str() + 7);
			if (n < rows.size())
				rows[n]->onSwitch->click();
		} else if (what.rfind("canvas:", 0) == 0) {
			/* canvas:N:id saves platform N on that canvas, as its window's Save would. */
			const size_t colon = what.find(':', 7);
			const size_t n = (size_t)std::atoi(what.c_str() + 7);
			std::vector<SvDest> list = sv_output_dests();
			if (colon != std::string::npos && n < list.size()) {
				list[n].canvas = what.substr(colon + 1) == "main" ? std::string() : what.substr(colon + 1);
				sv_output_save_dest(list[n]);
				refresh_running();
			}
		}
	}

private:
#endif

	bool own = false;

	svl::Words *acct = nullptr;
	svl::Words *updateLine = nullptr;
	svl::Info *acctInfo = nullptr;
	QString linkCode;
	QString linkedName;
	svl::Words *missedLine = nullptr;
	QLabel *linkIcon = nullptr;
	svl::Key *settingsKey = nullptr;
	QPointer<SvTour> tour;
	svl::Key *linkKey = nullptr;
	SvPreview *preview = nullptr;
	QWidget *statusRow = nullptr;
	QHBoxLayout *statusLayout = nullptr;
	QWidget *layoutRow = nullptr;
	svl::Key *layoutPrevKey = nullptr;
	svl::Key *layoutNameKey = nullptr;
	svl::Key *layoutNextKey = nullptr;
	QWidget *ctrlRow = nullptr;
	QWidget *clipsRow = nullptr;
	QWidget *toolsRow = nullptr;
	svl::Key *editSceneKey = nullptr;
	SvPopup *editPop = nullptr;
	svl::Key *goKey = nullptr;
	svl::Key *goMore = nullptr;
	svl::Key *recKey = nullptr;
	svl::Key *pauseKey = nullptr;
	svl::Key *btKey = nullptr;
	svl::Key *clipKey = nullptr;
	svl::Key *replayKey = nullptr;
	svl::Key *clipPlayKey = nullptr;
	svl::Key *replayMore = nullptr;
	SvPopup *clipsPop = nullptr;
	svl::Key *replayWholeKey = nullptr;
	svl::Key *replayBoxKey = nullptr;
	svl::Card *clipsCard = nullptr;
	svl::Words *clipsEmpty = nullptr;
	QWidget *clipsHost = nullptr;
	QVBoxLayout *clipsList = nullptr;
	svl::Key *clipsRowKey = nullptr;
	std::vector<std::pair<QCheckBox *, SvClip>> clipChecks;
	QWidget *replayRow = nullptr;
	svl::Key *replayPauseKey = nullptr;
	svl::Key *replayBackKey = nullptr;
	svl::Key *replayRestartKey = nullptr;
	svl::Key *replaySpeedKey = nullptr;
	svl::Key *replaySlowKey = nullptr;
	svl::Key *hudKey = nullptr;
	svl::Key *vcKey = nullptr;
	svl::Key *guidesKey = nullptr;
	SvPopup *livePop = nullptr;
	SvPopup *layoutPop = nullptr;
	SvScroll *inlineScroll = nullptr;
	QWidget *inlinePage = nullptr;
	QVBoxLayout *inlineLayout = nullptr;
	QWidget *foldCard = nullptr;
	QVBoxLayout *foldBody = nullptr;
	QPointer<QWidget> scenesPart;
	QWidget *partsRule = nullptr;
	QPointer<QWidget> sourcesPart;
	SvSidePanel *scenesSide = nullptr;
	SvSidePanel *sourcesSide = nullptr;
	svl::Key *scenesOutKey = nullptr;
	svl::Key *sourcesOutKey = nullptr;
	QToolButton *sourcesFoldKey = nullptr;
	QLabel *sourcesLabel = nullptr;
	bool scenesOut = false;
	bool sourcesOut = false;
	QWidget *noteRow = nullptr;
	svl::Words *note = nullptr;
	svl::Key *setupKey = nullptr;
	svl::Key *seSetupKey = nullptr;
	/* Set when the SE.Live button found no Multi-Streaming box, so the note asks the
	   member to sign in to StreamElements first. */
	bool seNeedsSignIn = false;

	svl::Card *layoutCard = nullptr;
	Flow *tilesFlow = nullptr;
	svl::Words *layoutEmpty = nullptr;
	std::vector<std::pair<svl::LayoutTile *, std::string>> layoutTiles;
	Flow *layoutKeys = nullptr;
	svl::Key *editKey = nullptr;
	svl::Key *scenesKey = nullptr;
	SvScenesCard *scenesCard = nullptr;
	QPointer<SvSourcesCard> sourcesCard;
	obs_source_t *watched = nullptr; /* the vertical scene whose changes we listen to */
	svl::Info *layoutInfo = nullptr;
	QCheckBox *followGame = nullptr;
	svl::Words *followSeen = nullptr;
	QWidget *holdRow = nullptr;
	QCheckBox *holdBox = nullptr;
	std::string layoutSignature;

	/* With another app running the vertical: what only Simply Vertical adds there. */
	QWidget *otherBox = nullptr;
	svl::Card *zoomCard = nullptr;
	Flow *zoomFlow = nullptr;
	svl::Words *zoomEmpty = nullptr;
	std::vector<std::pair<svl::Key *, std::string>> zoomKeys;
	std::string zoomSignature;
	std::vector<bool> zoomLit;
	QTimer *zoomTimer = nullptr;
	svl::Card *picksCard = nullptr;
	QComboBox *gameBox = nullptr;
	QComboBox *camBox = nullptr;
	std::string picksSignature;
	svl::Card *linksCard = nullptr;
	QWidget *linksHost = nullptr;
	QGridLayout *linksGrid = nullptr;
	svl::Words *linksEmpty = nullptr;
	svl::Key *linksKey = nullptr;
	QString linksSignature;

	svl::Card *liveCard = nullptr;
	svl::Key *goAll = nullptr;
	QWidget *emptyRow = nullptr;
	QWidget *rowsHost = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	svl::Key *addKey = nullptr;
	std::vector<DestRow *> rows;
	std::vector<SvDest> dests;
	std::string destSignature;

	svl::Card *clipCard = nullptr;
	svl::Words *clipName = nullptr;
	QString clipPath;
	uint64_t seenClip = 0;
	QTimer *clipTimer = nullptr;

	svl::Words *notice = nullptr;
	QLabel *statsIcon = nullptr;
	SvStatsCard *statsCard = nullptr;
	QHBoxLayout *headerRow = nullptr;
	std::vector<svl::Seg> statSegs;

	QTimer *tick = nullptr;
	QTimer *soon = nullptr;
	int listener = -1;
};

QPointer<SvDock> g_dock;

} // namespace

#ifdef SV_DEV_BUILD
/* Testing builds only: "shot" saves a picture of the panel and of every open Simply
   Vertical window to the plugin's settings folder, test-shots; anything else opens that
   part of the panel a moment later, so a window that waits for an answer never holds the
   caller up. */
std::string sv_ui_test(const std::string &what)
{
	if (what != "shot") {
		QTimer::singleShot(50, qApp, [what] {
			if (g_dock)
				g_dock->test_open(what);
		});
		return "opening " + what;
	}
	char *dirp = obs_module_config_path("test-shots");
	const QString dir = dirp ? QString::fromUtf8(dirp) : QString();
	bfree(dirp);
	if (dir.isEmpty())
		return "no folder";
	QDir().mkpath(dir);
	std::string saved;
	int n = 0;
	auto save = [&](QWidget *w, const QString &name) {
		const QString path = dir + "/" + name + ".png";
		if (w->grab().save(path))
			saved += path.toStdString() + "\n";
	};
	if (g_dock && g_dock->isVisible())
		save(g_dock, "panel");
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (!w->isVisible() || qobject_cast<QMainWindow *>(w) || w == g_dock)
			continue;
		if (w->windowType() == Qt::Popup && g_dock && w->parentWidget() == g_dock)
			save(w, QString("popup-%1").arg(++n));
		else if (qobject_cast<QDialog *>(w))
			save(w, QString("window-%1").arg(++n));
	}
	return saved.empty() ? "nothing open" : saved;
}
#endif

namespace {

void reveal_dock()
{
	if (!g_dock)
		return;
	auto *dock = qobject_cast<QDockWidget *>(g_dock->parentWidget());
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!dock || !main)
		return;
	if (dock->isHidden()) {
		main->addDockWidget(Qt::RightDockWidgetArea, dock);
		dock->setFloating(false);
		dock->show();
	}
	dock->raise();
}

/* Pops the vertical's Scenes or Sources out into a panel of its own, docked right
   under OBS's own Scenes or Sources, never hidden behind another panel. */
void show_part_dock(bool scenes)
{
	QDockWidget *dock = g_dock ? g_dock->part_dock(scenes) : nullptr;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!dock || !main)
		return;
	if (dock->isHidden()) {
		dock->setFloating(false);
		auto *beside = main->findChild<QDockWidget *>(scenes ? "scenesDock" : "sourcesDock");
		if (beside && !beside->isHidden() && !beside->isFloating())
			main->splitDockWidget(beside, dock, Qt::Vertical);
		else
			main->addDockWidget(Qt::BottomDockWidgetArea, dock);
		dock->show();
		/* Tall enough to show every scene, or a good run of sources, straight away. */
		const int want = std::max(scenes ? 150 : 300, dock->widget() ? dock->widget()->sizeHint().height() : 0);
		main->resizeDocks({dock}, {want}, Qt::Vertical);
	}
	dock->raise();
}

/* Scenes and Sources now sit in the main panel. A member who had the old separate
   Sources panel open gets it folded in once; after that it stays as they leave it. */
void settle_part_docks()
{
	if (!g_dock)
		return;
	if (sv_store_int("parts_v2", 0) == 0) {
		sv_store_set_int("parts_v2", 1);
		if (QDockWidget *d = g_dock->part_dock(false))
			d->hide();
	}
	g_dock->watch_part_docks();
}

/* ---------- the welcome box ---------- */

/* ---------- a live picture of one OBS source, or of the vertical ----------
   The setup shows the game and camera to pick by their own live pictures instead of
   their names ("Video Capture Device 2"), and the vertical itself at See it working.
   Drawn by OBS straight into the box, like the panel's canvas, and only while it is on
   screen, so a closed or hidden step costs nothing. */
class SvLiveView : public QWidget {
public:
	/* An empty uuid shows the vertical (the main canvas). */
	explicit SvLiveView(const std::string &sourceUuid, QWidget *parent = nullptr) : QWidget(parent), uuid(sourceUuid)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		if (!uuid.empty()) {
			if (obs_source_t *s = obs_get_source_by_uuid(uuid.c_str())) {
				weak = obs_source_get_weak_source(s);
				obs_source_release(s);
			}
		}
	}

	~SvLiveView() override
	{
		showing(false);
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
		obs_weak_source_release(weak);
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* A click on the picture (the native box takes the mouse, not the tile around it). */
	std::function<void()> onClick;

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		create();
		if (display)
			obs_display_set_enabled(display, true);
		showing(true);
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		if (display)
			obs_display_set_enabled(display, false);
		showing(false);
	}

	void paintEvent(QPaintEvent *) override { create(); }

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		create();
		if (display) {
			const QSize s = size() * devicePixelRatioF();
			obs_display_resize(display, (uint32_t)s.width(), (uint32_t)s.height());
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (onClick && e->button() == Qt::LeftButton) {
			onClick();
			return;
		}
		QWidget::mousePressEvent(e);
	}

private:
	/* A source that is not in any scene may not draw (a game capture waits to be shown), so
	   while its picture is on screen it counts as showing, like OBS's own Properties preview.
	   The vertical keeps drawing while this picture is up, whatever the panel does. */
	void showing(bool on)
	{
		if (on == shown)
			return;
		shown = on;
		if (uuid.empty()) {
			sv_canvas_set_user(SV_CANVAS_SETUP, on);
			return;
		}
		if (obs_source_t *s = obs_weak_source_get_source(weak)) {
			if (on)
				obs_source_inc_showing(s);
			else
				obs_source_dec_showing(s);
			obs_source_release(s);
		}
	}

	void create()
	{
		if (display || !isVisible() || !windowHandle() || !windowHandle()->isExposed())
			return;
		const QSize s = size() * devicePixelRatioF();
		gs_init_data info = {};
		info.cx = (uint32_t)std::max(1, s.width());
		info.cy = (uint32_t)std::max(1, s.height());
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		info.window.hwnd = (void *)winId();
		display = obs_display_create(&info, 0xFF130A0A);
		if (display)
			obs_display_add_draw_callback(display, draw, this);
	}

	/* The picture as big as the box allows, centred, never stretched. */
	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<SvLiveView *>(data);
		obs_source_t *src = nullptr;
		uint32_t w = 1080, h = 1920;
		if (!self->uuid.empty()) {
			src = obs_weak_source_get_source(self->weak);
			if (!src)
				return;
			w = obs_source_get_width(src);
			h = obs_source_get_height(src);
			if (!w || !h) {
				obs_source_release(src);
				return;
			}
		}
		const float k = std::min((float)cx / (float)w, (float)cy / (float)h);
		const int vw = std::max(1, (int)((float)w * k)), vh = std::max(1, (int)((float)h * k));
		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_set_viewport(((int)cx - vw) / 2, ((int)cy - vh) / 2, vw, vh);
		if (src)
			obs_source_video_render(src);
		else
			sv_canvas_render();
		gs_projection_pop();
		gs_viewport_pop();
		obs_source_release(src);
	}

	std::string uuid;
	obs_weak_source_t *weak = nullptr;
	obs_display_t *display = nullptr;
	bool shown = false;
};

/* One choice in the setup's picture grid: the source's live picture with its name under
   it, lit in the step's colour when picked. Without a uuid it is the "No camera" choice. */
class SvPickTile : public QWidget {
public:
	static constexpr int W = 200;
	static constexpr int PIC_H = 112;

	SvPickTile(const std::string &sourceUuid, const QString &name, const QColor &hue, QWidget *parent = nullptr)
		: QWidget(parent),
		  id(sourceUuid),
		  accent(hue),
		  label(name)
	{
		setCursor(Qt::PointingHandCursor);
		setFixedWidth(W);
		auto *col = new QVBoxLayout(this);
		col->setContentsMargins(6, 6, 6, 8);
		col->setSpacing(6);
		if (!id.empty()) {
			view = new SvLiveView(id);
			view->setFixedSize(W - 12, PIC_H);
			view->setCursor(Qt::PointingHandCursor);
			view->onClick = [this] { pick(); };
			col->addWidget(view);
		} else {
			col->addSpacing(PIC_H);
		}
		words = say(name, svl::INK, 14);
		col->addWidget(words);
		/* No camera: the box already says it. */
		words->setVisible(!id.empty());
		setToolTip(name);
	}

	void setOn(bool o)
	{
		on = o;
		words->setText(label, on ? svl::INK : svl::INK2, on);
		update();
	}

	std::string id;
	std::function<void()> onPick;

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton) {
			pick();
			return;
		}
		QWidget::mousePressEvent(e);
	}

	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}

	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
		p.setPen(Qt::NoPen);
		p.setBrush(on ? svl::alpha(accent, 0.14) : QColor(16, 16, 26));
		p.drawRoundedRect(r, 12, 12);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(svl::alpha(accent, on ? 0.95 : hover ? 0.6 : 0.22), on ? 2.5 : 1.2));
		p.drawRoundedRect(r, 12, 12);
		if (id.empty()) {
			/* No camera: a plain box in the picture's place, saying so. */
			const QRectF pic(6, 6, width() - 12, PIC_H);
			p.setPen(QPen(svl::alpha(accent, 0.35), 1, Qt::DashLine));
			p.drawRoundedRect(pic, 8, 8);
			p.setFont(svl::body(15, QFont::Bold));
			p.setPen(svl::alpha(accent, 0.9));
			p.drawText(pic, Qt::AlignCenter, label);
		}
	}

private:
	void pick()
	{
		if (onPick)
			onPick();
	}

	QColor accent;
	QString label;
	SvLiveView *view = nullptr;
	svl::Words *words = nullptr;
	bool on = false;
	bool hover = false;
};

/* The setup's steps along the top: a bar and a short name each, green once done, lit
   on the one showing, dim ahead. The same steps as the setup guide on the website. */
class SvStepTrack : public QWidget {
public:
	explicit SvStepTrack(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedHeight(40);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setSteps(const QStringList &names)
	{
		steps = names;
		update();
	}

	/* -1 shows no step lit (the opening screen). */
	void setCurrent(int i)
	{
		current = i;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (steps.isEmpty())
			return;
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const int n = (int)steps.size();
		const qreal gap = 6;
		const qreal w = (width() - gap * (n - 1)) / n;
		const QFont f = svl::body_exact(12.5, QFont::ExtraBold);
		p.setFont(f);
		for (int i = 0; i < n; i++) {
			const qreal x = i * (w + gap);
			const bool done = i < current, now = i == current;
			const QColor c = done ? svl::GREEN : now ? svl::BLUE : svl::alpha(svl::INK3, 0.35);
			const QRectF bar(x, 2, w, 5);
			if (now) {
				p.setPen(Qt::NoPen);
				p.setBrush(svl::alpha(svl::BLUE, 0.35));
				p.drawRoundedRect(bar.adjusted(-1, -2, 1, 2), 4, 4);
			}
			p.setPen(Qt::NoPen);
			p.setBrush(c);
			p.drawRoundedRect(bar, 2.5, 2.5);
			const QRectF tr(x, 13, w, 22);
			const QString name = (done ? QString::fromUtf8("✓ ") : QString()) + steps[i];
			if (now)
				svl::glow_text(p, tr, Qt::AlignHCenter | Qt::AlignVCenter, name, svl::INK, svl::BLUE, 0.6);
			else {
				p.setPen(done ? svl::mix(svl::GREEN, Qt::white, 0.25) : svl::INK3);
				p.drawText(tr, Qt::AlignHCenter | Qt::AlignVCenter,
					   QFontMetricsF(f).elidedText(name, Qt::ElideRight, w));
			}
		}
	}

private:
	QStringList steps;
	int current = -1;
};

/* ---------- the guided setup ----------
   Nick, 2026-11-19: the setup was "quite complicated" and members should be guided
   "through the plugin side of it as well". One step per screen instead of every card at
   once, with the steps along the top: Account, Game, Camera, (which app runs the
   vertical, only when Aitum Vertical or SE.Live is installed), See it, Platforms, Go live.
   The account step moves on by itself the moment the link is approved; the game and the
   camera are picked by their live pictures; See it starts the vertical, so the member sees
   it working before ever going live. Not now closes it and it stays reachable from the
   panel menu and OBS's Tools menu. Every step is reported to the website's setup guide
   (report_setup), so both places tick together. */
class SvWelcome : public SvWindow {
	enum Step { Welcome, Account, Game, Camera, Route, See, Platforms, Ready };

public:
	explicit SvWelcome(QWidget *parent) : SvWindow(parent, T("WelcomeTitle"), "tools")
	{
		setModal(false);
		setAttribute(Qt::WA_DeleteOnClose);
		setMinimumSize(700, 600);
		v->setSpacing(10);

		app = sv_detect_other_app();
		useOther = !app.name.empty() && sv_store_route() != SV_ROUTE_OWN;

		track = new SvStepTrack();
		v->addWidget(track);

		pages = new QStackedWidget();
		v->addWidget(pages, 1);
		build_welcome();
		build_account();
		build_game();
		build_camera();
		build_route();
		build_see();
		build_platforms();
		build_ready();

		QHBoxLayout *buttons = button_row();
		notNow = key(T("NotNow"), "x");
		back = key(T("WizardBack"), "chevron-left");
		next = key(T("WizardNext"), "chevron-right");
		next->setDefault(true);
		/* Enter always means Next: a key that has the focus would otherwise take it, and
		   Enter on Not now closed the setup halfway through. */
		notNow->setAutoDefault(false);
		back->setAutoDefault(false);
		buttons->addWidget(notNow);
		buttons->addStretch(1);
		buttons->addWidget(back);
		buttons->addWidget(next);

		connect(notNow, &QPushButton::clicked, this, [this] {
			sv_store_set_welcome_done(true);
			close();
		});
		connect(back, &QPushButton::clicked, this, [this] { go(prev_step(at)); });
		connect(next, &QPushButton::clicked, this, [this] { next_clicked(); });

		SvLinkListener l;
		QPointer<SvWelcome> self(this);
		l.onStatus = [self] {
			QMetaObject::invokeMethod(
				qApp,
				[self] {
					if (self)
						self->link_changed();
				},
				Qt::QueuedConnection);
		};
		listener = sv_link_listen(std::move(l), nullptr);

		wasLinked = sv_link_state().kind == SvLinkKind::Linked;
		load_picks();
		fill_live();
		/* Only Next answers Enter (see above), whichever key has the focus. */
		for (QPushButton *b : findChildren<QPushButton *>())
			if (b != next)
				b->setAutoDefault(false);
		go(Welcome);
	}

	~SvWelcome() override
	{
		if (listener >= 0)
			sv_link_unlisten(listener);
	}

protected:
	/* Closed (also as OBS shuts down, before the outputs go): the website hears the setup
	   window is no longer open. Not later, when it is deleted, by which time OBS may be gone. */
	void hideEvent(QHideEvent *e) override
	{
		SvWindow::hideEvent(e);
		g_setupStep.clear();
		report_setup(false);
	}

private:
	/* ---------- the screens ---------- */

	/* One screen: a big title, a line under it, then what that step needs. */
	QVBoxLayout *page(Step s, const QString &title, const QString &line)
	{
		auto *w = new QWidget();
		auto *col = new QVBoxLayout(w);
		col->setContentsMargins(2, 6, 2, 0);
		col->setSpacing(10);
		col->addWidget(new Title(title, 24));
		if (!line.isEmpty())
			col->addWidget(say(line, svl::INK2, 15));
		pages->addWidget(w);
		index[s] = pages->count() - 1;
		return col;
	}

	void build_welcome()
	{
		QVBoxLayout *col = page(Welcome, T("WelcomeHead"), T("WelcomeBody"));
		col->addSpacing(4);
		const char *const names[] = {"LinkHead", "WelcomeStepPick", "SeeHead", "PlatHead"};
		const char *const icons[] = {"link", "webcam", "canvas", "radio"};
		for (int i = 0; i < 4; i++) {
			auto *row = new QHBoxLayout();
			row->setSpacing(12);
			auto *num = new QLabel();
			num->setFixedWidth(28);
			welcomeTicks[i] = num;
			row->addWidget(num);
			row->addWidget(icon_label(icons[i], svl::BLUE, 18));
			row->addWidget(say(T(names[i]), svl::INK, 17), 1);
			col->addLayout(row);
		}
		col->addStretch(1);
	}

	void build_account()
	{
		QVBoxLayout *col = page(Account, T("LinkHead"), T("LinkBody"));
		col->addSpacing(8);
		linkButton = key(T("LinkButton"), "link", svl::BLUE);
		linkButton->setBig(true);
		col->addWidget(linkButton, 0, Qt::AlignLeft);
		linkSay = say(QString(), svl::INK2, 15);
		col->addWidget(linkSay);
		/* The code to type on the browser page, big enough to read across the room. */
		code = new Title(QString(), 44);
		col->addWidget(code);
		linkAgain = key(T("LinkAgain"), "refresh");
		col->addWidget(linkAgain, 0, Qt::AlignLeft);
		col->addStretch(1);
		connect(linkButton, &QPushButton::clicked, this, [] { sv_link_request(); });
		connect(linkAgain, &QPushButton::clicked, this, [] { sv_link_request(); });
	}

	void build_game()
	{
		QVBoxLayout *col = page(Game, T("GameHead"), T("GameBody"));
		gameFlow = new Flow(10);
		col->addWidget(gameFlow);
		gameNote = say(T("GameNotFound"), AMBER_INK);
		col->addWidget(gameNote);
		auto *look = key(T("LookAgain"), "refresh", svl::EMERALD);
		col->addWidget(look, 0, Qt::AlignLeft);
		col->addStretch(1);
		connect(look, &QPushButton::clicked, this, [this] { fill_tiles(); });
	}

	void build_camera()
	{
		QVBoxLayout *col = page(Camera, T("CamHead"), T("CamBody"));
		camFlow = new Flow(10);
		col->addWidget(camFlow);
		auto *look = key(T("LookAgain"), "refresh", svl::EMERALD);
		col->addWidget(look, 0, Qt::AlignLeft);
		col->addStretch(1);
		connect(look, &QPushButton::clicked, this, [this] { fill_tiles(); });
	}

	/* Only when Aitum Vertical or SE.Live is installed: keep it, or let us run the vertical. */
	void build_route()
	{
		QVBoxLayout *col = page(Route, T("RouteAsk"), QString(T("RouteHeadSub")).arg(qs(app.name)));
		auto *routes = new QHBoxLayout();
		routes->setSpacing(10);
		routeOther = new svl::Tile(QString(T("RouteKeep")).arg(qs(app.name)), "layers");
		routeOther->setSub(QString(T("RouteOtherSub")).arg(qs(app.name)));
		routeOther->setAccent(svl::VIOLET);
		routeOwn = new svl::Tile(T("RouteOwnShort"), "canvas");
		routeOwn->setSub(T("RouteOwnSub"));
		routeOwn->setAccent(svl::VIOLET);
		routes->addWidget(routeOther);
		routes->addWidget(routeOwn);
		col->addLayout(routes);
		routeNote = say(QString());
		col->addWidget(routeNote);
		col->addStretch(1);
		connect(routeOwn, &QAbstractButton::clicked, this, [this] { pick_route(false); });
		connect(routeOther, &QAbstractButton::clicked, this, [this] { pick_route(true); });
		pick_route(useOther);
	}

	void build_see()
	{
		QVBoxLayout *col = page(See, T("SeeHead"), T("SeeBody"));
		auto *row = new QHBoxLayout();
		row->addStretch(1);
		/* Its own picture of the vertical, canvas shaped. */
		seeView = new SvLiveView(std::string());
		seeView->setFixedSize(207, 368);
		row->addWidget(seeView);
		row->addStretch(1);
		col->addLayout(row);
		seeNote = say(QString(), AMBER_INK);
		seeNote->hide();
		col->addWidget(seeNote);
		col->addStretch(1);
	}

	void build_platforms()
	{
		QVBoxLayout *col = page(Platforms, T("PlatHead"), T("StepLiveShort"));
		rowHue = svl::PINK;
		addedFlow = new Flow(6);
		col->addWidget(addedFlow);
		col->addLayout(field_row(T("StepLiveAdd")));
		auto *addFlow = new Flow(6);
		/* Vertical first: the apps people go live on in portrait lead. */
		static const char *const order[] = {"tiktok", "youtube", "instagram", "twitch", "kick", "facebook", "custom"};
		for (const char *id : order) {
			for (int i = 0; i < PLATFORM_COUNT; i++) {
				if (std::strcmp(PLATFORMS[i].id, id) != 0)
					continue;
				auto *k = key(T(PLATFORMS[i].label), QString(), svl::platform_colour(id));
				if (std::strcmp(id, "custom") == 0)
					k->setIconName("plus");
				else
					k->setPlatform(id);
				addFlow->add(k);
				connect(k, &QPushButton::clicked, this, [this, id] {
					SvDest d;
					d.platform = id;
					edit_dest(d);
				});
			}
		}
		col->addWidget(addFlow);
		col->addWidget(say(T("PlatKeys"), svl::INK2, 14));
		auto *canvasNote = new QHBoxLayout();
		canvasNote->setSpacing(6);
		canvasNote->addWidget(say(T("StepLiveCanvas")), 1);
		canvasNote->addWidget(new svl::Info(T("StepLiveCanvasTip")), 0, Qt::AlignTop);
		col->addLayout(canvasNote);
		col->addStretch(1);
	}

	void build_ready()
	{
		QVBoxLayout *col = page(Ready, T("ReadyHead"), QString());
		readySay = say(QString(), svl::INK, 17);
		col->addWidget(readySay);
		readyMore = say(QString(), svl::INK2, 15);
		col->addWidget(readyMore);
		col->addStretch(1);
	}

	/* ---------- moving between screens ---------- */

	/* The steps this member goes through: no app question without another vertical app,
	   and no See it or Platforms while that app runs the vertical. */
	std::vector<Step> steps() const
	{
		std::vector<Step> s = {Welcome, Account, Game, Camera};
		if (!app.name.empty())
			s.push_back(Route);
		if (!useOther) {
			s.push_back(See);
			s.push_back(Platforms);
		}
		s.push_back(Ready);
		return s;
	}

	Step prev_step(Step s) const
	{
		const std::vector<Step> list = steps();
		for (size_t i = 1; i < list.size(); i++)
			if (list[i] == s)
				return list[i - 1];
		return Welcome;
	}

	Step next_step(Step s) const
	{
		const std::vector<Step> list = steps();
		for (size_t i = 0; i + 1 < list.size(); i++)
			if (list[i] == s)
				return list[i + 1];
		return Ready;
	}

	void go(Step s)
	{
		at = s;
		pages->setCurrentIndex(index[s]);
		QStringList names;
		int current = -1;
		for (Step x : steps()) {
			if (x == Welcome)
				continue;
			if (x == s)
				current = (int)names.size();
			names << T(x == Account     ? "TrackAccount"
				   : x == Game      ? "TrackGame"
				   : x == Camera    ? "TrackCamera"
				   : x == Route     ? "TrackApp"
				   : x == See       ? "TrackSee"
				   : x == Platforms ? "TrackPlatforms"
						    : "TrackLive");
		}
		track->setSteps(names);
		track->setCurrent(current);
		/* The website's setup guide says which screen this is. */
		g_setupStep = s == Welcome ? "welcome"
			      : s == Account ? "account"
			      : s == Game ? "game"
			      : s == Camera ? "camera"
			      : s == Route ? "app"
			      : s == See ? "see"
			      : s == Platforms ? "platforms"
					       : "ready";
		report_setup(false);
		if (s == Welcome)
			refresh_welcome();
		if (s == Account)
			link_changed();
		if (s == Game || s == Camera)
			fill_tiles();
		if (s == Platforms)
			fill_live();
		if (s == Ready)
			refresh_ready();
		refresh_buttons();
		if (next->isEnabled())
			next->setFocus();
	}

	void refresh_buttons()
	{
		back->setVisible(at != Welcome);
		const bool linked = sv_link_state().kind == SvLinkKind::Linked;
		next->setEnabled((at != Account || linked) && (at != Game || !picks.game.empty()));
		const bool none = at == Platforms && sv_output_dests().empty();
		next->setText(T(at == Welcome ? "WizardStart" : at == Ready ? "WizardFinish" : none ? "WizardSkip" : "WizardNext"));
		next->setIconName(at == Ready ? "check" : "chevron-right");
		notNow->setVisible(at != Ready);
	}

	void next_clicked()
	{
		if (at == Ready) {
			reveal_dock();
			if (g_dock)
				g_dock->refresh();
			/* The first time through, the panel tour follows, once the panel has settled.
			   With another app running the vertical the panel has no keys to show. */
			if (!useOther && sv_store_int("tour_done", 0) == 0)
				QTimer::singleShot(600, qApp, [] {
					if (g_dock)
						g_dock->start_tour();
				});
			close();
			return;
		}
		/* The game and camera are in, and the app question answered: the vertical starts
		   now, so the next screen can show it working. */
		const Step to = next_step(at);
		if ((at == Camera || at == Route) && (to == See || to == Ready))
			apply();
		go(to);
	}

	/* ---------- each step ---------- */

	void refresh_welcome()
	{
		const bool linked = sv_link_state().kind == SvLinkKind::Linked;
		const bool done[] = {linked, !picks.game.empty() && sv_store_welcome_done(), sv_canvas_running(),
				     !sv_output_dests().empty()};
		for (int i = 0; i < 4; i++) {
			QLabel *l = welcomeTicks[i];
			if (done[i]) {
				l->setPixmap(svl::icon("check", svl::GREEN, 20, devicePixelRatioF()));
			} else {
				l->setPixmap(QPixmap());
				l->setText(QString("<span style='color:rgb(77,124,255); font-size:18px; font-weight:800'>%1</span>")
						   .arg(i + 1));
			}
		}
	}

	/* The account step follows the link as it happens, and moves on once it is approved. */
	void link_changed()
	{
		const SvLinkState s = sv_link_state();
		const bool linked = s.kind == SvLinkKind::Linked;
		const bool waiting = s.kind == SvLinkKind::Waiting;
		linkButton->setVisible(s.kind == SvLinkKind::NotLinked);
		linkAgain->setVisible(waiting);
		code->setVisible(waiting && !s.code.empty());
		code->setText(qs(s.code));
		if (linked)
			linkSay->setText(s.account.empty() ? QString(T("LinkDoneNoName"))
							   : QString(T("LinkDone")).arg(qs(s.account)),
					 GREEN_INK, true);
		else if (waiting)
			linkSay->setText(T("LinkCodeSay"), svl::INK, false);
		else if (!s.problem.empty())
			linkSay->setText(T(s.problem.c_str()), AMBER_INK, false);
		else
			linkSay->setText(QString(), svl::INK2, false);
		if (linked && !wasLinked && at == Account) {
			QPointer<SvWelcome> self(this);
			QTimer::singleShot(900, this, [self] {
				if (self && self->at == Account)
					self->go(self->next_step(Account));
			});
		}
		wasLinked = linked;
		refresh_buttons();
	}

	/* What was picked before: this collection's saved picks, else the best guess. */
	void load_picks()
	{
		const std::vector<SvFound> found = sv_detect_sources();
		picks = sv_detect_best(found);
		const std::string collection = current_collection();
		if (sv_store_has_picks(collection)) {
			const SvPicks saved = sv_store_picks(collection);
			if (source_exists(saved.game))
				picks.game = saved.game;
			if (source_exists(saved.cam))
				picks.cam = saved.cam;
		}
	}

	/* The game and camera choices as live pictures, likeliest first. */
	void fill_tiles()
	{
		const std::vector<SvFound> found = sv_detect_sources();
		if (picks.game.empty() || !source_exists(picks.game))
			picks.game = sv_detect_best(found).game;
		fill_flow(gameFlow, gameTiles, found, &SvFound::gameScore, picks.game, svl::EMERALD, false);
		fill_flow(camFlow, camTiles, found, &SvFound::camScore, picks.cam, svl::PINK, true);
		gameNote->setVisible(gameTiles.empty());
		refresh_buttons();
	}

	void fill_flow(Flow *flow, std::vector<SvPickTile *> &tiles, const std::vector<SvFound> &found,
		       int SvFound::*score, const std::string &want, const QColor &hue, bool camera)
	{
		std::vector<SvFound> sorted = found;
		std::stable_sort(sorted.begin(), sorted.end(),
				 [score](const SvFound &a, const SvFound &b) { return a.*score > b.*score; });
		flow->clear();
		tiles.clear();
		const size_t most = camera ? 5 : 6;
		/* The ones that look like a game (or a camera) only, unless that leaves fewer than
		   two to pick from: then the rest too, so the right one is always there. */
		const size_t likely = (size_t)std::count_if(sorted.begin(), sorted.end(), [score](const SvFound &f) { return f.*score > 0; });
		for (const SvFound &f : sorted) {
			if (tiles.size() >= most || (likely >= 2 && f.*score == 0))
				break;
			auto *t = new SvPickTile(f.uuid, qs(f.name), hue);
			tiles.push_back(t);
			flow->add(t);
		}
		if (camera) {
			auto *t = new SvPickTile(std::string(), T("CamNone"), hue);
			tiles.push_back(t);
			flow->add(t);
		}
		for (SvPickTile *t : tiles) {
			t->setOn(t->id == want);
			const std::string id = t->id;
			t->onPick = [this, camera, id] {
				if (camera)
					picks.cam = id;
				else
					picks.game = id;
				for (SvPickTile *x : camera ? camTiles : gameTiles)
					x->setOn(x->id == id);
				refresh_buttons();
			};
		}
	}

	void pick_route(bool other)
	{
		useOther = other && !app.name.empty();
		routeOwn->setOn(!useOther);
		routeOther->setOn(useOther);
		routeNote->setText(useOther ? QString(T("RouteOtherNote")).arg(qs(app.name))
					    : QString(T("RouteOwnBeside")).arg(qs(app.name)),
				   svl::INK2);
		if (next)
			go(at);
	}

	/* Saves the picks and starts the vertical (or puts the layout into the other app's). */
	void apply()
	{
		sv_store_set_picks(current_collection(), picks);
		sv_store_set_current_picks(picks);
		sv_detect_apply_picks(picks);
		sv_store_set_welcome_done(true);

		QString manual;
		if (useOther) {
			sv_store_set_route(SV_ROUTE_OTHER);
			sv_canvas_stop();
			/* Look again: the member may have set their canvas up meanwhile. */
			app = sv_detect_other_app();
			/* Its vertical not switched on yet: our layout goes in by itself once it is. */
			const bool waiting = app.canvas.empty();
			sv_store_set_int("other_pending", waiting ? 1 : 0);
			if (waiting)
				manual = QString(T("RouteOtherWaiting")).arg(qs(app.name));
			else if (!sv_detect_add_to_canvas(app.canvas, picks))
				manual = QString(T("RouteOtherManual")).arg(qs(app.name));
		} else {
			sv_store_set_int("other_pending", 0);
			sv_store_set_route(SV_ROUTE_OWN);
			if (!sv_canvas_start(picks))
				manual = T("CanvasFailed");
		}
		seeNote->setVisible(!manual.isEmpty());
		seeNote->setText(manual, AMBER_INK);
		otherNote = useOther ? manual : QString();
		if (g_dock)
			g_dock->refresh();
	}

	void refresh_ready()
	{
		if (useOther) {
			readySay->setText(QString(T("ReadyOther")).arg(qs(app.name)), svl::INK);
			readyMore->setText(otherNote, AMBER_INK);
			readyMore->setVisible(!otherNote.isEmpty());
			return;
		}
		readySay->setText(T("ReadyOwn"), svl::INK);
		readyMore->setText(T("ReadyNoPlatforms"), svl::INK2);
		readyMore->setVisible(sv_output_dests().empty());
	}

	/* The platforms already added, each a key that opens it to change or remove. */
	void fill_live()
	{
		addedFlow->clear();
		int n = 0;
		const std::vector<SvVCanvas> canvases = sv_canvas_running() ? sv_canvases() : std::vector<SvVCanvas>();
		for (const SvDest &d : sv_output_dests()) {
			QString label = qs(d.name);
			QString onCanvas = d.dual ? QString(T("DestCanvasMain")) : QString();
			for (const SvVCanvas &c : canvases)
				if (!c.main && !d.canvas.empty() && c.uuid == d.canvas)
					onCanvas = qs(c.name);
			if (d.canvas == SV_WIDE)
				onCanvas = T("DestCanvasWide");
			else if (d.dual)
				onCanvas = QString(T("DestCanvasDual")).arg(onCanvas);
			if (!onCanvas.isEmpty())
				label = QString(T("StepLiveOnCanvas")).arg(label, onCanvas);
			auto *k = key(label, "check", svl::platform_colour(d.platform));
			k->setAutoDefault(false);
			k->setPlatform(d.platform);
			k->setToolTip(T("StepLiveEdit"));
			addedFlow->add(k);
			const SvDest copy = d;
			connect(k, &QPushButton::clicked, this, [this, copy] { edit_dest(copy); });
			n++;
		}
		addedFlow->setVisible(n > 0);
		refresh_buttons();
	}

	void edit_dest(const SvDest &dest)
	{
		SvDestDialog dialog(this, dest);
		if (dialog.exec() == QDialog::Accepted) {
			fill_live();
			if (g_dock)
				g_dock->refresh();
		}
	}

	SvStepTrack *track = nullptr;
	QStackedWidget *pages = nullptr;
	std::map<Step, int> index;
	Step at = Welcome;
	svl::Key *notNow = nullptr;
	svl::Key *back = nullptr;
	svl::Key *next = nullptr;
	QLabel *welcomeTicks[4] = {};
	svl::Key *linkButton = nullptr;
	svl::Key *linkAgain = nullptr;
	svl::Words *linkSay = nullptr;
	Title *code = nullptr;
	Flow *gameFlow = nullptr;
	Flow *camFlow = nullptr;
	std::vector<SvPickTile *> gameTiles;
	std::vector<SvPickTile *> camTiles;
	svl::Words *gameNote = nullptr;
	svl::Tile *routeOwn = nullptr;
	svl::Tile *routeOther = nullptr;
	svl::Words *routeNote = nullptr;
	SvLiveView *seeView = nullptr;
	svl::Words *seeNote = nullptr;
	Flow *addedFlow = nullptr;
	svl::Words *readySay = nullptr;
	svl::Words *readyMore = nullptr;
	QString otherNote;
	SvPicks picks;
	bool useOther = false;
	bool wasLinked = false;
	SvOtherApp app;
	int listener = -1;
};

QPointer<SvWelcome> g_welcome;

void show_welcome()
{
	if (!g_welcome)
		g_welcome = new SvWelcome(static_cast<QWidget *>(obs_frontend_get_main_window()));
	g_welcome->show();
	g_welcome->raise();
	g_welcome->activateWindow();
}

/* OBS can open its own first-run wizard or a notice as the app starts. Those
   block every other window, so the welcome box waits its turn behind them. */
bool g_shutting = false;
void try_pending_other(int triesLeft);

void show_welcome_when_clear()
{
	if (g_shutting)
		return;
	if (QApplication::activeModalWidget()) {
		QTimer::singleShot(500, [] { show_welcome_when_clear(); });
		return;
	}
	show_welcome();
}

/* ---------- following OBS ---------- */

/* Brings the picks in line with whichever scene collection is open. */
void sync_collection()
{
	const std::string collection = current_collection();
	SvPicks picks;
	if (sv_store_has_picks(collection))
		picks = sv_store_picks(collection);

	if (sv_store_welcome_done()) {
		const SvPicks best = sv_detect_best(sv_detect_sources());
		bool changed = !sv_store_has_picks(collection);
		if (!source_exists(picks.game) && picks.game != best.game) {
			picks.game = best.game;
			changed = true;
		}
		if (!source_exists(picks.cam) && picks.cam != best.cam) {
			picks.cam = best.cam;
			changed = true;
		}
		if (changed)
			sv_store_set_picks(collection, picks);
	}

	sv_store_set_current_picks(picks);
	if (sv_store_route() == SV_ROUTE_OWN)
		sv_canvas_start(picks);
	sv_action_follow_scene();
	if (g_dock)
		g_dock->refresh();
}

/* Start with OBS: OBS's own Start Streaming and Start Recording drive the vertical too. */
bool following_obs()
{
	return sv_store_route() == SV_ROUTE_OWN && sv_output_settings().follow_obs;
}

void follow_obs_event(obs_frontend_event event)
{
	if (!following_obs())
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		sv_action_go_live_all();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		sv_action_stop_all();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (sv_output_info(SV_RECORD_ID).state == SvRunState::Off)
			sv_action_toggle(SV_RECORD_ID);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (sv_output_info(SV_RECORD_ID).state != SvRunState::Off)
			sv_action_toggle(SV_RECORD_ID);
		break;
	default:
		return;
	}
	if (g_dock)
		g_dock->refresh();
}

/* Backtrack set to start when OBS opens. The website link and the vertical picture take a
   moment to come up, so this looks every two seconds for a minute, then gives up quietly. */
void auto_backtrack_on_open(int triesLeft)
{
	if (g_shutting || triesLeft <= 0)
		return;
	if (sv_store_route() != SV_ROUTE_OWN || sv_output_settings().backtrack_auto != SV_BACKTRACK_AUTO_OPEN)
		return;
	if (sv_output_info(SV_BACKTRACK_ID).state != SvRunState::Off)
		return;
	if (!sv_link_is_linked() || !sv_canvas_video()) {
		QTimer::singleShot(2000, [triesLeft] { auto_backtrack_on_open(triesLeft - 1); });
		return;
	}
	sv_action_toggle(SV_BACKTRACK_ID);
	if (g_dock)
		g_dock->refresh();
}

void shut_everything()
{
	g_shutting = true;
	if (g_welcome)
		g_welcome->close();
	if (g_dock)
		g_dock->shut();
	sv_replay_stop();
	sv_actions_shutdown();
	sv_output_shutdown();
	sv_sound_unload();
	sv_canvas_stop();
}

void on_frontend_event(obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		sv_sound_pause(false);
		sync_collection();
		settle_part_docks();
		if (!sv_store_welcome_done())
			QTimer::singleShot(400, [] { show_welcome_when_clear(); });
		QTimer::singleShot(1500, [] { try_pending_other(0); });
		QTimer::singleShot(3000, [] { auto_backtrack_on_open(30); });
		QTimer::singleShot(15000, [] {
			g_settled = true;
			sv_detect_remove_doubles();
			if (g_dock)
				g_dock->refresh();
		});
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* Let go of this collection's vertical before OBS removes it. It is
		   saved with the collection and comes back when the collection does. */
		sv_replay_stop();
		sv_canvas_stop();
		sv_sound_pause(true);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		sv_sound_pause(false);
		sync_collection();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		sv_action_follow_scene();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		sv_output_obs_stream_event(event);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		sv_output_obs_stream_event(event);
		follow_obs_event(event);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		follow_obs_event(event);
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		shut_everything();
		break;
	default:
		break;
	}
}

/* Kept another app whose vertical was not switched on yet. The moment that app makes
   its vertical canvas, our layout goes into it. OBS says when a canvas is made, sized
   or renamed, so
   nothing checks on a timer; two short follow-up tries cover an app that makes the
   canvas first and its scene a moment later. */
void try_pending_other(int triesLeft)
{
	if (g_shutting || sv_store_int("other_pending", 0) == 0 || sv_store_route() != SV_ROUTE_OTHER)
		return;
	const SvOtherApp app = sv_detect_other_app();
	if (!app.canvas.empty() && sv_detect_add_to_canvas(app.canvas, sv_store_current_picks())) {
		sv_store_set_int("other_pending", 0);
		obs_log(LOG_INFO, "layout added to the %s vertical canvas once it appeared", app.name.c_str());
		if (g_dock)
			g_dock->refresh();
		/* Should the app put back a saved copy of the layout after ours went in, keep one. */
		QTimer::singleShot(10000, [] { sv_detect_remove_doubles(); });
		return;
	}
	if (triesLeft > 0)
		QTimer::singleShot(2000, [triesLeft] { try_pending_other(triesLeft - 1); });
}

/* Also on a canvas's size or name changing: SE.Live makes its canvas with no size and
   only names and sizes it a few seconds later, once it has read the member's canvases
   from StreamElements. */
void on_canvas_created(void *, calldata_t *)
{
	QMetaObject::invokeMethod(
		qApp, [] { QTimer::singleShot(500, [] { try_pending_other(15); }); }, Qt::QueuedConnection);
}

} // namespace

void sv_ui_load()
{
	sv_store_load();
	sv_source_load_settings();
	sv_output_load();
	sv_sound_load();
	sv_actions_load();
	g_dock = new SvDock();
	if (!obs_frontend_add_dock_by_id("simply-vertical-panel", T("SimplyVertical"), g_dock))
		obs_log(LOG_WARNING, "the Simply Vertical panel could not be added");
	if (!obs_frontend_add_dock_by_id("simply-vertical-scenes", T("ScenesPanelTitle"), g_dock->scenes_panel()))
		obs_log(LOG_WARNING, "the Vertical scenes panel could not be added");
	if (!obs_frontend_add_dock_by_id("simply-vertical-sources", T("SourcesPanelTitle"), g_dock->sources_panel()))
		obs_log(LOG_WARNING, "the Vertical sources panel could not be added");
	g_dock->watch_part_docks();
	obs_frontend_add_tools_menu_item(T("SetupMenu"), [](void *) { show_welcome(); }, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "canvas_create", on_canvas_created, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "canvas_video_reset", on_canvas_created, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "canvas_rename", on_canvas_created, nullptr);
}

void sv_ui_unload()
{
	signal_handler_disconnect(obs_get_signal_handler(), "canvas_create", on_canvas_created, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "canvas_video_reset", on_canvas_created, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "canvas_rename", on_canvas_created, nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	sv_sound_unload();
	sv_store_unload();
}
