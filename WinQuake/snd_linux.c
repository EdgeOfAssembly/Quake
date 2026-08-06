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
/*
 * Linux sound: ALSA write backend (replaces OSS /dev/dsp mmap).
 *
 * Quake mixes into a power-of-two ring buffer (shm->buffer). We push
 * newly painted frames to ALSA with snd_pcm_writei and report play
 * position via snd_pcm_delay.
 *
 * Device: $AUDIODEV, else $ALSA_DEVICE, else "default".
 * Overrides: -sndspeed, -sndbits, -sndmono / -sndstereo,
 *            QUAKE_SOUND_SPEED / SAMPLEBITS / CHANNELS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "quakedef.h"

static snd_pcm_t	*pcm;
static int		snd_inited;
static int64_t		samples_written;	/* mono samples submitted to ALSA */
static int		frame_bytes;		/* bytes per ALSA frame (all channels) */

static int tryrates[] = { 44100, 48000, 22050, 11025, 8000 };

static const char *SND_DeviceName (void)
{
	const char	*d;

	d = getenv("AUDIODEV");
	if (d && d[0])
		return d;
	d = getenv("ALSA_DEVICE");
	if (d && d[0])
		return d;
	return "default";
}

static void SND_Recover (int err)
{
	if (err == -EPIPE)
	{
		/* underrun */
		snd_pcm_prepare(pcm);
	}
	else if (err == -ESTRPIPE)
	{
		while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
			usleep(1000);
		if (err < 0)
			snd_pcm_prepare(pcm);
	}
}

qboolean SNDDMA_Init (void)
{
	const char		*device;
	int			err;
	int			i;
	char			*s;
	unsigned int		rate;
	unsigned int		channels;
	snd_pcm_uframes_t	buffer_frames;
	snd_pcm_uframes_t	period_frames;
	snd_pcm_hw_params_t	*hw;
	int			bits;
	size_t			buf_bytes;

	snd_inited = 0;
	pcm = NULL;
	samples_written = 0;

	device = SND_DeviceName();
	err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0)
	{
		Con_Printf("ALSA: open '%s' failed: %s\n", device, snd_strerror(err));
		return false;
	}

	/* ---- format preferences (same knobs as old OSS path) ---- */
	bits = 16;
	s = getenv("QUAKE_SOUND_SAMPLEBITS");
	if (s)
		bits = atoi(s);
	else if ((i = COM_CheckParm("-sndbits")) != 0 && i + 1 < com_argc)
		bits = atoi(com_argv[i + 1]);
	if (bits != 8 && bits != 16)
		bits = 16;

	channels = 2;
	s = getenv("QUAKE_SOUND_CHANNELS");
	if (s)
		channels = (unsigned)atoi(s);
	else if (COM_CheckParm("-sndmono"))
		channels = 1;
	else if (COM_CheckParm("-sndstereo"))
		channels = 2;

	rate = 0;
	s = getenv("QUAKE_SOUND_SPEED");
	if (s)
		rate = (unsigned)atoi(s);
	else if ((i = COM_CheckParm("-sndspeed")) != 0 && i + 1 < com_argc)
		rate = (unsigned)atoi(com_argv[i + 1]);

	snd_pcm_hw_params_alloca(&hw);
	err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0)
	{
		Con_Printf("ALSA: hw_params_any: %s\n", snd_strerror(err));
		goto fail;
	}

	err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
	{
		Con_Printf("ALSA: set_access: %s\n", snd_strerror(err));
		goto fail;
	}

	if (bits == 8)
		err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_U8);
	else
		err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	if (err < 0)
	{
		/* fall back to S16 if U8 missing */
		bits = 16;
		err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
		if (err < 0)
		{
			Con_Printf("ALSA: set_format: %s\n", snd_strerror(err));
			goto fail;
		}
	}

	err = snd_pcm_hw_params_set_channels_near(pcm, hw, &channels);
	if (err < 0)
	{
		Con_Printf("ALSA: set_channels: %s\n", snd_strerror(err));
		goto fail;
	}

	if (rate == 0)
	{
		for (i = 0; i < (int)(sizeof(tryrates) / sizeof(tryrates[0])); i++)
		{
			rate = (unsigned)tryrates[i];
			if (snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0) >= 0)
				break;
		}
	}
	else
	{
		err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
		if (err < 0)
		{
			Con_Printf("ALSA: set_rate %u: %s\n", rate, snd_strerror(err));
			goto fail;
		}
	}

	/*
	 * Ring size must be power-of-two in mono samples (snd_mix masks).
	 * Aim ~0.25 s at the chosen rate, then round up to pow2 frames * ch.
	 */
	buffer_frames = (snd_pcm_uframes_t)(rate / 4);
	if (buffer_frames < 1024)
		buffer_frames = 1024;
	/* next power of two */
	{
		snd_pcm_uframes_t	p = 1;
		while (p < buffer_frames)
			p <<= 1;
		buffer_frames = p;
	}
	period_frames = buffer_frames / 8;
	if (period_frames < 64)
		period_frames = 64;

	err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_frames, 0);
	if (err < 0)
	{
		Con_Printf("ALSA: set_period_size: %s\n", snd_strerror(err));
		goto fail;
	}
	err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_frames);
	if (err < 0)
	{
		Con_Printf("ALSA: set_buffer_size: %s\n", snd_strerror(err));
		goto fail;
	}

	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0)
	{
		Con_Printf("ALSA: hw_params: %s\n", snd_strerror(err));
		goto fail;
	}

	/* Read back negotiated geometry */
	snd_pcm_hw_params_get_rate(hw, &rate, 0);
	snd_pcm_hw_params_get_channels(hw, &channels);
	snd_pcm_hw_params_get_buffer_size(hw, &buffer_frames);
	snd_pcm_hw_params_get_period_size(hw, &period_frames, 0);

	/*
	 * Quake ring: mono sample count must be power of two.
	 * Use 2x ALSA buffer (in frames) * channels, rounded to pow2.
	 */
	{
		int	want = (int)buffer_frames * 2 * (int)channels;
		int	p = 1;
		while (p < want)
			p <<= 1;
		want = p;

		shm = &sn;
		shm->splitbuffer = 0;
		shm->samplebits = bits;
		shm->channels = (int)channels;
		shm->speed = (int)rate;
		shm->samples = want;
		shm->samplepos = 0;
		shm->submission_chunk = 1;

		buf_bytes = (size_t)shm->samples * (size_t)(shm->samplebits / 8);
		shm->buffer = (unsigned char *)calloc(1, buf_bytes);
		if (!shm->buffer)
		{
			Con_Printf("ALSA: out of memory for %zu byte ring\n", buf_bytes);
			goto fail;
		}
	}

	frame_bytes = shm->channels * (shm->samplebits / 8);

	err = snd_pcm_prepare(pcm);
	if (err < 0)
	{
		Con_Printf("ALSA: prepare: %s\n", snd_strerror(err));
		goto fail;
	}

	snd_inited = 1;
	Con_Printf("ALSA: %s  %d Hz, %d bit, %s, ring %d samples (period %lu frames)\n",
		device, shm->speed, shm->samplebits,
		shm->channels == 1 ? "mono" : "stereo",
		shm->samples, (unsigned long)period_frames);
	return true;

