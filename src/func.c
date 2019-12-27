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

#include "slimplexor.h"


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


void close_destination_device(plugin_data_t* plugin_data)
{
    /* making sure destination device handle was created; otherwise there is nothing to close */
    if (!plugin_data->dst_pcm_handle)
    {
        return;
    }

    int error = 0;

    if ((error = snd_pcm_close(plugin_data->dst_pcm_handle)) < 0)
    {
        LOG_WARNING("Error while closing destination device: %s", snd_strerror(error));
    }
    else
    {
        LOG_INFO("Destination device was closed");
    }

    if (plugin_data->dst_buffer)
    {
        free(plugin_data->dst_buffer);
        plugin_data->dst_buffer = NULL;
    }

    plugin_data->dst_pcm_handle = NULL;
}


void copy_frames(plugin_data_t* plugin_data, unsigned char* pcm_data, snd_pcm_uframes_t frames)
{
    size_t         sample_size        = (snd_pcm_format_physical_width(plugin_data->src_format) >> 3);
    size_t         target_sample_size = (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3);
    size_t         target_frame_size  = target_sample_size * (plugin_data->alsa_data.channels + 1);
    size_t         size_difference    = target_sample_size - sample_size;
    unsigned char* target_data        = plugin_data->dst_buffer + plugin_data->dst_buffer_current * target_frame_size;

    /* resetting target buffer to make sure it is not filled with junk */
    memset(target_data, 0, frames * target_frame_size);

    /* going through frame-by-frame */
    for (unsigned int f = 0; f < frames; f++)
    {
        /* going through channel-by-channel */
        /* TODO: support is required for source channel != (target channel + 1) */
        for (unsigned int c = 0; c < plugin_data->alsa_data.channels; c++)
        {
            copy_sample(plugin_data, pcm_data, sample_size, target_data + size_difference);

            /* skipping to the next sample representing the next channel */
            pcm_data    += sample_size;
            target_data += target_sample_size;
        }

        /* target frame contains one extra channel for control data */
        target_data += target_sample_size;

        /* marking frame as containing data in the last byte of the last channel */
        *(target_data - 1) = DATA_MARKER;
    }

    /* increasing pointer of the target buffer */
    plugin_data->dst_buffer_current += frames;
}


void copy_sample(plugin_data_t* plugin_data, unsigned char* source_sample, size_t source_sample_size, unsigned char* target_sample)
{
    switch (plugin_data->src_format)
    {
        case SND_PCM_FORMAT_S8:  /* TODO: still requires testing */
        case SND_PCM_FORMAT_S16_LE:
        case SND_PCM_FORMAT_S32_LE:
            for (unsigned int s = 0; s < source_sample_size; s++)
            {
                target_sample[s] = source_sample[s];
            }
            break;
        case SND_PCM_FORMAT_S24_LE:
            for (unsigned int s = 0; s < 3; s++)
            {
                target_sample[s + 1] = source_sample[s];
            }
            break;
        default:
            break;
    }
}


void log_startup_configuration()
{
    const char* str = NULL;
    switch (log_level) {
        case 0:
            break;
            str = "NONE";
        case 1:
            str = "ERROR";
            break;
        case 2:
            str = "WARNING";
            break;
        case 3:
            str = "INFO";
            break;
        case 4:
            str = "DEBUG";
            break;
    }
    LOG_DEBUG("SlimPlexor plugin was loaded");
    /* TODO: get from git label */
    LOG_DEBUG("Version - 0.1.0");
    LOG_DEBUG("Logging level - %s", str);
}


