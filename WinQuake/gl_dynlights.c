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
// gl_dynlights.c -- automatic colored dynamic lights from map lamps / flames

#include "quakedef.h"

/*
 * Map light probes (tlight* faces + light* entities) are collected once per
 * map load.  Each frame the nearest probes become real dlights (MAX_DLIGHTS
 * is 32 because dlightbits is an int bitfield — leave room for weapons).
 */

#define MAX_DYN_PROBES		256
#define MAX_ACTIVE_MAP_LIGHTS	24
#define DYNLIGHT_KEY_BASE	10000
#define DYNLIGHT_MODEL_KEY_BASE	20000
#define DYNLIGHT_NORMAL_OFFSET	12.0f

typedef struct
{
	vec3_t	origin;
	vec3_t	color;
	float	radius;
} dynlight_probe_t;

static dynlight_probe_t	r_dyn_probes[MAX_DYN_PROBES];
static int		r_num_dyn_probes;

cvar_t	r_dynlights = {"r_dynlights", "1"};
cvar_t	r_dynlights_radius = {"r_dynlights_radius", "160"};
cvar_t	r_dynlights_map = {"r_dynlights_map", "1"};
cvar_t	r_dynlights_models = {"r_dynlights_models", "1"};
/* 0=tlight faces only, 1=also torch/flame/fluoro ents, 2=+plain light ents */
cvar_t	r_dynlights_entities = {"r_dynlights_entities", "1"};
cvar_t	r_dynlights_intensity = {"r_dynlights_intensity", "1.1"};

/*
===============
R_InitDynLights
===============
*/
void R_InitDynLights (void)
{
	Cvar_RegisterVariable (&r_dynlights);
	Cvar_RegisterVariable (&r_dynlights_radius);
	Cvar_RegisterVariable (&r_dynlights_map);
	Cvar_RegisterVariable (&r_dynlights_models);
	Cvar_RegisterVariable (&r_dynlights_entities);
	Cvar_RegisterVariable (&r_dynlights_intensity);
}

/*
===============
DynLight_AddProbe
===============
*/
static void DynLight_AddProbe (vec3_t origin, float r, float g, float b, float radius)
{
	dynlight_probe_t	*p;

	if (r_num_dyn_probes >= MAX_DYN_PROBES)
		return;

	p = &r_dyn_probes[r_num_dyn_probes++];
	VectorCopy (origin, p->origin);
	p->color[0] = r;
	p->color[1] = g;
	p->color[2] = b;
	p->radius = radius;
}

/*
===============
DynLight_ColorForTexture

Returns false if this texture should not emit (e.g. sliplite).
===============
*/
static qboolean DynLight_ColorForTexture (const char *name, float *r, float *g, float *b, float *radius)
{
	/* default warm panel */
	*r = 1.0f;
	*g = 0.85f;
	*b = 0.35f;
	*radius = 140.0f;

	if (!Q_strncmp ((char *)name, "tlight01", 8))
	{
		*r = 1.0f; *g = 0.75f; *b = 0.35f;
		*radius = 130.0f;
		return true;
	}
	if (!Q_strncmp ((char *)name, "tlight02", 8)
	 || !Q_strncmp ((char *)name, "tlight10", 8))
	{
		*r = 1.0f; *g = 0.85f; *b = 0.35f;
		*radius = 150.0f;
		return true;
	}
	if (!Q_strncmp ((char *)name, "tlight07", 8))
	{
		*r = 1.0f; *g = 0.55f; *b = 0.2f;
		*radius = 145.0f;
		return true;
	}
	if (!Q_strncmp ((char *)name, "tlight08", 8))
	{
		*r = 0.55f; *g = 0.4f; *b = 0.22f;
		*radius = 140.0f;	/* dim fixture housing */
		return true;
	}
	if (!Q_strncmp ((char *)name, "tlight11", 8))
	{
		*r = 1.0f; *g = 0.9f; *b = 0.7f;
		*radius = 200.0f;
		return true;
	}
	if (!Q_strncmp ((char *)name, "tlight", 6))
	{
		/* other tlight* variants */
		*radius = 200.0f;
		return true;
	}

	/* sliplite: skip (red slipgate accent, not room light) */
	return false;
}

