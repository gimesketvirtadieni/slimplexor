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
#include "globals.h"


static int callback_close(snd_pcm_ioplug_t *io)
{
    DBG("");

    int            error       = 0;
	plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    INF("HERE1.1");

    if (plugin_data->dst_pcm_handle)
	{
        INF("HERE1.2");

        write_stream_marker(plugin_data, END_OF_STREAM_MARKER);

		/* draining and closing destination stream even if there were any errors */
		if ((error = snd_pcm_drain(plugin_data->dst_pcm_handle)) < 0)
		{
			ERR("Error while draining target device: %s", snd_strerror(error));
		}

		INF("HERE1.3");

		/* closing destination device which will release relevant resources */
		close_destination_device(plugin_data);
    }

    INF("HERE1.4");

    return 0;
}


static int callback_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    DBG("");

    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	/* closing the target device first to allow multiple calls to snd_pcm_prepare */
	close_destination_device(plugin_data);

	/* setting up target device hardware parameters */
	return set_dst_hw_params(plugin_data, params);
}


static snd_pcm_sframes_t callback_pointer(snd_pcm_ioplug_t *io)
{
	plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	plugin_data->pointer %= io->buffer_size;

	/* DBG("pointer=%ld", plugin_data->pointer); */

	return plugin_data->pointer;
}


static int callback_prepare(snd_pcm_ioplug_t *io)
{
    DBG("");

    int            error       = 0;
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	/* resetting hw buffer pointer */
	plugin_data->pointer = 0;

	INF("HERE2.1 state=%d", snd_pcm_state(plugin_data->dst_pcm_handle));

	if (snd_pcm_state(plugin_data->dst_pcm_handle) > SND_PCM_STATE_PREPARED)
	{
        return error;
    }

    INF("HERE2.2 error=%d", error);

    /* preparing target device and starting playback */
    if ((error = snd_pcm_prepare(plugin_data->dst_pcm_handle)) < 0)
    {
        ERR("HERE2.3 error=%d", error);
    }

    if (!error)
    {
        INF("HERE2.4 error=%d", error);

        /* marking the biginning of PCM stream */
        write_stream_marker(plugin_data, BEGINNING_OF_STREAM_MARKER);
    }

    INF("HERE2.5 error=%d", error);

    return error;
}


static int callback_start(snd_pcm_ioplug_t *io)
{
	DBG("");

	return 0;
}


static int callback_stop(snd_pcm_ioplug_t *io)
{
	DBG("");

	return 0;
}


static int callback_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
    DBG("");

    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

    return set_dst_sw_params(plugin_data, params);
}


static snd_pcm_sframes_t callback_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames_avail)
{
	plugin_data_t*    plugin_data = (plugin_data_t*)io->private_data;
	snd_pcm_uframes_t frames      = plugin_data->dst_buffer_size - plugin_data->dst_buffer_current;
	unsigned char*    pcm_data    = (unsigned char*)areas->addr + (areas->first >> 3) + ((areas->step * offset) >> 3);

	/* adjusting amount of frames to be processed, which is max(available,provided) */
	if (frames > frames_avail)
	{
		/* it is ok to process less frames than provided as ALSA will call this callback with the rest of data */
		frames = frames_avail;
	}

	/* copying frames from the source buffer to the target buffer */
	copy_frames(plugin_data, pcm_data, frames);

	/* writting to the target device */
	snd_pcm_sframes_t result = write_to_dst(plugin_data);
	if (result < 0)
	{
		ERR("Error while writting to target device: %s", snd_strerror(result));
	}

	DBG("first=%u offset=%lu avail=%lu frames=%lu result=%ld", areas->first, offset, frames_avail, frames, result);

	return frames;
}


