# RTP Audio Stream Source Sample

This sample captures stereo audio from the ADC over I2S, encodes it as L24, and streams it over RTP
multicast to `239.0.1.1:5004` using payload type 97.

## Building and Running

```shell
west build -p -b aes67_devkit@2
west -v flash
```

## Listening with an SDP File

To listen to the audio stream on your PC, create an `audio_stream_rtp_source.sdp` file containing
the following contents:

```sdp
v=0
o=- 0 0 IN IP4 0.0.0.0
s=audio_stream_rtp_source
c=IN IP4 239.0.1.1/1
t=0 0
m=audio 5004 RTP/AVP 97
a=rtpmap:97 L24/48000/2
a=recvonly
```

## Testing with FFmpeg

It's possible to use ffmpeg to capture to the RTP stream from this sample in an output.wav file.
First create the SDP file as described above, then run:

```shell
ffmpeg -protocol_whitelist file,udp,rtp -i audio_stream_rtp_source.sdp -acodec pcm_s24le -ar 48000 \
    -ac 2 -f wav output.wav
```

## Testing with VLC

On Windows, just open the `audio_stream_rtp_source.sdp` file using VLC media player; the stream
should start playing.
