#ifndef IMAGE_LOAD_H
#define IMAGE_LOAD_H

/*
 * 24/32-bit image load for software Quake.
 * Include after quakedef.h so byte is defined.
 */

byte *Image_LoadRGBA (const char *path, int *out_w, int *out_h);
int Image_RGBAToMiptex8 (const byte *rgba, int w, int h,
	byte **out_pixels, int *out_pixels_size, int allow_fullbright);

#define QUAKE_TRANSPARENT_INDEX 255

#endif
