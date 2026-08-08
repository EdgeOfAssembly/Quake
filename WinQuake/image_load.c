/*
 * image_load.c - 24/32-bit TGA/RGBA load + convert to Quake 8-bit mips
 */
#include "quakedef.h"
#include "image_load.h"

#include <stdlib.h>
#include <string.h>

extern byte *host_basepal;

static qboolean Image_IsTransparentRGB (int r, int g, int b)
{
	int pr, pg, pb;
	if (!host_basepal)
		return (r == 255 && g == 0 && b == 255) ? true : false;
	pr = host_basepal[QUAKE_TRANSPARENT_INDEX * 3 + 0];
	pg = host_basepal[QUAKE_TRANSPARENT_INDEX * 3 + 1];
	pb = host_basepal[QUAKE_TRANSPARENT_INDEX * 3 + 2];
	if (r == pr && g == pg && b == pb)
		return true;
	if (r == 255 && g == 0 && b == 255)
		return true;
	return false;
}

static byte *Image_ReadFile (const char *path, int *len_out)
{
	int h, len;
	byte *buf;
	/* Optional hires art — do not spam FindFile: can't find */
	len = COM_OpenFileQuiet ((char *)path, &h);
	if (len < 0)
		return NULL;
	buf = (byte *)malloc ((size_t)len);
	if (!buf) {
		COM_CloseFile (h);
		return NULL;
	}
	Sys_FileRead (h, buf, len);
	COM_CloseFile (h);
	if (len_out)
		*len_out = len;
	return buf;
}

static byte *Image_LoadTGA (const byte *data, int len, int *out_w, int *out_h)
{
	int w, h, bpp, i, j, idlen, imagedesctype, pixsize, topdown;
	const byte *src;
	byte *rgba, *dst;

	if (len < 18)
		return NULL;
	idlen = data[0];
	imagedesctype = data[2];
	w = data[12] | (data[13] << 8);
	h = data[14] | (data[15] << 8);
	bpp = data[16];
	if (w < 1 || h < 1 || w > 4096 || h > 4096)
		return NULL;
	if (bpp != 24 && bpp != 32)
		return NULL;
	if (imagedesctype != 2)
		return NULL;
	src = data + 18 + idlen;
	pixsize = bpp / 8;
	if (18 + idlen + w * h * pixsize > len)
		return NULL;
	rgba = (byte *)malloc ((size_t)w * (size_t)h * 4);
	if (!rgba)
		return NULL;
	topdown = (data[17] & 0x20) != 0;
	for (i = 0; i < h; i++) {
		int row = topdown ? i : (h - 1 - i);
		for (j = 0; j < w; j++) {
			const byte *p = src + (row * w + j) * pixsize;
			dst = rgba + (i * w + j) * 4;
			dst[0] = p[2];
			dst[1] = p[1];
			dst[2] = p[0];
			if (pixsize == 4)
				dst[3] = p[3];
			else
				dst[3] = 255;
			if (dst[3] < 128)
				dst[3] = 0;
			else if (Image_IsTransparentRGB (dst[0], dst[1], dst[2]))
				dst[3] = 0;
			else
				dst[3] = 255;
		}
	}
	*out_w = w;
	*out_h = h;
	return rgba;
}

static byte *Image_LoadRGBA_Raw (const byte *data, int len, int *out_w, int *out_h)
{
	unsigned w, h;
	size_t need, n, i;
	byte *rgba;

	if (len < 12)
		return NULL;
	w = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
	h = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
	need = 12 + (size_t)w * (size_t)h * 4;
	if (w < 1 || h < 1 || w > 4096 || h > 4096 || (size_t)len < need)
		return NULL;
	rgba = (byte *)malloc ((size_t)w * (size_t)h * 4);
	if (!rgba)
		return NULL;
	memcpy (rgba, data + 12, (size_t)w * (size_t)h * 4);
	n = (size_t)w * (size_t)h;
	for (i = 0; i < n; i++) {
		byte *p = rgba + i * 4;
		if (p[3] < 128)
			p[3] = 0;
		else if (Image_IsTransparentRGB (p[0], p[1], p[2]))
			p[3] = 0;
		else
			p[3] = 255;
	}
	*out_w = (int)w;
	*out_h = (int)h;
	return rgba;
}

byte *Image_LoadRGBA (const char *path, int *out_w, int *out_h)
{
	byte *filedata, *rgba;
	int len;
	const char *ext;

	filedata = Image_ReadFile (path, &len);
	if (!filedata)
		return NULL;
	ext = strrchr (path, '.');
	rgba = NULL;
	if (ext) {
		if (!Q_strcasecmp ((char *)ext, ".tga"))
			rgba = Image_LoadTGA (filedata, len, out_w, out_h);
		else if (!Q_strcasecmp ((char *)ext, ".rgba"))
			rgba = Image_LoadRGBA_Raw (filedata, len, out_w, out_h);
	}
	free (filedata);
	return rgba;
}

