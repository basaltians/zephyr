# RTP Audio Stream Sink Sample

This sample receives an AES67-style L24 stereo audio stream over RTP multicast and plays it out
over I2S/DAC. It joins the multicast group `239.0.1.1:5004` and buffers a few packets in a jitter
buffer before starting playback.

Note that since no clock synchronisation is happening, it could happen that buffers become empty or
full after a period of time, due to differing playback speeds between the source and the sink.

## Building and Running

```shell
west build -p -b aes67_devkit@2
west -v flash
```

## Testing with FFmpeg

It's possible to send an RTP stream using ffmpeg and listen to it with this sample. However, on
Windows 11, ffmpeg seems to send at 50 kHz instead of 48 kHz, causing buffer overflows; it is
therefore advised to run ffmpeg on a Linux machine instead. It is also advised to start ffmpeg
before the sink, since startup transients (such as briefly sending more data than normal) can cause
unwanted behaviour in the sink.

Use `ffmpeg.conf` when testing with ffmpeg, as it increases buffer sizes dramatically:

```shell
west build -p -b aes67_devkit@2 --extra-conf ffmpeg.conf
```

```shell
ffmpeg -re -f lavfi -i anullsrc=r=48000:cl=stereo -acodec pcm_s24be -f rtp -rtp_flags latm \
    -pkt_size 300 rtp://239.0.1.1:5004