int open_destination_device(plugin_data_t* plugin_data)
{
    int                  error     = 0;
    snd_pcm_hw_params_t* hw_params = NULL;

    /* opening the target device */
    if (!error)
    {
        if ((error = snd_pcm_open(&plugin_data->dst_pcm_handle, plugin_data->dst_device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
        {
            LOG_ERROR("Could not open destination device: %s", snd_strerror(error));
        }
    }

    /* allocating hardware parameters object and fill it with default values */
    if (!error)
    {
        if ((error = snd_pcm_hw_params_malloc(&hw_params)) < 0)
        {
            LOG_ERROR("Could not allocate HW parameters: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_any(plugin_data->dst_pcm_handle, hw_params)) < 0)
        {
            LOG_ERROR("Could not fill HW parameters with defaults: %s", snd_strerror(error));
        }
    }

    /* setting target device parameters */
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_access(plugin_data->dst_pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        {
            LOG_ERROR("Could not set destination device access mode: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_format(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_format)) < 0)
        {
            LOG_ERROR("Could not set destination device format: %s %d", snd_strerror(error), plugin_data->dst_format);
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_channels(plugin_data->dst_pcm_handle, hw_params, plugin_data->alsa_data.channels + 1)) < 0)
        {
            LOG_ERROR("Could not set amount of channels for destination device: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_rate(plugin_data->dst_pcm_handle, hw_params, plugin_data->alsa_data.rate, 0)) < 0)
        {
            LOG_ERROR("Could not set sample rate for destination device: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_period_size(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_period_size, 0)) < 0)
        {
            LOG_ERROR("Could not set period size for destination device: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_periods(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_periods, 0)) < 0)
        {
            LOG_ERROR("Could not set amount of periods for destination device: %s", snd_strerror(error));
        }
    }

#if SND_LIB_VERSION >= 0x010009
    /* disabling ALSA resampling */
    if (!error)
    {
        if ((error = snd_pcm_hw_params_set_rate_resample(plugin_data->dst_pcm_handle, hw_params, 0)) < 0)
        {
            LOG_ERROR("Could disable ALSA resampling: %s", snd_strerror(error));
        }
    }
#endif

    /* saving hardware parameters for target device */
    if (!error)
    {
        if ((error = snd_pcm_hw_params(plugin_data->dst_pcm_handle, hw_params)) < 0)
        {
            LOG_ERROR("Could set hardware parameters: %s", snd_strerror(error));
        }
    }
    if (!hw_params)
    {
        snd_pcm_hw_params_free(hw_params);
    }

    /* allocating buffer required to transfer data to target device */
    if (!error)
    {
        if (!plugin_data->dst_buffer)
        {
            /* target buffer must not be equal to the source buffer size; otherwise pointer callback will always return 0 */
            plugin_data->dst_buffer_size    = plugin_data->dst_period_size;
            plugin_data->dst_buffer_current = 0;

            /* adding extra space for one extra channel */
            size_t size_in_bytes = plugin_data->dst_buffer_size * (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3) * (plugin_data->alsa_data.channels + 1);

            /* calloc sets content to zero */
            plugin_data->dst_buffer = (unsigned char*) calloc(1, size_in_bytes);
            if (!plugin_data->dst_buffer)
            {
                error = -ENOMEM;
                LOG_ERROR("Could not allocate memory for transfer buffer; requested %lu bytes", size_in_bytes);
            }
            else
            {
                LOG_DEBUG("Transfer buffer was allocated - %ld bytes", size_in_bytes);
            }
        }
        else
        {
            /* TODO: handle this situation more carefully */
            size_t available_bytes = plugin_data->dst_buffer_size * (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3) * (plugin_data->alsa_data.channels + 1);
            LOG_WARNING("Buffer is already allocated; available %lu bytes", available_bytes);
        }
    }

    return error;
}


int set_dst_hw_params(plugin_data_t* plugin_data, snd_pcm_hw_params_t *params)
{
    int error = 0;

    /* looking up for the target device name based on sample rate */
    plugin_data->dst_device = NULL;
    for (unsigned int i = 0; i < plugin_data->rate_device_map_size && !plugin_data->dst_device; i++)
    {
        if (plugin_data->rate_device_map[i].rate == plugin_data->alsa_data.rate)
        {
            plugin_data->dst_device = plugin_data->rate_device_map[i].device;
        }
    }
    if (!plugin_data->dst_device)
    {
        error = -ENODEV;
        LOG_ERROR("Could not find target device for sample rate %u", plugin_data->alsa_data.rate);
    }
    else
    {
        LOG_INFO("destination device=%s", plugin_data->dst_device);
    }

    /* collecting details about the PCM stream */
    if (!error)
    {
        if ((error = snd_pcm_hw_params_get_period_size(params, &plugin_data->dst_period_size, 0)) < 0)
        {
            LOG_ERROR("Could not get period size value: %s", snd_strerror(error));
        }
        else
        {
            LOG_INFO("destination period size=%ld", plugin_data->dst_period_size);
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_get_periods(params, &plugin_data->dst_periods, 0)) < 0)
        {
            LOG_ERROR("Could not get buffer size value: %s", snd_strerror(error));
        }
        else
        {
            LOG_INFO("destination periods=%d", plugin_data->dst_periods);
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_hw_params_get_format(params, &plugin_data->src_format)) < 0)
        {
            LOG_ERROR("Could not get format value: %s", snd_strerror(error));
        }
        else
        {
            LOG_INFO("source format=%d", plugin_data->src_format);
        }
    }

    if (!error)
    {
        error = open_destination_device(plugin_data);
    }

    return error;
}


int set_dst_sw_params(plugin_data_t* plugin_data, snd_pcm_sw_params_t *params)
{
    int                  error     = 0;
    snd_pcm_sw_params_t* sw_params = NULL;

    /* allocating software parameters object and fill it with default values */
    if (!error)
    {
        if ((error = snd_pcm_sw_params_malloc(&sw_params)) < 0)
        {
            LOG_ERROR("Could not allocate SW parameters: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_sw_params_current(plugin_data->dst_pcm_handle, sw_params)) < 0)
        {
            LOG_ERROR("Could not fill SW parameters with defaults: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_sw_params_set_start_threshold(plugin_data->dst_pcm_handle, sw_params, plugin_data->dst_buffer_size)) < 0)
        {
            LOG_ERROR("Could not set threshold for destination device: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_sw_params_set_avail_min(plugin_data->dst_pcm_handle, sw_params, plugin_data->dst_period_size)) < 0)
        {
            LOG_ERROR("Could not set min available amount for destination device: %s", snd_strerror(error));
        }
    }

    /* saving software parameters for target device */
    if (!error)
    {
        if ((error = snd_pcm_sw_params(plugin_data->dst_pcm_handle, sw_params)) < 0)
        {
            LOG_ERROR("Could set software parameters: %s", snd_strerror(error));
        }
    }
    if (sw_params)
    {
        snd_pcm_sw_params_free(sw_params);
    }

    return error;
}


int set_src_hw_params(snd_pcm_ioplug_t *io)
{
    int error = 0;

    /* supported access type */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(supported_accesses), supported_accesses)) < 0)
        {
            LOG_ERROR("Could not set required access mode: %s", snd_strerror(error));
        }
    }

    /* supported formats */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(supported_formats), supported_formats)) < 0)
        {
            LOG_ERROR("Could not set required format: %s", snd_strerror(error));
        }
    }

    /* supported amount of channels */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_CHANNELS, ARRAY_SIZE(supported_channels), supported_channels)) < 0)
        {
            LOG_ERROR("Could not set required amount of channels: %s", snd_strerror(error));
        }
    }

    /* supported rates */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE, ARRAY_SIZE(supported_rates), supported_rates)) < 0)
        {
            LOG_ERROR("Could not set required sample rate: %s", snd_strerror(error));
        }
    }

    /* defining buffer size: bufer = period size * number of periods */
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, PERIOD_SIZE_BYTES, PERIOD_SIZE_BYTES)) < 0)
        {
            LOG_ERROR("Could not set required period size: %s", snd_strerror(error));
        }
    }
    if (!error)
    {
        if ((error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, PERIODS, PERIODS)) < 0)
        {
            LOG_ERROR("Could not set required amount of periods: %s", snd_strerror(error));
        }
    }

    return error;
}