/*
===============
DynLight_SurfOrigin

Average poly verts, then offset along plane normal into the room.
Quake face normals point toward empty space, so origin + normal * offset
moves the probe off the wall/ceiling into the volume.
===============
*/
static qboolean DynLight_SurfOrigin (msurface_t *surf, vec3_t out)
{
	glpoly_t	*p;
	int		i, n;
	float		*v;
	vec3_t		sum;
	mplane_t	*plane;

	p = surf->polys;
	if (!p || p->numverts < 1)
		return false;

	sum[0] = sum[1] = sum[2] = 0;
	n = p->numverts;
	for (i = 0, v = p->verts[0]; i < n; i++, v += VERTEXSIZE)
	{
		sum[0] += v[0];
		sum[1] += v[1];
		sum[2] += v[2];
	}
	out[0] = sum[0] / n;
	out[1] = sum[1] / n;
	out[2] = sum[2] / n;

	plane = surf->plane;
	if (plane)
	{
		/* SURF_PLANEBACK: stored plane is flipped relative to the face */
		if (surf->flags & SURF_PLANEBACK)
		{
			out[0] -= plane->normal[0] * DYNLIGHT_NORMAL_OFFSET;
			out[1] -= plane->normal[1] * DYNLIGHT_NORMAL_OFFSET;
			out[2] -= plane->normal[2] * DYNLIGHT_NORMAL_OFFSET;
		}
		else
		{
			out[0] += plane->normal[0] * DYNLIGHT_NORMAL_OFFSET;
			out[1] += plane->normal[1] * DYNLIGHT_NORMAL_OFFSET;
			out[2] += plane->normal[2] * DYNLIGHT_NORMAL_OFFSET;
		}
	}

	return true;
}

/*
===============
DynLight_CollectSurfaces
===============
*/
static void DynLight_CollectSurfaces (void)
{
	int		i;
	msurface_t	*surf;
	texture_t	*tex;
	char		*name;
	vec3_t		org;
	float		r, g, b, rad;

	if (!cl.worldmodel)
		return;

	surf = cl.worldmodel->surfaces;
	for (i = 0; i < cl.worldmodel->numsurfaces; i++, surf++)
	{
		if (!surf->texinfo || !surf->texinfo->texture)
			continue;
		tex = surf->texinfo->texture;
		name = tex->name;
		if (!name || name[0] == 0)
			continue;

		/* only tlight* (and known lamp names handled in ColorForTexture) */
		if (Q_strncmp (name, "tlight", 6) != 0)
			continue;

		if (!DynLight_ColorForTexture (name, &r, &g, &b, &rad))
			continue;
		if (!DynLight_SurfOrigin (surf, org))
			continue;

		DynLight_AddProbe (org, r, g, b, rad);
	}
}

/*
===============
DynLight_ClassInfo

Map entity classname → color + default radius.  Returns false if not a light.
===============
*/
static qboolean DynLight_ClassInfo (const char *classname, float *r, float *g, float *b, float *radius)
{
	if (!classname || !classname[0])
		return false;

	if (!strcmp (classname, "light"))
	{
		/* baked already — only if user opts in (doubles haze) */
		if (r_dynlights_entities.value < 2)
			return false;
		*r = 1.0f; *g = 0.9f; *b = 0.7f;
		*radius = 180.0f;
		return true;
	}
	if (!Q_strncmp ((char *)classname, "light_torch", 11))
	{
		*r = 1.0f; *g = 0.55f; *b = 0.15f;
		*radius = 200.0f;
		return true;
	}
	if (!strcmp (classname, "light_flame_large_yellow"))
	{
		*r = 1.0f; *g = 0.65f; *b = 0.2f;
		*radius = 250.0f;
		return true;
	}
	if (!strcmp (classname, "light_flame_small_yellow"))
	{
		*r = 1.0f; *g = 0.6f; *b = 0.18f;
		*radius = 180.0f;
		return true;
	}
	if (!strcmp (classname, "light_flame_small_white"))
	{
		*r = 1.0f; *g = 0.95f; *b = 0.85f;
		*radius = 180.0f;
		return true;
	}
	if (!Q_strncmp ((char *)classname, "light_flame", 11))
	{
		*r = 1.0f; *g = 0.6f; *b = 0.2f;
		*radius = 200.0f;
		return true;
	}
	if (!Q_strncmp ((char *)classname, "light_fluoro", 12)
	 || !strcmp (classname, "light_fluorospark"))
	{
		*r = 0.7f; *g = 0.9f; *b = 1.0f;
		*radius = 250.0f;
		return true;
	}
	if (!strcmp (classname, "light_globe"))
	{
		*r = 1.0f; *g = 0.95f; *b = 0.8f;
		*radius = 200.0f;
		return true;
	}

	return false;
}

