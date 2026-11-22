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

#include "sv-look.h"
#include "sv-ease.h"

#include <obs-module.h>

#include <QCache>
#include <QDataStream>
#include <QDir>
#include <QEnterEvent>
#include <QMenu>
#include <QMutex>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QSpinBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace svl {

namespace {

/* ---------- fonts ---------- */

bool g_fontsLoaded = false;
QString g_body = "Segoe UI";
QString g_head = "Arial Black";
QString g_brand = "Arial Black";
QString g_script = "Segoe Print";

/* ---------- icons: Lucide line icons (ISC licence) ---------- */

struct IconDef {
	const char *name;
	const char *inner;
};

const IconDef ICONS[] = {
	{"settings",
	 "<path d='M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z'/>"
	 "<circle cx='12' cy='12' r='3'/>"},
	{"guides",
	 "<rect width='14' height='20' x='5' y='2' rx='2'/><path d='M5 7h14'/><path d='M5 15h9'/><path d='M14 15v7'/>"},
	{"tools",
	 "<path d='M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z'/>"},
	{"menu", "<line x1='4' x2='20' y1='6' y2='6'/><line x1='4' x2='20' y1='12' y2='12'/><line x1='4' x2='20' y1='18' y2='18'/>"},
	{"link",
	 "<path d='M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71'/>"
	 "<path d='M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71'/>"},
	{"unlink",
	 "<path d='m18.84 12.25 1.72-1.71h-.02a5.004 5.004 0 0 0-.12-7.07 5.006 5.006 0 0 0-6.95 0l-1.72 1.71'/>"
	 "<path d='m5.17 11.75-1.71 1.71a5.004 5.004 0 0 0 .12 7.07 5.006 5.006 0 0 0 6.95 0l1.71-1.71'/>"
	 "<line x1='8' x2='8' y1='2' y2='5'/><line x1='2' x2='5' y1='8' y2='8'/><line x1='16' x2='16' y1='19' y2='22'/>"
	 "<line x1='19' x2='22' y1='16' y2='16'/>"},
	{"smartphone", "<rect width='14' height='20' x='5' y='2' rx='2'/><path d='M12 18h.01'/>"},
	/* The vertical canvas: the corners of a tall 9:16 frame with an up and down arrow,
	   never a phone (Nick, 2026-11-21). */
	{"canvas", "<path d='M6 7V3h4'/><path d='M14 3h4v4'/><path d='M18 17v4h-4'/><path d='M10 21H6v-4'/><path d='M12 8v8'/><path d='m9.5 10.5 2.5-2.5 2.5 2.5'/><path d='m9.5 13.5 2.5 2.5 2.5-2.5'/>"},
	{"pip", "<path d='M21 9V6a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2v10c0 1.1.9 2 2 2h4'/><rect width='10' height='7' x='12' y='13' rx='2'/>"},
	{"gamepad",
	 "<line x1='6' x2='10' y1='11' y2='11'/><line x1='8' x2='8' y1='9' y2='13'/><line x1='15' x2='15.01' y1='12' y2='12'/>"
	 "<line x1='18' x2='18.01' y1='10' y2='10'/>"
	 "<path d='M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.545-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.151A4 4 0 0 0 17.32 5z'/>"},
	{"image", "<rect width='18' height='18' x='3' y='3' rx='2' ry='2'/><circle cx='9' cy='9' r='2'/><path d='m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21'/>"},
	{"type", "<polyline points='4 7 4 4 20 4 20 7'/><line x1='9' x2='15' y1='20' y2='20'/><line x1='12' x2='12' y1='4' y2='20'/>"},
	{"grip",
	 "<circle cx='9' cy='12' r='1'/><circle cx='9' cy='5' r='1'/><circle cx='9' cy='19' r='1'/>"
	 "<circle cx='15' cy='12' r='1'/><circle cx='15' cy='5' r='1'/><circle cx='15' cy='19' r='1'/>"},	{"phone", "<rect width='14' height='20' x='5' y='2' rx='2'/>"},
	{"globe",
	 "<circle cx='12' cy='12' r='10'/><path d='M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20'/><path d='M2 12h20'/>"},
	{"external", "<path d='M15 3h6v6'/><path d='M10 14 21 3'/><path d='M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6'/>"},
	{"scenes",
	 "<path d='M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3Z'/>"
	 "<path d='m6.2 5.3 3.1 3.9'/><path d='m12.4 3.4 3.1 4'/><path d='M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z'/>"},
	{"layers",
	 "<path d='m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z'/>"
	 "<path d='m22 17.65-9.17 4.16a2 2 0 0 1-1.66 0L2 17.65'/><path d='m22 12.65-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65'/>"},
	{"radio",
	 "<path d='M4.9 19.1C1 15.2 1 8.8 4.9 4.9'/><path d='M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5'/><circle cx='12' cy='12' r='2'/>"
	 "<path d='M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5'/><path d='M19.1 4.9C23 8.8 23 15.1 19.1 19'/>"},
	{"video",
	 "<path d='m16 13 5.223 3.482a.5.5 0 0 0 .777-.416V7.87a.5.5 0 0 0-.752-.432L16 10.5'/>"
	 "<rect x='2' y='6' width='14' height='12' rx='2'/>"},
	{"history", "<path d='M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8'/><path d='M3 3v5h5'/><path d='M12 7v5l4 2'/>"},
	{"webcam", "<circle cx='12' cy='10' r='8'/><circle cx='12' cy='10' r='3'/><path d='M7 22h10'/><path d='M12 22v-4'/>"},
	{"monitor", "<rect width='20' height='14' x='2' y='3' rx='2'/><line x1='8' x2='16' y1='21' y2='21'/><line x1='12' x2='12' y1='17' y2='21'/>"},
	{"plus", "<path d='M5 12h14'/><path d='M12 5v14'/>"},
	{"minus", "<path d='M5 12h14'/>"},
	{"pencil",
	 "<path d='M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z'/>"},
	{"stop", "<rect width='14' height='14' x='5' y='5' rx='1'/>"},
	{"pause", "<rect x='14' y='4' width='4' height='16' rx='1'/><rect x='6' y='4' width='4' height='16' rx='1'/>"},
	{"gauge", "<path d='m12 14 4-4'/><path d='M3.34 19a10 10 0 1 1 17.32 0'/>"},
	{"snail", "<path d='M2 13a6 6 0 1 0 12 0 4 4 0 1 0-8 0 2 2 0 0 0 4 0'/><circle cx='10' cy='13' r='8'/><path d='M2 21h12c4.4 0 8-3.6 8-8V7a2 2 0 1 0-4 0v6'/><path d='M18 3 19.1 5.2'/><path d='M22 3 20.9 5.2'/>"},
	{"chevron", "<path d='m6 9 6 6 6-6'/>"},
	{"chevron-right", "<path d='m9 18 6-6-6-6'/>"},
	{"chevron-left", "<path d='m15 18-6-6 6-6'/>"},
	{"arrow-right", "<path d='M5 12h14'/><path d='m12 5 7 7-7 7'/>"},
	{"layout",
	 "<rect width='7' height='9' x='3' y='3' rx='1'/><rect width='7' height='5' x='14' y='3' rx='1'/>"
	 "<rect width='7' height='9' x='14' y='12' rx='1'/><rect width='7' height='5' x='3' y='16' rx='1'/>"},
	{"putback", "<path d='M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4'/><polyline points='10 17 15 12 10 7'/><line x1='15' x2='3' y1='12' y2='12'/>"},
	{"scissors",
	 "<circle cx='6' cy='6' r='3'/><path d='M8.12 8.12 12 12'/><path d='M20 4 8.12 15.88'/><circle cx='6' cy='18' r='3'/>"
	 "<path d='M14.8 14.8 20 20'/>"},
	{"info", "<circle cx='12' cy='12' r='10'/><path d='M12 16v-4'/><path d='M12 8h.01'/>"},
	{"alert",
	 "<path d='m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3'/><path d='M12 9v4'/>"
	 "<path d='M12 17h.01'/>"},
	{"x", "<path d='M18 6 6 18'/><path d='m6 6 12 12'/>"},
	/* The apps' own icons, for the Real view of the app guides. FILL becomes the colour. */
	{"trophy",
	 "<path d='M6 9H4.5a2.5 2.5 0 0 1 0-5H6'/><path d='M18 9h1.5a2.5 2.5 0 0 0 0-5H18'/><path d='M4 22h16'/>"
	 "<path d='M10 14.66V17c0 .55-.47.98-.97 1.21C7.85 18.75 7 20.24 7 22'/>"
	 "<path d='M14 14.66V17c0 .55.47.98.97 1.21C16.15 18.75 17 20.24 17 22'/><path d='M18 2H6v7a6 6 0 0 0 12 0V2Z'/>"},
	{"heart",
	 "<path d='M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z'/>"},
	{"heart-fill",
	 "<path fill='FILL' d='M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z'/>"},
	{"users",
	 "<path d='M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2'/><circle cx='9' cy='7' r='4'/><path d='M22 21v-2a4 4 0 0 0-3-3.87'/><path d='M16 3.13a4 4 0 0 1 0 7.75'/>"},
	{"bag", "<path d='M6 2 3 6v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V6l-3-4Z'/><path d='M3 6h18'/><path d='M16 10a4 4 0 0 1-8 0'/>"},
	{"smile",
	 "<circle cx='12' cy='12' r='10'/><path d='M8 14s1.5 2 4 2 4-2 4-2'/><path d='M9 9h.01'/><path d='M15 9h.01'/>"},
	{"flower",
	 "<circle cx='12' cy='12' r='3'/><path d='M12 16.5A4.5 4.5 0 1 1 7.5 12 4.5 4.5 0 1 1 12 7.5a4.5 4.5 0 1 1 4.5 4.5 4.5 4.5 0 1 1-4.5 4.5'/>"
	 "<path d='M12 7.5V9'/><path d='M7.5 12H9'/><path d='M16.5 12H15'/><path d='M12 16.5V15'/>"},
	{"gift",
	 "<rect x='3' y='8' width='18' height='4' rx='1'/><path d='M12 8v13'/><path d='M19 12v7a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2v-7'/>"
	 "<path d='M7.5 8a2.5 2.5 0 0 1 0-5A4.8 8 0 0 1 12 8a4.8 8 0 0 1 4.5-5 2.5 2.5 0 0 1 0 5'/>"},
	{"share",
	 "<circle cx='18' cy='5' r='3'/><circle cx='6' cy='12' r='3'/><circle cx='18' cy='19' r='3'/>"
	 "<path d='m8.59 13.51 6.83 3.98'/><path d='m15.41 6.51-6.82 3.98'/>"},
	{"arrow-left", "<path d='m12 19-7-7 7-7'/><path d='M19 12H5'/>"},
	{"dots", "<circle cx='12' cy='12' r='1'/><circle cx='12' cy='5' r='1'/><circle cx='12' cy='19' r='1'/>"},
	{"dollar", "<path d='M12 2v20'/><path d='M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6'/>"},
	{"star",
	 "<path d='M11.525 2.295a.53.53 0 0 1 .95 0l2.31 4.679a2.123 2.123 0 0 0 1.595 1.16l5.166.756a.53.53 0 0 1 .294.904l-3.736 3.638a2.123 2.123 0 0 0-.611 1.878l.882 5.14a.53.53 0 0 1-.771.56l-4.618-2.428a2.122 2.122 0 0 0-1.973 0L6.396 21.01a.53.53 0 0 1-.77-.56l.881-5.139a2.122 2.122 0 0 0-.611-1.879L2.16 9.795a.53.53 0 0 1 .294-.906l5.165-.755a2.122 2.122 0 0 0 1.597-1.16z'/>"},
	{"pin",
	 "<path d='M12 17v5'/><path d='M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z'/>"},
	{"volume", "<path d='M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z'/><path d='M16 9a5 5 0 0 1 0 6'/><path d='M19.364 18.364a9 9 0 0 0 0-12.728'/>"},
	{"volume-x", "<path d='M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z'/><path d='m22 9-6 6'/><path d='m16 9 6 6'/>"},
	{"folder",
	 "<path d='M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z'/>"},
	{"play", "<path d='M6 3 20 12 6 21 6 3z'/>"},
	{"clapperboard", "<path d='M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3Z'/><path d='m6.2 5.3 3.1 3.9'/><path d='m12.4 3.4 3.1 4'/><path d='M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z'/>"},
	{"rotate-ccw", "<path d='M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8'/><path d='M3 3v5h5'/>"},
	{"skip-back", "<path d='M17.971 4.285A2 2 0 0 1 21 6v12a2 2 0 0 1-3.029 1.715l-9.997-5.998a2 2 0 0 1-.003-3.432z'/><path d='M3 20V4'/>"},
	{"replay", "<circle cx='12' cy='12' r='10'/><path d='m10 8 6 4-6 4z'/>"},
	{"move",
	 "<path d='M12 2v20'/><path d='m15 19-3 3-3-3'/><path d='m19 9 3 3-3 3'/><path d='M2 12h20'/>"
	 "<path d='m5 9-3 3 3 3'/><path d='m9 5 3-3 3 3'/>"},
	{"check", "<path d='M20 6 9 17l-5-5'/>"},
	{"refresh", "<path d='M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8'/><path d='M21 3v5h-5'/>"},
	{"help", "<circle cx='12' cy='12' r='10'/><path d='M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3'/><path d='M12 17h.01'/>"},
	{"wand",
	 "<path d='m21.64 3.64-1.28-1.28a1.21 1.21 0 0 0-1.72 0L2.36 18.64a1.21 1.21 0 0 0 0 1.72l1.28 1.28a1.2 1.2 0 0 0 1.72 0L21.64 5.36a1.2 1.2 0 0 0 0-1.72'/>"
	 "<path d='m14 7 3 3'/>"},
	{"eye",
	 "<path d='M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0'/>"
	 "<circle cx='12' cy='12' r='3'/>"},
	{"eye-off",
	 "<path d='M10.733 5.076a10.744 10.744 0 0 1 11.205 6.575 1 1 0 0 1 0 .696 10.747 10.747 0 0 1-1.444 2.49'/>"
	 "<path d='M14.084 14.158a3 3 0 0 1-4.242-4.242'/>"
	 "<path d='M17.479 17.499a10.75 10.75 0 0 1-15.417-5.151 1 1 0 0 1 0-.696 10.75 10.75 0 0 1 4.446-5.143'/>"
	 "<path d='m2 2 20 20'/>"},
	{"zoom", "<circle cx='11' cy='11' r='8'/><path d='m21 21-4.3-4.3'/><path d='M11 8v6'/><path d='M8 11h6'/>"},
	{"lock", "<rect width='18' height='11' x='3' y='11' rx='2' ry='2'/><path d='M7 11V7a5 5 0 0 1 10 0v4'/>"},
	{"unlock", "<rect width='18' height='11' x='3' y='11' rx='2' ry='2'/><path d='M7 11V7a5 5 0 0 1 9.9-1'/>"},
	{"copy",
	 "<rect width='14' height='14' x='8' y='8' rx='2' ry='2'/><path d='M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2'/>"},
	{"arrow-up", "<path d='m5 12 7-7 7 7'/><path d='M12 19V5'/>"},
	{"arrow-down", "<path d='M12 5v14'/><path d='m19 12-7 7-7-7'/>"},
	{"filter", "<polygon points='22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3'/>"},
	{"trash",
	 "<path d='M3 6h18'/><path d='M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6'/><path d='M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2'/>"},
	{"server",
	 "<rect width='20' height='8' x='2' y='2' rx='2'/><rect width='20' height='8' x='2' y='14' rx='2'/>"
	 "<line x1='6' x2='6.01' y1='6' y2='6'/><line x1='6' x2='6.01' y1='18' y2='18'/>"},
	{"instagram",
	 "<rect width='20' height='20' x='2' y='2' rx='5'/><path d='M16 11.37A4 4 0 1 1 12.63 8 4 4 0 0 1 16 11.37z'/>"
	 "<line x1='17.5' x2='17.51' y1='6.5' y2='6.5'/>"},
};

/* Platform logos in their own colours (Simple Icons, CC0). */
struct LogoDef {
	const char *platform;
	const char *svg;
};

const LogoDef LOGOS[] = {
	{"youtube",
	 "<path fill='#ff3040' d='M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432L15.818 12l-6.273 3.568z'/>"},
	{"twitch",
	 "<path fill='#a970ff' d='M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143l-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714z'/>"},
	{"tiktok",
	 "<path fill='#ffffff' d='M12.525.02c1.31-.02 2.61-.01 3.91-.02.08 1.53.63 3.09 1.75 4.17 1.12 1.11 2.7 1.62 4.24 1.79v4.03c-1.44-.05-2.89-.35-4.2-.97-.57-.26-1.1-.59-1.62-.93-.01 2.92.01 5.84-.02 8.75-.08 1.4-.54 2.79-1.35 3.94-1.31 1.92-3.58 3.17-5.91 3.21-1.43.08-2.86-.31-4.08-1.03-2.02-1.19-3.44-3.37-3.65-5.71-.02-.5-.03-1-.01-1.49.18-1.9 1.12-3.72 2.58-4.96 1.66-1.44 3.98-2.13 6.15-1.72.02 1.48-.04 2.96-.04 4.44-.99-.32-2.15-.23-3.02.37-.63.41-1.11 1.04-1.36 1.75-.21.51-.15 1.07-.14 1.61.24 1.64 1.82 3.02 3.5 2.87 1.12-.01 2.19-.66 2.77-1.61.19-.33.4-.67.41-1.06.1-1.79.06-3.57.07-5.36.01-4.03-.01-8.05.02-12.07z'/>"},
	{"kick", "<path fill='#53fc18' d='M2 2h4v8l6-8h5l-7 9 7 11h-5l-6-9v9H2z'/>"},
	{"facebook",
	 "<path fill='#3d8bff' d='M9.101 23.691v-7.98H6.627v-3.667h2.474v-1.58c0-4.085 1.848-5.978 5.858-5.978.401 0 .955.042 1.468.103a8.68 8.68 0 0 1 1.141.195v3.325a8.623 8.623 0 0 0-.653-.036 26.805 26.805 0 0 0-.733-.009c-.707 0-1.259.096-1.675.309a1.686 1.686 0 0 0-.679.622c-.258.42-.374.995-.374 1.752v1.297h3.919l-.386 2.103-.287 1.564h-3.246v8.245C19.396 23.238 24 18.179 24 12.044c0-6.627-5.373-12-12-12s-12 5.373-12 12c0 5.628 3.874 10.35 9.101 11.647Z'/>"},
};

QHash<QString, QPixmap> g_pixCache;

QPixmap render_svg(const QByteArray &svg, int px, qreal dpr)
{
	QPixmap pm(QSize(px, px) * dpr);
	pm.fill(Qt::transparent);
	QSvgRenderer r(svg);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	r.render(&p);
	p.end();
	pm.setDevicePixelRatio(dpr);
	return pm;
}

/* ---------- blur, for glows and soft shadows ---------- */

void box_pass(uint32_t *px, int count, int stride, int r, std::vector<uint32_t> &tmp)
{
	if (r <= 0 || count <= 1)
		return;
	tmp.resize((size_t)count);
	for (int i = 0; i < count; i++)
		tmp[(size_t)i] = px[(size_t)i * stride];
	const int win = 2 * r + 1;
	int sa = 0, sr = 0, sg = 0, sb = 0;
	auto add = [&](int i, int sign) {
		const int j = std::clamp(i, 0, count - 1);
		const uint32_t v = tmp[(size_t)j];
		sa += sign * (int)(v >> 24);
		sr += sign * (int)((v >> 16) & 255);
		sg += sign * (int)((v >> 8) & 255);
		sb += sign * (int)(v & 255);
	};
	for (int i = -r; i <= r; i++)
		add(i, 1);
	for (int i = 0; i < count; i++) {
		px[(size_t)i * stride] = ((uint32_t)(sa / win) << 24) | ((uint32_t)(sr / win) << 16) |
					 ((uint32_t)(sg / win) << 8) | (uint32_t)(sb / win);
		add(i + r + 1, 1);
		add(i - r, -1);
	}
}

void blur(QImage &img, int r)
{
	if (r <= 0)
		return;
	if (img.format() != QImage::Format_ARGB32_Premultiplied)
		img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const int w = img.width(), h = img.height();
	auto *bits = reinterpret_cast<uint32_t *>(img.bits());
	const int stride = (int)(img.bytesPerLine() / 4);
	std::vector<uint32_t> tmp;
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < h; y++)
			box_pass(bits + (size_t)y * stride, w, 1, r, tmp);
		for (int x = 0; x < w; x++)
			box_pass(bits + x, h, stride, r, tmp);
	}
}