static void close_destination_device(plugin_data_t* plugin_data)
{
	INF("HERE3.1");

    /* making sure destination device handle was created; otherwise there is nothing to close */
	if (!plugin_data->dst_pcm_handle)
	{
		return;
	}

	int error = 0;

	INF("HERE3.2");

    if ((error = snd_pcm_close(plugin_data->dst_pcm_handle)) < 0)
	{
		WRN("Error while closing destination device: %s", snd_strerror(error));
	}

	INF("HERE3.3");

    if (plugin_data->dst_buffer)
	{
        INF("HERE3.4");

        free(plugin_data->dst_buffer);
		plugin_data->dst_buffer = NULL;
	}

	INF("HERE3.5");

    plugin_data->dst_pcm_handle = NULL;
}


static void copy_frames(plugin_data_t* plugin_data, unsigned char* pcm_data, snd_pcm_uframes_t frames)
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
			/* going through sample-by-sample */
			for (unsigned int s = 0; s < sample_size; s++)
			{
				target_data[s + size_difference] = pcm_data[s];
			}

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


static int open_destination_device(plugin_data_t* plugin_data)
{
	int                  error     = 0;
	snd_pcm_hw_params_t* hw_params = NULL;

    /* opening the target device */
    if (!error)
    {
        if ((error = snd_pcm_open(&plugin_data->dst_pcm_handle, plugin_data->dst_device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		{
			ERR("Could not open destination device: %s", snd_strerror(error));
		}
    }

    /* allocating hardware parameters object and fill it with default values */
    if (!error)
    {
    	if ((error = snd_pcm_hw_params_malloc(&hw_params)) < 0)
		{
			ERR("Could not allocate HW parameters: %s", snd_strerror(error));
		}
    }
    if (!error)
    {
    	if ((error = snd_pcm_hw_params_any(plugin_data->dst_pcm_handle, hw_params)) < 0)
		{
			ERR("Could not fill HW parameters with defaults: %s", snd_strerror(error));
		}
    }

    /* setting target device parameters */
    if (!error)
    {
		if ((error = snd_pcm_hw_params_set_access(plugin_data->dst_pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		{
			ERR("Could not set destination device access mode: %s", snd_strerror(error));
		}
    }
	if (!error)
	{
		if ((error = snd_pcm_hw_params_set_format(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_format)) < 0)
		{
			ERR("Could not set destination device format: %s %d", snd_strerror(error), plugin_data->dst_format);
		}
	}
	if (!error)
	{
		if ((error = snd_pcm_hw_params_set_channels(plugin_data->dst_pcm_handle, hw_params, plugin_data->alsa_data.channels + 1)) < 0)
		{
			ERR("Could not set amount of channels for destination device: %s", snd_strerror(error));
		}
	}
	if (!error)
	{
		if ((error = snd_pcm_hw_params_set_rate(plugin_data->dst_pcm_handle, hw_params, plugin_data->alsa_data.rate, 0)) < 0)
		{
			ERR("Could not set sample rate for destination device: %s", snd_strerror(error));
		}
	}
    if (!error)
    {
    	if ((error = snd_pcm_hw_params_set_period_size(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_period_size, 0)) < 0)
		{
			ERR("Could not set period size for destination device: %s", snd_strerror(error));
		}
    }
    if (!error)
    {
    	if ((error = snd_pcm_hw_params_set_periods(plugin_data->dst_pcm_handle, hw_params, plugin_data->dst_periods, 0)) < 0)
		{
			ERR("Could not set amount of periods for destination device: %s", snd_strerror(error));
		}
    }

	/* saving hardware parameters for target device */
    if (!error)
	{
		if ((error = snd_pcm_hw_params(plugin_data->dst_pcm_handle, hw_params)) < 0)
		{
			ERR("Could set hardware parameters: %s", snd_strerror(error));
		}
	}
	if (!hw_params)
	{
		snd_pcm_hw_params_free(hw_params);
	}

	/* allocating buffer required to transfer data to target device */
	if (!error)
	{
		/* target buffer must not be equal to the source buffer size; otherwise pointer callback will always return 0 */
		plugin_data->dst_buffer_size    = plugin_data->dst_period_size;
		plugin_data->dst_buffer_current = 0;

		/* adding extra space for one extra channel */
		size_t target_size = plugin_data->dst_buffer_size * (snd_pcm_format_physical_width(plugin_data->dst_format) >> 3) * (plugin_data->alsa_data.channels + 1);

	    /* calloc sets content to zero */
		plugin_data->dst_buffer = (unsigned char*) calloc(1, target_size);
		if (!plugin_data->dst_buffer)
		{
			error = -ENOMEM;
	        ERR("Could not allocate memory for transfer buffer; requested %lu bytes", target_size);
		}
	}

	return error;
}


static int set_dst_hw_params(plugin_data_t* plugin_data, snd_pcm_hw_params_t *params)
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
        ERR("Could not find target device for sample rate %u", plugin_data->alsa_data.rate);
    }
    else
    {
	    INF("destination device=%s", plugin_data->dst_device);
    }

    /* collecting details about the PCM stream */
	if (!error)
	{
	    if ((error = snd_pcm_hw_params_get_period_size(params, &plugin_data->dst_period_size, 0)) < 0)
		{
			ERR("Could not get period size value: %s", snd_strerror(error));
		}
	    else
	    {
		    INF("destination period size=%ld", plugin_data->dst_period_size);
	    }
	}
	if (!error)
	{
		if ((error = snd_pcm_hw_params_get_periods(params, &plugin_data->dst_periods, 0)) < 0)
		{
			ERR("Could not get buffer size value: %s", snd_strerror(error));
		}
		else
		{
		    INF("destination periods=%d", plugin_data->dst_periods);
		}
	}
	if (!error)
	{
		if ((error = snd_pcm_hw_params_get_format(params, &plugin_data->src_format)) < 0)
		{
			ERR("Could not get format value: %s", snd_strerror(error));
		}
	}

    if (!error)
    {
        error = open_destination_device(plugin_data);
	}

	return error;
}


static int set_dst_sw_params(plugin_data_t* plugin_data, snd_pcm_sw_params_t *params)
{
	int                  error     = 0;
	snd_pcm_sw_params_t* sw_params = NULL;

	/* allocating software parameters object and fill it with default values */
    if (!error)
	{
		if ((error = snd_pcm_sw_params_malloc(&sw_params)) < 0)
		{
			ERR("Could not allocate SW parameters: %s", snd_strerror(error));
		}
	}
    if (!error)
	{
    	if ((error = snd_pcm_sw_params_current(plugin_data->dst_pcm_handle, sw_params)) < 0)
		{
			ERR("Could not fill SW parameters with defaults: %s", snd_strerror(error));
		}
	}
    if (!error)
	{
    	if ((error = snd_pcm_sw_params_set_start_threshold(plugin_data->dst_pcm_handle, sw_params, plugin_data->dst_buffer_size)) < 0)
		{
			ERR("Could not set threshold for destination device: %s", snd_strerror(error));
		}
	}
    if (!error)
	{
    	if ((error = snd_pcm_sw_params_set_avail_min(plugin_data->dst_pcm_handle, sw_params, plugin_data->dst_period_size)) < 0)
		{
			ERR("Could not set min available amount for destination device: %s", snd_strerror(error));
		}
	}

    /* saving software parameters for target device */
	if (!error)
	{
		if ((error = snd_pcm_sw_params(plugin_data->dst_pcm_handle, sw_params)) < 0)
		{
			ERR("Could set software parameters: %s", snd_strerror(error));
		}
	}
	if (sw_params)
	{
		snd_pcm_sw_params_free(sw_params);
	}

	return error;
}


static int set_src_hw_params(snd_pcm_ioplug_t *io)
{
	int error = 0;

    /* supported access type */
    if (!error)
    {
    	if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(supported_accesses), supported_accesses)) < 0)
		{
			ERR("Could not set required access mode: %s", snd_strerror(error));
		}
    }

    /* supported formats */
    if (!error)
    {
    	if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(supported_formats), supported_formats)) < 0)
		{
			ERR("Could not set required format: %s", snd_strerror(error));
		}
    }

    /* supported amount of channels */
    if (!error)
    {
    	if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_CHANNELS, ARRAY_SIZE(supported_channels), supported_channels)) < 0)
		{
			ERR("Could not set required amount of channels: %s", snd_strerror(error));
		}
    }

    /* supported rates */
    if (!error)
    {
    	if ((error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE, ARRAY_SIZE(supported_rates), supported_rates)) < 0)
		{
			ERR("Could not set required sample rate: %s", snd_strerror(error));
		}
    }

    /* defining buffer size: bufer = period size * number of periods */
    if (!error)
    {
    	if ((error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, PERIOD_SIZE_BYTES, PERIOD_SIZE_BYTES)) < 0)
		{
			ERR("Could not set required period size: %s", snd_strerror(error));
		}
    }
	if (!error)
	{
		if ((error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, PERIODS, PERIODS)) < 0)
		{
			ERR("Could not set required amount of periods: %s", snd_strerror(error));
		}
	}

    return error;
}


