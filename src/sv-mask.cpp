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

#include "sv-mask.h"

#include <algorithm>
#include <cmath>

namespace {

using Pt = std::pair<float, float>;
constexpr float PI = 3.14159265358979f;
constexpr int SUB = 4; /* rows sampled per pixel row, for smooth edges */

/* The rounded box path the website draws for plain pieces and circles. */
std::vector<Pt> rounded_box(float w, float h, float r)
{
	r = std::max(0.0f, std::min({r, w / 2, h / 2}));
	std::vector<Pt> pts;
	if (r < 0.5f) {
		pts = {{0.0f, 0.0f}, {w, 0.0f}, {w, h}, {0.0f, h}};
		return pts;
	}
	const int seg = std::max(6, std::min(32, (int)(r / 2)));
	const Pt centres[4] = {{w - r, r}, {w - r, h - r}, {r, h - r}, {r, r}};
	const float starts[4] = {-PI / 2, 0, PI / 2, PI};
	for (int c = 0; c < 4; c++)
		for (int i = 0; i <= seg; i++) {
			float a = starts[c] + (PI / 2) * (float)i / seg;
			pts.emplace_back(centres[c].first + std::cos(a) * r, centres[c].second + std::sin(a) * r);
		}
	return pts;
}

std::vector<Pt> outline(const SvPiece &p, float w, float h)
{
	if (p.mask == SvMask::Poly) {
		std::vector<Pt> pts;
		pts.reserve(p.pts.size());
		for (auto &q : p.pts)
			pts.emplace_back(q.first * w, q.second * h);
		return pts;
	}
	if (p.mask == SvMask::Circle)
		return rounded_box(w, h, std::min(w, h) / 2);
	return rounded_box(w, h, p.radius);
}

/* Fills a polygon (even-odd) into a w x h coverage picture with smooth edges. */
SvBitmap raster(const std::vector<Pt> &poly, int w, int h)
{
	SvBitmap b;
	b.w = w;
	b.h = h;
	b.a.assign((size_t)w * h, 0);
	if (poly.size() < 3 || w <= 0 || h <= 0)
		return b;
	std::vector<float> acc(w);
	std::vector<float> xs;
	const size_t n = poly.size();
	for (int y = 0; y < h; y++) {
		std::fill(acc.begin(), acc.end(), 0.0f);
		for (int s = 0; s < SUB; s++) {
			const float sy = y + (s + 0.5f) / SUB;
			xs.clear();
			for (size_t i = 0; i < n; i++) {
				const Pt &a = poly[i];
				const Pt &c = poly[(i + 1) % n];
				if ((a.second <= sy) == (c.second <= sy))
					continue;
				const float t = (sy - a.second) / (c.second - a.second);
				xs.push_back(a.first + t * (c.first - a.first));
			}
			std::sort(xs.begin(), xs.end());
			for (size_t k = 0; k + 1 < xs.size(); k += 2) {
				const float x0 = std::max(0.0f, xs[k]);
				const float x1 = std::min((float)w, xs[k + 1]);
				if (x1 <= x0)
					continue;
				const int p0 = (int)std::floor(x0);
				const int p1 = std::min(w - 1, (int)std::ceil(x1) - 1);
				for (int px = p0; px <= p1; px++) {
					const float cov = std::min(x1, (float)px + 1) - std::max(x0, (float)px);
					if (cov > 0)
						acc[px] += cov / SUB;
				}
			}
		}
		uint8_t *row = &b.a[(size_t)y * w];
		for (int x = 0; x < w; x++)
			row[x] = (uint8_t)std::lround(std::min(1.0f, acc[x]) * 255.0f);
	}
	return b;
}

/* Three box blurs in a row, which looks like a soft (Gaussian) blur. */
void blur(std::vector<float> &img, int w, int h, float sigma)
{
	if (sigma < 0.5f)
		return;
	const int passes = 3;
	const float ideal = std::sqrt(12.0f * sigma * sigma / passes + 1.0f);
	int wl = (int)std::floor(ideal);
	if (wl % 2 == 0)
		wl--;
	const int wu = wl + 2;
	const float mIdeal = (12 * sigma * sigma - passes * wl * wl - 4 * passes * wl - 3 * passes) / (-4.0f * wl - 4);
	const int m = (int)std::lround(mIdeal);
	std::vector<float> tmp(img.size());
	auto pass = [&](int r, bool horizontal) {
		const int len = horizontal ? w : h;
		const int lines = horizontal ? h : w;
		const float inv = 1.0f / (2 * r + 1);
		for (int line = 0; line < lines; line++) {
			auto at = [&](int i) -> float & {
				i = std::max(0, std::min(len - 1, i));
				return horizontal ? img[(size_t)line * w + i] : img[(size_t)i * w + line];
			};
			auto out = [&](int i) -> float & {
				return horizontal ? tmp[(size_t)line * w + i] : tmp[(size_t)i * w + line];
			};
			float sum = 0;
			for (int i = -r; i <= r; i++)
				sum += at(i);
			for (int i = 0; i < len; i++) {
				out(i) = sum * inv;
				sum += at(i + r + 1) - at(i - r);
			}
		}
		img.swap(tmp);
	};
	for (int i = 0; i < passes; i++) {
		const int size = i < m ? wl : wu;
		const int r = std::max(0, (size - 1) / 2);
		pass(r, true);
		pass(r, false);
	}
}

/* The mask on a bigger, padded picture, moved down by dy, blurred, times alpha. */
std::vector<float> shadow_layer(const SvBitmap &mask, int pad, int dy, float sigma, float alpha)
{
	const int W = mask.w + pad * 2, H = mask.h + pad * 2;
	std::vector<float> img((size_t)W * H, 0.0f);
	for (int y = 0; y < mask.h; y++) {
		const int ty = y + pad + dy;
		if (ty < 0 || ty >= H)
			continue;
		for (int x = 0; x < mask.w; x++)
			img[(size_t)ty * W + x + pad] = mask.a[(size_t)y * mask.w + x] / 255.0f;
	}
	blur(img, W, H, sigma);
	for (float &v : img)
		v *= alpha;
	return img;
}

/* Lays b over a, as see-through layers stack. */
void stack(std::vector<float> &a, const std::vector<float> &b)
{
	for (size_t i = 0; i < a.size(); i++)
		a[i] = b[i] + a[i] * (1 - b[i]);
}

SvBitmap to_bitmap(const std::vector<float> &img, int w, int h, int ox, int oy)
{
	SvBitmap b;
	b.w = w;
	b.h = h;
	b.ox = ox;
	b.oy = oy;
	b.a.resize(img.size());
	for (size_t i = 0; i < img.size(); i++)
		b.a[i] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, img[i])) * 255.0f);
	return b;
}

