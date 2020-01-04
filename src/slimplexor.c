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

#include <unistd.h>  /* fsync(...) */
#include "slimplexor.h"


/* define the default logging level (0 - NONE, 1 - ERROR, 2 - WARNING, 3 - INFO, 4 - DEBUG) */
unsigned int log_level          = 3;
FILE*        log_file           = NULL;
FILE*        pcm_dump_file      = NULL;
char*        pcm_dump_file_name = NULL;


static int callback_close(snd_pcm_ioplug_t *io)
{
    LOG_DEBUG("Close stream callback was invoked");

    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    if (plugin_data->dst_pcm_handle)
    {
        /* if there PCM data transfer was actually started then marking the end of stream and draining buffer */
        if (plugin_data->transfer_started)
        {
            write_stream_marker(plugin_data, END_OF_STREAM_MARKER);

            int tmp;
            if ((tmp = snd_pcm_drain(plugin_data->dst_pcm_handle)) < 0)
            {
                LOG_WARNING("Error while draining target device: %s", snd_strerror(tmp));
            }
            plugin_data->transfer_started = 0;
        }

        /* closing destination device which will release relevant resources */
        close_destination_device(plugin_data);
    }

    /* log file is flushed instead of closing it to be able to log in case of multiple calls to ALSA close routine */
    fsync(fileno(log_file));

    /* close routine may not fail */
    return 0;
}


static int callback_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    LOG_DEBUG("HW parameters setup callback was invoked");

    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    /* closing the target device first to allow multiple calls to snd_pcm_hw_params / snd_pcm_prepare */
    close_destination_device(plugin_data);

    /* setting up target device hardware parameters */
    return set_dst_hw_params(plugin_data, params);
}


static snd_pcm_sframes_t callback_pointer(snd_pcm_ioplug_t *io)
{
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    LOG_DEBUG("Pointer change callback was called (pointer=%ld, buffer size=%ld)", plugin_data->pointer, io->buffer_size);

    plugin_data->pointer %= io->buffer_size;

    return plugin_data->pointer;
}


static int callback_prepare(snd_pcm_ioplug_t *io)
{
    LOG_DEBUG("Prepare processing callback was invoked");

    int            error       = 0;
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    /* resetting hw buffer pointer */
    plugin_data->pointer = 0;

    if (snd_pcm_state(plugin_data->dst_pcm_handle) > SND_PCM_STATE_PREPARED)
    {
        return error;
    }

    /* preparing target device and starting playback */
    if ((error = snd_pcm_prepare(plugin_data->dst_pcm_handle)) < 0)
    {
        LOG_ERROR("Error while preparing destination device: %s", snd_strerror(error));
    }

    return error;
}


static int callback_start(snd_pcm_ioplug_t *io)
{
    LOG_DEBUG("Start processing callback was invoked");

    return 0;
}


static int callback_stop(snd_pcm_ioplug_t *io)
{
    LOG_DEBUG("Stop processing callback was invoked");

    return 0;
}


static int callback_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
    LOG_DEBUG("SW parameters setup callback was invoked");

    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    return set_dst_sw_params(plugin_data, params);
}


static snd_pcm_sframes_t callback_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames_provided)
{
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;
    unsigned char* pcm_data    = (unsigned char*)areas->addr + (areas->first >> 3) + ((areas->step * offset) >> 3);

    LOG_DEBUG("Data transfer callback was invoked (offset=%lu, frames provided=%lu, frames already present=%ld)", offset, frames_provided, plugin_data->dst_buffer_current);

    /* if this is the first time transfer is called then marking the biginning of PCM stream */
    if (!plugin_data->transfer_started)
    {
        write_stream_marker(plugin_data, BEGINNING_OF_STREAM_MARKER);
        plugin_data->transfer_started = 1;
    }

    /* it's ok to process less frames than provided as ALSA will call this callback with the rest of data */
    /* adjusting amount of frames to be processed, which is max(available,provided) */
    snd_pcm_uframes_t available_size     = plugin_data->dst_buffer_size - plugin_data->dst_buffer_current;
    snd_pcm_uframes_t frames_processable = frames_provided;
    if (available_size < frames_provided)
    {
        LOG_DEBUG("More frames provided than buffer available (frames provided=%lu, available buffer size=%ld)", frames_provided, available_size);
        frames_processable = available_size;
    }

    /* copying frames from the source buffer to the target buffer */
    copy_frames(plugin_data, pcm_data, frames_processable);

    /* writting to the target device */
    snd_pcm_sframes_t result = write_to_dst(plugin_data);
    if (result < 0)
    {
        LOG_ERROR("Error while writting to target device: %s", snd_strerror(result));
    }
    else if (result < frames_processable)
    {
        LOG_WARNING("Less frames were written to the target device than expected (written frames=%ld, expected to write frames=%ld)", result, frames_processable);
    }

    /* frames are 'consumed' as long as they were coppied to the transfer buffer, even though some are still pending for delivery */
    return frames_processable;
}


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