/*
===============
DynLight_CollectEntities

Parse cl.worldmodel->entities for light* classnames.
===============
*/
static void DynLight_CollectEntities (void)
{
	char	*data;
	char	key[128];
	char	classname[64];
	vec3_t	origin;
	float	light_val;
	float	r, g, b, rad;
	int		have_origin;
	int		have_class;

	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;
	if (r_dynlights_entities.value < 1)
		return;

	data = cl.worldmodel->entities;

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			break;
		if (com_token[0] != '{')
			break;

		classname[0] = 0;
		origin[0] = origin[1] = origin[2] = 0;
		light_val = 0;
		have_origin = 0;
		have_class = 0;

		while (1)
		{
			data = COM_Parse (data);
			if (!data)
				return;
			if (com_token[0] == '}')
				break;

			Q_strncpy (key, com_token, sizeof(key) - 1);
			key[sizeof(key) - 1] = 0;

			data = COM_Parse (data);
			if (!data)
				return;

			if (!strcmp (key, "classname"))
			{
				Q_strncpy (classname, com_token, sizeof(classname) - 1);
				classname[sizeof(classname) - 1] = 0;
				have_class = 1;
			}
			else if (!strcmp (key, "origin"))
			{
				sscanf (com_token, "%f %f %f", &origin[0], &origin[1], &origin[2]);
				have_origin = 1;
			}
			else if (!strcmp (key, "light"))
			{
				light_val = Q_atof (com_token);
			}
		}

		if (!have_class || !have_origin)
			continue;
		if (!DynLight_ClassInfo (classname, &r, &g, &b, &rad))
			continue;
		if (light_val > 0)
			rad = light_val;

		DynLight_AddProbe (origin, r, g, b, rad);
	}
}

/*
===============
R_ParseMapDynLights

Build probe list after world model is ready (R_NewMap).
===============
*/
void R_ParseMapDynLights (void)
{
	r_num_dyn_probes = 0;

	if (!cl.worldmodel)
		return;

	DynLight_CollectSurfaces ();
	DynLight_CollectEntities ();

	Con_DPrintf ("dynlights: %d map probes\n", r_num_dyn_probes);
}

/*
===============
DynLight_IsFlameModel
===============
*/
static qboolean DynLight_IsFlameModel (model_t *mod)
{
	if (!mod || !mod->name[0])
		return false;
	if (!strcmp (mod->name, "progs/flame.mdl"))
		return true;
	if (!strcmp (mod->name, "progs/flame2.mdl"))
		return true;
	if (!strcmp (mod->name, "progs/s_light.spr"))
		return true;
	return false;
}

/*
===============
DynLight_PushEntity
===============
*/
static void DynLight_PushEntity (entity_t *ent, int key)
{
	dlight_t	*dl;

	dl = CL_AllocDlight (key);
	VectorCopy (ent->origin, dl->origin);
	dl->origin[2] += 8.0f;
	dl->noflash = 1;
	dl->intensity = r_dynlights_intensity.value;
	dl->radius = 160.0f * (r_dynlights_radius.value / 160.0f);
	if (dl->radius < 40.0f)
		dl->radius = 40.0f;
	dl->die = cl.time + 0.1;
	dl->decay = 0;
	dl->minlight = 0;
	dl->color[0] = 1.0f;
	dl->color[1] = 0.55f;
	dl->color[2] = 0.15f;
}

