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

#ifndef SLIMPLEXOR_H
#define SLIMPLEXOR_H

/* required for ALSA plugin */
#define PIC

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <stddef.h>  /* size_t */
#include <stdio.h>


/* defined in slimplexor.c */
extern unsigned int log_level;
extern FILE*        log_file;
extern FILE*        pcm_dump_file;
extern char         pcm_dump_file_name[4096];

#define LOG_DEBUG(fmt, arg...)     if (log_level >= 4) fprintf(log_file, "D, %s, " fmt "\n", __FUNCTION__ , ## arg)
#define LOG_INFO(fmt, arg...)      if (log_level >= 3) fprintf(log_file, "I, %s, " fmt "\n", __FUNCTION__ , ## arg)
#define LOG_WARNING(fmt, arg...)   if (log_level >= 2) fprintf(log_file, "W, %s, " fmt "\n", __FUNCTION__ , ## arg)
#define LOG_ERROR(fmt, arg...)     if (log_level >= 1) fprintf(log_file, "E, %s, " fmt "\n", __FUNCTION__ , ## arg)

#define ARRAY_SIZE(a)              (sizeof(a)/sizeof((a)[0]))
#define TARGET_FORMAT              SND_PCM_FORMAT_S32_LE
#define PERIOD_SIZE_BYTES          16384  /* one period size = 16K bytes */
#define PERIODS                    8      /* buffer size 16K * 8 = 128K bytes */
#define BEGINNING_OF_STREAM_MARKER 1
#define END_OF_STREAM_MARKER       2
#define DATA_MARKER                3


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
    snd_pcm_format_t   src_format;
    char*              dst_device;
    snd_pcm_t*         dst_pcm_handle;
    unsigned int       dst_channels;
    unsigned int       dst_format;
    snd_pcm_uframes_t  dst_period_size;
    unsigned int       dst_periods;
    unsigned char*     dst_buffer;
    snd_pcm_uframes_t  dst_buffer_size;
    snd_pcm_uframes_t  dst_buffer_current;
    unsigned short     transfer_started;
} plugin_data_t;


int               open_destination_device(plugin_data_t* plugin_data);
void              close_destination_device(plugin_data_t* plugin_data);
void              copy_frames(plugin_data_t* plugin_data, unsigned char* pcm_data, snd_pcm_uframes_t frames);
void              copy_sample(plugin_data_t* plugin_data, unsigned char* source_sample, size_t source_sample_size, unsigned char* target_sample);
const char*       log_level_to_string();
int               set_src_hw_params(snd_pcm_ioplug_t *io);
int               set_dst_hw_params(plugin_data_t* plugin_data, snd_pcm_hw_params_t *params);
int               set_dst_sw_params(plugin_data_t* plugin_data, snd_pcm_sw_params_t *params);
void              write_stream_marker(plugin_data_t* plugin_data, unsigned char marker);
snd_pcm_sframes_t write_to_dst(plugin_data_t* plugin_data);


#endif  /* SLIMPLEXOR_H */
