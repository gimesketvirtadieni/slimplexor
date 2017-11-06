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
			DBG("Error while draining target device: %s", snd_strerror(error));
		}
		error = snd_pcm_close(plugin_data->target_pcm);
		if (error < 0)
		{
			DBG("Error while closing target device: %s", snd_strerror(error));
		}
	}

	/* releasing resources */
	release_resources(plugin_data);

	return 0;
}


static int callback_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    DBG("");

    int            error       = 0;
    plugin_data_t* plugin_data = (plugin_data_t*)io->private_data;

	if (!error)
	{
	    error = snd_pcm_hw_params_get_period_size(params, &plugin_data->target_period_size, 0);
		if (error)
		{
			ERR("Could not get period size value: %s", snd_strerror(error));
		}
	}
	if (!error)
	{
	    error = snd_pcm_hw_params_get_periods(params, &plugin_data->target_periods, 0);
		if (error)
		{
			ERR("Could not get buffer size value: %s", snd_strerror(error));
		}
	}
	if (!error)
	{
		error = snd_pcm_hw_params_get_format(params, &plugin_data->format);
		if (error)
		{
			ERR("Could not get format value: %s", snd_strerror(error));
		}
	}
	if (!error)
	{
		plugin_data->bytes_per_sample = snd_pcm_format_physical_width(plugin_data->format) >> 3;
	}

	return error;
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

	/* resetting hw buffer pointer */
	plugin_data->pointer = 0;

	/* setting up target device */
	if (!error)
	{
		error = setup_target_device(plugin_data);
		if (error)
		{
			ERR("Could not setup target device=%s, error=%s", plugin_data->target_device, snd_strerror(error));
		}
	}

	/* preparing target device and starting playback */
	if (!error)
	{
		error = snd_pcm_prepare(plugin_data->target_pcm);
	}

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

    return 0;
}


static snd_pcm_sframes_t callback_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t total_frames)
{
	plugin_data_t*    plugin_data = (plugin_data_t*)io->private_data;
	snd_pcm_uframes_t frames      = plugin_data->target_period_size - plugin_data->target_buffer_current;
	unsigned char*    pcm_data    = (unsigned char*)areas->addr + (areas->first >> 3) + ((areas->step * offset) >> 3);

	/* adjusting amount of frames to be processed, which is max(available,provided) */
	if (total_frames < frames)
	{
		frames = total_frames;
	}

	/* the whole buffer will be written so reseting it first */
	memset(plugin_data->target_buffer, 0, plugin_data->target_period_size * plugin_data->bytes_per_sample * (io->channels + 1));

	/* it is ok to process less frames than provided as ALSA will call this callback with the rest of data */
	for (snd_pcm_uframes_t i = 0; i < frames; i++)
	{
		unsigned int target_position = plugin_data->target_buffer_current * plugin_data->bytes_per_sample * (io->channels + 1);

		for (unsigned int j = 0; j < plugin_data->bytes_per_sample * io->channels; j++)
		{
			plugin_data->target_buffer[target_position] = *pcm_data;
			target_position++;
			pcm_data++;
		}

		/* this is required as target device uses source channels + 1 */
		target_position += plugin_data->bytes_per_sample;
		plugin_data->target_buffer_current++;

		/* marking frame as containing data in the last byte of the last channel */
		plugin_data->target_buffer[target_position - 1] = 1;
	}

	/* if there is anything to be written to the target device */
	if (frames > 0)
	{
		/* writing to the target device */
		snd_pcm_sframes_t result = snd_pcm_writei(plugin_data->target_pcm, plugin_data->target_buffer, frames);

		/* no need to restore from an error in case of -EAGAIN */
		if (result < 0 && result != -EAGAIN)
		{
			result = snd_pcm_prepare(plugin_data->target_pcm);
			if (result < 0)
			{
				DBG("Restore error: %s", snd_strerror(result));
			}
		}
		else if (result == -EAGAIN)
		{
			/* it will make ALSA call transfer callback again with the same data */
			frames = 0;
		}
		else
		{
			frames                              = result;
			plugin_data->target_buffer_current -= result;
			plugin_data->pointer               += result;
		}
	}

	DBG("first=%u offset=%lu total_frames=%lu proc_frames=%ld", areas->first, offset, total_frames, frames);

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
		error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, period_size_bytes, period_size_bytes);
    }
	if (!error)
	{
		error = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, periods, periods);
	}

    return error;
}