/*
===============
DynLight_PushModels

Flame / globe models among static and networked entities.
===============
*/
static void DynLight_PushModels (void)
{
	int		i;
	entity_t	*ent;
	int		n;

	/* static entities (torches often live here) */
	n = cl.num_statics;
	if (n > MAX_STATIC_ENTITIES)
		n = MAX_STATIC_ENTITIES;
	for (i = 0; i < n; i++)
	{
		ent = &cl_static_entities[i];
		if (!DynLight_IsFlameModel (ent->model))
			continue;
		DynLight_PushEntity (ent, DYNLIGHT_MODEL_KEY_BASE + i);
	}

	/* dynamic entities */
	n = cl.num_entities;
	if (n > MAX_EDICTS)
		n = MAX_EDICTS;
	for (i = 1; i < n; i++)
	{
		ent = &cl_entities[i];
		if (!DynLight_IsFlameModel (ent->model))
			continue;
		DynLight_PushEntity (ent, DYNLIGHT_MODEL_KEY_BASE + MAX_STATIC_ENTITIES + i);
	}
}

/*
===============
DynLight_PushNearestProbes

Activate up to MAX_ACTIVE_MAP_LIGHTS probes nearest to the view origin.
===============
*/
static void DynLight_PushNearestProbes (void)
{
	int		i, j, k;
	int		best[MAX_ACTIVE_MAP_LIGHTS];
	float		bestd[MAX_ACTIVE_MAP_LIGHTS];
	float		dx, dy, dz, d2;
	float		scale;
	int		nactive;
	dlight_t	*dl;
	dynlight_probe_t *p;

	if (r_num_dyn_probes <= 0)
		return;

	for (i = 0; i < MAX_ACTIVE_MAP_LIGHTS; i++)
	{
		best[i] = -1;
		bestd[i] = 1e30f;
	}

	for (i = 0; i < r_num_dyn_probes; i++)
	{
		p = &r_dyn_probes[i];
		dx = p->origin[0] - r_refdef.vieworg[0];
		dy = p->origin[1] - r_refdef.vieworg[1];
		dz = p->origin[2] - r_refdef.vieworg[2];
		d2 = dx*dx + dy*dy + dz*dz;

		/* insert into sorted best[] if closer than worst */
		if (d2 >= bestd[MAX_ACTIVE_MAP_LIGHTS - 1])
			continue;
		for (j = 0; j < MAX_ACTIVE_MAP_LIGHTS; j++)
		{
			if (d2 < bestd[j])
			{
				for (k = MAX_ACTIVE_MAP_LIGHTS - 1; k > j; k--)
				{
					bestd[k] = bestd[k - 1];
					best[k] = best[k - 1];
				}
				bestd[j] = d2;
				best[j] = i;
				break;
			}
		}
	}

	scale = r_dynlights_radius.value / 160.0f;
	if (scale < 0.1f)
		scale = 0.1f;

	nactive = 0;
	for (i = 0; i < MAX_ACTIVE_MAP_LIGHTS; i++)
	{
		if (best[i] < 0)
			continue;
		p = &r_dyn_probes[best[i]];
		dl = CL_AllocDlight (DYNLIGHT_KEY_BASE + i);
		VectorCopy (p->origin, dl->origin);
		dl->radius = p->radius * scale;
		if (dl->radius < 48.0f)
			dl->radius = 48.0f;
		if (dl->radius > 280.0f)
			dl->radius = 280.0f;
		dl->die = cl.time + 0.1;
		dl->decay = 0;
		dl->minlight = 0;
		dl->color[0] = p->color[0];
		dl->color[1] = p->color[1];
		dl->color[2] = p->color[2];
		dl->intensity = r_dynlights_intensity.value;
		dl->noflash = 1;	/* surface pools only — no onion-ring discs */
		nactive++;
	}

	(void)nactive;
}

/*
===============
R_PushMapDynLights

Call each frame before R_PushDlights.
===============
*/
void R_PushMapDynLights (void)
{
	if (!r_dynlights.value)
		return;
	if (!cl.worldmodel)
		return;

	if (r_dynlights_map.value)
		DynLight_PushNearestProbes ();

	if (r_dynlights_models.value)
		DynLight_PushModels ();
}