/* A CSS box-shadow: the shape, offset and blurred. `blurPx` is the CSS blur length. */
void soft_shadow(QPainter &p, const QPainterPath &path, QPointF offset, qreal blurPx, const QColor &c)
{
	const qreal dpr = p.device()->devicePixelRatioF();
	const qreal pad = blurPx + 2;
	const QRectF b = path.boundingRect().adjusted(-pad, -pad, pad, pad);
	/* Blurring is the slow part, and the same shape comes round on every repaint (a hover,
	   a ticked box), so each blurred shadow is made once and kept. The key is the shape
	   measured from its own corner, so a shape that only moved is still found. */
	QByteArray id;
	{
		QDataStream ds(&id, QIODevice::WriteOnly);
		ds << blurPx << dpr << c.rgba() << path.elementCount();
		for (int i = 0; i < path.elementCount(); i++) {
			const QPainterPath::Element e = path.elementAt(i);
			ds << (qint8)e.type << (qint32)std::lround((e.x - b.x()) * 4) << (qint32)std::lround((e.y - b.y()) * 4);
		}
	}
	static QCache<QByteArray, QImage> kept(32 * 1024); /* in KB: 32 MB at most */
	static QMutex keptLock;
	{
		QMutexLocker lock(&keptLock);
		if (const QImage *hit = kept.object(id)) {
			p.drawImage(b.topLeft() + offset, *hit);
			return;
		}
	}
	QImage img((b.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	{
		QPainter q(&img);
		q.setRenderHint(QPainter::Antialiasing);
		q.scale(dpr, dpr);
		q.translate(-b.topLeft());
		q.fillPath(path, c);
	}
	blur(img, (int)std::lround(0.55 * blurPx * dpr));
	img.setDevicePixelRatio(dpr);
	p.drawImage(b.topLeft() + offset, img);
	QMutexLocker lock(&keptLock);
	kept.insert(id, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
}

/* The same glow behind a picture (an icon). */
void glow_pixmap(QPainter &p, const QPointF &at, const QPixmap &pm, qreal blurPx, qreal opacity)
{
	const qreal dpr = pm.devicePixelRatio();
	const int pad = (int)std::ceil(blurPx * dpr) + 2;
	QImage img(pm.size() + QSize(pad * 2, pad * 2), QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	{
		QPainter q(&img);
		q.drawImage(QPoint(pad, pad), pm.toImage());
	}
	blur(img, (int)std::lround(0.55 * blurPx * dpr));
	img.setDevicePixelRatio(dpr);
	p.save();
	p.setOpacity(p.opacity() * opacity);
	p.drawImage(at - QPointF(pad / dpr, pad / dpr), img);
	p.restore();
}

/* Carved in: a dark rim along the top left, a faint light one along the bottom right. */
void carve(QPainter &p, const QPainterPath &path, qreal strength = 1.0)
{
	const QRectF r = path.boundingRect();
	p.save();
	p.setClipPath(path);
	QLinearGradient dark(r.topLeft(), r.bottomRight());
	dark.setColorAt(0, QColor(0, 0, 0, (int)(170 * strength)));
	dark.setColorAt(0.28, QColor(0, 0, 0, 0));
	p.fillPath(path, dark);
	QLinearGradient light(r.bottomRight(), r.topLeft());
	light.setColorAt(0, QColor(255, 255, 255, (int)(14 * strength)));
	light.setColorAt(0.22, QColor(255, 255, 255, 0));
	p.fillPath(path, light);
	p.restore();
}

QString upper(const QString &s)
{
	return s.toUpper();
}

} // namespace

/* A soft CSS-style shadow of a shape, for other parts of the plugin. */
void shape_shadow(QPainter &p, const QPainterPath &path, QPointF offset, qreal blurPx, const QColor &c)
{
	soft_shadow(p, path, offset, blurPx, c);
}

/* ---------- fonts ---------- */

void load_fonts()
{
	if (g_fontsLoaded)
		return;
	g_fontsLoaded = true;
	char *dir = obs_module_file("fonts");
	if (!dir)
		return;
	const QDir d(QString::fromUtf8(dir));
	bfree(dir);
	QStringList families;
	for (const QString &f : d.entryList({"*.ttf", "*.otf"}, QDir::Files)) {
		const int id = QFontDatabase::addApplicationFont(d.filePath(f));
		if (id >= 0)
			families += QFontDatabase::applicationFontFamilies(id);
	}
	if (families.contains("Inter"))
		g_body = "Inter";
	if (families.contains("Archivo"))
		g_head = "Archivo";
	if (families.contains("Montserrat"))
		g_brand = "Montserrat";
	if (families.contains("Architects Daughter"))
		g_script = "Architects Daughter";
}

QFont body(qreal px, int weight)
{
	return body_exact(px * TEXT_SCALE, weight);
}

QFont body_exact(qreal px, int weight)
{
	QFont f(g_body);
	f.setPixelSize(std::max(1, (int)std::lround(px)));
	f.setWeight((QFont::Weight)weight);
	/* Plain grey edges, as Chrome draws the website: Windows' coloured ClearType edges put
	   orange and blue fringes on the letters and made them look blurry (Nick, 2026-11-21). */
	f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
	/* Smooth letters, the way Chrome draws the website. Qt's default snaps every stroke to
	   the pixel grid, which made heavy capitals look blocky next to the website (Nick, 2026-11-21). */
	f.setHintingPreference(QFont::PreferNoHinting);
	/* Inter's space shrinks as the weight grows (2 px at 12 px Black), so
	   capital labels like GO LIVE ran together. Give heavy text a wider gap. */
	if (weight >= QFont::ExtraBold)
		f.setWordSpacing(px * 0.16);
	else if (weight >= QFont::Bold)
		f.setWordSpacing(px * 0.06);
	return f;
}

QFont head(qreal px)
{
	QFont f(g_head);
	f.setPixelSize(std::max(1, (int)std::lround(px * TEXT_SCALE)));
	f.setWeight(QFont::Black);
	f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
	f.setHintingPreference(QFont::PreferNoHinting);
	return f;
}

/* ---------- colour ---------- */

QColor mix(const QColor &a, const QColor &b, qreal t)
{
	return QColor::fromRgbF((float)(a.redF() * t + b.redF() * (1 - t)), (float)(a.greenF() * t + b.greenF() * (1 - t)),
				(float)(a.blueF() * t + b.blueF() * (1 - t)), (float)(a.alphaF() * t + b.alphaF() * (1 - t)));
}

QColor tone(const QColor &k, qreal t)
{
	return mix(k, Qt::white, t);
}

void set_hue(QWidget *w, const QColor &c)
{
	w->setProperty("svHue", c);
	w->update();
}

QColor hue_of(const QWidget *w)
{
	for (; w; w = w->parentWidget()) {
		const QVariant v = w->property("svHue");
		if (v.isValid() && v.value<QColor>().isValid())
			return v.value<QColor>();
	}
	return BLUE;
}

QColor alpha(const QColor &c, qreal a)
{
	QColor o = c;
	o.setAlphaF((float)std::clamp(a, 0.0, 1.0));
	return o;
}

/* ---------- icons ---------- */

QPixmap icon(const QString &name, const QColor &colour, int px, qreal dpr)
{
	const QString key = QString("i|%1|%2|%3|%4").arg(name, colour.name(QColor::HexArgb)).arg(px).arg(dpr);
	auto it = g_pixCache.find(key);
	if (it != g_pixCache.end())
		return *it;
	const QByteArray want = name.toUtf8();
	const char *inner = "";
	for (const IconDef &d : ICONS) {
		if (want == d.name) {
			inner = d.inner;
			break;
		}
	}
	const QByteArray svg = QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='") +
			       colour.name(QColor::HexRgb).toUtf8() + "' stroke-opacity='" +
			       QByteArray::number(colour.alphaF(), 'f', 3) +
			       "' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>" +
			       QByteArray(inner).replace("FILL", colour.name(QColor::HexRgb).toUtf8()) + "</svg>";
	const QPixmap pm = render_svg(svg, px, dpr);
	if (g_pixCache.size() > 400)
		g_pixCache.clear();
	g_pixCache.insert(key, pm);
	return pm;
}

QPixmap logo(const std::string &platform, int px, qreal dpr)
{
	for (const LogoDef &d : LOGOS) {
		if (platform == d.platform) {
			const QString key = QString("l|%1|%2|%3").arg(d.platform).arg(px).arg(dpr);
			auto it = g_pixCache.find(key);
			if (it != g_pixCache.end())
				return *it;
			const QByteArray svg = QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>") +
					       d.svg + "</svg>";
			const QPixmap pm = render_svg(svg, px, dpr);
			g_pixCache.insert(key, pm);
			return pm;
		}
	}
	if (platform == "instagram")
		return icon("instagram", QColor(255, 79, 154), px, dpr);
	return icon("server", BLUE, px, dpr);
}

QColor platform_colour(const std::string &platform)
{
	if (platform == "youtube")
		return QColor(255, 48, 64);
	if (platform == "twitch")
		return QColor(169, 112, 255);
	if (platform == "tiktok")
		return QColor(37, 244, 238);
	if (platform == "kick")
		return QColor(83, 252, 24);
	if (platform == "instagram")
		return QColor(255, 79, 154);
	if (platform == "facebook")
		return QColor(61, 139, 255);
	return BLUE;
}

/* ---------- text ---------- */

/* One line of words drawn as smooth shapes, the way Chrome draws the website. Qt's own text
   drawing on Windows adds blue and orange ClearType edges to every letter, which looked blurry
   and pixelated next to the website (Nick, 2026-11-21), and no font setting turned them off. */
static void smooth_text(QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &colour)
{
	if (text.isEmpty())
		return;
	const QFont &f = p.font();
	const QFontMetricsF fm(f);
	const qreal w = fm.horizontalAdvance(text);
	qreal x = r.left();
	if (flags & Qt::AlignHCenter)
		x = r.center().x() - w / 2;
	else if (flags & Qt::AlignRight)
		x = r.right() - w;
	qreal y;
	if (flags & Qt::AlignTop)
		y = r.top() + fm.ascent();
	else if (flags & Qt::AlignBottom)
		y = r.bottom() - fm.descent();
	else
		y = r.center().y() + (fm.ascent() - fm.descent()) / 2;
	QPainterPath path;
	path.addText(QPointF(x, y), f, text);
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.fillPath(path, colour);
	p.restore();
}

void glow_text(QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &colour, const QColor &glow,
	       qreal glowStrength)
{
	if (text.isEmpty())
		return;
	if (glowStrength > 0 && glow.alpha() > 0) {
		const qreal dpr = p.device()->devicePixelRatioF();
		const qreal pad = 7;
		QImage img((QSizeF(r.width() + pad * 2, r.height() + pad * 2) * dpr).toSize(),
			   QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		{
			QPainter q(&img);
			q.setRenderHint(QPainter::TextAntialiasing);
			q.scale(dpr, dpr);
			q.setFont(p.font());
			smooth_text(q, QRectF(pad, pad, r.width(), r.height()), flags, text, glow);
		}
		blur(img, (int)std::lround(3.0 * dpr));
		img.setDevicePixelRatio(dpr);
		p.save();
		p.setOpacity(p.opacity() * std::min(1.0, glowStrength * 1.6));
		p.drawImage(r.topLeft() - QPointF(pad, pad), img);
		p.restore();
	}
	smooth_text(p, r.translated(0, 1), flags, text, QColor(0, 0, 0, 215));
	smooth_text(p, r, flags, text, colour);
}

/* ---------- window style ---------- */

QString sheet()
{
	char *chev = obs_module_file("icons/chevron-down.svg");
	char *check = obs_module_file("icons/check.svg");
	const QString chevron = chev ? QString::fromUtf8(chev).replace('\\', '/') : QString();
	const QString tick = check ? QString::fromUtf8(check).replace('\\', '/') : QString();
	bfree(chev);
	bfree(check);
	QString s;
	s += "QLabel { color: #b7c6ff; background: transparent; }";
	s += "QLineEdit, QSpinBox, QComboBox { background: #101019; color: #eef2ff; border: 1px solid rgba(77,124,255,0.28);"
	     " border-radius: 8px; padding: 6px 9px; min-height: 18px; selection-background-color: rgba(77,124,255,0.55);"
	     " font-family: '" + g_body + "'; font-size: 17px; }";
	s += "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid rgba(77,124,255,0.8); }";
	s += "QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { color: #8ea6ff; border-color: rgba(77,124,255,0.14); }";
	s += "QSpinBox::up-button, QSpinBox::down-button { width: 0px; border: 0; }";
	/* The typing part of a NumberBox: the box itself is painted around it. */
	s += "QSpinBox#svNum, QSpinBox#svNum:focus, QSpinBox#svNum:disabled { background: transparent; border: 0;"
	     " padding: 0px; margin: 0px; font-weight: 700; font-size: 18px; }";
	s += "QComboBox::drop-down { border: 0; width: 26px; }";
	if (!chevron.isEmpty())
		s += "QComboBox::down-arrow { image: url('" + chevron + "'); width: 13px; height: 13px; }";
	s += "QComboBox QAbstractItemView { background: #13131c; color: #eef2ff; border: 1px solid rgba(77,124,255,0.35);"
	     " selection-background-color: rgba(77,124,255,0.3); outline: 0; padding: 4px; }";
	s += "QCheckBox { color: #d6e0ff; spacing: 8px; font-family: '" + g_body + "'; font-size: 17px; }";
	s += "QCheckBox::indicator, QListWidget::indicator { width: 18px; height: 18px; border-radius: 4px; background: #101019;"
	     " border: 1px solid rgba(77,124,255,0.4); }";
	s += "QCheckBox::indicator:checked, QListWidget::indicator:checked { background: rgb(77,124,255); border-color: rgb(140,170,255);";
	if (!tick.isEmpty())
		s += " image: url('" + tick + "');";
	s += " }";
	s += "QListWidget { background: #101019; color: #eef2ff; border: 1px solid rgba(77,124,255,0.22); border-radius: 10px;"
	     " padding: 6px; outline: 0; font-family: '" + g_body + "'; font-size: 17px; }";
	s += "QListWidget::item { padding: 6px 4px; border-radius: 6px; }";
	s += "QListWidget::item:hover { background: rgba(77,124,255,0.10); }";
	s += "QListWidget::item:selected { background: rgba(77,124,255,0.18); color: #ffffff; }";
	s += "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: 0; }";
	s += "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }";
	s += "QScrollBar::handle:vertical { background: rgba(77,124,255,0.38); border-radius: 3px; min-height: 28px; }";
	s += "QScrollBar::handle:vertical:hover { background: rgba(77,124,255,0.6); }";
	s += "QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { height: 0; background: transparent; }";
	s += "QScrollBar:horizontal { height: 0; }";
	s += "QMenu { background: #13131c; color: #d6e0ff; border: 1px solid rgba(77,124,255,0.35); border-radius: 8px; padding: 5px;"
	     " font-family: '" + g_body + "'; font-size: 17px; }";
	s += "QMenu::item { padding: 7px 16px 7px 10px; border-radius: 6px; }";
	s += "QMenu::item:selected { background: rgba(77,124,255,0.22); color: #ffffff; }";
	s += "QMenu::icon { padding-left: 8px; }";
	s += "QMenu::separator { height: 1px; background: rgba(77,124,255,0.18); margin: 4px 6px; }";
	return s;
}

/* Our menus are lit cards, like every box in the panel: a rounded slab with a soft shadow,
   a lit line along the top and a glow rising from the bottom. The window itself is see
   through, so only the card shows, and each row lights up in the menu's colour on hover. */
namespace {
class MenuSlab : public QObject {
public:
	explicit MenuSlab(QMenu *menu, const QColor &a) : QObject(menu), a(a) {}

protected:
	bool eventFilter(QObject *obj, QEvent *e) override
	{
		if (e->type() == QEvent::Paint) {
			auto *w = static_cast<QWidget *>(obj);
			QPainter p(w);
			p.setRenderHint(QPainter::Antialiasing);
			const QRectF r = QRectF(w->rect()).adjusted(5, 4, -6, -7);
			const qreal rad = 14;
			QPainterPath path;
			path.addRoundedRect(r, rad, rad);
			soft_shadow(p, path, QPointF(2, 3), 6, QColor(0, 0, 0, 200));
			QLinearGradient slab(r.topLeft(), r.bottomLeft());
			slab.setColorAt(0, mix(a, QColor(0x1d, 0x1d, 0x2a), 0.10));
			slab.setColorAt(0.5, QColor(0x14, 0x14, 0x1e));
			slab.setColorAt(1, QColor(0x0e, 0x0e, 0x16));
			p.fillPath(path, slab);
			p.save();
			p.setClipPath(path);
			/* The wash falls from the top left corner, like light from the lit line. */
			QRadialGradient wash(QPointF(0, 0), 1.0);
			wash.setColorAt(0, alpha(a, 0.30));
			wash.setColorAt(0.6, alpha(a, 0.06));
			wash.setColorAt(1, alpha(a, 0.0));
			p.save();
			p.translate(r.topLeft());
			p.scale(r.width() * 0.9, 70);
			p.fillRect(QRectF(0, 0, 1.2, 1.2), wash);
			p.restore();
			QLinearGradient hair(r.topLeft(), r.topRight());
			hair.setColorAt(0, alpha(tone(a, 0.35), 1.0));
			hair.setColorAt(0.55, alpha(a, 0.7));
			hair.setColorAt(1, alpha(a, 0.05));
			QPainterPath hairPath;
			hairPath.addRect(QRectF(r.left(), r.top(), r.width(), 2));
			soft_shadow(p, hairPath, QPointF(0, 0), 10, alpha(a, 0.9));
			p.fillRect(QRectF(r.left(), r.top(), r.width(), 2), hair);
			/* A faint light along the inside of the top edge, so the card reads as raised. */
			p.fillRect(QRectF(r.left(), r.top() + 2, r.width(), 1), QColor(255, 255, 255, 14));
			QRadialGradient up(QPointF(0, 0), 1.0);
			up.setColorAt(0, alpha(a, 0.24));
			up.setColorAt(0.75, alpha(a, 0.0));
			p.translate(r.center().x(), r.bottom());
			p.scale(r.width() * 0.7, 40);
			p.setCompositionMode(QPainter::CompositionMode_Screen);
			p.fillRect(QRectF(-1, -1, 2, 1), up);
			p.restore();
			p.setPen(QPen(alpha(a, 0.40), 1));
			p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
		}
		return false;
	}

private:
	QColor a;
};

QString rgba_css(const QColor &c, qreal a)
{
	return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
}

void lit_one(QMenu *menu, const QColor &a)
{
	if (menu->property("svLit").toBool())
		return;
	menu->setProperty("svLit", true);
	menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
	menu->setAttribute(Qt::WA_TranslucentBackground);
	menu->installEventFilter(new MenuSlab(menu, a));
	const QColor t = tone(a);
	QString s;
	s += "QMenu { background: transparent; border: 0; padding: 12px 13px 15px 13px; color: #e8eeff;"
	     " font-family: '" + body(14.5, QFont::Medium).family() + "'; font-size: 17px; font-weight: 500; }";
	/* icon-size makes the icon column wider than the 16px icons, which sit in its middle,
	   so there is a clear gap between each icon and its words. */
	s += "QMenu { icon-size: 30px; }";
	s += "QMenu::item { padding: 8px 24px 8px 4px; margin: 1px 0px; border-radius: 9px; border: 1px solid transparent;"
	     " background: transparent; }";
	s += "QMenu::item:selected { color: " + t.name() + ";"
	     " background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 " + rgba_css(a, 0.34) + ", stop:1 " +
	     rgba_css(a, 0.16) + "); border: 1px solid " + rgba_css(a, 0.55) + "; }";
	s += "QMenu::item:disabled { color: " + INK3.name() + "; }";
	s += "QMenu::separator { height: 1px; margin: 6px 10px;"
	     " background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 " + rgba_css(a, 0.0) + ", stop:0.5 " +
	     rgba_css(a, 0.5) + ", stop:1 " + rgba_css(a, 0.0) + "); }";
	s += "QMenu::right-arrow { width: 10px; height: 10px; padding-right: 8px; }";
	menu->setStyleSheet(s);
}
} // namespace

void lit_menu(QMenu *menu, const QColor &accent)
{
	lit_one(menu, accent);
	/* Sub menus are often added after this runs, so they are lit just before the menu opens. */
	QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, accent] {
		for (QMenu *sub : menu->findChildren<QMenu *>())
			lit_one(sub, accent);
	});
}

/* The website's lamp, drawn once per width: a 2px line, bright across the middle and fading
   at both ends, with a soft blue glow, and a bloom of light that falls from it. */
namespace {
constexpr int LAMP_ABOVE = 48;
constexpr int LAMP_BELOW = 120;

QImage lamp_image(int w, qreal dpr)
{
	static QImage cached;
	static int cachedW = -1;
	static qreal cachedDpr = 0;
	if (w == cachedW && dpr == cachedDpr)
		return cached;
	const int h = LAMP_ABOVE + LAMP_BELOW;
	const qreal cx = w / 2.0;
	const qreal y = LAMP_ABOVE;

	QImage img(QSize(w, h) * dpr, QImage::Format_ARGB32_Premultiplied);
	img.setDevicePixelRatio(dpr);
	img.fill(Qt::transparent);
	QPainter q(&img);
	q.setRenderHint(QPainter::Antialiasing);

	/* The bloom: mostly falling below the line, a little spilling above it. */
	auto bloom = [&](qreal reach, bool below, qreal strength) {
		QRadialGradient g(QPointF(cx, y), 1.0);
		g.setColorAt(0.0, QColor(140, 175, 255, int(0.46 * strength * 255)));
		g.setColorAt(0.18, QColor(90, 130, 240, int(0.26 * strength * 255)));
		g.setColorAt(0.40, QColor(90, 130, 240, int(0.11 * strength * 255)));
		g.setColorAt(0.68, QColor(90, 130, 240, 0));
		q.save();
		q.translate(cx, y);
		q.scale(w * 0.5, reach);
		q.translate(-cx, -y);
		q.fillRect(QRectF(cx - 1, below ? y : y - 1, 2, 1), g);
		q.restore();
	};
	/* Kept short and soft so it glows round the line and is gone before the buttons. */
	bloom(70, true, 0.9);
	bloom(26, false, 0.5);

	/* The line and its glow, faded out at both ends. */
	QImage band(QSize(w, h) * dpr, QImage::Format_ARGB32_Premultiplied);
	band.setDevicePixelRatio(dpr);
	band.fill(Qt::transparent);
	{
		QPainter b(&band);
		QLinearGradient glow(0, y - 22, 0, y + 22);
		glow.setColorAt(0.0, QColor(90, 130, 255, 0));
		glow.setColorAt(0.25, QColor(90, 130, 255, int(0.10 * 255)));
		glow.setColorAt(0.42, QColor(90, 130, 255, int(0.38 * 255)));
		glow.setColorAt(0.5, QColor(90, 130, 255, int(0.62 * 255)));
		glow.setColorAt(0.58, QColor(90, 130, 255, int(0.38 * 255)));
		glow.setColorAt(0.75, QColor(90, 130, 255, int(0.10 * 255)));
		glow.setColorAt(1.0, QColor(90, 130, 255, 0));
		b.fillRect(QRectF(0, y - 22, w, 44), glow);
		b.fillRect(QRectF(0, y - 1, w, 2), QColor(190, 210, 255));
		b.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		QLinearGradient ends(0, 0, w, 0);
		ends.setColorAt(0.0, QColor(0, 0, 0, 0));
		ends.setColorAt(0.18, QColor(0, 0, 0, 255));
		ends.setColorAt(0.82, QColor(0, 0, 0, 255));
		ends.setColorAt(1.0, QColor(0, 0, 0, 0));
		b.fillRect(QRectF(0, 0, w, h), ends);
	}
	q.drawImage(QPointF(0, 0), band);
	q.end();

	cached = img;
	cachedW = w;
	cachedDpr = dpr;
	return cached;
}
} // namespace

/* The lighter lit slab the website's control panel sits on (DESIGN.md Ref #15, the same
   one Adopt and Time It use behind their cards): a hint of Vertical blue at the top,
   #1b1b27, easing to #13131c by the middle and #0e0e16 at the bottom. Lighter than the
   old dimmed page picture, so the raised cards' shadows and edges stand out against it.
   Never darken or drop it. Made once per size and kept, so painting it costs one copy. */
static QImage slab_image(const QSize &size, qreal dpr)
{
	struct Entry {
		QSize size;
		qreal dpr;
		QImage img;
	};
	static std::vector<Entry> cache;
	for (const Entry &e : cache)
		if (e.size == size && e.dpr == dpr)
			return e.img;

	QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
	img.setDevicePixelRatio(dpr);
	img.fill(BG);
	QPainter q(&img);
	QLinearGradient slab(0, 0, 0, size.height());
	slab.setColorAt(0.0, mix(BLUE, QColor(0x1b, 0x1b, 0x27), 0.06));
	slab.setColorAt(0.5, QColor(0x13, 0x13, 0x1c));
	slab.setColorAt(1.0, QColor(0x0e, 0x0e, 0x16));
	q.fillRect(QRect(QPoint(0, 0), size), slab);
	q.end();

	if (cache.size() >= 8)
		cache.erase(cache.begin());
	cache.push_back({size, dpr, img});
	return img;
}

void paint_backdrop(QPainter &p, const QRect &r, bool topGlow, int lampY)
{
	const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
	p.drawImage(r.topLeft(), slab_image(r.size(), dpr));
	if (lampY >= 0) {
		const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		p.drawImage(QPointF(r.left(), r.top() + lampY - LAMP_ABOVE), lamp_image(r.width(), dpr));
		return;
	}
	if (topGlow) {
		QRadialGradient g(QPointF(r.center().x(), r.top()), 1.0);
		g.setColorAt(0, alpha(BLUE, 0.10));
		g.setColorAt(0.7, alpha(BLUE, 0.0));
		p.save();
		p.translate(r.center().x(), r.top());
		p.scale(r.width() * 0.6, std::max(160.0, r.height() * 0.3));
		p.translate(-r.center().x(), -r.top());
		p.fillRect(QRectF(r.center().x() - 1, r.top() - 1, 2, 2), g);
		p.restore();
	}
}

/* ---------- Key ---------- */

Key::Key(const QString &text, QWidget *parent) : QPushButton(text, parent)
{
	setCursor(Qt::PointingHandCursor);
	setAttribute(Qt::WA_Hover);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setFlat(true);
}

bool Key::event(QEvent *e)
{
	/* OBS's theme locks every button to one input's height (36 px), which squashed keys
	   that carry a status line. The key sizes itself, so the lock is undone each time the
	   theme is applied. */
	if (e->type() == QEvent::Polish || e->type() == QEvent::StyleChange) {
		const bool r = QPushButton::event(e);
		setMinimumHeight(0);
		setMaximumHeight(QWIDGETSIZE_MAX);
		return r;
	}
	/* A key is named by its hover tip, so a key that shows only an icon still has a name
	   that screen readers and other helpers can read out. */
	if (e->type() == QEvent::ToolTipChange)
		setAccessibleName(toolTip());
	return QPushButton::event(e);
}

void Key::setAccent(const QColor &c)
{
	if (k == c)
		return;
	k = c;
	update();
}

void Key::setIconName(const QString &name)
{
	iconName = name;
	updateGeometry();
	update();
}

void Key::setPlatform(const std::string &p)
{
	platform = p;
	/* The logo takes the icon's place, so the sizing counts it as one. */
	iconName = p.empty() ? QString() : QStringLiteral("logo");
	updateGeometry();
	update();
}

void Key::setBig(bool b)
{
	big = b;
	setSizePolicy(b ? QSizePolicy::Expanding : QSizePolicy::Fixed, QSizePolicy::Fixed);
	updateGeometry();
	update();
}

void Key::setUnchosen(bool off)
{
	unchosen = off;
	update();
}

void Key::setSub(const QString &s, const QColor &colour)
{
	if (sub == s && subColour == colour)
		return;
	const bool grew = sub.isEmpty() != s.isEmpty();
	sub = s;
	subColour = colour;
	if (grew)
		updateGeometry();
	update();
}

static QFont sub_font()
{
	return body(12, QFont::DemiBold);
}

/* The website key's proportions, measured on Nick's screen (2026-11-21): 15 px words in a
   37 px tall key, a 19 px icon, 12 px each side and a 6 px gap. The key used to be 32 px tall
   round 16 px words, so the words filled it and it looked clunky next to the website. */
static qreal key_icon_px(bool big)
{
	return big ? 21 : 19;
}

static qreal key_gap()
{
	return 6;
}

/* 15 px words, the website key's size on Nick's screen (17 px on a big key). */
static QFont key_font(bool big)
{
	QFont f = body(big ? 13.5 : 12.5, QFont::Black);
	f.setLetterSpacing(QFont::PercentageSpacing, 104);
	/* The website's normal gap between words, not the plugin's wider one. */
	f.setWordSpacing(0);
	return f;
}

QSize Key::sizeHint() const
{
	QFont f = key_font(big);
	const QFontMetricsF fm(f);
	const QString t = upper(text());
	const qreal iconPx = key_icon_px(big);
	const qreal padX = t.isEmpty() ? 9 : (big ? 14 : 13);
	const qreal padY = sub.isEmpty() ? (big ? 10.5 : 9) : 9;
	qreal w = padX * 2;
	if (!iconName.isEmpty())
		w += iconPx;
	if (!iconName.isEmpty() && !t.isEmpty())
		w += key_gap();
	if (!t.isEmpty())
		w += fm.horizontalAdvance(t);
	qreal h = padY * 2 + std::max(iconPx, fm.height() * 0.82);
	if (!sub.isEmpty()) {
		const QFontMetricsF sm(sub_font());
		w = std::max(w, padX * 2 + sm.horizontalAdvance(sub));
		h += sm.height() + 3;
	}
	return QSize((int)std::ceil(w) + 8, (int)std::ceil(h) + 9);
}

void Key::enterEvent(QEnterEvent *e)
{
	hover = true;
	update();
	QPushButton::enterEvent(e);
}

void Key::leaveEvent(QEvent *e)
{
	hover = false;
	update();
	QPushButton::leaveEvent(e);
}

/* A key's words the website's way (text-shadow: 0 1px 1px rgba(0,0,0,.85), 0 1px 6px glow .6):
   a soft one pixel dark drop and a wide, light haze in the key's colour. A hard black copy and
   a thick haze made the words look stamped on and fuzzy (Nick, 2026-11-21). */
static void key_text(QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &colour,
		     const QColor &glow, qreal glowAlpha)
{
	if (text.isEmpty())
		return;
	const qreal dpr = p.device()->devicePixelRatioF();
	const qreal pad = 9;
	/* blurPx > 0: the usual two-pass blur. -1: one light pass, for the website's 1 px blur, which
	   is barely soft at all; the full blur smeared the dark drop and made the words look blurry. */
	auto layer = [&](const QColor &c, int blurPx, qreal opacity) {
		QImage img((QSizeF(r.width() + pad * 2, r.height() + pad * 2) * dpr).toSize(),
			   QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		{
			QPainter q(&img);
			q.setRenderHint(QPainter::TextAntialiasing);
			q.scale(dpr, dpr);
			q.setFont(p.font());
			smooth_text(q, QRectF(pad, pad, r.width(), r.height()), flags, text, c);
		}
		if (blurPx > 0) {
			blur(img, blurPx);
		} else {
			auto *bits = reinterpret_cast<uint32_t *>(img.bits());
			const int stride = (int)(img.bytesPerLine() / 4);
			std::vector<uint32_t> tmp;
			for (int y = 0; y < img.height(); y++)
				box_pass(bits + (size_t)y * stride, img.width(), 1, 1, tmp);
			for (int x = 0; x < img.width(); x++)
				box_pass(bits + x, img.height(), stride, 1, tmp);
		}
		img.setDevicePixelRatio(dpr);
		p.save();
		p.setOpacity(p.opacity() * opacity);
		/* Both shadows sit 1 px down, as on the website. */
		p.drawImage(r.topLeft() - QPointF(pad, pad - 1), img);
		p.restore();
	};
	if (glowAlpha > 0)
		layer(glow, (int)std::lround(3.0 * dpr), glowAlpha);
	layer(QColor(0, 0, 0), -1, 0.85);
	smooth_text(p, r, flags, text, colour);
}

void Key::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	/* Room round the key for its shadow (1 px right, 2 px down, 4 px blur) and its lamp's
	   glow to fade out to nothing; any less and the widget's edge cuts them off in a hard
	   straight line. */
	QRectF r = QRectF(rect()).adjusted(2, 2, -6, -7);
	if (isEnabled() && !unchosen) {
		if (isDown())
			r.translate(0, 1);
		else if (hover)
			r.translate(0, -1);
	}
	const qreal rad = 6;
	QPainterPath path;
	path.addRoundedRect(r, rad, rad);

	if (!unchosen) {
		soft_shadow(p, path, QPointF(1, 2), 4, QColor(0, 0, 0, 153));
		soft_shadow(p, path, QPointF(-1, -1), 3, QColor(255, 255, 255, 13));
		QLinearGradient fill(r.topLeft(), r.bottomLeft());
		fill.setColorAt(0, mix(k, QColor(0x23, 0x23, 0x2f), 0.16));
		fill.setColorAt(0.55, QColor(0x16, 0x16, 0x1f));
		fill.setColorAt(1, QColor(0x0f, 0x0f, 0x17));
		p.fillPath(path, fill);

		p.save();
		p.setClipPath(path);
		/* The rising wash of light from the lamp. */
		QRadialGradient wash(QPointF(0, 0), 1.0);
		wash.setColorAt(0, alpha(k, 0.30));
		wash.setColorAt(0.7, alpha(k, 0.0));
		p.save();
		p.translate(r.center().x(), r.bottom() + r.height() * 0.25);
		p.scale(r.width() * 0.75, r.height() * 0.95);
		p.setCompositionMode(QPainter::CompositionMode_Screen);
		p.fillRect(QRectF(-1, -1, 2, 2), wash);
		p.restore();
		/* inset 0 -8px 12px -8px: a thin glow along the bottom edge. */
		/* The website's inset shadow fades like a soft blur from 0.3 at the edge, not in a
		   straight line from 0.42: the straight fade read as a coloured stripe along the
		   whole bottom (Nick, 2026-11-21). 16 px is its reach at his 125% page zoom. */
		QLinearGradient under(QPointF(0, r.bottom()), QPointF(0, r.bottom() - 16));
		under.setColorAt(0, alpha(k, 0.30));
		under.setColorAt(0.16, alpha(k, 0.22));
		under.setColorAt(0.31, alpha(k, 0.15));
		under.setColorAt(0.47, alpha(k, 0.095));
		under.setColorAt(0.69, alpha(k, 0.04));
		under.setColorAt(1, alpha(k, 0.0));
		p.fillRect(QRectF(r.left(), r.bottom() - 16, r.width(), 16), under);
		/* inset 0 1px 1px white .12 */
		p.setPen(QPen(QColor(255, 255, 255, 31), 1));
		p.drawLine(QPointF(r.left() + rad * 0.6, r.top() + 0.5), QPointF(r.right() - rad * 0.6, r.top() + 0.5));
		p.restore();
	}

	/* The lamp line along the bottom. */
	/* 2 px on the website, 2.5 at Nick's 125% page zoom. */
	const QRectF lamp(r.left() + r.width() * 0.16, r.bottom() - 2.5, r.width() * 0.68, 2.5);
	QLinearGradient lg(lamp.topLeft(), lamp.topRight());
	lg.setColorAt(0, alpha(k, 0));
	lg.setColorAt(0.5, alpha(k, 1));
	lg.setColorAt(1, alpha(k, 0));
	p.save();
	if (unchosen)
		p.setOpacity(p.opacity() * 0.25);
	else
		/* The line and its glow stay inside the key, as on the website (its key clips them):
		   spilling out under the key, the glow read as a coloured smear below it. */
		p.setClipPath(path);
	QPainterPath lampPath;
	lampPath.addRoundedRect(lamp, lamp.height() / 2, lamp.height() / 2);
	if (!unchosen) {
		/* The glow comes from the line itself (the website's drop-shadow), so it fades out
		   towards both ends with the line. A glow of the whole bar stopped short at its ends
		   and looked like a chunky stripe (Nick, 2026-11-21). */
		const qreal dpr = devicePixelRatioF();
		const qreal pad = 8;
		const QRectF area = lamp.adjusted(-pad, -pad, pad, pad);
		QImage img((area.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		{
			QPainter q(&img);
			q.setRenderHint(QPainter::Antialiasing);
			q.scale(dpr, dpr);
			q.translate(-area.topLeft());
			q.fillPath(lampPath, lg);
		}
		blur(img, (int)std::lround(3 * dpr));
		img.setDevicePixelRatio(dpr);
		p.save();
		p.setOpacity(p.opacity() * 0.9);
		p.drawImage(area.topLeft(), img);
		p.restore();
	}
	p.fillPath(lampPath, lg);
	p.restore();

	/* Icon and words. */
	QFont f = key_font(big);
	p.setFont(f);
	const QFontMetricsF fm(f);
	QString t = upper(text());
	const qreal iconPx = key_icon_px(big);
	/* With a status line the name sits in the top part and the status under it. */
	QRectF line = r;
	QRectF subLine;
	if (!sub.isEmpty()) {
		const QFontMetricsF sm(sub_font());
		const qreal nameH = std::max(iconPx, fm.height() * 0.82);
		const qreal block = nameH + 3 + sm.height();
		const qreal top = r.center().y() - block / 2;
		line = QRectF(r.left(), top, r.width(), nameH);
		subLine = QRectF(r.left() + 6, top + nameH + 3, r.width() - 12, sm.height());
	}
	/* Too narrow for it all: the icon goes first, then the name is cut short. */
	const qreal room = r.width() - 12;
	bool showIcon = !iconName.isEmpty();
	qreal tw = t.isEmpty() ? 0 : fm.horizontalAdvance(t);
	if (showIcon && !t.isEmpty() && iconPx + key_gap() + tw > room)
		showIcon = false;
	if (!t.isEmpty() && tw > room) {
		t = fm.elidedText(t, Qt::ElideRight, room);
		tw = fm.horizontalAdvance(t);
	}
	const qreal gap = (showIcon && !t.isEmpty()) ? key_gap() : 0;
	const qreal total = (showIcon ? iconPx : 0) + gap + tw;
	qreal x = line.center().x() - total / 2;
	const qreal dpr = devicePixelRatioF();
	if (showIcon && !platform.empty()) {
		p.save();
		if (unchosen)
			p.setOpacity(p.opacity() * 0.75);
		p.drawPixmap(QPointF(x, line.center().y() - iconPx / 2), svl::logo(platform, (int)iconPx, dpr));
		p.restore();
		x += iconPx + gap;
	} else if (showIcon) {
		const QPixmap pm = svl::icon(iconName, unchosen ? alpha(k, 0.8) : k, (int)iconPx, dpr);
		p.drawPixmap(QPointF(x, line.center().y() - iconPx / 2), pm);
		x += iconPx + gap;
	}
	if (!t.isEmpty()) {
		const QColor colour = unchosen ? alpha(k, 0.85) : tone(k);
		key_text(p, QRectF(x, line.top(), tw + 2, line.height()), Qt::AlignVCenter | Qt::AlignLeft, t, colour, k,
			 unchosen ? 0.25 : 0.6);
	}
	if (!sub.isEmpty()) {
		const QFont sf = sub_font();
		p.setFont(sf);
		const QString s = QFontMetricsF(sf).elidedText(sub, Qt::ElideRight, subLine.width());
		key_text(p, subLine, Qt::AlignCenter, s, subColour, subColour, 0.35);
	}
}

/* ---------- a list row ---------- */

void paint_list_row(QPainter &p, const QRectF &rIn, const QColor &k, const QString &iconName, const QString &name,
		    const QString &note, bool on, bool picked, bool hover, qreal nameRight)
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const qreal dpr = p.device()->devicePixelRatioF();
	QRectF r = rIn;
	if (on && hover && !picked)
		r.translate(0, -1);
	const qreal rad = 8;
	QPainterPath path;
	path.addRoundedRect(r, rad, rad);

	if (picked)
		soft_shadow(p, path, QPointF(0, 0), 7, alpha(k, 0.45));
	if (on) {
		/* Raised: the same build as a lamp key, lower and wider. */
		soft_shadow(p, path, QPointF(2, 2), 5, QColor(0, 0, 0, 153));
		soft_shadow(p, path, QPointF(-1, -1), 3, QColor(255, 255, 255, 13));
		QLinearGradient fill(r.topLeft(), r.bottomLeft());
		fill.setColorAt(0, mix(k, QColor(0x23, 0x23, 0x2f), picked ? 0.30 : (hover ? 0.22 : 0.16)));
		fill.setColorAt(0.55, QColor(0x16, 0x16, 0x1f));
		fill.setColorAt(1, QColor(0x0f, 0x0f, 0x17));
		p.fillPath(path, fill);

		p.save();
		p.setClipPath(path);
		/* The wash of light rising from the lamp along the bottom. */
		QRadialGradient wash(QPointF(0, 0), 1.0);
		wash.setColorAt(0, alpha(k, picked ? 0.34 : (hover ? 0.24 : 0.15)));
		wash.setColorAt(0.7, alpha(k, 0.0));
		p.save();
		p.translate(r.center().x(), r.bottom() + r.height() * 0.3);
		p.scale(r.width() * 0.7, r.height() * 1.1);
		p.setCompositionMode(QPainter::CompositionMode_Screen);
		p.fillRect(QRectF(-1, -1, 2, 2), wash);
		p.restore();
		p.setPen(QPen(QColor(255, 255, 255, 31), 1));
		p.drawLine(QPointF(r.left() + rad * 0.6, r.top() + 0.5), QPointF(r.right() - rad * 0.6, r.top() + 0.5));
		p.restore();

		/* The lamp line along the bottom: soft at rest, bright when picked. */
		const QRectF lamp(r.left() + r.width() * 0.12, r.bottom() - 2, r.width() * 0.76, 2);
		QLinearGradient lg(lamp.topLeft(), lamp.topRight());
		const qreal la = picked ? 1.0 : (hover ? 0.8 : 0.6);
		lg.setColorAt(0, alpha(k, 0));
		lg.setColorAt(0.5, alpha(k, la));
		lg.setColorAt(1, alpha(k, 0));
		QPainterPath lampPath;
		lampPath.addRoundedRect(lamp, 1, 1);
		soft_shadow(p, lampPath, QPointF(0, 0), 4, alpha(k, 0.9 * la));
		p.fillPath(lampPath, lg);
	} else {
		/* Hidden: carved into the panel, no lamp. */
		p.fillPath(path, QColor(0x12, 0x12, 0x1a));
		carve(p, path);
	}
	if (picked) {
		p.setPen(QPen(alpha(k, 0.8), 1));
		p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
	}

	/* The drag handle. */
	const qreal cy = r.center().y();
	p.drawPixmap(QPointF(r.left() + 7, cy - 7), icon("grip", alpha(k, on ? 0.6 : 0.35), 14, dpr));

	/* The kind icon in a small carved well, lit in the row's colour. */
	const QRectF well(r.left() + 25, cy - 13, 26, 26);
	QPainterPath wp;
	wp.addRoundedRect(well, 6, 6);
	p.fillPath(wp, QColor(0x0d, 0x0d, 0x15));
	carve(p, wp);
	const QPixmap ic = icon(iconName, on ? k : alpha(k, 0.5), 16, dpr);
	const QPointF at(well.center().x() - 8, well.center().y() - 8);
	if (on)
		glow_pixmap(p, at, ic, 4, picked ? 0.9 : 0.6);
	p.drawPixmap(at, ic);

	/* The note at the right, then the name in the room left. */
	qreal right = nameRight;
	if (!note.isEmpty()) {
		const QFont nf = body(11, QFont::Bold);
		p.setFont(nf);
		const qreal nw = QFontMetricsF(nf).horizontalAdvance(note) + 2;
		glow_text(p, QRectF(right - nw, r.top(), nw, r.height()), Qt::AlignVCenter | Qt::AlignRight, note, RED, RED,
			  0.5);
		right -= nw + 8;
	}
	const QFont f = body(12, QFont::DemiBold);
	p.setFont(f);
	const QRectF nr(well.right() + 9, r.top(), std::max<qreal>(10, right - well.right() - 9), r.height());
	const QString shown = QFontMetricsF(f).elidedText(name, Qt::ElideRight, nr.width());
	const QColor ink = picked ? QColor(Qt::white) : (on ? INK : INK3);
	glow_text(p, nr, Qt::AlignVCenter | Qt::AlignLeft, shown, ink, k, picked ? 0.7 : (on ? 0.25 : 0.0));
	p.restore();
}

/* ---------- Card ---------- */

Card::Card(const QString &t, const QString &i, QWidget *parent) : QWidget(parent), title(t), iconName(i)
{
	auto *outer = new QVBoxLayout(this);
	outer->setSpacing(0);
	bodyHost = new QWidget(this);
	bodyLayout = new QVBoxLayout(bodyHost);
	bodyLayout->setSpacing(12);
	outer->addWidget(bodyHost);
	place_body();
}

void Card::place_body()
{
	if (bare) {
		layout()->setContentsMargins(0, 0, 0, 0);
		bodyLayout->setContentsMargins(2, 2, 2, 2);
	} else if (headless) {
		layout()->setContentsMargins(6, 6, 7, 7);
		bodyLayout->setContentsMargins(10, 10, 10, 10);
	} else {
		layout()->setContentsMargins(6, 6 + HEAD, 7, 7);
		bodyLayout->setContentsMargins(12, 4, 12, 12);
	}
	updateGeometry();
	update();
}

void Card::setHeadless(bool on)
{
	headless = on;
	place_body();
}

void Card::setBare(bool on)
{
	bare = on;
	place_body();
}

void Card::setAccent(const QColor &c)
{
	if (a == c)
		return;
	a = c;
	update();
}

void Card::setTitle(const QString &t)
{
	if (title == t)
		return;
	title = t;
	update();
}

void Card::setValue(const QString &v)
{
	if (value == v)
		return;
	value = v;
	update(QRect(0, 0, width(), HEAD + 8));
}

void Card::setValueColour(const QColor &c)
{
	if (valueCol == c)
		return;
	valueCol = c;
	update(QRect(0, 0, width(), HEAD + 8));
}

void Card::setFoldable(bool f)
{
	foldable = f;
	setMouseTracking(f);
	setCursor(Qt::ArrowCursor);
	if (!f)
		setFolded(false);
	update();
}

void Card::setFolded(bool f)
{
	if (f == isFolded)
		return;
	isFolded = f;
	bodyHost->setVisible(!f);
	updateGeometry();
	update();
}

void Card::mousePressEvent(QMouseEvent *e)
{
	if (foldable && e->position().y() < HEAD + 6) {
		setFolded(!isFolded);
		if (onFold)
			onFold(isFolded);
		return;
	}
	QWidget::mousePressEvent(e);
}

void Card::mouseMoveEvent(QMouseEvent *e)
{
	setCursor(foldable && e->position().y() < HEAD + 6 ? Qt::PointingHandCursor : Qt::ArrowCursor);
	QWidget::mouseMoveEvent(e);
}

void Card::paintEvent(QPaintEvent *)
{
	if (bare)
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r = QRectF(rect()).adjusted(5, 5, -6, -6);
	const qreal rad = 16;
	QPainterPath path;
	path.addRoundedRect(r, rad, rad);
	soft_shadow(p, path, QPointF(3, 3), 8, QColor(0, 0, 0, 190));
	soft_shadow(p, path, QPointF(-3, -3), 8, QColor(255, 255, 255, 12));

	QLinearGradient slab(r.topLeft(), r.bottomLeft());
	slab.setColorAt(0, mix(a, QColor(0x1b, 0x1b, 0x27), 0.06));
	slab.setColorAt(0.5, QColor(0x13, 0x13, 0x1c));
	slab.setColorAt(1, QColor(0x0e, 0x0e, 0x16));
	p.fillPath(path, slab);
	p.fillPath(path, QColor(2, 6, 23, 140));
	QLinearGradient sink(r.topLeft(), r.bottomLeft());
	sink.setColorAt(0, QColor(0, 0, 0, 0));
	sink.setColorAt(0.55, QColor(0, 0, 0, 0));
	sink.setColorAt(1, QColor(0, 0, 0, 153));
	p.fillPath(path, sink);

	p.save();
	p.setClipPath(path);
	/* The header wash. A box with no header keeps only a faint one. */
	const QRectF headR(r.left(), r.top(), r.width(), headless ? 24 : HEAD);
	QLinearGradient wash(headR.topLeft(), headR.topRight());
	wash.setColorAt(0, alpha(a, 0.22));
	wash.setColorAt(0.46, alpha(a, 0.055));
	wash.setColorAt(0.78, alpha(a, 0.0));
	p.fillRect(headR, wash);
	/* The lit hairline along the top. */
	QLinearGradient hair(headR.topLeft(), headR.topRight());
	hair.setColorAt(0, alpha(a, 0.9));
	hair.setColorAt(0.4, alpha(a, 0.25));
	hair.setColorAt(0.75, alpha(a, 0.0));
	QPainterPath hairPath;
	hairPath.addRect(QRectF(r.left(), r.top(), r.width() * 0.75, 1));
	soft_shadow(p, hairPath, QPointF(0, 0), 5, alpha(a, 0.4));
	p.fillRect(QRectF(r.left(), r.top(), r.width(), 1), hair);
	/* The glow rising from the bottom. */
	if (!isFolded) {
		QRadialGradient up(QPointF(0, 0), 1.0);
		up.setColorAt(0, alpha(a, 0.22));
		up.setColorAt(0.75, alpha(a, 0.0));
		p.save();
		p.translate(r.center().x(), r.bottom());
		p.scale(r.width() * 0.7, 40);
		p.setCompositionMode(QPainter::CompositionMode_Screen);
		p.fillRect(QRectF(-1, -1, 2, 1), up);
		p.restore();
	}
	p.restore();

	p.setPen(QPen(alpha(a, 0.18), 1));
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
	if (headless)
		return;

	/* Header: icon, title, value. */
	const qreal dpr = devicePixelRatioF();
	qreal x = r.left() + 12;
	const qreal cy = r.top() + HEAD / 2.0;
	if (!iconName.isEmpty()) {
		p.drawPixmap(QPointF(x, cy - 8.5), svl::icon(iconName, a, 17, dpr));
		x += 25;
	}
	QFont tf = svl::body(14, QFont::Black);
	tf.setLetterSpacing(QFont::PercentageSpacing, 110);
	p.setFont(tf);
	const QString t = upper(title);
	const qreal tw = QFontMetricsF(tf).horizontalAdvance(t);
	glow_text(p, QRectF(x, r.top(), tw + 2, HEAD), Qt::AlignVCenter | Qt::AlignLeft, t, tone(a), a, 0.55);

	qreal right = r.right() - 12;
	if (foldable) {
		const QPixmap chev = svl::icon("chevron", alpha(a, 0.7), 13, dpr);
		p.save();
		p.translate(right - 6.5, cy);
		if (!isFolded)
			p.rotate(180);
		p.drawPixmap(QPointF(-6.5, -6.5), chev);
		p.restore();
		right -= 18;
	}
	if (!value.isEmpty()) {
		const QFont vf = svl::body(13.5, QFont::Bold);
		p.setFont(vf);
		const qreal room = right - (x + tw + 10);
		const QString v = QFontMetricsF(vf).elidedText(value, Qt::ElideRight, std::max(0.0, room));
		glow_text(p, QRectF(x + tw + 10, r.top(), room, HEAD), Qt::AlignVCenter | Qt::AlignRight, v,
			  valueCol.isValid() ? tone(valueCol, 0.25) : tone(a, 0.45), valueCol.isValid() ? valueCol : a, 0.45);
	}
}

/* ---------- Tile ---------- */

Tile::Tile(const QString &n, const QString &i, QWidget *parent) : QAbstractButton(parent), name(n), iconName(i)
{
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Tile::setAccent(const QColor &col)
{
	c = col;
	update();
}

void Tile::setOn(bool o)
{
	if (on == o)
		return;
	on = o;
	update();
}

void Tile::setName(const QString &n)
{
	if (name == n)
		return;
	name = n;
	update();
}

void Tile::setSub(const QString &s)
{
	if (sub == s)
		return;
	sub = s;
	update();
}

void Tile::setLogo(const std::string &p)
{
	platform = p;
	update();
}

void Tile::setMotion(int m, int f, int d, int length)
{
	motion = m;
	feel = f;
	dir = d;
	ms = length;
	if (!anim) {
		anim = new QTimer(this);
		anim->setInterval(33); /* about 30 times a second, only while it shows */
		connect(anim, &QTimer::timeout, this, qOverload<>(&QWidget::update));
		clock.start();
		if (isVisible())
			anim->start();
	}
	updateGeometry();
	update();
}

void Tile::showEvent(QShowEvent *e)
{
	QAbstractButton::showEvent(e);
	if (anim)
		anim->start();
}

void Tile::hideEvent(QHideEvent *e)
{
	QAbstractButton::hideEvent(e);
	if (anim)
		anim->stop();
}

/* Two layouts on a little phone: a camera box and a game box that swap places. */
void Tile::paintPhone(QPainter &p, const QRectF &ph) const
{
	struct Look {
		QRectF game, cam;
	};
	auto at = [&](qreal x, qreal y, qreal w, qreal h) {
		return QRectF(ph.left() + ph.width() * x, ph.top() + ph.height() * y, ph.width() * w, ph.height() * h);
	};
	const Look A{at(0.12, 0.40, 0.76, 0.50), at(0.12, 0.09, 0.76, 0.24)};
	const Look B{at(0.12, 0.09, 0.76, 0.50), at(0.12, 0.66, 0.76, 0.24)};

	const qreal hold = 900, len = motion == 0 ? 0 : std::clamp<qreal>(ms, 150, 1200);
	const qreal cycle = 2 * (hold + len);
	const qreal now = std::fmod((qreal)clock.elapsed(), cycle);
	/* Which way it is going this half, and how far along. */
	const bool back = now >= hold + len;
	const qreal in = back ? now - (hold + len) : now;
	const float t = len > 0 && in > hold ? (float)((in - hold) / len) : (in > hold ? 1.0f : 0.0f);
	const Look &from = back ? B : A;
	const Look &to = back ? A : B;

	const QColor gameFill = alpha(c, on ? 0.42 : 0.28);
	const QColor camFill = on ? c : alpha(c, 0.7);
	auto draw = [&](const Look &l, qreal opacity, QPointF off) {
		p.save();
		p.setOpacity(p.opacity() * opacity);
		p.setPen(Qt::NoPen);
		p.setBrush(gameFill);
		p.drawRoundedRect(l.game.translated(off), 2.5, 2.5);
		p.setBrush(camFill);
		p.drawRoundedRect(l.cam.translated(off), 2.5, 2.5);
		p.restore();
	};

	QPainterPath body;
	body.addRoundedRect(ph, 6, 6);
	p.fillPath(body, QColor(0x0d, 0x0d, 0x15));
	p.save();
	p.setClipPath(body);
	const float k = sv_ease(feel, t);
	if (motion == 0) {
		draw(t >= 1.0f ? to : from, 1, {});
	} else if (motion == 1) {
		const qreal e = std::clamp<qreal>(k, 0, 1);
		draw(from, 1 - e, {});
		draw(to, e, {});
	} else if (motion == 2) {
		const qreal e = std::clamp<qreal>(k, 0, 1);
		QPointF away(-ph.width(), 0);
		if (dir == SV_DIR_RIGHT)
			away = QPointF(ph.width(), 0);
		else if (dir == SV_DIR_UP)
			away = QPointF(0, -ph.height());
		else if (dir == SV_DIR_DOWN)
			away = QPointF(0, ph.height());
		draw(from, 1, away * e);
		draw(to, 1, -away * (1 - e));
	} else {
		auto lerp = [&](const QRectF &a, const QRectF &b) {
			return QRectF(a.x() + (b.x() - a.x()) * k, a.y() + (b.y() - a.y()) * k,
				      std::max<qreal>(1, a.width() + (b.width() - a.width()) * k),
				      std::max<qreal>(1, a.height() + (b.height() - a.height()) * k));
		};
		draw(Look{lerp(from.game, to.game), lerp(from.cam, to.cam)}, 1, {});
	}
	p.restore();
	p.setPen(QPen(alpha(c, on ? 0.9 : 0.55), 1.5));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(ph.adjusted(0.75, 0.75, -0.75, -0.75), 6, 6);
}

QSize Tile::sizeHint() const
{
	if (motion >= 0)
		return QSize(118, 150);
	return QSize(118, sub.isEmpty() ? 90 : 110);
}

QSize Tile::minimumSizeHint() const
{
	return QSize(84, sizeHint().height());
}

void Tile::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	const QRectF r = QRectF(rect()).adjusted(6, 6, -7, -7);
	const qreal rad = 12;
	QPainterPath path;
	path.addRoundedRect(r, rad, rad);
	const qreal dpr = devicePixelRatioF();

	if (on) {
		soft_shadow(p, path, QPointF(0, 0), 14, alpha(c, 0.42));
		soft_shadow(p, path, QPointF(4, 4), 10, QColor(0, 0, 0, 200));
		soft_shadow(p, path, QPointF(-3, -3), 8, QColor(255, 255, 255, 11));
		QLinearGradient g(r.topLeft(), r.bottomLeft());
		g.setColorAt(0, mix(c, QColor(0x14, 0x14, 0x1e), 0.09));
		g.setColorAt(0.5, QColor(0x14, 0x14, 0x1e));
		g.setColorAt(1, QColor(0x10, 0x10, 0x19));
		p.fillPath(path, g);
		p.save();
		p.setClipPath(path);
		p.setPen(QPen(QColor(255, 255, 255, 26), 1.5));
		p.drawLine(QPointF(r.left() + rad, r.top() + 0.75), QPointF(r.right() - rad, r.top() + 0.75));
		p.restore();
		p.setPen(QPen(alpha(c, 0.35), 1));
		p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
	} else {
		p.fillPath(path, QColor(0x13, 0x13, 0x1c));
		carve(p, path);
		if (underMouse() && isEnabled()) {
			p.setPen(QPen(alpha(c, 0.22), 1));
			p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
		}
	}

	const bool phone = motion >= 0;
	const qreal iconPx = phone ? 84 : 22;
	const bool hasSub = !sub.isEmpty();
	const QFont nf = body(13, QFont::ExtraBold);
	const QFont sf = body(12, QFont::DemiBold);
	const qreal nameH = QFontMetricsF(nf).height();
	const qreal subH = hasSub ? QFontMetricsF(sf).height() : 0;
	const qreal block = iconPx + 5 + nameH + (hasSub ? 2 + subH : 0);
	qreal y = r.center().y() - block / 2;
	const QPointF at(r.center().x() - iconPx / 2, y);
	if (phone) {
		const qreal pw = 47;
		paintPhone(p, QRectF(r.center().x() - pw / 2, y, pw, iconPx));
	} else if (!platform.empty()) {
		const QPixmap pm = logo(platform, (int)iconPx, dpr);
		if (!on)
			p.setOpacity(p.opacity() * 0.8);
		p.drawPixmap(at, pm);
		p.setOpacity(isEnabled() ? 1.0 : 0.45);
	} else {
		const QPixmap pm = svl::icon(iconName, on ? c : alpha(c, 0.75), (int)iconPx, dpr);
		if (on)
			glow_pixmap(p, at, svl::icon(iconName, c, (int)iconPx, dpr), 6, 0.9);
		p.drawPixmap(at, pm);
	}
	y += iconPx + 5;
	p.setFont(nf);
	const QString n = QFontMetricsF(nf).elidedText(name, Qt::ElideRight, r.width() - 8);
	glow_text(p, QRectF(r.left() + 4, y, r.width() - 8, nameH), Qt::AlignCenter, n, on ? Qt::white : INK2, c,
		  on ? 0.25 : 0.0);
	if (hasSub) {
		y += nameH + 2;
		p.setFont(sf);
		const QString s = QFontMetricsF(sf).elidedText(sub, Qt::ElideRight, r.width() - 8);
		glow_text(p, QRectF(r.left() + 4, y, r.width() - 8, subH), Qt::AlignCenter, s,
			  on ? tone(c, 0.4) : alpha(tone(c, 0.7), 0.9), c, on ? 0.6 : 0.0);
	}
}

/* ---------- LayoutTile ---------- */

LayoutTile::LayoutTile(QWidget *parent) : QAbstractButton(parent)
{
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void LayoutTile::setWebsite(const QString &n)
{
	website = true;
	haveLayout = false;
	name = n;
	setToolTip(n);
	update();
}

void LayoutTile::setLayout(const QString &n, const SvLayout &l, int num)
{
	website = false;
	haveLayout = true;
	name = n;
	layout = l;
	number = num;
	/* Each layout has its own colour so they read apart at a glance. */
	static const QColor hues[] = {ORANGE, EMERALD, PINK, CYAN, VIOLET, LIME, AMBER, GREEN};
	hue = hues[(std::max)(0, num - 1) % 8];
	setToolTip(n);
	update();
}

/* The name line under the phone. The layout's number sits in the phone's corner instead. */
static QString tile_label(const QString &name, int)
{
	return name;
}

int LayoutTile::wantedWidth() const
{
	const QFontMetricsF fm(body(11.5, QFont::Bold));
	return (int)std::ceil((std::min)(220.0, fm.horizontalAdvance(tile_label(name, number)) + 14));
}

void LayoutTile::setTileWidth(int w)
{
	w = (std::max)(84, w);
	if (tileW == w)
		return;
	tileW = w;
	updateGeometry();
	update();
}

void LayoutTile::setSelected(bool s)
{
	if (selected == s)
		return;
	selected = s;
	update();
}

void LayoutTile::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const qreal dpr = devicePixelRatioF();
	const QRectF ph((width() - 58) / 2.0, 9, 58, 103);
	const qreal rad = 9;
	QPainterPath path;
	path.addRoundedRect(ph, rad, rad);
	/* The website tile keeps the blue; each saved layout has its own colour. */
	const QColor tint = website || !hue.isValid() ? BLUE : hue;
	if (selected) {
		soft_shadow(p, path, QPointF(0, 0), 16, alpha(tint, 0.5));
		soft_shadow(p, path, QPointF(3, 3), 8, QColor(0, 0, 0, 205));
		QLinearGradient g(ph.topLeft(), ph.bottomLeft());
		g.setColorAt(0, mix(tint, QColor(0x14, 0x14, 0x1e), 0.10));
		g.setColorAt(0.5, QColor(0x14, 0x14, 0x1e));
		g.setColorAt(1, QColor(0x10, 0x10, 0x19));
		p.fillPath(path, g);
	} else {
		p.fillPath(path, QColor(0x13, 0x13, 0x1c));
		carve(p, path);
	}

	p.save();
	p.setClipPath(path);
	if (website) {
		const QPixmap g = svl::icon("globe", BLUE, 22, dpr);
		const QPointF at(ph.center().x() - 11, ph.center().y() - 11);
		glow_pixmap(p, at, g, 6, 0.8);
		p.drawPixmap(at, g);
	} else if (haveLayout) {
		const qreal s = ph.width() / (qreal)std::max(1, layout.w);
		const qreal sy = ph.height() / (qreal)std::max(1, layout.h);
		auto box = [&](const SvRect &b) {
			return QRectF(ph.left() + b.x * s, ph.top() + b.y * sy, b.w * s, b.h * sy);
		};
		if (layout.bg.mode != SvBgMode::None)
			p.fillRect(ph, alpha(tint, 0.06));
		/* The gameplay band at its place in the layer order. */
		auto band = [&]() {
			if (layout.main.show)
				p.fillRect(QRectF(ph.left(), ph.top() + layout.main.y * sy, ph.width(), layout.main.h * sy),
					   alpha(tint, 0.16));
		};
		for (size_t pi = 0; pi < layout.pieces.size(); pi++) {
			if (pi == layout.main.at)
				band();
			const SvPiece &piece = layout.pieces[pi];
			const QRectF b = box(piece.box);
			QPainterPath shape;
			if (piece.mask == SvMask::Circle) {
				shape.addEllipse(b);
			} else if (piece.mask == SvMask::Poly && piece.pts.size() >= 3) {
				QPolygonF poly;
				for (const auto &pt : piece.pts)
					poly << QPointF(b.left() + pt.first * b.width(), b.top() + pt.second * b.height());
				shape.addPolygon(poly);
				shape.closeSubpath();
			} else {
				/* Each corner rounded on its own: top left, top right, bottom right, bottom left. */
				const qreal most = std::min(b.width(), b.height()) / 2;
				qreal r[4];
				for (int c = 0; c < 4; c++)
					r[c] = std::min(std::max(1.5, (qreal)piece.corners[c] * s), most);
				shape.moveTo(b.left() + r[0], b.top());
				shape.lineTo(b.right() - r[1], b.top());
				shape.arcTo(b.right() - 2 * r[1], b.top(), 2 * r[1], 2 * r[1], 90, -90);
				shape.lineTo(b.right(), b.bottom() - r[2]);
				shape.arcTo(b.right() - 2 * r[2], b.bottom() - 2 * r[2], 2 * r[2], 2 * r[2], 0, -90);
				shape.lineTo(b.left() + r[3], b.bottom());
				shape.arcTo(b.left(), b.bottom() - 2 * r[3], 2 * r[3], 2 * r[3], 270, -90);
				shape.lineTo(b.left(), b.top() + r[0]);
				shape.arcTo(b.left(), b.top(), 2 * r[0], 2 * r[0], 180, -90);
				shape.closeSubpath();
			}
			p.fillPath(shape, alpha(tint, 0.38));
			p.setPen(QPen(alpha(tint, 0.7), 1));
			p.drawPath(shape);
		}
		if (layout.main.at >= layout.pieces.size())
			band();
	}
	p.restore();
	if (selected) {
		p.setPen(QPen(alpha(tint, 0.75), 1));
		p.drawRoundedRect(ph.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
	}
	if (number > 0 && number <= 12) {
		p.setFont(body(10, QFont::Black));
		glow_text(p, QRectF(ph.left(), ph.bottom() - 15, ph.width() - 6, 13), Qt::AlignRight | Qt::AlignVCenter,
			  QString::number(number), selected ? QColor(Qt::white) : mix(tint, INK, 0.35), tint, 0.7);
	}

	/* Every name the same size and on one line; one too long for its tile is cut short
	   (the hover and the card's header show it whole). */
	const QString label = tile_label(name, number);
	const qreal room = width() - 8;
	const QFont nf = body(11.5, QFont::Bold);
	p.setFont(nf);
	const QFontMetricsF fm(nf);
	const QRectF nr(4, ph.bottom() + 5, room, fm.height() + 2);
	const QString shown = fm.elidedText(label, Qt::ElideRight, nr.width());
	glow_text(p, nr, Qt::AlignHCenter | Qt::AlignTop, shown, selected ? Qt::white : mix(tint, INK2, 0.55), tint,
		  selected ? 0.8 : 0.0);
}

/* ---------- Spark ---------- */

Spark::Spark(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(20);
}

void Spark::setValues(const std::vector<float> &v, const QColor &col)
{
	values = v;
	colour = col;
	update();
}

void Spark::paintEvent(QPaintEvent *)
{
	if (values.size() < 2)
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const float top = std::max(1.0f, *std::max_element(values.begin(), values.end()) * 1.15f);
	const qreal w = width(), h = height() - 1;
	/* Always the last minute across the whole width, newest on the right. */
	const int span = 60;
	const size_t n = values.size();
	QPainterPath line;
	for (size_t i = 0; i < n; i++) {
		const qreal x = w - (qreal)(n - 1 - i) * w / (span - 1);
		const qreal y = 1 + h - (values[i] / top) * (h - 2);
		if (i == 0)
			line.moveTo(x, y);
		else
			line.lineTo(x, y);
	}
	QPainterPath area = line;
	area.lineTo(w, h + 1);
	area.lineTo(w - (qreal)(n - 1) * w / (span - 1), h + 1);
	area.closeSubpath();
	p.fillPath(area, alpha(colour, 0.16));
	p.setPen(QPen(colour, 1.5));
	p.drawPath(line);
}

/* ---------- Meter ---------- */

Meter::Meter(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(10);
	setMinimumWidth(90);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Meter::setLevel(float level)
{
	const float next = std::max(std::clamp(level, 0.0f, 1.0f), shown - 0.035f);
	if (std::abs(next - shown) < 0.002f)
		return;
	shown = std::max(0.0f, next);
	update();
}

void Meter::setAccent(const QColor &c)
{
	k = c;
	update();
}

void Meter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);
	QPainterPath track;
	track.addRoundedRect(r, 4, 4);
	p.fillPath(track, QColor(0x10, 0x10, 0x19));
	carve(p, track, 1.1);
	if (shown <= 0.0f)
		return;
	QLinearGradient g(r.topLeft(), r.topRight());
	g.setColorAt(0, k);
	g.setColorAt(0.78, k);
	g.setColorAt(0.86, AMBER);
	g.setColorAt(1, RED);
	p.save();
	p.setClipRect(QRectF(r.left(), r.top(), r.width() * shown, r.height()));
	QPainterPath lit;
	lit.addRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 3, 3);
	p.fillPath(lit, g);
	p.restore();
}

/* ---------- Well ---------- */

Well::Well(const std::string &pl, QWidget *parent) : QWidget(parent), platform(pl)
{
	setFixedSize(30, 30);
}

void Well::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r(1, 1, 28, 28);
	QPainterPath path;
	path.addRoundedRect(r, 7, 7);
	p.fillPath(path, QColor(0x10, 0x10, 0x19));
	carve(p, path, 1.2);
	p.drawPixmap(QPointF(r.center().x() - 8, r.center().y() - 8), logo(platform, 16, devicePixelRatioF()));
}

/* ---------- Logo ---------- */

Logo::Logo(QWidget *parent) : QWidget(parent)
{
	setFixedSize(sizeHint());
}

void Logo::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	QFont simply(g_brand);
	simply.setPixelSize(14);
	simply.setWeight(QFont::Black);
	p.setFont(simply);
	glow_text(p, QRectF(1, 0, 100, 16), Qt::AlignLeft | Qt::AlignVCenter, "Simply", Qt::white, QColor(0, 0, 0), 0.6);

	/* "Vertical" and its smile, copied from the website logo (the
	   Simply Vertical header: 70 px Architects Daughter, tilted 0.06 radians so its right
	   end sits lower). Everything below is in the website's own units, scaled by k from its
	   70 px script to this 24 px one, so the shape is the website's, not a lookalike. */
	QFont script(g_script);
	script.setPixelSize(24);
	const qreal k = 24.0 / 70.0;
	const qreal tiltDeg = 0.06 * 180.0 / 3.14159265358979;
	const QPointF at(1, 27);
	/* The website's frame: move to the word, tilt, then 4 units down. */
	QTransform word;
	word.translate(at.x(), at.y());
	word.rotate(tiltDeg);
	word.translate(0, 4 * k);

	/* The website writes the word on its middle line; the letters' baseline sits 14.69
	   units below it (measured in Chrome with the real font). */
	const QFontMetricsF fm(script);
	const qreal baseline = 14.6875 * k;
	p.save();
	p.setTransform(word, true);
	p.setFont(script);
	glow_text(p, QRectF(0, baseline - fm.ascent(), 116, fm.ascent() + fm.descent()), Qt::AlignLeft | Qt::AlignVCenter,
		  "Vertical", BLUE, BLUE, 0.85);
	p.restore();

	/* The smile, level with the screen, starting where the word starts. These are the exact
	   numbers the website's placeSmile() works out for "Vertical" (left end, lowest point
	   under the middle of the word, lower right end), 3.5 thick at the left, 5 at the dip,
	   3 at the right. */
	const qreal x0 = -2, xm = 124.8725, x2 = 251.7451;
	const qreal yl = 25.0155, yb = 63.7295, yr = 44.0821;
	const qreal cl = (x0 + xm) / 2, cr = (xm + x2) / 2;
	QPainterPath smile;
	smile.moveTo(x0, yl - 1.75);
	smile.quadTo(cl, yb - 2.5, xm, yb - 2.5);
	smile.quadTo(cr, yb - 2.5, x2, yr - 1.5);
	smile.lineTo(x2 + 1, yr + 1.5);
	smile.quadTo(cr, yb + 2.5, xm, yb + 2.5);
	smile.quadTo(cl, yb + 2.5, x0, yl + 1.75);
	smile.closeSubpath();
	QTransform level = word;
	level.rotate(-tiltDeg);
	level.scale(k, k);
	smile = level.map(smile);
	soft_shadow(p, smile, QPointF(0, 0), 4, BLUE);
	p.fillPath(smile, BLUE);
}