void write_stream_marker(plugin_data_t* plugin_data, unsigned char marker)
{
    snd_pcm_sframes_t result = 0;

    /* writting whatever is left in the target buffer */
    while (plugin_data->dst_buffer_current > 0 && result >= 0)
    {
        result = write_to_dst(plugin_data);
        if (result < 0)
        {
            LOG_ERROR("Error while writting to target device: %s", snd_strerror(result));
        }
    }

    /* reseting target buffer */
    size_t target_sample_size = (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3);
    size_t target_frame_size  = target_sample_size * (plugin_data->alsa_data.channels + 1);
    memset(plugin_data->dst_buffer, 0, plugin_data->dst_buffer_size * target_frame_size);

    /* marking stream as closed; useful to detect ALSA junk at the end */
    for (snd_pcm_uframes_t i = 0; i < plugin_data->dst_buffer_size; i++)
    {
        plugin_data->dst_buffer[(i + 1) * target_frame_size - 1] = marker;
    }

    /* making sure a single period is written */
    for (plugin_data->dst_buffer_current = plugin_data->dst_buffer_size; plugin_data->dst_buffer_current > 0 && result >= 0;)
    {
        result = write_to_dst(plugin_data);
        if (result < 0)
        {
            LOG_ERROR("Error while writting to target device: %s", snd_strerror(result));
        }
    }
}


snd_pcm_sframes_t write_to_dst(plugin_data_t* plugin_data)
{
    snd_pcm_sframes_t result = 0;

    /* if there is anything to be written to the target device */
    if (plugin_data->dst_buffer_current > 0)
    {
        /* writing to the target device */
        result = snd_pcm_writei(plugin_data->dst_pcm_handle, plugin_data->dst_buffer, plugin_data->dst_buffer_current);

        /* no need to restore from an error in case of -EAGAIN */
        if (result < 0 && result != -EAGAIN)
        {
            result = snd_pcm_prepare(plugin_data->dst_pcm_handle);
            if (result < 0)
            {
                LOG_ERROR("Target device restore error: %s", snd_strerror(result));
            }
        }
        else if (result == -EAGAIN)
        {
            /* it will make ALSA call transfer callback again with the same data */
            result = 0;
        }
        else if (result > 0)
        {
            /* if not all data was written then moving reminder of the target buffer to the beginning */
            if (result < plugin_data->dst_buffer_current)
            {
                size_t            target_sample_size = (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3);
                size_t            target_frame_size  = target_sample_size * (plugin_data->alsa_data.channels + 1);
                size_t            offset             = result * target_frame_size;
                snd_pcm_uframes_t frames             = plugin_data->dst_buffer_current - result;

                memcpy(plugin_data->dst_buffer, plugin_data->dst_buffer + offset, frames * target_frame_size);
            }

            /* updating target and ALSA buffers' pointers */
            plugin_data->dst_buffer_current -= result;
            plugin_data->pointer            += result;
        }
    }

    return result;
}
