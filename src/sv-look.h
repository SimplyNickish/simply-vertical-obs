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

// The SimplyNickish look, drawn by the plugin itself so the panel matches the
// website whatever OBS theme the member uses: lit section cards, raised lamp
// keys, carved and raised tiles, the product logo and Vertical Blue.

#pragma once

#include "sv-layout.h"

#include <QAbstractButton>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <deque>
#include <functional>
#include <string>
#include <vector>

class QHBoxLayout;
class QLabel;
class QMenu;
class QSpinBox;
class QVBoxLayout;

namespace svl {

inline const QColor BLUE(77, 124, 255);
inline const QColor RED(239, 68, 68);
inline const QColor GREEN(34, 197, 94);
inline const QColor AMBER(245, 158, 11);
/* Far apart on the wheel, so parts that each have their own colour never look alike. */
inline const QColor VIOLET(167, 139, 250);
inline const QColor CYAN(34, 211, 238);
inline const QColor ORANGE(251, 146, 60);
inline const QColor EMERALD(52, 211, 153);
inline const QColor PINK(244, 114, 182);
inline const QColor LIME(163, 230, 53);
inline const QColor YELLOW(250, 204, 21);
inline const QColor BG(10, 10, 19);
inline const QColor INK(238, 242, 255);
inline const QColor INK2(183, 198, 255);
inline const QColor INK3(142, 166, 255);

/* Loads the packed fonts once. Safe to call again. */
void load_fonts();
/* Every word in the plugin is drawn this much bigger than the sizes written in the code,
   so it stays readable on big and high resolution monitors. */
inline constexpr qreal TEXT_SCALE = 1.2;
QFont body(qreal px, int weight = QFont::Normal);
/* The size exactly as written, for words drawn on the vertical canvas itself. */
QFont body_exact(qreal px, int weight);
QFont head(qreal px);

/* color-mix(in srgb, a t, b). */
QColor mix(const QColor &a, const QColor &b, qreal t);
/* The text colour of a lit title or key: color-mix(k 22%, white). */
QColor tone(const QColor &k, qreal t = 0.22);
QColor alpha(const QColor &c, qreal a);

/* A colour for a whole part of a window. Everything inside it (labels, (i), number boxes,
   choices, switches) lights in that colour instead of blue. */
void set_hue(QWidget *w, const QColor &c);
QColor hue_of(const QWidget *w);

/* A line icon in one colour. */
QPixmap icon(const QString &name, const QColor &colour, int px, qreal dpr);
/* A platform logo in its own colours. */
QPixmap logo(const std::string &platform, int px, qreal dpr);
QColor platform_colour(const std::string &platform);

/* Text with a dark drop and a soft coloured glow, like the website. */
void glow_text(QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &colour,
	       const QColor &glow, qreal glowStrength = 0.5);

/* The style sheet for inputs, lists, menus and scroll bars in our windows. */
QString sheet();

/* Turns a menu into a lit card in one colour (its sub menus too). */
void lit_menu(QMenu *menu, const QColor &accent = BLUE);

/* The panel and window background: the website panel's lighter lit slab, with a soft
   blue glow at the top.
   With a lampY it has the website's lamp instead: a glowing line across the panel at that
   height, with a soft bloom of light falling from it. */
void paint_backdrop(QPainter &p, const QRect &r, bool topGlow, int lampY = -1);

/* A soft CSS-style shadow of a shape: offset, blurred, in one colour. */
void shape_shadow(QPainter &p, const QPainterPath &path, QPointF offset, qreal blurPx, const QColor &c);

/* One row of a list (a source): a slab raised and lit in its colour while it shows, carved in
   while it is hidden, glowing when picked. Draws the drag handle, the kind icon in a carved well,
   the name and an optional note (like "Hidden", in red); the row's own buttons sit right of nameRight. */
void paint_list_row(QPainter &p, const QRectF &r, const QColor &k, const QString &iconName, const QString &name,
		    const QString &note, bool on, bool picked, bool hover, qreal nameRight);

/* ---------- a raised lamp key ---------- */

class Key : public QPushButton {
public:
	explicit Key(const QString &text = QString(), QWidget *parent = nullptr);
	void setAccent(const QColor &c);
	void setIconName(const QString &name);
	/* A platform logo in its own colours instead of a line icon. */
	void setPlatform(const std::string &platform);
	void setBig(bool big);
	/* An unchosen option: flat, never for an action. */
	void setUnchosen(bool off);
	/* A second, smaller line under the name that says how things stand ("Steady 6.0 Mbps"). */
	void setSub(const QString &sub, const QColor &colour);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	bool event(QEvent *e) override;
	void paintEvent(QPaintEvent *) override;
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	QColor k = BLUE;
	QString iconName;
	std::string platform;
	bool big = false;
	bool unchosen = false;
	bool hover = false;
	QString sub;
	QColor subColour;
};

/* ---------- a lit section card ---------- */

class Card : public QWidget {
public:
	explicit Card(const QString &title, const QString &iconName, QWidget *parent = nullptr);
	void setAccent(const QColor &c);
	void setTitle(const QString &title);
	void setValue(const QString &value);
	/* The header value in its own colour (a status: green when done, amber when not). */
	void setValueColour(const QColor &c);
	/* A card that folds to its header line; clicking the header opens it again. */
	void setFoldable(bool foldable);
	void setFolded(bool folded);
	bool folded() const { return isFolded; }
	QVBoxLayout *body() const { return bodyLayout; }
	/* Called when the member folds or opens the card by clicking its header. */
	std::function<void(bool folded)> onFold;
	/* The lit slab with no header line: a box that holds other things. */
	void setHeadless(bool on);
	/* Nothing drawn and no padding: the card's contents sit straight on what holds it. */
	void setBare(bool on);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;

private:
	void place_body();
	QColor a = BLUE;
	QColor valueCol;
	QString title, value, iconName;
	bool foldable = false;
	bool isFolded = false;
	bool headless = false;
	bool bare = false;
	QWidget *bodyHost = nullptr;
	QVBoxLayout *bodyLayout = nullptr;
	static constexpr int HEAD = 44;
};

/* ---------- a row of tabs, each an icon over a word ---------- */

class TabBar : public QWidget {
public:
	explicit TabBar(QWidget *parent = nullptr);
	int addTab(const QString &text, const QString &iconName);
	/* A tab with its own colour, lit in that colour and showing it at rest. */
	/* strong: the word keeps its colour even when not picked, for choices told apart by colour. */
	void setTabHue(int index, const QColor &c, bool strong = false);
	void setCurrent(int index);
	int current() const { return cur; }
	void setTabVisible(int index, bool visible);
	void setTabText(int index, const QString &text);
	bool tabVisible(int index) const;
	int visibleCount() const;
	std::function<void(int index)> onPick;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;

protected:
	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	struct Tab {
		QString text, iconName;
		QColor hue;
		bool strong = false;
		bool visible = true;
	};
	std::vector<int> shown() const;
	/* How many tabs fit on one row at a given width. Too narrow and the tabs move onto more rows. */
	int perRow(qreal width, int count) const;
	QRectF cell(int slot, int count) const;
	int at(const QPointF &pos) const;
	qreal cellWidth() const;
	std::vector<Tab> tabs;
	int cur = 0;
	int hot = -1;
};

/* ---------- a tool tile: carved in when off, raised and lit when on ---------- */

class Tile : public QAbstractButton {
public:
	explicit Tile(const QString &name, const QString &iconName, QWidget *parent = nullptr);
	void setAccent(const QColor &c);
	void setOn(bool on);
	void setName(const QString &name);
	void setSub(const QString &sub);
	/* A platform logo instead of a line icon. */
	void setLogo(const std::string &platform);
	/* Instead of an icon, a small phone that keeps showing one layout switch (0 cut, 1 fade,
	   2 slide, 3 glide) with this feel, slide direction and length, over and over. */
	void setMotion(int mode, int feel, int dir, int ms);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;
	void showEvent(QShowEvent *) override;
	void hideEvent(QHideEvent *) override;

private:
	void paintPhone(QPainter &p, const QRectF &phone) const;

