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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

unsigned char	*r_turb_pbase, *r_turb_pdest;
fixed16_t		r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int				*r_turb_turb;
int				r_turb_spancount;

/* Truecolor turb: full-res RGBA (NULL → 8-bit + d_8to24table). */
const byte		*r_turb_rgba;
int				r_turb_rgba_w;
int				r_turb_rgba_h;

void D_DrawTurbulent8Span (void);


/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
//
// 32bpp: copy whole pixels (was byte-only → scrambled underwater view).
=============
*/
void D_WarpScreen (void)
{
	int		w, h;
	int		u,v;
	byte	*dest;
	int		*turb;
	int		*col;
	byte	**row;
	byte	*rowptr[MAXHEIGHT+(AMP2*2)];
	int		column[MAXWIDTH+(AMP2*2)];
	float	wratio, hratio;

	w = r_refdef.vrect.width;
	h = r_refdef.vrect.height;

	wratio = w / (float)scr_vrect.width;
	hratio = h / (float)scr_vrect.height;

	/* screenwidth is byte stride of d_viewbuffer */
	for (v=0 ; v<scr_vrect.height+AMP2*2 ; v++)
	{
		rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
				 (screenwidth * (int)((float)v * hratio * h / (h + AMP2 * 2)));
	}

	/* column[] = horizontal pixel offset within the viewbuffer scanline */
	for (u=0 ; u<scr_vrect.width+AMP2*2 ; u++)
	{
		column[u] = r_refdef.vrect.x +
				(int)((float)u * wratio * w / (w + AMP2 * 2));
	}

	turb = intsintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	dest = vid.buffer + scr_vrect.y * vid.rowbytes + scr_vrect.x * r_pixbytes;

	if (r_pixbytes == 4)
	{
		for (v = 0; v < scr_vrect.height; v++, dest += vid.rowbytes)
		{
			unsigned	*pdest = (unsigned *)dest;

			col = &column[turb[v]];
			row = &rowptr[v];
			for (u = 0; u < scr_vrect.width; u++)
			{
				/* row[] is scanline start; col[] is pixel x */
				pdest[u] = ((unsigned *)row[turb[u]])[col[u]];
			}
		}
	}
	else
	{
		for (v=0 ; v<scr_vrect.height ; v++, dest += vid.rowbytes)
		{
			col = &column[turb[v]];
			row = &rowptr[v];

			for (u=0 ; u<scr_vrect.width ; u+=4)
			{
				dest[u+0] = row[turb[u+0]][col[u+0]];
				dest[u+1] = row[turb[u+1]][col[u+1]];
				dest[u+2] = row[turb[u+2]][col[u+2]];
				dest[u+3] = row[turb[u+3]][col[u+3]];
			}
		}
	}
}


#if	!id386

/*
=============
D_DrawTurbulent8Span
=============
*/
void D_DrawTurbulent8Span (void)
{
	int		sturb, tturb;
	byte	tex;
	/* UV math in base/cachewidth space (usually 64); hires only at sample. */
	const int	tw = (cachewidth > 0) ? cachewidth : 64;
	const int	tmask = tw - 1;
	const int	use_rgba = (r_turb_rgba != NULL && r_pixbytes == 4
				&& r_turb_rgba_w > 0 && r_turb_rgba_h > 0);
	const int	rw = use_rgba ? r_turb_rgba_w : tw;
	const int	rh = use_rgba ? r_turb_rgba_h : tw;

	do
	{
		/* Stock warp: sintable ±AMP in base texel space, & CYCLE for table */
		sturb = ((r_turb_s + r_turb_turb[(r_turb_t >> 16) & (CYCLE - 1)]) >> 16)
			& tmask;
		tturb = ((r_turb_t + r_turb_turb[(r_turb_s >> 16) & (CYCLE - 1)]) >> 16)
			& tmask;

		if (use_rgba)
		{
			const byte	*p;
			int		rs, rt;
			/* Map base UV → hires texel (nearest) */
			rs = (sturb * rw) / tw;
			rt = (tturb * rh) / tw;
			if (rs < 0) rs = 0;
			if (rt < 0) rt = 0;
			if (rs >= rw) rs = rw - 1;
			if (rt >= rh) rt = rh - 1;
			p = r_turb_rgba + ((size_t)rt * (size_t)rw + (size_t)rs) * 4;
			*(unsigned *)r_turb_pdest = D_PackRGB (p[0], p[1], p[2]);
			r_turb_pdest += 4;
		}
		else
		{
			tex = *(r_turb_pbase + tturb * tw + sturb);
			if (r_pixbytes == 4)
			{
				*(unsigned *)r_turb_pdest = d_8to24table[tex];
				r_turb_pdest += 4;
			}
			else
				*r_turb_pdest++ = tex;
		}
		r_turb_s += r_turb_sstep;
		r_turb_t += r_turb_tstep;
	} while (--r_turb_spancount > 0);
}

