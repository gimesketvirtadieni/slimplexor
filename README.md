# SlimPlexor

SlimPlexor is an ALSA plugin required to capture and deliver PCM stream to SlimStrimer application via ALSA loopback devices.
Basically SlimPlexor does two things:
  * It adds an additional channel to the PCM stream with 'meta' data (start / stop markers, etc.)
  * It directs PCM stream to a predefined-by-rate ALSA loopback device. This is required for SlimStrimer to read PCM data from the same predefined-by-rate loopback devices.

Below description provides steps to build (compile) and setup ALSA with SlimSplexor as default device.  
Please note that setup process is very dependant on Linux distro and even distro version, so use it as a 'direction' rather than an 'instruction'.


## Required prerequisites

SlimPlexor is written in C so it requires C/C++ compiler and ALSA header files.
An easy way to install required prerequisites is:

```
sudo get update
sudo apt-get install build-essential libasound2-dev
```


## Configuring ALSA

Configuring ALSA is the most tricky part of installation steps as it requires a bit dipper knowledge of ALSA setup.  
IMPORTANT NOTE: ALSA setup is Linux distro specific, so you might need to check ALSA documentation if it does not work for you.  

1. The first thing to do is to make sure ALSA loopback module is loaded:

```
andrej@sandbox:~$ sudo lsmod |grep snd_aloop
snd_aloop              24576  0
snd_pcm               102400  4 snd_usb_audio,snd_ac97_codec,snd_ens1371,snd_aloop
snd                    77824  11 snd_hwdep,snd_seq,snd_usb_audio,snd_ac97_codec,snd_timer,snd_rawmidi,snd_usbmidi_lib,snd_ens1371,snd_seq_device,snd_aloop,snd_pcm
```

If this command does not produce result as above then it means loopback module is not loaded.
One way to correct that (and make sure ALSA configuration does not change after reboot):

```
andrej@sandbox:~$ echo "snd-aloop" | sudo tee /etc/modules-load.d/alsa-loopback.conf
snd-aloop
andrej@sandbox:~$ sudo reboot
```
 
After a reboot ALSA loopback module should be loaded (this can be validated as described above).  

2. The next step is to define 2 ALSA loopback devices (one device is not enough due to a limit for max subdevices per one card)

One way to define required loopback devices is:

```
andrej@sandbox:~$ echo "options snd-aloop enable=1,1 index=1,2 pcm_substreams=8,8 id=Loopback1,Loopback2" | sudo tee /etc/modprobe.d/alsa-loopback.conf
options snd-aloop enable=1,1 index=1,2 pcm_substreams=8,8 id=Loopback1,Loopback2
andrej@sandbox:~$ sudo reboot
```

After a reboot ALSA loopback devices can be validated by:

```
andrej@sandbox:~$ aplay -l|grep Loopback
card 1: Loopback1 [Loopback], device 0: Loopback PCM [Loopback PCM]
card 1: Loopback1 [Loopback], device 1: Loopback PCM [Loopback PCM]
card 2: Loopback2 [Loopback], device 0: Loopback PCM [Loopback PCM]
card 2: Loopback2 [Loopback], device 1: Loopback PCM [Loopback PCM]
```

Basically the output shows that there are two loopback cards with two divices per card (full duplex).
To check subdevices you can skip '|grep Loopback' part in the command and will see all (sub)devices available on the system (you should be able to see 8 subdevices per each loopback device).

3. The last step to configure ALSA setup is enabling SlimPlexor plugin

To enable SlimPlexor, one should edit /etc/asound.conf file to look like this:
```
andrej@sandbox:~$ more /etc/asound.conf 

# Define SlimPlexor plugin
pcm.slimplexor {
  type slimplexor
}

# Define 'plug' plugin which might be used as a fallback (optional)
pcm.plug {
  type plug
  slave {
    pcm "hw:0,0"
  }
}

# Following lines define SlimPlexor as a default device
pcm.!default {
  type slimplexor
}

# Following lines are a backup for regular default device (can be used to disable SlimPlexor)
#pcm.!default {
#  type hw
#  card 0
#}

#ctl.!default {
#  type hw
#  card 0
#}
andrej@sandbox:~$
```


## Compiling SlimPlexor

Building SlimPlexor is as easy as that (assuming all prerequisites are installed):

```
git clone https://github.com/gimesketvirtadieni/slimplexor.git
cd slimplexor
cd make
make
```

If compilation is successful then there is a shared library created (along with the main file itself):

```
andrej@sandbox:~/slimplexor/make$ ls
libasound_module_pcm_slimplexor.so  Makefile
```


## Installing SlimPlexor

Installing SlimPlexor is just copying shared library (libasound_module_pcm_slimplexor.so) to the ALSA plugins directory.
One way to find out what ALSA plugin directory is - try playing sound without SlimPlexor library installed.
Then output would look similar to:

```
andrej@sandbox:~$ aplay sample.wav
ALSA lib dlmisc.c:254:(snd1_dlobj_cache_get) Cannot open shared library /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_slimplexor.so
aplay: main:788: audio open error: No such device or address
```

So obviously in my case the required directory where SlimPlexor's shared library should be placed is '/usr/lib/x86_64-linux-gnu/alsa-lib':

```
andrej@sandbox:~/slimplexor/make$ sudo cp ./libasound_module_pcm_slimplexor.so /usr/lib/x86_64-linux-gnu/alsa-lib/
```


## Validating SlimPlexor

If SlimPlexor was configured properly, then output to the terminal while playing audio by any application should look like this:

```
andrej@sandbox:~$ aplay sample.wav 
Playing WAVE 'sample.wav' : Signed 32 bit Little Endian, Rate 44100 Hz, Stereo
INFO: set_dst_hw_params: destination device=hw:1,0,7
INFO: set_dst_hw_params: destination period size=2048
INFO: set_dst_hw_params: destination periods=8
```

That's it, SlimPlexor is setup as required.
Proceed to [SlimStreamer](https://github.com/gimesketvirtadieni/slimstreamer) for streaming audio being played by any application that outouts it to the default ALSA device ;)
