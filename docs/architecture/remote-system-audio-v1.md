# Remote System Audio v1

Controller playback of the sound the Host is already playing. This is one-way system output, not a microphone and not a talkback channel. Opening the Controller speaker control starts Host capture, transmission, and local playback. The Host does not ask for a second confirmation.

No audio packet is sent while the Host output is digitally silent.

## Transport

Desktop video stays on `redclaw-media-v1`: ordered, zero retransmit, and paced with the video congestion controller. System audio uses a separate optional SCTP data channel so a video rate drop or latest-frame discard cannot queue or starve sound.

| Kind | Label | Delivery | Payload |
|---|---|---|---|
| Audio | `redclaw-audio-v1` | Unordered, `rexmit=0` | One self-describing Opus frame |

The channel is not part of the initial offer. The Host is the offerer and calls `ensureDataChannel` only after the Controller requests playback and both peers advertise `audio_version >= 1`. A create that has not opened is left alone for about three seconds, then replaced. Older peers omit the new fields, read as version 0, and never change the SDP.

`StreamControlMessageV1` gains two optional fields and no new message type. Unknown control enums are rejected for the whole message, so a new type would break older peers.

| Field | Number | Meaning |
|---|---|---|
| `audio_version` | 88 | `1` when this build can carry system audio. `0` means an older peer. |
| `audio_playback_requested` | 89 | Set by the Controller. `1` starts the stream; `0` stops it. |

Hello and the capabilities reply advertise `audio_version`. The Controller sends a new capabilities message when the speaker control changes. The Host applies that request. The Controller records the Host version but does not treat the Host's request flag as local user intent.

Closing the control, ending the session, closing the audio channel, or entering the secure desktop stops Host capture. The Controller closes its player and drops queued audio. `CapabilitySet::audio_capture` remains a user-session placeholder and is not this switch.

## What the Host captures

The capture point is the Windows audio engine render loopback, in the same interactive user process as desktop capture. WASAPI shared mode, DirectSound, MME/waveOut, XAudio2, and Media Foundation shared-mode output all enter that engine mix. The loopback sees the engine output, not an application's private buffer.

Every active render endpoint with an active shared session is opened with `AUDCLNT_STREAMFLAGS_LOOPBACK`. Output that an application sends to a non-default device is still captured. Each endpoint is read in its own `GetMixFormat()` and converted before the endpoints are mixed into one stream.

Accepted mix formats:

- `WAVE_FORMAT_PCM` 16-bit, packed 24-bit, and 32-bit
- 24-bit samples packed into a 32-bit container (`valid_bits == 24`)
- `WAVE_FORMAT_IEEE_FLOAT`
- `WAVE_FORMAT_EXTENSIBLE` with its channel mask
- mono, stereo, 5.1, and 7.1, downmixed to stereo
- 44100, 48000, 96000, and 192000 Hz, resampled to 48 kHz

Opus input is fixed: 48 kHz, stereo, 20 ms (960 samples), about 96 kbps, through libopus. Resampling uses libswresample. The send queue holds about four frames (80 ms) and drops the oldest. It does not enter the video pacer.

A WASAPI buffer marked `AUDCLNT_BUFFERFLAGS_SILENT`, or a frame whose samples sit in digital silence, is not encoded and not sent. Opus DTX filler is not sent either. The sequence number increases only for a frame that is actually sent.

WASAPI exclusive mode and ASIO bypass the audio engine. This version does not capture them. Covering those paths would require a kernel driver or a virtual device, which is outside the desktop stream.

## Packet and playback

Each SCTP message is one frame:

`version, channels, sample_rate, frame_samples, sequence, timestamp_us, payload`

The timestamp is reserved for later picture alignment. This version does not lock audio to the video clock.

The Controller decodes Opus in playback order into the existing XAudio2 `AudioPlayer`. A jitter buffer of about 60 ms absorbs reordering. One or two missing sequence numbers are concealed with Opus PLC while packets are still arriving. If about 100 ms passes with no later packet, concealment stops, the player is closed, and local output stays silent until the next real frame. A full playback queue drops the new block and does not block receive or video decode.

The speaker control lives on the Controller playback bar. It stays disabled, with an explanation, while the peer `audio_version` is 0. It starts off.