/* ---------- Words ---------- */

Words::Words(QWidget *parent, qreal size) : QWidget(parent), px(size)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void Words::setSegs(const std::vector<Seg> &s)
{
	bool same = s.size() == segs.size();
	for (size_t i = 0; same && i < s.size(); i++)
		same = s[i].text == segs[i].text && s[i].colour == segs[i].colour && s[i].glow == segs[i].glow &&
		       s[i].bold == segs[i].bold;
	if (same)
		return;
	segs = s;
	updateGeometry();
	update();
}

void Words::setText(const QString &text, const QColor &colour, bool glow)
{
	setSegs({{text, colour, glow, false}});
}

int Words::lineHeight() const
{
	return (int)std::ceil(QFontMetricsF(body(px, QFont::Bold)).height()) + 1;
}

std::vector<Words::Placed> Words::flow(int width, int *height) const
{
	std::vector<Placed> out;
	const int lh = lineHeight();
	qreal x = 0, y = 0;
	const qreal segGap = 9;
	for (size_t si = 0; si < segs.size(); si++) {
		const Seg &s = segs[si];
		if (s.text.isEmpty())
			continue;
		const QFontMetricsF fm(body(px, s.bold || s.glow ? QFont::Bold : QFont::Medium));
		const qreal space = fm.horizontalAdvance(' ');
		if (x > 0)
			x += segGap;
		/* A segment stays on one line if it fits on a line of its own. */
		const qreal whole = fm.horizontalAdvance(s.text);
		if (whole <= width) {
			if (x + whole > width && x > 0) {
				x = 0;
				y += lh;
			}
			out.push_back({QRectF(x, y, whole + 1, lh), s.text, (int)si});
			x += whole;
			continue;
		}
		for (const QString &word : s.text.split(' ', Qt::SkipEmptyParts)) {
			const qreal ww = fm.horizontalAdvance(word);
			if (x + ww > width && x > 0) {
				x = 0;
				y += lh;
			}
			out.push_back({QRectF(x, y, ww + 1, lh), word, (int)si});
			x += ww + space;
		}
	}
	if (height)
		*height = out.empty() ? 0 : (int)(y + lh);
	return out;
}

