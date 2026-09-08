# Overview

Crossfade between songs for the Spotify TV app. The bundle patches the app so that a native
library, `libxfade.so`, sits between Spotify's embedded playback engine (the eSDK) and the system
OpenSL ES, buffers the PCM stream and mixes the end of one track into the start of the next.

## What it does

- **Automatic track changes fade.** When a track plays to its end, the outgoing and incoming tracks
  are mixed with an equal-power curve for the configured length, 0 to 12 seconds.
- **Manual changes are instant.** Skip, previous, picking a track from a list and seeking inside a
  track all cut. So do ads, podcast episodes and anything that recreates the player.
- **The length is adjustable** in one second steps, default 5 seconds. 0 disables the fade; the
  library still buffers a small lead but behaviour and latency then match the stock app.

## Supported app and device

| | |
|---|---|
| App | Spotify for TV, `com.spotify.tv.android` |
| Version | 1.134.2 only (`patches/src/main/kotlin/dev/spotifytv/crossfade/patches/shared/Constants.kt`) |
| ABI | `armeabi-v7a` (32-bit ARM) only |
| Verified on | Fire TV Stick 4K Max |

## Setting the crossfade length

Long-press the remote's Menu button for about 0.7 s to open the on-screen setting. Left and Right
adjust it by one second, Back closes it. The screen also shows the library's live status.

The same setting can be changed over adb without the UI:

```bash
adb shell am start -n com.spotify.tv.android/dev.spotifytv.crossfade.extension.CrossfadeSettingsActivity --ei crossfade_seconds 6
```

The value is stored in the app's own `crossfade` preferences and pushed to the native library at
app start and whenever it changes.

## How it works

**The dependency swap.** The eSDK (`lib/armeabi-v7a/libspotify_tv_jni.so`) links against
`libOpenSLES.so`. The native-library patch rewrites that dependency string in place to
`libxfade.so` (same length, zero padded) and adds `libxfade.so` to the APK, so every OpenSL call
the eSDK makes lands in our code instead of the system's.

**The front end.** `native/src/fake_sl.cpp` implements the part of the OpenSL ES surface the eSDK
uses: `slCreateEngine`, the engine object, an output mix, an audio player with `SLPlayItf`, the
Android simple buffer queue and `SLVolumeItf`. Volume and mute calls are forwarded to the real
player; buffer queue calls (`Enqueue`, `Clear`, `GetState`) go to the pipeline.

**The ring and the lead.** `RingPipeline` copies every buffer the eSDK enqueues into a 30 second
ring addressed by absolute frame number, and drives the real OpenSL player itself with four 10 ms
buffers. It acknowledges the eSDK's buffers from a thread of its own, and only while the ring holds
less than the lead target `T = max(X + 1 s, 0.5 s)` (X being the crossfade length). That gate is
what keeps the eSDK a crossfade ahead of what is actually being heard, which is what makes an
overlap possible at all: at the moment one track ends, the start of the next is already in the ring.

**Boundary decisions.** The patched app hands the eSDK's metadata JSON to the library, and the
buffer queue's `Clear` calls arrive as flushes. `BoundaryModel` turns those two event streams into
one decision, a cut or a fade at a specific frame:

- a flush followed by metadata is a manual change, so it cuts at the flush frame;
- a flush with no metadata behind it cuts once, on a timeout;
- a new track with no flush is a natural end, so it fades. Its boundary frame is predicted from the
  outgoing track's duration and the position it started at; the eSDK announces the next track up to
  about 1.6 s early, so a prediction further than 2.5 s from the announcement is discarded and the
  announced frame is used instead;
- ads, podcast episodes and a tail shorter than 200 ms cut rather than fade.

**The fade.** `mixer.cpp` mixes with an equal-power curve, `out = a * cos(theta) + b * sin(theta)` over
the fade length. The two sides have independent gain positions and independent ramp lengths, so an
incoming track whose audio is delivered late still fades in from its own first frame rather than
being skipped into.

**Padding drop and re-anchoring.** After any boundary the eSDK pads with digital silence whenever
its decoder falls behind our demand, interleaved with the little real audio it has. Those all-zero
buffers are dropped rather than written to the ring, and acknowledged on a slow probe schedule so
the eSDK's own loop keeps running without being asked for a burst it can only answer with more
padding. The first real audio afterwards is the incoming track's true first frame, so a scheduled
fade is re-anchored onto it; the outgoing side keeps the gain it has already reached and its
remaining ramp is refitted to reach zero at the new boundary.

**Minimum lead.** Even with crossfade off the pipeline holds 0.5 s ahead of the real player,
because the eSDK's refill latency is longer than the 40 ms output queue and a zero lead ran the
ring dry between buffers.

The full mechanism, with the constants and the thread and lock rules, is in
[architecture.md](architecture.md).

## Known limitations

- **Video content bypasses the eSDK entirely.** Its audio never reaches this library, so no
  crossfade is possible into or out of it and none is attempted. The two directions are asymmetric:
  going from a normal track into a video track still fades the outgoing side out while the video's
  audio starts unfaded on its own stream, while going from a video track into a normal one has no
  real outgoing audio at all, so what should be a crossfade is only the incoming track fading in
  from nothing. Spotify TV plays the video by default whenever a track has one, so this is common
  rather than a corner case.
- **Exact digital silence shortly after a boundary is removed.** Silence inside a track is
  indistinguishable from the eSDK's padding while the drop window is open, so it is dropped with it.
  The window is bounded by a 12 s cap on dropped silence and a 45 s cap on its own lifetime.
- **The progress bar runs ahead of the audio** by roughly the crossfade length plus a second,
  because the eSDK's position clock keeps counting while its padding is being dropped. Spotify
  Connect's "now playing" position leads by the same amount.
- **At settings of 11 to 12 seconds the boundary prediction is rejected** by the 2.5 s sanity
  window (the eSDK's reported position tracks wall clock rather than delivered audio at that
  depth), so the fade rests on the announced frame plus the re-anchor to the first real incoming
  audio. That is only safe while the eSDK pads before every incoming track, which it does today.
- **A fade can end up shorter than the setting** when the incoming track's audio arrives late
  relative to the outgoing tail. The re-anchor keeps it gain continuous rather than gapped, but the
  overlap itself can be less than the configured length.
- **Pause during a fade, and podcast episode behaviour, were not re-verified on the current
  build.**
- **32-bit ARM only**, and pinned to app version 1.134.2. A different app version will not match
  the patch fingerprints, and a 64-bit build of the app would need a 64-bit library.