#endif	// !id386


/*
=============
Turbulent8
=============
*/
void Turbulent8 (espan_t *pspan)
{
	int				count;
	fixed16_t		snext, tnext;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz16stepu, tdivz16stepu, zi16stepu;
	
	r_turb_turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));

	r_turb_sstep = 0;	// keep compiler happy
	r_turb_tstep = 0;	// ditto

	r_turb_pbase = (unsigned char *)cacheblock;
	/* r_turb_rgba set by D_DrawSurfaces when texture has truecolor */

	sdivz16stepu = d_sdivzstepu * 16;
	tdivz16stepu = d_tdivzstepu * 16;
	zi16stepu = d_zistepu * 16;

	do
	{
		r_turb_pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u * r_pixbytes);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)
			r_turb_s = bbextents;
		else if (r_turb_s < 0)
			r_turb_s = 0;

		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)
			r_turb_t = bbextentt;
		else if (r_turb_t < 0)
			r_turb_t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 16)
				r_turb_spancount = 16;
			else
				r_turb_spancount = count;

			count -= r_turb_spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;	// guard against round-off error on <0 steps

				r_turb_sstep = (snext - r_turb_s) >> 4;
				r_turb_tstep = (tnext - r_turb_t) >> 4;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;	// guard against round-off error on <0 steps

				if (r_turb_spancount > 1)
				{
					r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
					r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
				}
			}

			/* Stock wrap: CYCLE period for turb table (base UV space). */
			r_turb_s = r_turb_s & ((CYCLE << 16) - 1);
			r_turb_t = r_turb_t & ((CYCLE << 16) - 1);

			D_DrawTurbulent8Span ();

			r_turb_s = snext;
			r_turb_t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}


#if	!id386

/*
=============
D_DrawSpans8
=============
*/
void D_DrawSpans8 (espan_t *pspan)
{
	int				count, spancount;
	unsigned char	*pbase, *pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz8stepu, tdivz8stepu, zi8stepu;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	pbase = (unsigned char *)cacheblock;

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				*pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans16

C port of the 16-pixel subdivision path (was ASM-only on id386).
Fewer perspective corrections per span → less work in the hot loop.
=============
*/
void D_DrawSpans16 (espan_t *pspan)
{
	int				count, spancount;
	unsigned char	*pbase, *pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz16stepu, tdivz16stepu, zi16stepu;

	sstep = 0;
	tstep = 0;

	pbase = (unsigned char *)cacheblock;

	sdivz16stepu = d_sdivzstepu * 16;
	tdivz16stepu = d_tdivzstepu * 16;
	zi16stepu = d_zistepu * 16;

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
			if (count >= 16)
				spancount = 16;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi;

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				sstep = (snext - s) >> 4;
				tstep = (tnext - t) >> 4;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				*pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans32bpp

Native 32-bit color spans. cacheblock holds lit uint32_t texels;
screenwidth is byte stride; horizontal step is r_pixbytes (4).
=============
*/
void D_DrawSpans32bpp (espan_t *pspan)
{
	int				count, spancount;
	unsigned		*pbase, *pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz32stepu, tdivz32stepu, zi32stepu;
	const int		cwidth = cachewidth;
	const int		cshift = (cwidth > 0 && (cwidth & (cwidth - 1)) == 0)
					? __builtin_ctz ((unsigned)cwidth) : -1;
	byte			*scan;

	sstep = 0;
	tstep = 0;
	pbase = (unsigned *)cacheblock;

	/* 64-pixel affine subdiv: fewer 1/z; measure before keeping */
	sdivz32stepu = d_sdivzstepu * 64;
	tdivz32stepu = d_tdivzstepu * 64;
	zi32stepu = d_zistepu * 64;

	do
	{
		scan = (byte *)d_viewbuffer + (screenwidth * pspan->v);
		pdest = (unsigned *)(scan + pspan->u * 4);
		count = pspan->count;

		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
			if (count >= 64)
				spancount = 64;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz32stepu;
				tdivz += tdivz32stepu;
				zi += zi32stepu;
				z = (float)0x10000 / zi;

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				sstep = (snext - s) >> 6;
				tstep = (tnext - t) >> 6;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			/* PoT cache width: shift instead of multiply in the pixel loop */
			if (cshift >= 0)
			{
				while (spancount >= 4)
				{
					pdest[0] = pbase[(s >> 16) + ((t >> 16) << cshift)];
					s += sstep; t += tstep;
					pdest[1] = pbase[(s >> 16) + ((t >> 16) << cshift)];
					s += sstep; t += tstep;
					pdest[2] = pbase[(s >> 16) + ((t >> 16) << cshift)];
					s += sstep; t += tstep;
					pdest[3] = pbase[(s >> 16) + ((t >> 16) << cshift)];
					s += sstep; t += tstep;
					pdest += 4;
					spancount -= 4;
				}
				while (spancount-- > 0)
				{
					*pdest++ = pbase[(s >> 16) + ((t >> 16) << cshift)];
					s += sstep;
					t += tstep;
				}
			}
			else
			{
				while (spancount >= 4)
				{
					pdest[0] = pbase[(s >> 16) + (t >> 16) * cwidth];
					s += sstep; t += tstep;
					pdest[1] = pbase[(s >> 16) + (t >> 16) * cwidth];
					s += sstep; t += tstep;
					pdest[2] = pbase[(s >> 16) + (t >> 16) * cwidth];
					s += sstep; t += tstep;
					pdest[3] = pbase[(s >> 16) + (t >> 16) * cwidth];
					s += sstep; t += tstep;
					pdest += 4;
					spancount -= 4;
				}
				while (spancount-- > 0)
				{
					*pdest++ = pbase[(s >> 16) + (t >> 16) * cwidth];
					s += sstep;
					t += tstep;
				}
			}

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans32

Wider subdivision (32 px) — fewer 1/z corrections. Slightly more affine
error; measure with timedemo before keeping.
=============
*/
void D_DrawSpans32 (espan_t *pspan)
{
	int				count, spancount;
	unsigned char	*pbase, *pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz32stepu, tdivz32stepu, zi32stepu;
	const int		cwidth = cachewidth;

	sstep = 0;
	tstep = 0;
	pbase = (unsigned char *)cacheblock;

	sdivz32stepu = d_sdivzstepu * 32;
	tdivz32stepu = d_tdivzstepu * 32;
	zi32stepu = d_zistepu * 32;

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);
		count = pspan->count;

		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
			if (count >= 32)
				spancount = 32;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz32stepu;
				tdivz += tdivz32stepu;
				zi += zi32stepu;
				z = (float)0x10000 / zi;

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 32)
					snext = 32;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 32)
					tnext = 32;

				sstep = (snext - s) >> 5;
				tstep = (tnext - t) >> 5;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 32)
					snext = 32;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 32)
					tnext = 32;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				*pdest++ = *(pbase + (s >> 16) + (t >> 16) * cwidth);
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