int Words::heightForWidth(int w) const
{
	int h = 0;
	flow(std::max(20, w), &h);
	return std::max(h, lineHeight());
}

QSize Words::sizeHint() const
{
	int h = 0;
	flow(std::max(60, width()), &h);
	return QSize(160, std::max(h, lineHeight()));
}

void Words::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	const int h = heightForWidth(width());
	if (h != lastHeight) {
		lastHeight = h;
		updateGeometry();
	}
}

void Words::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::TextAntialiasing);
	for (const Placed &pl : flow(width(), nullptr)) {
		const Seg &s = segs[(size_t)pl.seg];
		p.setFont(body(px, s.bold || s.glow ? QFont::Bold : QFont::Medium));
		if (s.glow)
			glow_text(p, pl.r, Qt::AlignLeft | Qt::AlignVCenter, pl.text, s.colour, s.colour, 0.75);
		else
			glow_text(p, pl.r, Qt::AlignLeft | Qt::AlignVCenter, pl.text, s.colour, QColor(), 0.0);
	}
}

/* ---------- Info ---------- */

Info::Info(const QString &tip, QWidget *parent) : QWidget(parent)
{
	setToolTip(tip);
	setFixedSize(18, 18);
	setCursor(Qt::WhatsThisCursor);
}

void Info::setWarn(bool on)
{
	if (warn == on)
		return;
	warn = on;
	set_tip_colour(this, on ? AMBER : QColor());
	update();
}

