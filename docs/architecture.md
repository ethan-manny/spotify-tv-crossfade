# Architecture

How `libxfade.so` and the Java extension fit together, and where every rule and constant lives.
File paths are relative to the repository root.

## Runtime data flow

```
 Spotify eSDK (libspotify_tv_jni.so)
   |  OpenSL ES calls (dependency string rewritten to libxfade.so)
   v
 fake OpenSL front end            native/src/fake_sl.cpp
   |  Enqueue / Clear / GetState / SetPlayState
   v
 RingPipeline                     native/src/ring_pipeline.{h,cpp}
   |  30 s ring of absolute frames        native/src/ring.h
   |  boundary decisions                  native/src/boundary.{h,cpp}
   |  equal-power mix                     native/src/mixer.{h,cpp}
   |  debug PCM dump                      native/src/dump.{h,cpp}
   v
 RealBackend                      native/src/real_backend.{h,cpp}
   |  dlopen("libOpenSLES.so"), one engine and output mix per process
   v
 system audio

 patched app code
   |  SpotifyTVApplication.onCreate  -> CrossfadeHooks.init
   |  metadata lambda                -> CrossfadeHooks.onMetadataJson -> MetadataParser
   |  SpotifyTVActivity.dispatchKeyEvent -> CrossfadeHooks.onKeyEvent -> LongPressDetector
   |  CrossfadeSettingsActivity      -> CrossfadeSettings (SharedPreferences)
   v
 CrossfadeNative (JNI)            native/src/xfade_jni.cpp -> RingPipeline / status
```

The eSDK reports its own buffer count when it creates the player, but the pipeline always drives
the real player with its own queue of `kOutBuffers = 4` buffers of `kOutMs = 10` ms.

## Threads and locks

| Thread | What it runs |
|---|---|
| eSDK audio thread | `enqueue`, `clear`, `getState`, `setPlayState`, `playState` through the front end |
| ack thread | `RingPipeline::ackLoop`, one per started pipeline: ticks the boundary model, releases acknowledged buffers back to the eSDK |
| audio thread (real OpenSL) | `onBackendDone` -> `fill` -> `PcmDumper::push` -> `Backend::enqueue` |
| Java threads | `CrossfadeNative`: `getStatus`, `setCrossfadeMs`, `onMetadata`, `setDumpDir` |

Locks, in the order they may be taken:

1. `RingPipeline::registryMutex()` (`ring_pipeline.cpp`), a process-wide lock over the current
   pipeline pointer and the dump directory. Every JNI entry point that reaches a live pipeline
   holds it for the whole use of the pointer; the destructor blocks on it, so a Java call can never
   overlap player destruction.
2. the status lock in `native/src/status.cpp`, held by `statusLine()` while it calls `stats()`.
3. `RingPipeline::mu_`, the pipeline's single mutex, paired with one condition variable.

Rules that hold everywhere: the eSDK is never called with `mu_` held (the ack thread drops the lock
before invoking the buffer-done callback), `PcmDumper::push` and the backend enqueue run outside
`mu_` so file writes never block the eSDK, and `stats()` is called with the status lock held and
must not call back into `status.cpp`.

`backend_ != nullptr` is what every other thread reads as "this pipeline is started"; `start()`
publishes it last, under `mu_`, once the ring, the output buffers, the arm and the ack thread are
all in place. `stop()` called from the ack thread itself (the eSDK can destroy the player from
inside its own buffer callback) detaches the thread and clears a shared "alive" token instead of
joining, so the loop returns without touching a destroyed object.

## State machines

### Boundary model (`native/src/boundary.{h,cpp}`)

Pure: no locks, no threads, no clock. The pipeline passes frame counters and a millisecond
timestamp in and gets back `None`, `Cut at b` or `Fade at b of n frames`.

- `onFlush` records a pending flush at the current write frame. A burst of `Clear` calls collapses
  into the latest one.
- `onMetadata` with a flush pending consumes it and always cuts at the flush frame. The log
  distinguishes the three ways it got there: a flush already older than `kFlushWindowMs = 500` ms,
  a skip (the track changed), and a seek (it did not).
- `onMetadata` with no flush pending and a new track is a natural transition. The boundary frame is
  predicted as `startFrame + (duration - startPosition) * rate`, and used only when it is within
  `kSanityWindowMs = 2500` ms of the announced frame; otherwise the announced frame is used. The
  difference is reported as `last_delta_ms`.
- `decide` fades when the crossfade length is not zero, neither side is an ad or a
  `spotify:episode:` URI, and the outgoing tail is at least `kMinTailMs = 200` ms; the fade length
  is `min(tail, crossfade)`. Anything else cuts. The eSDK's `video` flag flaps within a track and
  is deliberately not a criterion.
- `tick` turns a flush that got no metadata within the window into a cut, and remembers it for
  `kLateMetadataMs = 2000` ms so the track's late announcement adopts that cut instead of
  scheduling a second boundary.

### The arm and the padding drop (`RingPipeline`)

An arm opens at every boundary: player start, a flush, and a new playback id in `onMetadata`.
While armed:

- an all-zero buffer is dropped instead of written, counted, and queued for acknowledgement at
  `kProbeMs = 100` ms intervals, released only while playing;
- the first non-zero buffer after something was dropped is the incoming track's real start. It
  spends the arm's single re-anchor chance, which fires only if a fade is scheduled at or after the
  arm's own start frame and the audio landed within `kReanchorAheadMs = 2500` ms after it or
  `kReanchorBehindMs = 12000` ms before it;
- the arm ends when the audio delivered since the boundary reaches the lead target (plus the fade's
  own length when one is scheduled), or after `kArmCapSeconds = 45` s of audio;