static void write_stream_marker(plugin_data_t* plugin_data, unsigned char marker)
{
    snd_pcm_sframes_t result = 0;

	/* writting whatever is left in the target buffer */
	while (plugin_data->dst_buffer_current > 0 && result >= 0)
	{
		result = write_to_dst(plugin_data);
		if (result < 0)
		{
			ERR("Error while writting to target device: %s", snd_strerror(result));
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
			ERR("Error while writting to target device: %s", snd_strerror(result));
		}
	}
}


static snd_pcm_sframes_t write_to_dst(plugin_data_t* plugin_data)
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
				ERR("Target device restore error: %s", snd_strerror(result));
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


SND_PCM_PLUGIN_DEFINE_FUNC(slimplexor)
{
	int                   error          = 0;
	plugin_data_t*        plugin_data    = NULL;
	unsigned short        plugin_created = 0;
	snd_config_iterator_t i;
	snd_config_iterator_t next;

	snd_config_for_each(i, next, conf)
	{
		snd_config_t *n = snd_config_iterator_entry(i);
		const char   *id;

		if (snd_config_get_id(n, &id) < 0)
		{
			continue;
		}

		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
		{
			continue;
		}
	}

    /* allocating memory for plugin data structure and initializing it with zeros */
    if (!error)
    {
    	plugin_data = calloc(1, sizeof(plugin_data_t));
		if (!plugin_data)
		{
			error = -ENOMEM;
		}
    }

    /* initializing plugin data structure */
    if (!error)
    {
		plugin_data->alsa_data.version      = SND_PCM_IOPLUG_VERSION;
		plugin_data->alsa_data.name         = "SlimPlexor - An ALSA plugin used by SlimStreamer";
		plugin_data->alsa_data.callback     = &callbacks;
		plugin_data->alsa_data.private_data = plugin_data;

	    /* TODO: should come from config */
		plugin_data->rate_device_map_size   = 13;

    	/* allocating memory for rate->device data structure */
    	plugin_data->rate_device_map = calloc(plugin_data->rate_device_map_size, sizeof(rate_device_map_t));
		if (!plugin_data->rate_device_map)
		{
			error = -ENOMEM;
		}
    }

    /* useing a fixed format while writing to the target device */
    plugin_data->dst_format = TARGET_FORMAT;

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
            ERR("Could not disable thread-safety for ALSA library");
		}
	}

    /* creating ALSA plugin */
    if (!error)
    {
		if ((error = snd_pcm_ioplug_create(&plugin_data->alsa_data, name, stream, mode)) < 0)
		{
	        ERR("Could not register plugin within ALSA: %s", snd_strerror(error));
		} else {
			plugin_created = 1;
		}
    }

	/* setting up hw parameters; error is printed within setup routing */
	if (!error)
	{
		error = set_src_hw_params(&plugin_data->alsa_data);
	}

    /* this assigment must occure only in case when everything went fine */
	if (!error)
	{
		*pcmp = plugin_data->alsa_data.pcm;
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