void Info::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (warn)
		p.drawPixmap(QPointF(1, 1), icon("alert", AMBER, 16, devicePixelRatioF()));
	else
		p.drawPixmap(QPointF(1, 1), icon("info", underMouse() ? hue_of(this) : alpha(hue_of(this), 0.75), 16,
					      devicePixelRatioF()));
}

/* ---------- NumberBox ---------- */

namespace {
constexpr int NB_GLOW = 5; /* room around the box for its focus glow */
constexpr int NB_H = 42;
constexpr int NB_STEP = 26;
constexpr int NB_PAD = 12;
QFont nb_number_font()
{
	return body(15, QFont::Bold);
}
QFont nb_unit_font()
{
	return body(13.5, QFont::DemiBold);
}

/* A small raised key with a minus or plus on it. Holding it keeps stepping. */
class StepKey : public QAbstractButton {
public:
	StepKey(const QString &iconName, QWidget *parent) : QAbstractButton(parent), iconName(iconName)
	{
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::NoFocus);
		setAutoRepeat(true);
		setAutoRepeatDelay(400);
		setAutoRepeatInterval(70);
		setFixedSize(NB_STEP + 4, NB_STEP + 6);
	}

protected:
	void enterEvent(QEnterEvent *) override { update(); }
	void leaveEvent(QEvent *) override { update(); }
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (!isEnabled())
			p.setOpacity(0.35);
		const bool pressed = isDown();
		const bool hot = underMouse() && isEnabled();
		const QColor BLUE = hue_of(this);
		const QRectF r = QRectF(2, pressed ? 3 : 2, NB_STEP, NB_STEP);
		QPainterPath path;
		path.addRoundedRect(r, 7, 7);
		soft_shadow(p, path, QPointF(0, pressed ? 1 : 2.5), pressed ? 2 : 4, QColor(0, 0, 0, 190));
		if (hot)
			soft_shadow(p, path, QPointF(0, 0), 6, alpha(BLUE, 0.35));
		QLinearGradient g(r.topLeft(), r.bottomLeft());
		g.setColorAt(0, QColor(0x22, 0x23, 0x36));
		g.setColorAt(1, QColor(0x15, 0x15, 0x21));
		p.fillPath(path, g);
		p.save();
		p.setClipPath(path);
		p.setPen(QPen(QColor(255, 255, 255, 30), 1.2));
		p.drawLine(QPointF(r.left() + 6, r.top() + 0.7), QPointF(r.right() - 6, r.top() + 0.7));
		p.restore();
		p.setPen(QPen(alpha(BLUE, hot ? 0.7 : 0.35), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
		const int ip = 14;
		const QPointF at(r.center().x() - ip / 2.0, r.center().y() - ip / 2.0);
		if (hot)
			glow_pixmap(p, at, svl::icon(iconName, BLUE, ip, devicePixelRatioF()), 5, 0.8);
		p.drawPixmap(at, svl::icon(iconName, hot ? QColor(Qt::white) : BLUE, ip, devicePixelRatioF()));
	}

private:
	QString iconName;
};
} // namespace

