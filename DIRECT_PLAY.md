# Direct play

WiiFin plays a file as it is when the Wii can decode it in real time, and asks Jellyfin to transcode everything else. Direct play spares the server, keeps the original quality and makes seeks almost instant (WiiFin reads the file at the new position instead of restarting a transcode).

The Wii decodes everything in software on its 729 MHz CPU, through MPlayer CE. The limits below are cautious: they have not been measured on a real console yet. If a file turns out too heavy (fewer than 85% of its frames shown over 10 s), WiiFin switches to a transcode at the same position.

Settings > **Direct Play**: *Auto* (default) or *Off* (everything is transcoded).

Legend: ✅ played as it is · ❌ transcoded by the server

## Video codecs by container

| Video codec | AVI | MKV / WebM | MP4 / M4V / MOV | TS / M2TS | MPG / VOB | Limits for direct play | Reason shown otherwise |
|---|---|---|---|---|---|---|---|
| **MPEG-4 ASP** (DivX 4+, Xvid) | ✅ | ✅ | ✅ | ✅ | ❌ | ≤ 720×576, ≤ 30 fps | `VideoResolutionNotSupported`, `VideoFramerateNotSupported` |
| **H.264** up to 480p | ✅ | ✅ | ✅ | ✅ | ❌ | ≤ 720×480, ≤ 30 fps, level ≤ 3.1, 8-bit, video ≤ 2.5 Mb/s | `VideoResolutionNotSupported`, `VideoLevelNotSupported`, `VideoBitDepthNotSupported`, `VideoBitrateNotSupported` |
| **H.264** 720p, 1080p, 10-bit | ❌ | ❌ | ❌ | ❌ | ❌ | Too heavy for the CPU | as above |
| **MPEG-2** | ✅ | ✅ | ❌ | ✅ | ✅ | ≤ 720×576, ≤ 30 fps | `VideoResolutionNotSupported` |
| **MPEG-1** | ✅ | ✅ | ❌ | ❌ | ✅ | ≤ 720×576, ≤ 30 fps | `VideoResolutionNotSupported` |
| **VP8** | ❌ | ✅ | ❌ | ❌ | ❌ | ≤ 640×480 | `VideoResolutionNotSupported` |
| **HEVC / H.265** | ❌ | ❌ | ❌ | ❌ | ❌ | No decoder | `VideoCodecNotSupported` |
| **VP9, AV1** | ❌ | ❌ | ❌ | ❌ | ❌ | No decoder | `VideoCodecNotSupported` |
| **DivX 3** (MS-MPEG4 v3) | ❌ | ❌ | ❌ | ❌ | ❌ | No decoder | `VideoCodecNotSupported` |
| **VC-1 / WMV3** | ❌ | ❌ | ❌ | ❌ | ❌ | Decoder present, no ASF demuxer | `VideoCodecNotSupported` |
| **Theora, VP6, H.263, MJPEG** | ❌ | ❌ | ❌ | ❌ | ❌ | Decoders present, not enabled for direct play | `VideoCodecNotSupported` |

## Audio codecs (in a video) by container

