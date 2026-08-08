/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#include "r_local.h"

drawsurf_t	r_drawsurf;

int				lightleft, sourcesstep, blocksize, sourcetstep;
int				lightdelta, lightdeltastep;
int				lightright, lightleftstep, lightrightstep, blockdivshift;
unsigned		blockdivmask;
void			*prowdestbase;
unsigned char	*pbasesource;
int				surfrowbytes;	// used by ASM files
unsigned		*r_lightptr;
int				r_stepback;
int				r_lightwidth;
int				r_numhblocks, r_numvblocks;
unsigned char	*r_source, *r_sourcemax;

/*
 * Hires scale: source texels per BSP UV texel (width/base_width).
 * Full-res surface cache: cache is scale× larger; sample source 1:1;
 * span UVs scaled in D_CalcGradients.
 */
int				r_src_scale_s = 1;
int				r_src_scale_t = 1;
/* Cache pixels per lightmap cell on T (S uses blocksize). */
int				r_block_height = 16;

/* Truecolor source for R_DrawSurfaceBlock32 (NULL → 8-bit+colormap path). */
static const byte	*r_rgba;
static int			r_rgba_w;
static int			r_rgba_h;
static int			r_rgba_mip;
static int			r_mip_width;

void R_DrawSurfaceBlock8_mip0 (void);
void R_DrawSurfaceBlock8_mip1 (void);
void R_DrawSurfaceBlock8_mip2 (void);
void R_DrawSurfaceBlock8_mip3 (void);
void R_DrawSurfaceBlock8 (void);
void R_DrawSurfaceBlock32 (void);

/**
 * @brief Apply Quake lightmap shade to RGB, matched to vid.colormap.
 *
 * 8-bit path uses colormap[(light & 0xFF00) + pix]. We measure how that
 * row darkens a mid palette entry and scale truecolor RGB the same way
 * (avoids linear falloff looking much too dark).
 */
static unsigned R_LitPackRGB (int r, int g, int b, int light)
{
	int				s;
	int				ref, lit;
	int				pr, pg, pb, lr, lg, lb, denom;
	unsigned char	*cm;
	extern byte		*host_basepal;

	cm = (unsigned char *)vid.colormap;
	if (cm && host_basepal)
	{
		/* Palette ~13 is a stable mid/light gray in the Quake palette */
		ref = 13;
		lit = cm[(light & 0xFF00) + ref];
		pr = host_basepal[ref * 3 + 0];
		pg = host_basepal[ref * 3 + 1];
		pb = host_basepal[ref * 3 + 2];
		lr = host_basepal[lit * 3 + 0];
		lg = host_basepal[lit * 3 + 1];
		lb = host_basepal[lit * 3 + 2];
		denom = pr + pg + pb;
		if (denom > 0)
			s = ((lr + lg + lb) * 256) / denom;
		else
			s = 256;
	}
	else
	{
		/* Fallback: linear row scale */
		int	row = light >> 8;
		if (row < 0) row = 0;
		if (row > 63) row = 63;
		s = (64 - row) << 2;	/* 256 .. 4 */
	}
	if (s < 0)
		s = 0;
	if (s > 256)
		s = 256;
	r = (r * s) >> 8;
	g = (g * s) >> 8;
	b = (b * s) >> 8;
	return D_PackRGB (r, g, b);
}

/**
 * @brief Sample texture_t.rgba at mip-space (s,t) with box filter when mip>0.
 */
static unsigned R_SampleRGBALit (int s, int t, int light)
{
	int			mip = r_rgba_mip;
	int			rw = r_rgba_w;
	int			rh = r_rgba_h;
	const byte	*rgba = r_rgba;
	int			step, rs0, rt0, dx, dy, stride;
	int			rsum, gsum, bsum, count;
	const byte	*p;

	if (!rgba || rw < 1 || rh < 1)
		return 0;

	if (mip <= 0)
	{
		if (s < 0) s = 0;
		if (t < 0) t = 0;
		if (s >= rw) s = rw - 1;
		if (t >= rh) t = rh - 1;
		p = rgba + ((size_t)t * (size_t)rw + (size_t)s) * 4;
		if (p[3] < 128)
			return 0;
		return R_LitPackRGB (p[0], p[1], p[2], light);
	}

	/* Box over 2^mip block; cap samples at 4×4 for speed at mip3 */
	step = 1 << mip;
	rs0 = s * step;
	rt0 = t * step;
	stride = (step > 4) ? (step / 4) : 1;
	rsum = gsum = bsum = count = 0;
	for (dy = 0; dy < step; dy += stride)
	{
		int	rt = rt0 + dy;
		if (rt >= rh)
			break;
		for (dx = 0; dx < step; dx += stride)
		{
			int	rs = rs0 + dx;
			if (rs >= rw)
				break;
			p = rgba + ((size_t)rt * (size_t)rw + (size_t)rs) * 4;
			if (p[3] < 128)
				continue;
			rsum += p[0];
			gsum += p[1];
			bsum += p[2];
			count++;
		}
	}
	if (count < 1)
		return 0;
	return R_LitPackRGB (rsum / count, gsum / count, bsum / count, light);
}