#endif


#if	!id386

/*
=============
D_DrawZSpans
=============
*/
void D_DrawZSpans (espan_t *pspan)
{
	int				count, doublecount, izistep;
	int				izi;
	short			*pdest;
	unsigned		ltemp;
	double			zi;
	float			du, dv;
	const int		zwidth = d_zwidth;

// FIXME: check for clamping/range problems
// we count on FP exceptions being turned off to avoid range problems
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = d_pzbuffer + (zwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
	// we count on FP exceptions being turned off to avoid range problems
		izi = (int)(zi * 0x8000 * 0x10000);

		if ((uintptr_t)pdest & 0x02)
		{
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}

		if ((doublecount = count >> 1) > 0)
		{
			/* Unroll ×4 pairs (8 z-samples) for long spans */
			while (doublecount >= 4)
			{
				ltemp = (unsigned)(izi >> 16);
				izi += izistep;
				ltemp |= (unsigned)izi & 0xFFFF0000u;
				izi += izistep;
				((unsigned *)pdest)[0] = ltemp;

				ltemp = (unsigned)(izi >> 16);
				izi += izistep;
				ltemp |= (unsigned)izi & 0xFFFF0000u;
				izi += izistep;
				((unsigned *)pdest)[1] = ltemp;

				ltemp = (unsigned)(izi >> 16);
				izi += izistep;
				ltemp |= (unsigned)izi & 0xFFFF0000u;
				izi += izistep;
				((unsigned *)pdest)[2] = ltemp;

				ltemp = (unsigned)(izi >> 16);
				izi += izistep;
				ltemp |= (unsigned)izi & 0xFFFF0000u;
				izi += izistep;
				((unsigned *)pdest)[3] = ltemp;

				pdest += 8;
				doublecount -= 4;
			}
			while (doublecount-- > 0)
			{
				ltemp = (unsigned)(izi >> 16);
				izi += izistep;
				ltemp |= (unsigned)izi & 0xFFFF0000u;
				izi += izistep;
				*(unsigned *)pdest = ltemp;
				pdest += 2;
			}
		}

		if (count & 1)
			*pdest = (short)(izi >> 16);

	} while ((pspan = pspan->pnext) != NULL);
}

#endif

