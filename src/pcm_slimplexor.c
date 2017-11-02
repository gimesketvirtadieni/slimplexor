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

#include "pcm_slimplexor.h"


static int callback_close(snd_pcm_ioplug_t *io)
{
    DBG("");

    int            error       = 0;
	plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	if (plugin_data->target_pcm)
	{
		error = snd_pcm_drain(plugin_data->target_pcm);
		if (error < 0)
		{
			DBG("Error while draining target device (%i)", error);
		}
		error = snd_pcm_close(plugin_data->target_pcm);
		if (error < 0)
		{
			DBG("Error while closing target device (%i)", error);
		}
	}

	/* releasing resources */
	release_resources(plugin_data);

	return 0;
}


static int callback_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    DBG("");

    return 0;
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


static snd_pcm_sframes_t callback_pointer(snd_pcm_ioplug_t *io)
{
	plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	plugin_data->pointer %= io->buffer_size;

    return plugin_data->pointer;
}


static int callback_prepare(snd_pcm_ioplug_t *io)
{
    DBG("");

    int            error       = 0;
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;
    char*          device      = NULL;

    /* looking up for the target device name based on sample rate */
    for (unsigned int i = 0; i < plugin_data->rate_device_map_size && !device; i++)
    {
    	if (plugin_data->rate_device_map[i].rate == io->rate)
    	{
    		device = plugin_data->rate_device_map[i].device;
    	}
    }
    if (!device)
    {
		error = -ENODEV;
        ERR("Could not find target device for sample rate %u", io->rate);
    }
    DBG("Target device=%s", device);

	/* allocating buffer required to transfer data to target device */
	if (!error)
	{
		/* adding extra space for one extra channel */
		size_t target_size = period_size + (period_size / io->channels);
		plugin_data->target_buffer = (unsigned char*) calloc(1, target_size);
		if (!plugin_data->target_buffer)
		{
			error = -ENOMEM;
		}

		/* setting index of the last frame in the buffer */
		plugin_data->target_buffer_last = target_size / 6;  /* TODO: bytes per frame */
	}

	/* resetting hw buffer pointer */
	plugin_data->pointer = 0;

	/* setting up target device */
	if (!error)
	{
		error = setup_target_device(plugin_data, device, io->rate);
		if (error)
		{
			ERR("Could not setup target device=%s, error=%s", device, snd_strerror(error));
		}
	}

	/* preparing target device and starting playback */
	if (!error)
	{
		error = snd_pcm_prepare(plugin_data->target_pcm);
	}

	return error;
}


static snd_pcm_sframes_t callback_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t total_frames)
{
	DBG("offset = %lu, size = %lu", offset, total_frames);

	plugin_data_t*    plugin_data     = (plugin_data_t*)io->private_data;
	snd_pcm_uframes_t frames          = plugin_data->target_buffer_last - plugin_data->target_buffer_current;
	unsigned char*    pcm_data        = areas[offset].addr + (areas[offset].first >> 3);
	unsigned int      bytes_per_frame = 6;  /* TODO: ...*/

	/* adjusting amount of frames to be processed, which is max(available,provided) */
	if (total_frames < frames)
	{
		frames = total_frames;
	}

	/* processing frame-by-frame */
	for (snd_pcm_uframes_t i = 0; i < frames; i++)
	{
		unsigned int target_position = plugin_data->target_buffer_current * bytes_per_frame;

		for (unsigned int j = 0; j < 4 /* TODO: frame_size */; j++)
		{
			plugin_data->target_buffer[target_position] = *pcm_data;
			target_position++;
			pcm_data++;
		}

		/* this is required as target device uses source channels + 1 */
		target_position += 2;  /* TODO: ... */
		plugin_data->target_buffer_current++;

		/* marking frame as containing data in the last byte of the last channel */
		plugin_data->target_buffer[target_position - 1] = 1;
	}

	/* if there is anything to be written to the target device */
	if (frames > 0)
	{
		snd_pcm_sframes_t written_frames = snd_pcm_writei(plugin_data->target_pcm, plugin_data->target_buffer, frames);
		DBG("written_frames=%li", written_frames);
		if (written_frames < 0 && written_frames != -EAGAIN)
		{
			written_frames = snd_pcm_prepare(plugin_data->target_pcm);
			if (written_frames < 0)
			{
				printf("restore error: %s\n", snd_strerror(written_frames));
			}
			else
			{
				printf("restored!");
			}
		}
	}

	/* TODO: pretending that #size frames was written to the target device */
	plugin_data->target_buffer_current -= frames;

	/* updating pointer so it can be used from a pointer callback */
	plugin_data->pointer += frames;

	return frames;
}


static void release_resources(plugin_data_t* plugin_data)
{
	if (!plugin_data)
	{
		if (!plugin_data->target_buffer)
		{
			free(plugin_data->target_buffer);
		}
		if (!plugin_data->target_sw_params)
		{
			snd_pcm_sw_params_free(plugin_data->target_sw_params);
		}
		if (!plugin_data->target_hw_params)
		{
			snd_pcm_hw_params_free(plugin_data->target_hw_params);
		}
		if (!plugin_data->rate_device_map)
		{
			free(plugin_data->rate_device_map);
		}
		free(plugin_data);
	}
}