/**
 * @brief BSP UV → source mip scale for hires replacements.
 */
void R_GetTextureScale (texture_t *mt, int mip, int *scale_s, int *scale_t)
{
	int	base_w, base_h, src_w, src_h, log_w, log_h;

	if (!mt)
	{
		*scale_s = 1;
		*scale_t = 1;
		return;
	}
	base_w = mt->base_width ? (int)mt->base_width : (int)mt->width;
	base_h = mt->base_height ? (int)mt->base_height : (int)mt->height;
	src_w = (int)mt->width >> mip;
	src_h = (int)mt->height >> mip;
	log_w = base_w >> mip;
	log_h = base_h >> mip;
	if (log_w < 1) log_w = 1;
	if (log_h < 1) log_h = 1;
	if (src_w < 1) src_w = 1;
	if (src_h < 1) src_h = 1;
	*scale_s = src_w / log_w;
	*scale_t = src_h / log_h;
	if (*scale_s < 1) *scale_s = 1;
	if (*scale_t < 1) *scale_t = 1;
}

static void	(*surfmiptable[4])(void) = {
	R_DrawSurfaceBlock8_mip0,
	R_DrawSurfaceBlock8_mip1,
	R_DrawSurfaceBlock8_mip2,
	R_DrawSurfaceBlock8_mip3
};



unsigned		blocklights[18*18];

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (void)
{
	msurface_t *surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1u<<lnum) ) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
#ifdef QUAKE2
				{
					unsigned temp;
					temp = (rad - dist)*256;
					i = t*smax + s;
					if (!cl_dlights[lnum].dark)
						blocklights[i] += temp;
					else
					{
						if (blocklights[i] > temp)
							blocklights[i] -= temp;
						else
							blocklights[i] = 0;
					}
				}
#else
					blocklights[t*smax + s] += (rad - dist)*256;
