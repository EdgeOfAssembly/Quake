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
	len = COM_OpenFile ((char *)path, &h);
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
	for (level = 1; level < 4; level++) {
		int nw = pw >> 1, nh = ph >> 1;
		byte *dst = mips + off;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
		for (y = 0; y < nh; y++)
			for (x = 0; x < nw; x++)
				dst[y * nw + x] = prev[(y * 2) * pw + (x * 2)];
		off += nw * nh;
		prev = dst;
		pw = nw;
		ph = nh;
	}
	*out_pixels = mips;
	*out_pixels_size = pixels;
	return 0;
}