	QColor c = BLUE;
	QString name, sub, iconName;
	std::string platform;
	bool on = false;
	int motion = -1, feel = 0, dir = 0, ms = 300;
	class QTimer *anim = nullptr;
	QElapsedTimer clock;
};

/* ---------- a saved layout drawn as a small phone ---------- */

class LayoutTile : public QAbstractButton {
public:
	explicit LayoutTile(QWidget *parent = nullptr);
	/* The website tile shows a globe instead of pieces. */
	void setWebsite(const QString &name);
	void setLayout(const QString &name, const SvLayout &layout, int number);
	void setSelected(bool selected);
	/* How wide this tile wants to be so its name fits on one line. */
	int wantedWidth() const;
	/* All tiles in a row share one width so the grid stays even. */
	void setTileWidth(int w);
	QSize sizeHint() const override { return QSize(tileW, 136); }
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	QString name;
	SvLayout layout;
	bool website = false;
	bool haveLayout = false;
	bool selected = false;
	int number = 0;
	int tileW = 84;
	QColor hue;
};

/* ---------- the last minute of a stream's speed ---------- */

class Spark : public QWidget {
public:
	explicit Spark(QWidget *parent = nullptr);
	void setValues(const std::vector<float> &values, const QColor &colour);
	QSize sizeHint() const override { return QSize(200, 20); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	std::vector<float> values;
	QColor colour = GREEN;
};

/* ---------- a sound level ----------
   Carved in, and lit from the left as far as the sound reaches: the tab's colour, then amber
   and red near the top. It falls back slowly so it is easy to read. */

class Meter : public QWidget {
public:
	explicit Meter(QWidget *parent = nullptr);
	/* 0 silent to 1 full. */
	void setLevel(float level);
	void setAccent(const QColor &c);
	QSize sizeHint() const override { return QSize(150, 10); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	float shown = 0;
	QColor k = GREEN;
};

/* ---------- a platform logo sitting in a carved well ---------- */

class Well : public QWidget {
public:
	explicit Well(const std::string &platform, QWidget *parent = nullptr);
	QSize sizeHint() const override { return QSize(30, 30); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	std::string platform;
};

/* ---------- the product logo ---------- */

class Logo : public QWidget {
public:
	explicit Logo(QWidget *parent = nullptr);
	/* Tall enough for the smile under "Vertical". */
	QSize sizeHint() const override { return QSize(118, 58); }

protected:
	void paintEvent(QPaintEvent *) override;
};

/* ---------- a line of coloured words that wraps ---------- */

struct Seg {
	QString text;
	QColor colour = INK2;
	bool glow = false;
	bool bold = false;
};

class Words : public QWidget {
public:
	explicit Words(QWidget *parent = nullptr, qreal px = 12);
	void setSegs(const std::vector<Seg> &segs);
	void setText(const QString &text, const QColor &colour, bool glow = false);
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return QSize(40, lineHeight()); }

protected:
	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *) override;

private:
	struct Placed {
		QRectF r;
		QString text;
		int seg;
	};
	int lineHeight() const;
	std::vector<Placed> flow(int width, int *height) const;
	std::vector<Seg> segs;
	qreal px;
	int lastHeight = -1;
};

/* ---------- an (i) that explains on hover ---------- */

class Info : public QWidget {
public:
	explicit Info(const QString &tip, QWidget *parent = nullptr);
	void setTip(const QString &tip) { setToolTip(tip); }
	/* Warn turns the (i) into an amber warning sign, so a problem shows without words. */
	void setWarn(bool on);
	QSize sizeHint() const override { return QSize(18, 18); }

protected:
	void paintEvent(QPaintEvent *) override;
	void enterEvent(QEnterEvent *) override { update(); }
	void leaveEvent(QEvent *) override { update(); }

private:
	bool warn = false;
};

/* ---------- a typed number box ----------
   Carved into the panel like the tiles, the number in bold with its unit beside it, and raised
   minus and plus keys to step it. The edge lights up while you type in it. With offText, the
   lowest value shows that word instead of a number (for example Off). */

class NumberBox : public QWidget {
public:
	NumberBox(int min, int max, int step, int value, const QString &unit, const QString &offText = QString(),
		  QWidget *parent = nullptr);
	int value() const;
	void setValue(int v);
	/* The unit's word for exactly 1, for example "second" next to "seconds". */
	void setUnitOne(const QString &one);
	/* Called whenever the number changes, by typing, the steps or setValue. */
	std::function<void(int)> onChange;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override;
	bool eventFilter(QObject *o, QEvent *e) override;
	void enterEvent(QEnterEvent *) override { update(); }
	void leaveEvent(QEvent *) override { update(); }
	void changeEvent(QEvent *e) override;
	void mousePressEvent(QMouseEvent *) override;

private:
	void refresh();
	QSpinBox *spin = nullptr;
	QAbstractButton *down = nullptr;
	QAbstractButton *up = nullptr;
	QString unit;
	QString unitOne;
	QString offText;
	int spinW = 40;
	int unitW = 0;
};

/* ---------- an on/off switch with its words beside it ----------
   Carved in when off, lit and glowing when on, with a raised white knob. */

class Switch : public QAbstractButton {
public:
	explicit Switch(const QString &text, QWidget *parent = nullptr);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override;
	void enterEvent(QEnterEvent *) override { update(); }
	void leaveEvent(QEvent *) override { update(); }
};

/* A small uppercase field title for our windows. */
QLabel *field(const QString &text);
QLabel *field(const QString &text, const QColor &hue);

/* ---------- hover cards ---------- */

/* Our hovers look like the website's: a dark card with a lit coloured edge. Every widget
   inside a root marked with mark_tips shows its tooltip this way, even while OBS is not the
   window in front. A widget can pick the edge colour with set_tip_colour. */
void install_tips();
void mark_tips(QWidget *root);
void set_tip_colour(QWidget *w, const QColor &c);

} // namespace svl