static int setup_target_device(plugin_data_t* plugin_data)
{
	int                  error     = 0;
	snd_pcm_hw_params_t* hw_params = NULL;
	snd_pcm_sw_params_t* sw_params = NULL;

    /* looking up for the target device name based on sample rate */
	plugin_data->target_device = NULL;
	for (unsigned int i = 0; i < plugin_data->rate_device_map_size && !plugin_data->target_device; i++)
    {
    	if (plugin_data->rate_device_map[i].rate == plugin_data->alsa_data.rate)
    	{
    		plugin_data->target_device = plugin_data->rate_device_map[i].device;
    	}
    }
    if (!plugin_data->target_device)
    {
		error = -ENODEV;
        ERR("Could not find target device for sample rate %u", plugin_data->alsa_data.rate);
    }

    /* allocating buffer required to transfer data to target device */
    if (!error)
	{
	    DBG("Target device=%s", plugin_data->target_device);
	    if (plugin_data->target_buffer)
	    {
	    	free(plugin_data->target_buffer);
	    }

	    /* adding extra space for one extra channel */
		size_t target_size = plugin_data->target_period_size * plugin_data->bytes_per_sample * (plugin_data->alsa_data.channels + 1);
		plugin_data->target_buffer = (unsigned char*) calloc(1, target_size);
		if (!plugin_data->target_buffer)
		{
			error = -ENOMEM;
		}
	}

    /* opening the target device */
    if (!error)
    {
    	if (plugin_data->target_pcm)
    	{
        	snd_pcm_close(plugin_data->target_pcm);
    	}
    	error = snd_pcm_open(&plugin_data->target_pcm, plugin_data->target_device, SND_PCM_STREAM_PLAYBACK, 0);
    }

    /* allocating hardware parameters object and fill it with default values */
    if (!error)
    {
		error = snd_pcm_hw_params_malloc(&hw_params);
    }
    if (!error)
    {
		error = snd_pcm_hw_params_any(plugin_data->target_pcm, hw_params);
    }

    /* setting target device parameters */
    if (!error)
    {
        /* TODO: derive from the source */
		error = snd_pcm_hw_params_set_access(plugin_data->target_pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
	if (!error)
	{
		error = snd_pcm_hw_params_set_format(plugin_data->target_pcm, hw_params, plugin_data->format);
	}
	if (!error)
	{
		error = snd_pcm_hw_params_set_channels(plugin_data->target_pcm, hw_params, plugin_data->alsa_data.channels + 1);
	}
	if (!error)
	{
		error = snd_pcm_hw_params_set_rate(plugin_data->target_pcm, hw_params, plugin_data->alsa_data.rate, 0);
	}

	/* defining buffer size: bufer = period size * number of periods */
    if (!error)
    {
		error = snd_pcm_hw_params_set_period_size(plugin_data->target_pcm, hw_params, plugin_data->target_period_size, 0);
    }
    if (!error)
    {
		error = snd_pcm_hw_params_set_periods(plugin_data->target_pcm, hw_params, plugin_data->target_periods, 0);
    }

	/* saving hardware parameters for target device */
    if (!error)
	{
		error = snd_pcm_hw_params(plugin_data->target_pcm, hw_params);
	}
	if (!hw_params)
	{
		snd_pcm_hw_params_free(hw_params);
	}

    /* allocating software parameters object and fill it with default values */
    if (!error)
	{
		error = snd_pcm_sw_params_malloc(&sw_params);
	}
    if (!error)
	{
    	error = snd_pcm_sw_params_current(plugin_data->target_pcm, sw_params);
	}
    if (!error)
	{
        error = snd_pcm_sw_params_set_start_threshold(plugin_data->target_pcm, sw_params, plugin_data->target_period_size * plugin_data->target_periods);
	}
    if (!error)
	{
        error = snd_pcm_sw_params_set_avail_min(plugin_data->target_pcm, sw_params, plugin_data->target_period_size);
	}

    /* saving software parameters for target device */
	if (!error)
	{
		error = snd_pcm_sw_params(plugin_data->target_pcm, sw_params);
	}
	if (sw_params)
	{
		snd_pcm_sw_params_free(sw_params);
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
