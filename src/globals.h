/*
 * Copyright 2017, Andrej Kislovskij
 *
 * This is PUBLIC DOMAIN software so use at your own risk as it comes
 * with no warranties. This code is yours to share, use and modify without
 * any restrictions or obligations.
 *
 * For more information see conwrap/LICENSE or refer refer to http://unlicense.org
 *
 * Author: gimesketvirtadieni at gmail dot com (Andrej Kislovskij)
 */

#include <stdlib.h>

#include "pcm_slimplexor.h"


const unsigned int supported_accesses[] =
{
	SND_PCM_ACCESS_RW_INTERLEAVED
};


const unsigned int supported_formats[] =
{
	SND_PCM_FORMAT_S8,
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_S24_LE,
	SND_PCM_FORMAT_S32_LE,
};


const unsigned int supported_channels[] =
{
	2
};


const unsigned int supported_rates[] =
{
	8000,
	11025,
	12000,
	16000,
	22500,
	24000,
	32000,
	44100,
	48000,
	88200,
	96000,
	176400,
	192000
};


const snd_pcm_ioplug_callback_t callbacks = {
	.start     = callback_start,
	.stop      = callback_stop,
	.pointer   = callback_pointer,
	.close     = callback_close,
	.hw_params = callback_hw_params,
	.sw_params = callback_sw_params,
	.prepare   = callback_prepare,
	.transfer  = callback_transfer,
};