static int Image_NearestPal (int r, int g, int b, int allow_fullbright)
{
	int i, best, best_d, d, pr, pg, pb, imax;
	if (!host_basepal)
		return 0;
	imax = allow_fullbright ? 256 : 224;
	best = 0;
	best_d = 1 << 30;
	for (i = 0; i < imax; i++) {
		if (i == QUAKE_TRANSPARENT_INDEX)
			continue;
		pr = host_basepal[i * 3 + 0] - r;
		pg = host_basepal[i * 3 + 1] - g;
		pb = host_basepal[i * 3 + 2] - b;
		d = 2 * pr * pr + 4 * pg * pg + 3 * pb * pb;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

int Image_RGBAToMiptex8 (const byte *rgba, int w, int h,
	byte **out_pixels, int *out_pixels_size, int allow_fullbright)
{
	int level, x, y, pixels, pw, ph, off;
	byte *mips, *level0, *prev;
	const byte *src;

	if ((w & 15) || (h & 15) || w < 16 || h < 16)
		return -1;
	pixels = (w * h * 85) / 64;
	mips = (byte *)malloc ((size_t)pixels);
	if (!mips)
		return -1;
	level0 = mips;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			src = rgba + (y * w + x) * 4;
			if (src[3] < 128)
				level0[y * w + x] = (byte)QUAKE_TRANSPARENT_INDEX;
			else
				level0[y * w + x] = (byte)Image_NearestPal (src[0], src[1], src[2], allow_fullbright);
		}
	}
	prev = level0;
	pw = w;
	ph = h;
	off = w * h;
	/*
	 * Box-filter mips in RGB then re-quantize (was nearest-neighbor).
	 * Needs host_basepal — set before texture load.
	 */
	for (level = 1; level < 4; level++) {
		int nw = pw >> 1, nh = ph >> 1;
		byte *dst = mips + off;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
		for (y = 0; y < nh; y++) {
			for (x = 0; x < nw; x++) {
				int	x0 = x * 2, y0 = y * 2;
				int	dx, dy, ntrans = 0, nopaque = 0;
				int	sr = 0, sg = 0, sb = 0;
				for (dy = 0; dy < 2; dy++) {
					int yy = y0 + dy;
					if (yy >= ph) yy = ph - 1;
					for (dx = 0; dx < 2; dx++) {
						int xx = x0 + dx;
						int idx;
						if (xx >= pw) xx = pw - 1;
						idx = prev[yy * pw + xx];
						if (idx == QUAKE_TRANSPARENT_INDEX) {
							ntrans++;
							continue;
						}
						if (host_basepal) {
							sr += host_basepal[idx * 3 + 0];
							sg += host_basepal[idx * 3 + 1];
							sb += host_basepal[idx * 3 + 2];
						}
						nopaque++;
					}
				}
				if (nopaque == 0 || ntrans >= 3)
					dst[y * nw + x] = (byte)QUAKE_TRANSPARENT_INDEX;
				else if (host_basepal && nopaque > 0)
					dst[y * nw + x] = (byte)Image_NearestPal (
						sr / nopaque, sg / nopaque, sb / nopaque,
						allow_fullbright);
				else
					dst[y * nw + x] = prev[y0 * pw + x0];
			}
		}
		off += nw * nh;
		prev = dst;
		pw = nw;
		ph = nh;
	}
	*out_pixels = mips;
	*out_pixels_size = pixels;
	return 0;
}


/*
=================
Image_FitTextureSize

Policy: keep art as large as possible; engine supports up to max_edge and
16-aligned sizes. If needed, box-downscale RGBA to the largest valid size.
Returns new buffer (caller frees old if different) or same pointer.
=================
*/
byte *Image_FitTextureSize (byte *rgba, int *w, int *h, int max_edge)
{
	int	ow, oh, nw, nh, x, y, xi, yi, count;
	int	r, g, b, a;
	byte	*out;
	const byte *src;

	if (!rgba || !w || !h)
		return rgba;
	ow = *w;
	oh = *h;
	if (max_edge < IMAGE_TEX_ALIGN)
		max_edge = IMAGE_TEX_ALIGN;
	max_edge &= ~(IMAGE_TEX_ALIGN - 1);

	nw = ow;
	nh = oh;
	if (nw > max_edge)
		nw = max_edge;
	if (nh > max_edge)
		nh = max_edge;
	nw &= ~(IMAGE_TEX_ALIGN - 1);
	nh &= ~(IMAGE_TEX_ALIGN - 1);
	if (nw < IMAGE_TEX_ALIGN)
		nw = IMAGE_TEX_ALIGN;
	if (nh < IMAGE_TEX_ALIGN)
		nh = IMAGE_TEX_ALIGN;

	if (nw == ow && nh == oh)
		return rgba;

	out = (byte *)malloc ((size_t)nw * (size_t)nh * 4);
	if (!out)
		return rgba;

	/* box filter */
	for (y = 0; y < nh; y++)
	{
		int y0 = y * oh / nh;
		int y1 = (y + 1) * oh / nh;
		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0; x < nw; x++)
		{
			int x0 = x * ow / nw;
			int x1 = (x + 1) * ow / nw;
			if (x1 <= x0)
				x1 = x0 + 1;
			r = g = b = a = count = 0;
			for (yi = y0; yi < y1; yi++)
			{
				for (xi = x0; xi < x1; xi++)
				{
					src = rgba + ((size_t)yi * (size_t)ow + (size_t)xi) * 4;
					r += src[0];
					g += src[1];
					b += src[2];
					a += src[3];
					count++;
				}
			}
			if (count < 1)
				count = 1;
			out[((size_t)y * (size_t)nw + (size_t)x) * 4 + 0] = (byte)(r / count);
			out[((size_t)y * (size_t)nw + (size_t)x) * 4 + 1] = (byte)(g / count);
			out[((size_t)y * (size_t)nw + (size_t)x) * 4 + 2] = (byte)(b / count);
			out[((size_t)y * (size_t)nw + (size_t)x) * 4 + 3] = (byte)(a / count);
		}
	}
	free (rgba);
	*w = nw;
	*h = nh;
	return out;
}
