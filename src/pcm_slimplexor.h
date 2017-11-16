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

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <stddef.h>  /* size_t */


/* TODO: logging level from conf */
#define DBG(fmt, arg...)  /* TODO: parametrize */ printf("DEBUG: %s: "   fmt "\n" , __FUNCTION__ , ## arg)
#define ERR(fmt, arg...)  printf("ERROR: %s: "   fmt "\n" , __FUNCTION__ , ## arg)
#define WRN(fmt, arg...)  printf("WARNING: %s: " fmt "\n" , __FUNCTION__ , ## arg)
#define ARRAY_SIZE(a)     (sizeof(a)/sizeof((a)[0]))
#define TARGET_FORMAT     SND_PCM_FORMAT_S32_LE
/* TODO: use latency in ms instead derived from conf */
#define PERIOD_SIZE_BYTES (1024 * 8)
#define PERIODS           2


typedef struct rate_device_map
{
    unsigned int rate;
    char*        device;
} rate_device_map_t;


typedef struct plugin_data
{
	snd_pcm_ioplug_t   alsa_data;
	unsigned int       rate_device_map_size;
	rate_device_map_t* rate_device_map;
	snd_pcm_sframes_t  pointer;
	snd_pcm_format_t   format;
	char*              target_device;
	snd_pcm_t*         target_pcm;
	unsigned int       target_channels;
	unsigned int       target_format;
	snd_pcm_uframes_t  target_period_size;
	unsigned int       target_periods;
	unsigned char*     target_buffer;
	snd_pcm_uframes_t  target_buffer_size;
	snd_pcm_uframes_t  target_buffer_current;
} plugin_data_t;


static int               callback_close(snd_pcm_ioplug_t *io);
static int               callback_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params);
static snd_pcm_sframes_t callback_pointer(snd_pcm_ioplug_t *io);
static int               callback_prepare(snd_pcm_ioplug_t *io);
static int               callback_start(snd_pcm_ioplug_t *io);
static int               callback_stop(snd_pcm_ioplug_t *io);
static int               callback_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params);
static snd_pcm_sframes_t callback_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t size);
static void              copy_frames(plugin_data_t* plugin_data, unsigned char* pcm_data, snd_pcm_uframes_t frames);
static void              release_resources(plugin_data_t* plugin_data);
static int               setup_hw_params(snd_pcm_ioplug_t *io);
static int               setup_target_hw_params(plugin_data_t* plugin_data, snd_pcm_hw_params_t *params);
static int               setup_target_sw_params(plugin_data_t* plugin_data, snd_pcm_sw_params_t *params);
static snd_pcm_sframes_t write_to_target(plugin_data_t* plugin_data);


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
	5512,
	8000,
	11025,
	16000,
	22050,
	32000,
	44100,
	48000,
	64000,
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