- `kMaxZeroDropMs = 12000` ms of dropped silence in one arm disarms it and keeps the zeros.

`onMetadata` subtracts the padding dropped in the current arm from the eSDK's reported position,
so the natural-transition prediction is not biased by silence that never entered the ring.

### Fade rendering (`RingPipeline::fill`)

A scheduled fade has a boundary frame `b`, an outgoing ramp of `fadeNA_` frames ending at `b`, and
an incoming ramp of `fadeN_` frames starting at `b`. `fill` walks three stages:

1. **Outgoing.** While `read_ < b`, the outgoing tail is mixed with whatever of the incoming has
   been delivered. The outgoing gain index follows `read_ - fadeStart_` over `fadeNA_`; the
   incoming index follows `fadeKB_`, which only advances over frames actually consumed. Missing
   incoming frames are silence and count as an underrun, warned once per fade.
2. **Incoming only.** Once the outgoing tail is spent but the incoming ramp is not finished,
   `read_` parks at `b` so the acknowledgement frontier (`read_ + fadeN_`) keeps the eSDK
   delivering, and the incoming continues its own ramp.
3. **Completion.** When both ramps are done, `read_` jumps to the next unconsumed incoming frame
   and the fade counter increases.

A re-anchor moves `b` to the frame where the real incoming audio starts. Any incoming frames
already mixed are skipped rather than replayed; the outgoing side keeps its current gain and its
ramp is refitted so it still reaches zero at the new `b`; and a fade that had not started yet with
less than `kMinTailMs` of tail left is cancelled outright, so the outgoing simply plays out.

Acknowledgement gate: a normal buffer is released when its end frame is at or before
`frontier() + T_`, where `frontier()` is `read_` plus the scheduled fade's length. `T_` is
`max(X + 1 s, kMinLeadMs)` with `kMinLeadMs = 500`; the ring itself is `kRingSeconds = 30` s, which
covers the 25 s the pipeline asks for at the maximum `kMaxCrossfadeMs = 12000` ms. The ack loop
waits at most 50 ms, less when a probe deadline falls sooner.

## Exported symbols

`native/jni/xfade.map` caps the export set at nine unversioned symbols, five OpenSL and four JNI:

```
SL_IID_ENGINE  SL_IID_PLAY  SL_IID_BUFFERQUEUE  SL_IID_VOLUME  slCreateEngine
Java_..._CrossfadeNative_{getStatus, setCrossfadeMs, onMetadata, setDumpDir}
```

`NEEDED` stays `liblog, libdl, libc, libm`: the real OpenSL ES is `dlopen`ed at runtime, not linked.

## Status line

`CrossfadeNative.getStatus()` returns one line, assembled in `native/src/status.cpp`:

```
state=active rate=44100 ch=2 lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0
```

`state` is `no-player`, `passthrough` (crossfade off) or `active`. The counters after `ch` come
from the live pipeline; with no pipeline registered the line ends at `crossfade_ms`.
`StatusFormatter` in the extension turns this into the one-line summary the settings screen shows.

## Logging

Everything logs under the tag `xfade` (`native/src/log.h`, and the same tag in the Java classes).

```bash
adb logcat -v time -s xfade
```

Info lines worth knowing:

| Line | Meaning |
|---|---|
| `init: native available=... crossfade_ms=...` | the extension loaded the library and pushed the saved length |
| `ring: started rate=.. ch=.. out=.. X=.. T=..` | pipeline start, with the crossfade and lead targets in frames |
| `boundary: natural -> fade at B (announced A, delta +N ms) n=..` | a natural transition and how far the prediction was from the announcement |
| `boundary: skip -> cut at B` / `seek -> cut at B` | a flush resolved by metadata |
| `boundary: flush without metadata -> cut at B` | the flush timed out |
| `ring: fade scheduled at B n=..` | the pipeline accepted a fade |
| `ring: dropped N ms of zeros since the boundary (...)` | the arm closed, with the reason |
| `ring: fade re-anchored to B (was A) outgoing n=.. incoming n=..` | the fade moved onto the real incoming audio |
| `ring: cut at B, skipping N frames` | a cut discarded that much unplayed audio |
| `ring: fade done, ... incoming_consumed=k/n underruns=N` | a fade completed |
| `ring: stopped written=.. read=.. fades=N underruns=N` | player teardown; the counters are that player's lifetime totals |

Warnings: `ring: incoming head not yet delivered` (late incoming audio, those frames were silence,
once per fade), `ring: overflow` (unplayed audio dropped), `ring: zero run cap reached`, and
`pipeline: N-bit PCM is not processed; passthrough`.

## Debug PCM dump

`PcmDumper` keeps the last `kHistorySeconds = 5` s of output and, when triggered, writes them plus
the next `kTailSeconds = 15` s to `<dir>/xfade_dump_<n>.pcm` as raw interleaved 16-bit PCM at the
player's rate and channel count. Cuts trigger it, and so does each fade, once, when it actually
starts rendering. It enables itself only when the system property `debug.xfade.dump` is `1` at
player creation and a directory has been set (the extension sets the app's own files directory).
Reading the file back needs the **Debuggable build** patch so that `run-as` works:

```bash
bash scripts/patch-and-install.sh --debug --install
adb shell setprop debug.xfade.dump 1
adb shell am force-stop com.spotify.tv.android
# play something, let a transition happen, then:
adb shell run-as com.spotify.tv.android cat files/xfade_dump_1.pcm > dump1.pcm
```

Import as raw PCM, signed 16-bit little endian, at the rate and channel count from `ring: started`.
