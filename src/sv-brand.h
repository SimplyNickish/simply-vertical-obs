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

// Brand kit pieces: the member's logo, channel name and social rotator, drawn
// once into a picture the size of the piece's box (never per frame; a rotator
// has one picture per link and only picks which one shows). The graphics card
// then stamps that picture through the piece's shape, like a camera.

#pragma once

#include "sv-layout.h"
#include "sv-mask.h"

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

/* True for an address in the website's public brand kit storage, the only place a logo may come from. */
bool sv_brand_logo_url_ok(const std::string &url);

/* The picture for a logo or channel name piece, the size of its box. Empty while
   a logo is still downloading, or when it could not be loaded: then nothing is drawn. */
SvColorBitmap sv_brand_picture(const SvPiece &p);

/* A social rotator's links, one picture each, all the size of its box and with
   the words at one size: the platform's icon, then the handle, centred. */
std::vector<SvColorBitmap> sv_brand_socials(const SvPiece &p);

/* Which link of a rotator shows now, and how much of it (it fades in and out at the ends). */
void sv_brand_social_at(size_t count, float secs, uint64_t ns, size_t &index, float &alpha);

/* Runs on the download thread each time a logo finishes, so layouts can be drawn again with it. */
void sv_brand_on_logo(std::function<void()> f);

void sv_brand_unload();