/* How far each inside pixel is from the outline, in pixels (a quick estimate). */
std::vector<float> inside_distance(const SvBitmap &mask)
{
	const int w = mask.w, h = mask.h;
	const int INF = 1 << 28;
	std::vector<int> d((size_t)w * h);
	for (size_t i = 0; i < d.size(); i++)
		d[i] = mask.a[i] >= 128 ? INF : 0;
	auto get = [&](int x, int y) { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : d[(size_t)y * w + x]; };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int &v = d[(size_t)y * w + x];
			if (!v)
				continue;
			v = std::min({v, get(x - 1, y) + 3, get(x, y - 1) + 3, get(x - 1, y - 1) + 4, get(x + 1, y - 1) + 4});
		}
	for (int y = h - 1; y >= 0; y--)
		for (int x = w - 1; x >= 0; x--) {
			int &v = d[(size_t)y * w + x];
			if (!v)
				continue;
			v = std::min({v, get(x + 1, y) + 3, get(x, y + 1) + 3, get(x + 1, y + 1) + 4, get(x - 1, y + 1) + 4});
		}
	std::vector<float> out(d.size());
	for (size_t i = 0; i < d.size(); i++)
		out[i] = d[i] / 3.0f;
	return out;
}

/* A line of the given width just inside the outline. */
SvBitmap edge_line(const SvBitmap &mask, float width, float alpha)
{
	const std::vector<float> dist = inside_distance(mask);
	std::vector<float> img(dist.size());
	for (size_t i = 0; i < dist.size(); i++) {
		const float cov = mask.a[i] / 255.0f;
		const float inLine = std::min(1.0f, std::max(0.0f, width + 0.5f - dist[i]));
		img[i] = cov * inLine * alpha;
	}
	return to_bitmap(img, mask.w, mask.h, 0, 0);
}