NumberBox::NumberBox(int min, int max, int step, int value, const QString &u, const QString &off, QWidget *parent)
	: QWidget(parent),
	  unit(u),
	  offText(off)
{
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setCursor(Qt::IBeamCursor);
	spin = new QSpinBox(this);
	spin->setObjectName("svNum");
	spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
	spin->setFrame(false);
	spin->setGroupSeparatorShown(true);
	spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	spin->setRange(min, max);
	spin->setSingleStep(step);
	spin->setFocusPolicy(Qt::StrongFocus);
	if (!offText.isEmpty())
		spin->setSpecialValueText(offText);
	spin->setValue(value);
	spin->installEventFilter(this);

	/* Room for the widest number it can hold, so the box never changes size. */
	const QFontMetricsF nf(nb_number_font());
	qreal w = std::max(nf.horizontalAdvance(QLocale().toString(max)), nf.horizontalAdvance(QLocale().toString(min)));
	if (!offText.isEmpty())
		w = std::max(w, nf.horizontalAdvance(offText));
	spinW = (int)std::ceil(w) + 8;
	unitW = unit.isEmpty() ? 0 : (int)std::ceil(QFontMetricsF(nb_unit_font()).horizontalAdvance(unit));

	down = new StepKey("minus", this);
	up = new StepKey("plus", this);
	QObject::connect(down, &QAbstractButton::clicked, this, [this] { spin->stepBy(-1); });
	QObject::connect(up, &QAbstractButton::clicked, this, [this] { spin->stepBy(1); });
	QObject::connect(spin, &QSpinBox::valueChanged, this, [this](int v) {
		refresh();
		if (onChange)
			onChange(v);
	});
	QObject::connect(spin, &QSpinBox::textChanged, this, [this](const QString &) { update(); });

	/* The typing line is exactly one line of text tall and centred, so the number and its
	   unit sit on the same line. It runs up to the minus key; the unit is drawn under it. */
	const int h = NB_H + 2 * NB_GLOW;
	const int boxW = sizeHint().width() - 2 * NB_GLOW;
	up->move(NB_GLOW + boxW - 5 - up->width(), (h - up->height()) / 2);
	down->move(up->x() - 2 - down->width(), (h - down->height()) / 2);
	const int th = (int)std::ceil(QFontMetricsF(nb_number_font()).height()) + 4;
	spin->setGeometry(NB_GLOW + NB_PAD - 2, (h - th) / 2, down->x() - 6 - (NB_GLOW + NB_PAD - 2), th);
	refresh();
}