#endif
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (void)
{
	int			smax, tmax;
	int			t;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (r_fullbright.value || !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction		
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights ();

// bound, invert, and shift
	for (i=0 ; i<size ; i++)
	{
		t = (255*256 - (int)blocklights[i]) >> (8 - VID_CBITS);

		if (t < (1 << 6))
			t = (1 << 6);

		blocklights[i] = t;
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
===============
R_DrawSurface

Lightmaps stay in BSP base UV space. Surface cache is hires-scaled
(surfwidth = base_extent * r_src_scale_s) and filled 1:1 from source mips.
===============
*/
void R_DrawSurface (void)
{
	unsigned char	*basetptr;
	int				smax, tmax;
	int				u;
	int				soffset, basetoffset, texwidth;
	int				horzblockstep;
	int				base_bs;
	int				log_w, log_h;
	unsigned char	*pcolumndest;
	void			(*pblockdrawer)(void);
	texture_t		*mt;

// calculate the lightings
	R_BuildLightMap ();
	
	surfrowbytes = r_drawsurf.rowbytes;

	mt = r_drawsurf.texture;
	
	r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];

	/* Truecolor path: full-res RGBA when present (hires TGA load). */
	if (mt->rgba && mt->rgba_width > 0 && mt->rgba_height > 0 && r_pixbytes == 4)
	{
		r_rgba = mt->rgba;
		r_rgba_w = mt->rgba_width;
		r_rgba_h = mt->rgba_height;
		r_rgba_mip = r_drawsurf.surfmip;
	}
	else
	{
		r_rgba = NULL;
		r_rgba_w = r_rgba_h = 0;
		r_rgba_mip = 0;
	}
	
// the fractional light values should range from 0 to (VID_GRADES - 1) << 16
// from a source range of 0 - 255

	R_GetTextureScale (mt, r_drawsurf.surfmip, &r_src_scale_s, &r_src_scale_t);

	{
		int	base_w = mt->base_width ? (int)mt->base_width : (int)mt->width;
		int	base_h = mt->base_height ? (int)mt->base_height : (int)mt->height;
		int	src_w = (int)mt->width >> r_drawsurf.surfmip;
		int	src_h = (int)mt->height >> r_drawsurf.surfmip;

		log_w = base_w >> r_drawsurf.surfmip;
		log_h = base_h >> r_drawsurf.surfmip;
		if (log_w < 1) log_w = 1;
		if (log_h < 1) log_h = 1;
		if (src_w < 1) src_w = 1;
		if (src_h < 1) src_h = 1;

		/*
		 * Full-res cache: wrap and step in source (hires) space; sample 1:1.
		 * Lightmap cells still cover base_bs logical texels → base_bs*scale
		 * cache pixels each.
		 */
		texwidth = src_w;
		r_mip_width = src_w;
		smax = src_w;
		tmax = src_h;
		sourcetstep = src_w;
		r_stepback = src_h * src_w;
		r_sourcemax = r_source + r_stepback;
	}

	/* lightmap cell size in base UV texels at this mip */
	base_bs = 16 >> r_drawsurf.surfmip;
	/* cache pixels per lightmap cell (S and T may differ if art is non-square scale) */
	blocksize = base_bs * r_src_scale_s;
	r_block_height = base_bs * r_src_scale_t;
	if (blocksize < 1)
		blocksize = 1;
	if (r_block_height < 1)
		r_block_height = 1;
	/*
	 * blockdivshift used by mip drawers when scale==1.
	 * Scaled path uses integer division by blocksize instead.
	 */
	blockdivshift = 4 - r_drawsurf.surfmip;
	blockdivmask = (1 << blockdivshift) - 1;
	
	r_lightwidth = (r_drawsurf.surf->extents[0]>>4)+1;

	/* number of lightmap cells (base space), not cache pixels */
	r_numhblocks = (r_drawsurf.surf->extents[0] >> r_drawsurf.surfmip) / base_bs;
	r_numvblocks = (r_drawsurf.surf->extents[1] >> r_drawsurf.surfmip) / base_bs;
	if (r_numhblocks < 1) r_numhblocks = 1;
	if (r_numvblocks < 1) r_numvblocks = 1;

//==============================

	if (r_pixbytes == 1)
	{
		if (r_src_scale_s == 1 && r_src_scale_t == 1)
			pblockdrawer = surfmiptable[r_drawsurf.surfmip];
		else
			pblockdrawer = R_DrawSurfaceBlock8;
		horzblockstep = blocksize;
	}
	else if (r_pixbytes == 4)
	{
		pblockdrawer = R_DrawSurfaceBlock32;
		horzblockstep = blocksize * 4;
	}
	else
	{
		pblockdrawer = R_DrawSurfaceBlock16;
		horzblockstep = blocksize << 1;
	}

	/* texturemins are base UV; convert to source texels */
	soffset = r_drawsurf.surf->texturemins[0];
	basetoffset = r_drawsurf.surf->texturemins[1];

// << 16 components are to guarantee positive values for %
	soffset = ((soffset >> r_drawsurf.surfmip) + (log_w << 16)) % log_w;
	soffset *= r_src_scale_s;
	{
		int	log_t = (((basetoffset >> r_drawsurf.surfmip) + (log_h << 16)) % log_h);
		int	src_t = log_t * r_src_scale_t;
		basetptr = &r_source[src_t * texwidth];
	}

	pcolumndest = r_drawsurf.surfdat;

	for (u=0 ; u<r_numhblocks; u++)
	{
		r_lightptr = blocklights + u;

		prowdestbase = pcolumndest;

		/* source s (1:1 into cache) */
		pbasesource = basetptr + soffset;

		(*pblockdrawer)();

		soffset = soffset + blocksize;
		if (soffset >= smax)
			soffset = 0;

		pcolumndest += horzblockstep;
	}
}


//=============================================================================

#if	!id386

/*
================
R_DrawSurfaceBlock8_mip0
================
*/
void R_DrawSurfaceBlock8_mip0 (void)
{
	int				v, i, b, lightstep, light;
	int				ll, lr, llstep, lrstep;
	unsigned char	pix, *psource, *prowdest;
	unsigned char	*colormap;
	const int		srow = surfrowbytes;
	const int		sstep = sourcetstep;

	psource = pbasesource;
	prowdest = (unsigned char *)prowdestbase;
	colormap = (unsigned char *)vid.colormap;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		ll = (int)r_lightptr[0];
		lr = (int)r_lightptr[1];
		r_lightptr += r_lightwidth;
		llstep = ((int)r_lightptr[0] - ll) >> 4;
		lrstep = ((int)r_lightptr[1] - lr) >> 4;

		for (i=0 ; i<16 ; i++)
		{
			lightstep = (ll - lr) >> 4;
			light = lr;

			/* 16 texels right-to-left (matches original; scale==1 path) */
			for (b=15; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = colormap[(light & 0xFF00) + pix];
				light += lightstep;
			}

			psource += sstep;
			lr += lrstep;
			ll += llstep;
			prowdest += srow;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip1
================
*/
void R_DrawSurfaceBlock8_mip1 (void)
{
	int				v, i, b, lightstep, light;
	int				ll, lr, llstep, lrstep;
	unsigned char	pix, *psource, *prowdest;
	unsigned char	*colormap;
	const int		srow = surfrowbytes;
	const int		sstep = sourcetstep;

	psource = pbasesource;
	prowdest = (unsigned char *)prowdestbase;
	colormap = (unsigned char *)vid.colormap;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		ll = (int)r_lightptr[0];
		lr = (int)r_lightptr[1];
		r_lightptr += r_lightwidth;
		llstep = ((int)r_lightptr[0] - ll) >> 3;
		lrstep = ((int)r_lightptr[1] - lr) >> 3;

		for (i=0 ; i<8 ; i++)
		{
			lightstep = (ll - lr) >> 3;
			light = lr;

			for (b=7; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = colormap[(light & 0xFF00) + pix];
				light += lightstep;
			}

			psource += sstep;
			lr += lrstep;
			ll += llstep;
			prowdest += srow;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip2
================
*/
void R_DrawSurfaceBlock8_mip2 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 2;
		lightrightstep = (r_lightptr[1] - lightright) >> 2;

		for (i=0 ; i<4 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 2;

			light = lightright;

			for (b=3; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip3
================
*/
void R_DrawSurfaceBlock8_mip3 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 1;
		lightrightstep = (r_lightptr[1] - lightright) >> 1;

		for (i=0 ; i<2 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 1;

			light = lightright;

			for (b=1; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8

Generic 8-bit lit block for hires-scaled caches (blocksize may be >16).
Samples source 1:1; light interpolates across blocksize texels.
================
*/
void R_DrawSurfaceBlock8 (void)
{
	int				v, i, b, lightstep, light;
	int				ll, lr, llstep, lrstep;
	unsigned char	pix, *psource, *prowdest;
	unsigned char	*colormap;
	const int		srow = surfrowbytes;
	const int		sstep = sourcetstep;
	const int		bs = blocksize;
	const int		bt = r_block_height;

	psource = pbasesource;
	prowdest = (unsigned char *)prowdestbase;
	colormap = (unsigned char *)vid.colormap;

	for (v = 0; v < r_numvblocks; v++)
	{
		ll = (int)r_lightptr[0];
		lr = (int)r_lightptr[1];
		r_lightptr += r_lightwidth;
		llstep = ((int)r_lightptr[0] - ll) / bt;
		lrstep = ((int)r_lightptr[1] - lr) / bt;

		for (i = 0; i < bt; i++)
		{
			lightstep = (ll - lr) / bs;
			light = lr;

			for (b = bs - 1; b >= 0; b--)
			{
				pix = psource[b];
				prowdest[b] = colormap[(light & 0xFF00) + pix];
				light += lightstep;
			}

			psource += sstep;
			lr += lrstep;
			ll += llstep;
			prowdest += srow;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock32

Lit surface → native 32-bit pixels for X11.
- If texture_t.rgba: sample truecolor (box at mip>0), apply lightmap shade.
- Else: 8-bit mips + colormap + d_8to24table.
Hires: cache is scale× larger; sample 1:1 in mip space.
================
*/
void R_DrawSurfaceBlock32 (void)
{
	int				v, i, b, lightstep, light;
	int				ll, lr, llstep, lrstep;
	unsigned char	pix, *psource;
	unsigned		*prowdest;
	unsigned char	*colormap;
	const int		srow = surfrowbytes >> 2;
	const int		sstep = sourcetstep;
	const int		bs = blocksize;
	const int		bt = r_block_height;
	const int		mip_w = r_mip_width > 0 ? r_mip_width : 1;
	const int		use_rgba = (r_rgba != NULL);

	psource = pbasesource;
	prowdest = (unsigned *)prowdestbase;
	colormap = (unsigned char *)vid.colormap;

	for (v = 0; v < r_numvblocks; v++)
	{
		ll = (int)r_lightptr[0];
		lr = (int)r_lightptr[1];
		r_lightptr += r_lightwidth;
		llstep = ((int)r_lightptr[0] - ll) / bt;
		lrstep = ((int)r_lightptr[1] - lr) / bt;

		for (i = 0; i < bt; i++)
		{
			lightstep = (ll - lr) / bs;
			light = lr;

			if (use_rgba)
			{
				ptrdiff_t	pos = (ptrdiff_t)(psource - r_source);
				int			s0, t0;

				if (pos < 0)
					pos = 0;
				s0 = (int)(pos % mip_w);
				t0 = (int)(pos / mip_w);
				for (b = bs - 1; b >= 0; b--)
				{
					prowdest[b] = R_SampleRGBALit (s0 + b, t0, light);
					light += lightstep;
				}
			}
			else
			{
				for (b = bs - 1; b >= 0; b--)
				{
					pix = psource[b];
					if (pix == 255)
						prowdest[b] = 0;
					else
						prowdest[b] = d_8to24table[colormap[(light & 0xFF00) + pix]];
					light += lightstep;
				}
			}

			psource += sstep;
			lr += lrstep;
			ll += llstep;
			prowdest += srow;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}

/*
================
R_DrawSurfaceBlock16

FIXME: make this work
================
*/
void R_DrawSurfaceBlock16 (void)
{
	int				k;
	unsigned char	*psource;
	int				lighttemp, lightstep, light;
	unsigned short	*prowdest;

	prowdest = (unsigned short *)prowdestbase;

	for (k=0 ; k<blocksize ; k++)
	{
		unsigned short	*pdest;
		unsigned char	pix;
		int				b;

		psource = pbasesource;
		lighttemp = lightright - lightleft;
		lightstep = lighttemp >> blockdivshift;

		light = lightleft;
		pdest = prowdest;

		for (b=0; b<blocksize; b++)
		{
			pix = *psource;
			*pdest = vid.colormap16[(light & 0xFF00) + pix];
			psource += sourcesstep;
			pdest++;
			light += lightstep;
		}

		pbasesource += sourcetstep;
		lightright += lightrightstep;
		lightleft += lightleftstep;
		prowdest = (unsigned short *)((long)prowdest + surfrowbytes);
	}

	prowdestbase = prowdest;
}

#endif


//============================================================================

/*
================
R_GenTurbTile
================
*/
void R_GenTurbTile (pixel_t *pbasetex, void *pdest)
{
	int		*turb;
	int		i, j, s, t;
	byte	*pd;
	
	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (byte *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = *(pbasetex + (t<<6) + s);
		}
	}
}


/*
================
R_GenTurbTile16
================
*/
void R_GenTurbTile16 (pixel_t *pbasetex, void *pdest)
{
	int				*turb;
	int				i, j, s, t;
	unsigned short	*pd;

	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (unsigned short *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = d_8to16table[*(pbasetex + (t<<6) + s)];
		}
	}
}


/*
================
R_GenTile
================
*/
void R_GenTile (msurface_t *psurf, void *pdest)
{
	if (psurf->flags & SURF_DRAWTURB)
	{
		if (r_pixbytes == 1)
		{
			R_GenTurbTile ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
		else
		{
			R_GenTurbTile16 ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
	}
	else if (psurf->flags & SURF_DRAWSKY)
	{
		if (r_pixbytes == 1)
		{
			R_GenSkyTile (pdest);
		}
		else
		{
			R_GenSkyTile16 (pdest);
		}
	}
	else
	{
		Sys_Error ("Unknown tile type");
	}
}