static int setup_hw_params(snd_pcm_ioplug_t *io)
{
	int error = 0;

    /* supported access type */
    if (!error)
    {
    	error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(supported_accesses), supported_accesses);
    }

    /* supported formats */
    if (!error)
    {
		error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(supported_formats), supported_formats);
    }

    /* supported amount of channels */
    if (!error)
    {
    	error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_CHANNELS, ARRAY_SIZE(supported_channels), supported_channels);
    }

    /* supported rates */
    if (!error)
    {
		error = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE, ARRAY_SIZE(supported_rates), supported_rates);
    }

    /* defining buffer size: bufer = period size * number of periods */
    if (!error)
    {
		error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, period_size, period_size);
    }
	if (!error)
	{
		error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, buffer_size, buffer_size);
	}

    return error;
}


static int setup_target_device(plugin_data_t* plugin_data, const char* device, unsigned int rate)
{
	int error = 0;

    /* opening the target device */
    if (!error)
    {
    	error = snd_pcm_open(&plugin_data->target_pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    }

    /* allocating hardware parameters object and fill it with default values */
    if (!error)
    {
		error = snd_pcm_hw_params_malloc(&plugin_data->target_hw_params);
    }
    if (!error)
    {
		error = snd_pcm_hw_params_any(plugin_data->target_pcm, plugin_data->target_hw_params);
    }

    /* setting target device parameters */
    if (!error)
    {
		error = snd_pcm_hw_params_set_access(plugin_data->target_pcm, plugin_data->target_hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
	if (!error)
	{
		error = snd_pcm_hw_params_set_format(plugin_data->target_pcm, plugin_data->target_hw_params, SND_PCM_FORMAT_S16_LE);
	}
	if (!error)
	{
		error = snd_pcm_hw_params_set_channels(plugin_data->target_pcm, plugin_data->target_hw_params, /* TODO: ... */ 3);
	}
	if (!error)
	{
		error = snd_pcm_hw_params_set_rate(plugin_data->target_pcm, plugin_data->target_hw_params, rate, 0);
	}

	/* defining buffer size: bufer = period size * number of periods */
    if (!error)
    {
		error = snd_pcm_hw_params_set_period_size(plugin_data->target_pcm, plugin_data->target_hw_params, period_size, 0);
    }
    if (!error)
    {
		error = snd_pcm_hw_params_set_buffer_size(plugin_data->target_pcm, plugin_data->target_hw_params, buffer_size);
    }

	/* saving hardware parameters for target device */
    if (!error)
	{
		error = snd_pcm_hw_params(plugin_data->target_pcm, plugin_data->target_hw_params);
	}

    /* allocating software parameters object and fill it with default values */
    if (!error)
	{
		error = snd_pcm_sw_params_malloc(&plugin_data->target_sw_params);
	}
    if (!error)
	{
    	error = snd_pcm_sw_params_current(plugin_data->target_pcm, plugin_data->target_sw_params);
	}

    /* TODO: set available min and threshold */
    if (!error)
	{
        error = snd_pcm_sw_params_set_start_threshold(plugin_data->target_pcm, plugin_data->target_sw_params, buffer_size);
	}
    if (!error)
	{
        error = snd_pcm_sw_params_set_avail_min(plugin_data->target_pcm, plugin_data->target_sw_params, period_size);
	}

    /* saving software parameters for target device */
	if (!error)
	{
		error = snd_pcm_sw_params(plugin_data->target_pcm, plugin_data->target_sw_params);
	}

	return error;
}


SND_PCM_PLUGIN_DEFINE_FUNC(slimplexor)
{
	int                   error       = 0;
	snd_config_iterator_t i;
	snd_config_iterator_t next;
	plugin_data_t*        plugin_data = NULL;
	short                 created     = 0;

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
		plugin_data->rate_device_map_size   = 2;

    	/* allocating memory for rate->device data structure */
    	plugin_data->rate_device_map = calloc(plugin_data->rate_device_map_size, sizeof(rate_device_map_t));
		if (!plugin_data->rate_device_map)
		{
			error = -ENOMEM;
		}
    }

    /* TODO: should come from config */
    /* initializing rate->device map data structure */
    if (!error)
    {
    	plugin_data->rate_device_map[0].rate   = 44100;
    	plugin_data->rate_device_map[0].device = "hw:1,0,1";
    	plugin_data->rate_device_map[1].rate   = 48000;
    	plugin_data->rate_device_map[1].device = "hw:1,0,2";
	}

    /* creating ALSA plugin */
    if (!error)
    {
		error   = snd_pcm_ioplug_create(&plugin_data->alsa_data, name, stream, mode);
		created = (!error ? 1 : 0);
    }

    /* setting up hw parameters */
    if (!error)
    {
        error = setup_hw_params(&plugin_data->alsa_data);
        if (error)
        {
            ERR("Could not setup hardware parameters for source device");
        }
    }

    /* this assigment must occure only in case when everything went fine */
	if (!error)
	{
		*pcmp = plugin_data->alsa_data.pcm;
	}
	else
	{
		/* releasing resources if required */
		if (!plugin_data && created)
		{
			snd_pcm_ioplug_delete(&plugin_data->alsa_data);
		}
		release_resources(plugin_data);
	}

	return error;
}
SND_PCM_PLUGIN_SYMBOL(slimplexor)