int NumberBox::value() const
{
	return spin->value();
}

void NumberBox::setValue(int v)
{
	spin->setValue(v);
}

void NumberBox::setUnitOne(const QString &one)
{
	unitOne = one;
	if (!unitOne.isEmpty())
		unitW = std::max(unitW, (int)std::ceil(QFontMetricsF(nb_unit_font()).horizontalAdvance(unitOne)));
	update();
}

QSize NumberBox::sizeHint() const
{
	const int inner = NB_PAD + spinW + (unitW ? 6 + unitW : 0) + 14 + 2 * (NB_STEP + 4) + 2 + 5;
	return QSize(inner + 2 * NB_GLOW, NB_H + 2 * NB_GLOW);
}

void NumberBox::refresh()
{
	if (down)
		down->setEnabled(isEnabled() && spin->value() > spin->minimum());
	if (up)
		up->setEnabled(isEnabled() && spin->value() < spin->maximum());
	update();
}

/* A click anywhere in the box puts you in it, ready to type. */
void NumberBox::mousePressEvent(QMouseEvent *)
{
	if (isEnabled()) {
		spin->setFocus(Qt::MouseFocusReason);
		spin->selectAll();
	}
}

void NumberBox::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::EnabledChange)
		refresh();
}

bool NumberBox::eventFilter(QObject *o, QEvent *e)
{
	if (o == spin) {
		/* While typing the number has no thousands comma, so selecting it takes the
		   whole number and what you type replaces all of it. The comma comes back after. */
		if (e->type() == QEvent::FocusIn) {
			spin->setGroupSeparatorShown(false);
			QTimer::singleShot(0, spin, [s = spin] { s->selectAll(); });
			update();
		} else if (e->type() == QEvent::FocusOut) {
			spin->setGroupSeparatorShown(true);
			update();
		} else if (e->type() == QEvent::KeyPress) {
			/* Enter keeps the typed number instead of pressing the window's Save. */
			const int k = static_cast<QKeyEvent *>(e)->key();
			if (k == Qt::Key_Return || k == Qt::Key_Enter) {
				spin->interpretText();
				spin->selectAll();
				return true;
			}
		}
	}
	return QWidget::eventFilter(o, e);
}

void NumberBox::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	const QRectF r = QRectF(rect()).adjusted(NB_GLOW, NB_GLOW, -NB_GLOW, -NB_GLOW);
	QPainterPath path;
	path.addRoundedRect(r, 10, 10);
	const bool focus = spin->hasFocus();
	const bool hot = underMouse() && isEnabled();
	const QColor BLUE = hue_of(this);

	if (focus)
		soft_shadow(p, path, QPointF(0, 0), 7, alpha(BLUE, 0.4));
	p.fillPath(path, QColor(0x10, 0x10, 0x19));
	carve(p, path);
	p.setPen(QPen(alpha(BLUE, focus ? 0.85 : hot ? 0.45 : 0.28), 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

	/* The unit, straight after the number. Hidden while the box shows its Off word. */
	const bool isOff = !offText.isEmpty() && spin->value() == spin->minimum();
	if (unitW && !isOff) {
		/* Sits right after the number as it is now, on the number's own line. */
		const qreal numW = QFontMetricsF(nb_number_font()).horizontalAdvance(spin->text());
		const QRectF line(spin->x() + 2 + numW + 6, spin->y(), unitW + 2, spin->height());
		p.setFont(nb_unit_font());
		p.setPen(tone(BLUE, 0.4));
		p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
			   spin->value() == 1 && !unitOne.isEmpty() ? unitOne : unit);
	}
}

/* ---------- Switch ---------- */

namespace {
constexpr qreal SWITCH_W = 38;
constexpr qreal SWITCH_H = 20;
constexpr qreal SWITCH_X = 4;
constexpr qreal SWITCH_GAP = 10;
QFont switch_font()
{
	return body(13.5, QFont::Bold);
}
} // namespace

Switch::Switch(const QString &text, QWidget *parent) : QAbstractButton(parent)
{
	setText(text);
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize Switch::sizeHint() const
{
	const qreal tw = QFontMetricsF(switch_font()).horizontalAdvance(text());
	return QSize((int)std::ceil(SWITCH_X + SWITCH_W + SWITCH_GAP + tw + 4), 34);
}

void Switch::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	const QRectF track(SWITCH_X, (height() - SWITCH_H) / 2.0, SWITCH_W, SWITCH_H);
	QPainterPath path;
	path.addRoundedRect(track, SWITCH_H / 2, SWITCH_H / 2);
	const bool on = isChecked();
	const QColor BLUE = hue_of(this);

	if (on) {
		soft_shadow(p, path, QPointF(0, 0), 8, alpha(BLUE, 0.45));
		QLinearGradient g(track.topLeft(), track.bottomLeft());
		g.setColorAt(0, alpha(tone(BLUE, 0.15), 0.95));
		g.setColorAt(1, alpha(BLUE, 0.9));
		p.fillPath(path, g);
	} else {
		p.fillPath(path, QColor(0x13, 0x13, 0x1c));
		carve(p, path);
		p.setPen(QPen(alpha(BLUE, underMouse() && isEnabled() ? 0.4 : 0.18), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), SWITCH_H / 2, SWITCH_H / 2);
	}

	/* The knob, raised off the track. */
	const qreal kd = SWITCH_H - 6;
	const qreal kx = on ? track.right() - 3 - kd : track.left() + 3;
	QPainterPath knob;
	knob.addEllipse(QRectF(kx, track.top() + 3, kd, kd));
	soft_shadow(p, knob, QPointF(0, 1.5), 3, QColor(0, 0, 0, 170));
	p.fillPath(knob, on ? QColor(Qt::white) : QColor(0xc8, 0xd2, 0xff));

	p.setFont(switch_font());
	const QRectF tr(track.right() + SWITCH_GAP, 0, width() - track.right() - SWITCH_GAP, height());
	/* A switch marked "svStrongText" keeps its words in its colour, for choices told apart by colour. */
	glow_text(p, tr, Qt::AlignLeft | Qt::AlignVCenter, text(), property("svStrongText").toBool() ? BLUE : INK, BLUE,
		  on ? 0.3 : 0.0);
}

/* ---------- TabBar ---------- */

namespace {
constexpr qreal TAB_H = 42;
constexpr qreal TAB_ICON = 17;
QFont tab_font()
{
	return body(12.5, QFont::ExtraBold);
}
} // namespace

TabBar::TabBar(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
	setCursor(Qt::PointingHandCursor);
	QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Fixed);
	sp.setHeightForWidth(true);
	setSizePolicy(sp);
}

int TabBar::addTab(const QString &text, const QString &iconName)
{
	tabs.push_back({text, iconName, QColor(), true});
	updateGeometry();
	update();
	return (int)tabs.size() - 1;
}

void TabBar::setTabHue(int index, const QColor &c, bool strong)
{
	if (index < 0 || index >= (int)tabs.size())
		return;
	tabs[(size_t)index].hue = c;
	tabs[(size_t)index].strong = strong;
	update();
}

void TabBar::setCurrent(int index)
{
	if (index < 0 || index >= (int)tabs.size() || index == cur)
		return;
	cur = index;
	update();
}

void TabBar::setTabVisible(int index, bool visible)
{
	if (index < 0 || index >= (int)tabs.size() || tabs[(size_t)index].visible == visible)
		return;
	tabs[(size_t)index].visible = visible;
	updateGeometry();
	update();
}