struct Stop {
	float at;
	uint32_t argb;
};

/* The colour at t along a left to right fade, as a website canvas works it out. */
void fade_at(const Stop *stops, int n, float t, float out[4])
{
	int i = 0;
	while (i < n - 2 && t > stops[i + 1].at)
		i++;
	const float k = std::min(1.0f, std::max(0.0f, (t - stops[i].at) / (stops[i + 1].at - stops[i].at)));
	const int shifts[4] = {16, 8, 0, 24};
	for (int c = 0; c < 4; c++) {
		const float a = ((stops[i].argb >> shifts[c]) & 255) / 255.0f;
		const float b = ((stops[i + 1].argb >> shifts[c]) & 255) / 255.0f;
		out[c] = a + (b - a) * k;
	}
}

/* A thin horizontal line that fades along its length, over its own soft glow. */
SvColorBitmap lamp_line(int lineW, int lineH, const Stop *stops, int n, uint32_t glow, float sigma, int pad)
{
	const int W = lineW + pad * 2, H = lineH + pad * 2;
	std::vector<float> line((size_t)W * H, 0.0f), halo;
	std::vector<float> rgb((size_t)W * H * 3, 0.0f);
	for (int y = pad; y < pad + lineH; y++)
		for (int x = 0; x < lineW; x++) {
			float c[4];
			fade_at(stops, n, (x + 0.5f) / lineW, c);
			const size_t i = (size_t)y * W + x + pad;
			line[i] = c[3];
			rgb[i * 3] = c[0];
			rgb[i * 3 + 1] = c[1];
			rgb[i * 3 + 2] = c[2];
		}
	halo = line;
	blur(halo, W, H, sigma);
	const float gr = ((glow >> 16) & 255) / 255.0f, gg = ((glow >> 8) & 255) / 255.0f, gb = (glow & 255) / 255.0f;

	SvColorBitmap b;
	b.w = W;
	b.h = H;
	b.ox = -pad;
	b.oy = -pad;
	b.rgba.resize((size_t)W * H * 4);
	for (size_t i = 0; i < line.size(); i++) {
		const float la = line[i], ga = halo[i] * (1 - la), a = la + ga;
		const float k = a > 0 ? 1.0f / a : 0.0f;
		auto byte = [](float v) { return (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f); };
		b.rgba[i * 4] = byte((rgb[i * 3] * la + gr * ga) * k);
		b.rgba[i * 4 + 1] = byte((rgb[i * 3 + 1] * la + gg * ga) * k);
		b.rgba[i * 4 + 2] = byte((rgb[i * 3 + 2] * la + gb * ga) * k);
		b.rgba[i * 4 + 3] = byte(a);
	}
	return b;
}

} // namespace

SvColorBitmap sv_band_lamp(int w)
{
	const Stop stops[] = {{0.0f, 0x004d7cff}, {0.25f, 0xff4d7cff}, {0.5f, 0xffcfe0ff}, {0.75f, 0xff4d7cff}, {1.0f, 0x004d7cff}};
	return lamp_line(std::max(1, w), 2, stops, 5, 0xff4d7cff, 5.0f, 14);
}