| Audio codec | AVI | MKV / WebM | MP4 / M4V / MOV | TS / M2TS | MPG / VOB | Max channels | Reason shown otherwise |
|---|---|---|---|---|---|---|---|
| **MP3** | ✅ | ✅ | ✅ | ✅ | ✅ | 2 | `AudioChannelsNotSupported` |
| **MP2** | ✅ | ✅ | ❌ | ✅ | ✅ | 2 | `AudioChannelsNotSupported` |
| **AAC** | ✅ | ✅ | ✅ | ✅ | ❌ | 2 | `AudioChannelsNotSupported` |
| **AC3** stereo | ✅ | ✅ | ✅ | ✅ | ✅ | 2 | – |
| **AC3** 5.1 | ❌ ¹ | ✅ | ✅ | ✅ | ✅ | 6 (mixed down to stereo) | `AudioChannelsNotSupported` |
| **Vorbis** | ❌ | ✅ | ❌ | ❌ | ❌ | 2 | `AudioCodecNotSupported` |
| **FLAC** | ❌ | ✅ | ❌ | ❌ | ❌ | 2 | `AudioCodecNotSupported` |
| **DTS** | ❌ | ❌ | ❌ | ❌ | ❌ | Too heavy next to the video | `AudioCodecNotSupported` |
| **E-AC3, TrueHD, DTS-HD, Opus** | ❌ | ❌ | ❌ | ❌ | ❌ | No decoder | `AudioCodecNotSupported` |
| **PCM** | ❌ | ❌ | ❌ | ❌ | ❌ | Not enabled in videos | `AudioCodecNotSupported` |

¹ AVI files made by FFmpeg describe 5.1 audio with a WAVEFORMATEXTENSIBLE header that MPlayer CE's AVI demuxer misreads (noise, and the picture drops to 2 fps).

Other containers (WMV/ASF, FLV, OGV, RM/RMVB, 3GP, MXF…) are always transcoded: `ContainerNotSupported`.

## Conditions for any video

| Condition | Otherwise | Reason shown |
|---|---|---|
| Settings > Direct Play is *Auto* | Transcode | `DirectPlayError` |
| Subtitles: none, or a text track (SRT, ASS, VTT…), which WiiFin draws over the picture | Picture subtitles (PGS, VobSub) are burned in: transcode | `SubtitleCodecNotSupported` |
| Any audio track, except another than the first in an MPEG-PS (MPG/VOB) file | Transcode | `SecondaryAudioNotSupported` |
| Total bitrate ≤ 75% of the measured link speed, and ≤ 8 Mb/s | Transcode | `ContainerBitrateExceedsLimit` |
| The file opens and starts | New try, transcoded | `DirectPlayError` |
| ≥ 85% of the frames shown over 10 s | Switches to a transcode at the same position | `DirectPlayError` |
| No rebuffering twice within 2 minutes | Transcode at a lower quality | `DirectPlayError` |
| The picture moves (not stuck for 20 s while the data still comes in) | Transcode, then a lower quality; then a message saying so | `DirectPlayError` |

The reasons are sent with the transcode, so the server's dashboard shows why a video is converted.

## Music

The Wii outputs 16-bit stereo at 48 kHz at most: higher sample rates would only cost CPU on the Wii for nothing.

| Format | Conditions | Direct | Reason shown otherwise |
|---|---|---|---|
| **MP3** | ≤ 320 kb/s | ✅ | `AudioBitrateNotSupported` |
| **WAV** (PCM, 16 or 24-bit) | ≤ 48 kHz, stereo | ✅ | `AudioSampleRateNotSupported`, `AudioChannelsNotSupported` |
| **FLAC** | ≤ 48 kHz, ≤ 24-bit, stereo | ✅ | `AudioSampleRateNotSupported`, `AudioBitDepthNotSupported`, `AudioChannelsNotSupported` |
| **FLAC / WAV** above 48 kHz or multichannel | – | ❌ to MP3 320 kb/s | as above |
| **AAC / M4A, Vorbis, Opus, ALAC, WMA** | – | ❌ to MP3 320 kb/s | `ContainerNotSupported` or `AudioCodecNotSupported` |

## What has been checked

In Dolphin, with the test files of `tools/test/make_media.sh formats` (scenario `formats`): AVI Xvid + MP3 and + AC3 stereo, MKV H.264 + AAC and + AC3 5.1, MP4 with its index first and last, MPEG-2 PS and TS, each with a seek in the file; resuming an AVI; episodes following each other; WAV 24-bit 44.1 kHz and FLAC. Not yet: every limit on a real Wii (H.264 480p and AC3 5.1 are the least certain). Reports from real consoles are welcome.
