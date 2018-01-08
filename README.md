# SlimPlexor

SlimPlexor is an ALSA plugin required to capture and deliver PCM stream to SlimStrimer application via ALSA loopback devices.
Basically SlimPlexor does two things:
  * It adds an additional channel to the PCM stream with 'meta' data (start / stop markers, etc.)
  * It directs PCM stream to a predefined-by-rate ALSA loopback device. This is required as SlimStrimer reads PCM data from the same predefined-by-rate loopback devices.

## Prerequisites

To be described...


## Configuring ALSA

To be described...


## Compiling SlimPlexor

Building SlimPlexor is as easy as that:

```
git clone https://github.com/gimesketvirtadieni/slimplexor.git
cd slimplexor
cd make
make
```


## Installing SlimPlexor

Installing SlimPlexor is just copying shared library (libasound_module_pcm_slimplexor.so) to the ALSA plugins directory.
One way to find out the required directory is to try playing sound with this library installed.
Then output would look something similar:

```
andrej@sandbox:~$ aplay sample.wav
ALSA lib dlmisc.c:254:(snd1_dlobj_cache_get) Cannot open shared library /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_slimplexor.so
aplay: main:788: audio open error: No such device or address
```

So obviously in my case the required directory where SlimPlexor's shared library should be copied to is '/usr/lib/x86_64-linux-gnu/alsa-lib'


# Validating SlimPlexor

To be described...