SvBitmap sv_band_shadow(int w, int h)
{
	SvBitmap rect;
	rect.w = std::max(1, w);
	rect.h = std::max(1, h);
	rect.a.assign((size_t)rect.w * rect.h, 255);
	const int pad = 70;
	const int W = rect.w + pad * 2, H = rect.h + pad * 2;
	std::vector<float> base = shadow_layer(rect, pad, 0, 0, 1.0f);
	std::vector<float> acc((size_t)W * H, 0.0f);
	stack(acc, shadow_layer(rect, pad, 0, 20.0f, 0.9f));
	stack(acc, base);
	stack(acc, shadow_layer(rect, pad, 0, 6.0f, 0.75f));
	stack(acc, base);
	return to_bitmap(acc, W, H, -pad, -pad);
}

SvPieceArt sv_piece_art(const SvPiece &p, float k)
{
	/* k scales every frame size (shadow, glow, border, lamp line) with the
	   piece, for a zoomed piece's art made at its zoomed size. Kept within
	   reason so a tiny piece zoomed huge never makes a giant picture. */
	k = std::min(4.0f, std::max(1.0f, k));
	SvPieceArt art;
	const int w = std::max(1, (int)std::ceil(p.box.w));
	const int h = std::max(1, (int)std::ceil(p.box.h));
	art.mask = raster(outline(p, p.box.w, p.box.h), w, h);

	const int pad = (int)std::ceil(70 * k);
	if (p.frame == SvFrame::Depth || p.frame == SvFrame::Glow) {
		const int W = w + pad * 2, H = h + pad * 2;
		/* The solid shape itself, under the piece, as the website draws it. */
		std::vector<float> base = shadow_layer(art.mask, pad, 0, 0, 1.0f);
		std::vector<float> acc((size_t)W * H, 0.0f);
		if (p.frame == SvFrame::Depth) {
			/* Two drop shadows: a deep soft one and a tight contact one. */
			stack(acc, shadow_layer(art.mask, pad, (int)std::lround(14 * k), 17.0f * k, 0.72f));
			stack(acc, base);
			stack(acc, shadow_layer(art.mask, pad, (int)std::lround(4 * k), 4.5f * k, 0.55f));
			stack(acc, base);
			art.underColor = 0xff000000;
		} else {
			stack(acc, shadow_layer(art.mask, pad, 0, 14.0f * k, 1.0f));
			stack(acc, base);
			art.underColor = p.color;
		}
		/* A logo or channel name is see-through, so its shadow or glow stays
		   outside its box and never fills in behind it, the same as the website. */
		if (p.src == SvSrc::Logo || p.src == SvSrc::Name || p.src == SvSrc::Social)
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					acc[(size_t)(y + pad) * W + x + pad] *= 1.0f - art.mask.a[(size_t)y * w + x] / 255.0f;
		art.under = to_bitmap(acc, W, H, -pad, -pad);
	}

	if (p.frame == SvFrame::Border) {
		art.over = edge_line(art.mask, 6.0f * k, 1.0f);
		art.overColor = p.color;
	} else if (p.frame == SvFrame::Depth && p.lit) {
		art.over = edge_line(art.mask, 2.0f * k, 0.38f);
		art.overColor = 0xff8fb0ff;
		/* Square-sided pieces only, the same as the website. */
		if (p.mask == SvMask::Rect) {
			const int lineW = std::max(1, (int)std::lround(p.box.w * 0.72f));
			const Stop stops[] = {{0.0f, 0x00000000}, {0.3f, p.color}, {0.5f, 0xffcfe0ff}, {0.7f, p.color}, {1.0f, 0x00000000}};
			art.lamp = lamp_line(lineW, std::max(1, (int)std::lround(3 * k)), stops, 5, p.color, 4.0f * k,
					     (int)std::lround(12 * k));
			art.lamp.ox += (int)std::lround(p.box.w * 0.14f);
		}
	}
	return art;
}