void TabBar::setTabText(int index, const QString &text)
{
	if (index < 0 || index >= (int)tabs.size() || tabs[(size_t)index].text == text)
		return;
	tabs[(size_t)index].text = text;
	updateGeometry();
	update();
}

bool TabBar::tabVisible(int index) const
{
	return index >= 0 && index < (int)tabs.size() && tabs[(size_t)index].visible;
}

int TabBar::visibleCount() const
{
	return (int)shown().size();
}

std::vector<int> TabBar::shown() const
{
	std::vector<int> out;
	for (int i = 0; i < (int)tabs.size(); i++)
		if (tabs[(size_t)i].visible)
			out.push_back(i);
	return out;
}

/* Every tab is as wide as the longest word needs, so no word is ever cut or wrapped. */
qreal TabBar::cellWidth() const
{
	const QFontMetricsF fm(tab_font());
	qreal w = 0;
	for (const Tab &t : tabs)
		w = std::max(w, (t.iconName.isEmpty() ? 26 : TAB_ICON + 6 + 12) + fm.horizontalAdvance(t.text));
	return w;
}

/* One row of every tab side by side when there is room. */
QSize TabBar::sizeHint() const
{
	return QSize((int)std::ceil(cellWidth() * std::max<size_t>(1, shown().size())) + 8, (int)TAB_H + 8);
}

/* The narrowest it goes is one tab wide, with the tabs stacked. */
QSize TabBar::minimumSizeHint() const
{
	return QSize((int)std::ceil(cellWidth()) + 8, heightForWidth((int)std::ceil(cellWidth()) + 8));
}

/* The rows are kept even, so six tabs that need two rows sit three and three, never five and one. */
int TabBar::perRow(qreal width, int count) const
{
	if (count <= 1)
		return 1;
	const qreal room = width - 7;
	const int fit = std::clamp((int)std::floor(room / std::max<qreal>(1, cellWidth())), 1, count);
	const int rows = (count + fit - 1) / fit;
	return (count + rows - 1) / rows;
}

int TabBar::heightForWidth(int w) const
{
	const int count = std::max(1, (int)shown().size());
	const int rows = (count + perRow(w, count) - 1) / perRow(w, count);
	return (int)std::ceil(TAB_H * rows) + 8;
}

void TabBar::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	/* A new width can need a different number of rows, so ask the window for the new height. */
	if (height() != heightForWidth(width()))
		updateGeometry();
}

QRectF TabBar::cell(int slot, int count) const
{
	const QRectF track = QRectF(rect()).adjusted(3, 3, -4, -4);
	const int cols = perRow(width(), count);
	const int row = slot / cols;
	const int col = slot % cols;
	const qreal w = track.width() / cols;
	/* A shorter last row stays centred under the rows above it. */
	const int inRow = std::min(cols, count - row * cols);
	const qreal shift = (cols - inRow) * w / 2;
	return QRectF(track.left() + shift + col * w, track.top() + row * TAB_H, w, TAB_H - 1).adjusted(3, 3, -3, -3);
}

int TabBar::at(const QPointF &pos) const
{
	const std::vector<int> s = shown();
	for (int slot = 0; slot < (int)s.size(); slot++)
		if (cell(slot, (int)s.size()).adjusted(-3, -3, 3, 3).contains(pos))
			return s[(size_t)slot];
	return -1;
}

void TabBar::mousePressEvent(QMouseEvent *e)
{
	const int i = at(e->position());
	if (i < 0 || e->button() != Qt::LeftButton)
		return QWidget::mousePressEvent(e);
	if (i != cur) {
		cur = i;
		update();
		if (onPick)
			onPick(i);
	}
}

void TabBar::mouseMoveEvent(QMouseEvent *e)
{
	const int i = at(e->position());
	if (i != hot) {
		hot = i;
		update();
	}
	QWidget::mouseMoveEvent(e);
}

void TabBar::leaveEvent(QEvent *e)
{
	hot = -1;
	update();
	QWidget::leaveEvent(e);
}

void TabBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	const qreal dpr = devicePixelRatioF();

	/* The carved track the tabs sit in. */
	const QRectF track = QRectF(rect()).adjusted(3, 3, -4, -4);
	QPainterPath trackPath;
	trackPath.addRoundedRect(track, 12, 12);
	p.fillPath(trackPath, QColor(0x0d, 0x0d, 0x15));
	carve(p, trackPath);

	const std::vector<int> s = shown();
	const QFont f = tab_font();
	const QFontMetricsF fm(f);
	for (int slot = 0; slot < (int)s.size(); slot++) {
		const int i = s[(size_t)slot];
		const Tab &t = tabs[(size_t)i];
		const QRectF r = cell(slot, (int)s.size());
		const bool on = i == cur;
		const QColor BLUE = t.hue.isValid() ? t.hue : hue_of(this);
		const qreal rad = 9;
		QPainterPath path;
		path.addRoundedRect(r, rad, rad);
		if (on) {
			soft_shadow(p, path, QPointF(2, 2), 5, QColor(0, 0, 0, 160));
			QLinearGradient fill(r.topLeft(), r.bottomLeft());
			fill.setColorAt(0, mix(BLUE, QColor(0x23, 0x23, 0x2f), 0.18));
			fill.setColorAt(0.55, QColor(0x17, 0x17, 0x22));
			fill.setColorAt(1, QColor(0x10, 0x10, 0x19));
			p.fillPath(path, fill);
			p.save();
			p.setClipPath(path);
			QLinearGradient under(QPointF(0, r.bottom()), QPointF(0, r.bottom() - 9));
			under.setColorAt(0, alpha(BLUE, 0.45));
			under.setColorAt(1, alpha(BLUE, 0.0));
			p.fillRect(QRectF(r.left(), r.bottom() - 9, r.width(), 9), under);
			p.setPen(QPen(QColor(255, 255, 255, 31), 1));
			p.drawLine(QPointF(r.left() + rad * 0.6, r.top() + 0.5), QPointF(r.right() - rad * 0.6, r.top() + 0.5));
			p.restore();
			/* The lamp line along the bottom. */
			const QRectF lamp(r.left() + r.width() * 0.18, r.bottom() - 2, r.width() * 0.64, 2);
			QLinearGradient lg(lamp.topLeft(), lamp.topRight());
			lg.setColorAt(0, alpha(BLUE, 0));
			lg.setColorAt(0.5, alpha(BLUE, 1));
			lg.setColorAt(1, alpha(BLUE, 0));
			QPainterPath lampPath;
			lampPath.addRoundedRect(lamp, 1, 1);
			soft_shadow(p, lampPath, QPointF(0, 0), 4, alpha(BLUE, 0.9));
			p.fillPath(lampPath, lg);
		} else if (i == hot) {
			p.fillPath(path, alpha(BLUE, 0.10));
		}

		/* The icon and the word side by side, centred in the tab. */
		/* A tab without an icon is a plain word, used for short rows of choices. */
		const qreal fh = fm.height();
		const qreal iconW = t.iconName.isEmpty() ? 0 : TAB_ICON + 6;
		const qreal block = iconW + fm.horizontalAdvance(t.text);
		const qreal left = r.center().x() - block / 2;
		if (iconW > 0) {
			const QPointF iconAt(left, r.center().y() - TAB_ICON / 2);
			const QColor iconColour = on ? BLUE : alpha(BLUE, 0.85);
			if (on)
				glow_pixmap(p, iconAt, svl::icon(t.iconName, BLUE, (int)TAB_ICON, dpr), 6, 0.8);
			p.drawPixmap(iconAt, svl::icon(t.iconName, iconColour, (int)TAB_ICON, dpr));
		}
		p.setFont(f);
		glow_text(p, QRectF(left + iconW, r.center().y() - fh / 2, block - iconW + 4, fh),
			  Qt::AlignLeft | Qt::AlignVCenter, t.text,
			  on ? QColor(Qt::white) : tone(BLUE, t.strong ? 0.85 : 0.5), BLUE, on ? 0.5 : t.strong ? 0.22 : 0.0);
	}
}

QLabel *field(const QString &text)
{
	return field(text, BLUE);
}

QLabel *field(const QString &text, const QColor &hue)
{
	auto *l = new QLabel(text.toUpper());
	l->setStyleSheet(QString("color: %1; font-family: '%2'; font-size: 15px; font-weight: 900; letter-spacing: 1px;")
				 .arg(tone(hue, 0.35).name(), g_body));
	return l;
}

/* ---------- hover cards ---------- */

namespace {

constexpr const char *TIPS_ROOT = "svTipsRoot";
constexpr const char *TIP_COLOUR = "svTipColour";
constexpr int TIP_DELAY_MS = 450;
constexpr qreal TIP_PAD_X = 13;
constexpr qreal TIP_PAD_Y = 9;
constexpr qreal TIP_GLOW = 14;
constexpr int TIP_MAX_TEXT = 280;

QFont tip_font()
{
	return body(13, QFont::Medium);
}

/* The card itself: the website's info hover, a near black card, a thin lit edge in the
   accent and a soft glow of the same colour around it. */
class TipCard : public QWidget {
public:
	TipCard() : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setTip(const QString &t, const QColor &c)
	{
		text = t;
		colour = c;
		const QFontMetricsF fm(tip_font());
		const qreal oneLine = fm.horizontalAdvance(text);
		textW = std::min<qreal>(TIP_MAX_TEXT, std::ceil(oneLine) + 1);
		const QRectF br = fm.boundingRect(QRectF(0, 0, textW, 10000), Qt::TextWordWrap | Qt::AlignLeft, text);
		textW = std::max<qreal>(std::ceil(br.width()), 10);
		textH = std::ceil(br.height());
		const int m = (int)TIP_GLOW;
		resize((int)(textW + 2 * TIP_PAD_X) + 2 * m, (int)(textH + 2 * TIP_PAD_Y) + 2 * m);
		update();
	}

	/* The card's own edge, without the room left round it for the glow. */
	QSize card() const { return QSize(width() - 2 * (int)TIP_GLOW, height() - 2 * (int)TIP_GLOW); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF r = QRectF(rect()).adjusted(TIP_GLOW, TIP_GLOW, -TIP_GLOW, -TIP_GLOW);
		QPainterPath path;
		path.addRoundedRect(r, 12, 12);
		soft_shadow(p, path, QPointF(0, 6), 12, QColor(0, 0, 0, 170));
		soft_shadow(p, path, QPointF(0, 0), 9, alpha(colour, 0.30));
		p.fillPath(path, QColor(0x0f, 0x0f, 0x18));
		QLinearGradient top(r.topLeft(), r.bottomLeft());
		top.setColorAt(0, alpha(colour, 0.10));
		top.setColorAt(0.5, alpha(colour, 0.0));
		p.fillPath(path, top);
		p.setPen(QPen(alpha(colour, 0.55), 1));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		/* The faint top highlight, as on the site. */
		p.setPen(QPen(QColor(255, 255, 255, 16), 1));
		p.drawLine(QPointF(r.left() + 12, r.top() + 1.5), QPointF(r.right() - 12, r.top() + 1.5));

		p.setFont(tip_font());
		const QRectF tr(r.left() + TIP_PAD_X, r.top() + TIP_PAD_Y, textW, textH);
		p.setPen(QColor(0, 0, 0, 215));
		p.drawText(tr.translated(0, 1), Qt::TextWordWrap | Qt::AlignLeft, text);
		p.setPen(INK);
		p.drawText(tr, Qt::TextWordWrap | Qt::AlignLeft, text);
	}

private:
	QString text;
	QColor colour = BLUE;
	qreal textW = 0, textH = 0;
};

bool in_tips_root(const QWidget *w)
{
	for (const QWidget *at = w; at; at = at->parentWidget())
		if (at->property(TIPS_ROOT).toBool())
			return true;
	return false;
}

/* The nearest widget, from w upwards, that has something to say on hover. */
QWidget *tip_owner(QWidget *w)
{
	for (QWidget *at = w; at; at = at->parentWidget()) {
		if (!at->toolTip().isEmpty())
			return at;
		if (at->property(TIPS_ROOT).toBool())
			break;
	}
	return nullptr;
}

class Tips : public QObject {
public:
	Tips()
	{
		delay.setSingleShot(true);
		delay.setInterval(TIP_DELAY_MS);
		connect(&delay, &QTimer::timeout, this, [this] { show_now(); });
	}

	bool eventFilter(QObject *obj, QEvent *e) override
	{
		switch (e->type()) {
		case QEvent::ToolTip: {
			/* Ours are drawn by us, so Windows' plain one never shows on top. */
			auto *w = qobject_cast<QWidget *>(obj);
			return w && in_tips_root(w);
		}
		case QEvent::Enter: {
			auto *w = qobject_cast<QWidget *>(obj);
			if (w && in_tips_root(w))
				aim(tip_owner(w));
			break;
		}
		case QEvent::Leave:
			if (qobject_cast<QWidget *>(obj) && (pending || shown))
				QTimer::singleShot(0, this, [this] { recheck(); });
			break;
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::Wheel:
		case QEvent::KeyPress:
			if (pending || shown)
				drop();
			break;
		case QEvent::Hide:
		case QEvent::Destroy:
			if (obj == pending.data() || obj == shown.data())
				drop();
			break;
		default:
			break;
		}
		return false;
	}

private:
	void aim(QWidget *owner)
	{
		if (owner && (owner == pending.data() || owner == shown.data()))
			return;
		drop();
		if (!owner)
			return;
		pending = owner;
		delay.start();
	}

	void recheck()
	{
		QWidget *under = QApplication::widgetAt(QCursor::pos());
		QWidget *owner = under && in_tips_root(under) ? tip_owner(under) : nullptr;
		if (owner != pending.data() && owner != shown.data())
			aim(owner);
	}

	void drop()
	{
		delay.stop();
		pending = nullptr;
		shown = nullptr;
		if (card)
			card->hide();
	}

	void show_now()
	{
		QWidget *w = pending.data();
		pending = nullptr;
		if (!w || !w->isVisible() || w->toolTip().isEmpty())
			return;
		if (!w->rect().contains(w->mapFromGlobal(QCursor::pos())))
			return;
		if (!card)
			card = new TipCard();
		const QVariant v = w->property(TIP_COLOUR);
		card->setTip(w->toolTip(), v.isValid() && v.value<QColor>().isValid() ? v.value<QColor>() : hue_of(w));

		/* Centred under the widget, kept on its screen, above it when there is no room below. */
		const QSize c = card->card();
		const QPoint below = w->mapToGlobal(QPoint(w->width() / 2, w->height() + 8));
		const QPoint above = w->mapToGlobal(QPoint(w->width() / 2, -8 - c.height()));
		QPoint at(below.x() - c.width() / 2, below.y());
		if (QScreen *screen = QGuiApplication::screenAt(below)) {
			const QRect area = screen->availableGeometry();
			if (at.y() + c.height() > area.bottom())
				at.setY(above.y());
			at.setX(std::clamp(at.x(), area.left() + 4, (std::max)(area.left() + 4, area.right() - 4 - c.width())));
		}
		card->move(at - QPoint((int)TIP_GLOW, (int)TIP_GLOW));
		card->show();
		card->raise();
		shown = w;
	}

	QTimer delay;
	QPointer<QWidget> pending;
	QPointer<QWidget> shown;
	TipCard *card = nullptr;
};

Tips *g_tips = nullptr;

} // namespace

void install_tips()
{
	if (g_tips || !qApp)
		return;
	g_tips = new Tips();
	g_tips->setParent(qApp);
	qApp->installEventFilter(g_tips);
}

void mark_tips(QWidget *root)
{
	if (root)
		root->setProperty(TIPS_ROOT, true);
	install_tips();
}

void set_tip_colour(QWidget *w, const QColor &c)
{
	if (w)
		w->setProperty(TIP_COLOUR, QVariant::fromValue(c));
}

} // namespace svl