fail:
	if (pcm)
	{
		snd_pcm_close(pcm);
		pcm = NULL;
	}
	if (shm && shm->buffer)
	{
		free(shm->buffer);
		shm->buffer = NULL;
	}
	return false;
}

int SNDDMA_GetDMAPos (void)
{
	snd_pcm_sframes_t	delay = 0;
	int64_t			played_frames;
	int			pos;

	if (!snd_inited || !pcm)
		return 0;

	if (snd_pcm_delay(pcm, &delay) < 0)
		delay = 0;
	if (delay < 0)
		delay = 0;

	/* samples_written is mono samples; delay is frames still in device */
	played_frames = (samples_written / shm->channels) - (int64_t)delay;
	if (played_frames < 0)
		played_frames = 0;

	pos = (int)((played_frames * shm->channels) % (int64_t)shm->samples);
	shm->samplepos = pos;
	return pos;
}

void SNDDMA_Shutdown (void)
{
	if (pcm)
	{
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
		pcm = NULL;
	}
	if (shm && shm->buffer)
	{
		free(shm->buffer);
		shm->buffer = NULL;
	}
	snd_inited = 0;
	samples_written = 0;
}

/*
==============
SNDDMA_Submit

Push painted samples that have not yet been written to ALSA.
paintedtime is in sample pairs (stereo frames); see snd_dma.c.
==============
*/
void SNDDMA_Submit (void)
{
	int		samplebits_bytes;
	int		ring_bytes;
	int64_t		painted_mono;
	int64_t		to_write_mono;
	int		frames;
	int		err;
	unsigned char	*src;
	int		idx;
	int		chunk_frames;
	int		chunk_mono;

	if (!snd_inited || !pcm || !shm || !shm->buffer)
		return;

	samplebits_bytes = shm->samplebits / 8;
	ring_bytes = shm->samples * samplebits_bytes;

	/* paintedtime = sample pairs; convert to mono sample count */
	painted_mono = (int64_t)paintedtime * (int64_t)shm->channels;
	to_write_mono = painted_mono - samples_written;
	if (to_write_mono <= 0)
		return;

	/* Don't try to write more than one ring at once */
	if (to_write_mono > shm->samples)
		to_write_mono = shm->samples;

	/* Align to whole frames */
	to_write_mono -= to_write_mono % shm->channels;
	if (to_write_mono <= 0)
		return;

	frames = (int)(to_write_mono / shm->channels);

	while (frames > 0)
	{
		idx = (int)(samples_written % (int64_t)shm->samples);
		/* Contiguous bytes until end of ring */
		chunk_mono = shm->samples - idx;
		if (chunk_mono > frames * shm->channels)
			chunk_mono = frames * shm->channels;
		chunk_mono -= chunk_mono % shm->channels;
		if (chunk_mono <= 0)
			break;
		chunk_frames = chunk_mono / shm->channels;

		src = shm->buffer + idx * samplebits_bytes;

		err = (int)snd_pcm_writei(pcm, src, (snd_pcm_uframes_t)chunk_frames);
		if (err == -EAGAIN)
		{
			/* device full — try again next frame */
			break;
		}
		if (err < 0)
		{
			SND_Recover(err);
			err = (int)snd_pcm_writei(pcm, src, (snd_pcm_uframes_t)chunk_frames);
			if (err < 0)
			{
				Con_DPrintf("ALSA write: %s\n", snd_strerror(err));
				break;
			}
		}

		samples_written += (int64_t)err * shm->channels;
		frames -= err;
		(void)ring_bytes;
	}
}