SND_PCM_PLUGIN_DEFINE_FUNC(slimplexor)
{
    int                   error          = 0;
    plugin_data_t*        plugin_data    = NULL;
    unsigned short        plugin_created = 0;
    snd_config_iterator_t i;
    snd_config_iterator_t next;
    const char*           log_level_name;
    const char*           log_file_name;
    int                   log_file_open_error = 0;

    snd_config_for_each(i, next, conf)
    {
        snd_config_t* n = snd_config_iterator_entry(i);
        const char*   id;

        if (snd_config_get_id(n, &id) < 0)
        {
            continue;
        }

        if (strcasecmp(id, "comment") == 0 || strcasecmp(id, "type") == 0)
        {
            continue;
        }

        /* setting logging level */
        if (strcasecmp(id, "log_level") == 0)
        {
            if (snd_config_get_string(n, &log_level_name) < 0)
            {
                continue;
            }

            if (strcasecmp(log_level_name, "none") == 0)
            {
                log_level = 0;
            }
            else if (strcasecmp(log_level_name, "error") == 0)
            {
                log_level = 1;
            }
            else if (strcasecmp(log_level_name, "warning") == 0)
            {
                log_level = 2;
            }
            else if (strcasecmp(log_level_name, "info") == 0)
            {
                log_level = 3;
            }
            else if (strcasecmp(log_level_name, "debug") == 0)
            {
                log_level = 4;
            }

            continue;
        }

        /* setting log file */
        if (strcasecmp(id, "log_file") == 0)
        {
            if (snd_config_get_string(n, &log_file_name) < 0)
            {
                continue;
            }

            if (strcasecmp(log_file_name, "stdout") == 0)
            {
                log_file = stdout;
            }
            else if (strcasecmp(log_file_name, "stderr") == 0)
            {
                log_file = stderr;
            }
            else
            {
                /* log file is never closed, which allows support of multiple calls to ALSA close routine */
                log_file = fopen(log_file_name, "a");
                if (!log_file)
                {
                    log_file_open_error = errno;
                }
            }
        }

        /* setting PCM dump file if provided */
        if (strcasecmp(id, "pcm_dump_file") == 0)
        {
            const char* str;
            if (snd_config_get_string(n, &str) < 0)
            {
                continue;
            }

            /* allocated memory here will be released when plugin is unloaded */
            pcm_dump_file_name = calloc(1, stdlen(str) + 1);
            if (!pcm_dump_file_name)
            {
                error = -ENOMEM;
                LOG_ERROR("Could not allocate memory for dump file name (requested %lu bytes)", stdlen(str) + 1);
                break;
            }
            strcpy(pcm_dump_file_name, str);
        }
    }

    /* making sure log_file is always initialized */
    if (!log_file)
    {
        log_file = stdout;
    }

    if (!error)
    {
        LOG_INFO("-----------------------------------");
        LOG_INFO("Loading SlimPlexor v0.1.0 plugin...");
        if (log_level)
        {
            LOG_INFO("Logging level is %s", log_level_to_string(log_level));
            if (!log_file_open_error)
            {
                LOG_INFO("Logging destination is %s", log_file_name);
            }
            else
            {
                LOG_ERROR("Could not open log file defined in configuration, using stdout instead (error=%s, provided file name=%s)", strerror(errno), log_file_name);
            }
        }
        else
        {
            LOG_INFO("Logging is disabled");
        }

        if (strlen(pcm_dump_file_name))
        {
            LOG_INFO("PCM dump file name is %s", pcm_dump_file_name);
        }
        else
        {
            LOG_INFO("PCM dump file is not used");
        }
    }

    /* allocating memory for plugin data structure and initializing it with zeros */
    if (!error)
    {
        plugin_data = calloc(1, sizeof(plugin_data_t));
        if (!plugin_data)
        {
            error = -ENOMEM;
            LOG_ERROR("Could not allocate memory for plugin data (requested %lu bytes)", sizeof(plugin_data_t));
        }
    }

    /* initializing plugin data structure */
    if (!error)
    {
        plugin_data->alsa_data.version      = SND_PCM_IOPLUG_VERSION;
        plugin_data->alsa_data.name         = "SlimPlexor - An ALSA plugin used by SlimStreamer";
        plugin_data->alsa_data.callback     = &callbacks;
        plugin_data->alsa_data.private_data = plugin_data;

        /* useing a fixed format while writing to the target device */
        plugin_data->dst_format = TARGET_FORMAT;

        /* TODO: should come from config */
        plugin_data->rate_device_map_size = 13;

        /* allocating memory for rate->device data structure */
        plugin_data->rate_device_map = calloc(plugin_data->rate_device_map_size, sizeof(rate_device_map_t));
        if (!plugin_data->rate_device_map)
        {
            error = -ENOMEM;
            LOG_ERROR("Could not allocate memory for sampling rate mapping (requested %lu bytes)", sizeof(rate_device_map_t));
        }
    }

    /* TODO: should come from config */
    /* initializing rate->device map data structure */
    if (!error)
    {
        plugin_data->rate_device_map[0].rate    = 8000;
        plugin_data->rate_device_map[0].device  = "hw:1,0,1";
        plugin_data->rate_device_map[1].rate    = 11025;
        plugin_data->rate_device_map[1].device  = "hw:1,0,2";
        plugin_data->rate_device_map[2].rate    = 12000;
        plugin_data->rate_device_map[2].device  = "hw:1,0,3";
        plugin_data->rate_device_map[3].rate    = 16000;
        plugin_data->rate_device_map[3].device  = "hw:1,0,4";
        plugin_data->rate_device_map[4].rate    = 22500;
        plugin_data->rate_device_map[4].device  = "hw:1,0,5";
        plugin_data->rate_device_map[5].rate    = 24000;
        plugin_data->rate_device_map[5].device  = "hw:1,0,6";
        plugin_data->rate_device_map[6].rate    = 32000;
        plugin_data->rate_device_map[6].device  = "hw:1,0,7";
        plugin_data->rate_device_map[7].rate    = 44100;
        plugin_data->rate_device_map[7].device  = "hw:2,0,1";
        plugin_data->rate_device_map[8].rate    = 48000;
        plugin_data->rate_device_map[8].device  = "hw:2,0,2";
        plugin_data->rate_device_map[9].rate    = 88200;
        plugin_data->rate_device_map[9].device  = "hw:2,0,3";
        plugin_data->rate_device_map[10].rate   = 96000;
        plugin_data->rate_device_map[10].device = "hw:2,0,4";
        plugin_data->rate_device_map[11].rate   = 176400;
        plugin_data->rate_device_map[11].device = "hw:2,0,5";
        plugin_data->rate_device_map[12].rate   = 192000;
        plugin_data->rate_device_map[12].device = "hw:2,0,6";
    }

    /* this hack is required to avoid ALSA mutex deadlocks; ALSA does not expose this functionality via API */
    if (!error)
    {
        /* TODO: should come from config */
        if (setenv("LIBASOUND_THREAD_SAFE", "0", 1) < 0)
        {
            error = -EPERM;
            LOG_ERROR("Could not disable thread-safety for ALSA library");
        }
    }

    /* creating ALSA plugin */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_create(&plugin_data->alsa_data, name, stream, mode)) < 0)
        {
            LOG_ERROR("Could not register plugin within ALSA: %s", snd_strerror(error));
        }
        else
        {
            plugin_created = 1;
        }
    }

    /* setting up hw parameters; error is logged by setup function */
    if (!error)
    {
        error = set_src_hw_params(&plugin_data->alsa_data);
    }

    if (!error)
    {
        /* this assignment must occur only in case when everything went fine */
        *pcmp = plugin_data->alsa_data.pcm;
        LOG_INFO("Plugin was loaded");
    }
    else
    {
        /* plugin was not created properly */
        if (!plugin_data && plugin_created)
        {
            snd_pcm_ioplug_delete(&plugin_data->alsa_data);
        }
        if (plugin_data->rate_device_map)
        {
            free(plugin_data->rate_device_map);
            plugin_data->rate_device_map = NULL;
        }
    }

    return error;
}
SND_PCM_PLUGIN_SYMBOL(slimplexor)
