# Testing

Three layers: native tests that run on the device, JVM tests for the Java extension, and a manual
checklist for what only a person with a remote can judge.

## Native tests on the device

`native/test` builds into a second ndk-build target, `xfade_tests`, a plain ARM executable with a
tiny registration harness (`native/test/test.h`). It links the same sources as the library and
replaces only the output side with `FakeBackend` (`native/test/fake_backend.h`), so the ring, the
boundary model, the mixer and the front end are exercised as built.

```bash
source scripts/env.sh
bash scripts/device-tests.sh              # push and run all 81 tests
bash scripts/device-tests.sh fade_        # only tests whose name contains "fade_"
bash scripts/device-tests.sh --real       # also run the one test that opens the real audio device
```

The script builds, pushes the binary to `/data/local/tmp`, runs it and fails unless the exit code
is 0. Each test prints `[ RUN ]` and `[ OK ]` or `[ FAIL ]` with the file and line of the failing
expectation, and the run ends with `N tests, M failures`.

| File | Tests | Covers |
|---|---|---|
| `test_status.cpp` | 3 | the process-wide status: clamping the crossfade to 0-12 s, the state names, appending a registered pipeline's counters, and a stopping pipeline not silencing a newer one |
| `test_ring_mixer.cpp` | 7 | the ring's absolute-frame addressing and wrap-around, its refusal of impossible sizes, and the equal-power mixer: gains, clamping, independent positions, independent ramp lengths, index clamping at a ramp end, and multi-frame calls matching per-frame ones |
| `test_boundary.cpp` | 15 | every boundary rule: the prediction anchor, natural transitions, a bad duration falling back to the announced frame, skip and seek after a flush, the flush timeout and its late announcement, a newer flush superseding it, ads and episodes cutting while the video flag does not, short tails, and crossfade off |
| `test_fake_sl.cpp` | 7 | the OpenSL front end driven the way the eSDK drives it: creating and realizing a player, passing buffers through, both buffer-queue locators, relaying the back end's result, a repeated realize, destroying an unrealized player, a stray completion after a clear, and an unknown interface |
| `test_ring_pipeline.cpp` | 43 | the pipeline itself: priming and copying, the lead target and the minimum lead, underruns, clear, pause, changing the crossfade live, stats, stop from the ack thread, idempotent stop, and then the fades: a natural transition, the dump trigger firing once per fade, the frontier keeping the eSDK fed, skip and seek cuts, short tails, a stalled or late incoming head, pause mid-fade, a cut cancelling a fade, the padding drop and its probe schedule, the arm's lifetime and caps, and every re-anchor case |
| `test_wiring.cpp` | 5 | what `library.cpp` wires up: the ring pipeline for 16-bit PCM and passthrough otherwise, the fallback when the ring cannot start, `no-player` when the back end fails, and the dumper's history and tail |
| `test_real_backend.cpp` | 1 | plays 0.5 s of a 440 Hz tone through the real OpenSL back end. Skipped unless `--real` is passed |

## JVM tests for the extension

```bash
./gradlew :extensions:extension:testDebugUnitTest
```

Ten tests over the classes that have no Android dependencies:

- `MetadataParserTest` (4): the real metadata shape parses, ad and video flags are read, a missing
  or null `playback_id` yields nothing, and malformed input never throws.
- `LongPressDetectorTest` (4): a short press passes through, a long press fires exactly once and
  then consumes events until the key is released, other keys always pass, and a repeat with no
  preceding down starts a press.
- `SettingsAndStatusTest` (2): the setting clamps to 0-12 s and rounds to whole seconds, and the
  status line formats into the settings screen's summary.

## Manual device checklist

The remaining behaviour needs ears. Install the build, then stream the log in a second shell:

```bash
adb logcat -v time -s xfade | tee xfade.log
```

1. **Natural transition at 5 s.** Set the length to 5 s, play an album, and let a track end on its
   own. The next track should be audible under the outgoing one for about five seconds with no gap
   and no volume step.
2. **Natural transition at 12 s.** Repeat at the maximum. This is the setting that depends on the
   re-anchor, so check the log for a `re-anchored` line and listen for the overlap actually
   lasting.
3. **Skip.** Press next during a track. The change must be instant, with no fade and no audible
   remnant of the outgoing track.
4. **Seek.** Scrub within a track. Audio must resume at the new position immediately.
5. **Crossfade off.** Set the length to 0 and repeat the natural transition: tracks should follow
   each other the way the stock app plays them.
6. **Settings screen.** Long-press Menu for about 0.7 s. The screen should open, Left and Right
   should step the value by a second, the status line should update once a second, and Back should
   close it. Reopen it to confirm the value was persisted.

## Reading the log

A clean natural transition looks like this, in order:

```
metadata playback_id=... uri=spotify:track:... duration=... position=...
boundary: natural -> fade at 12345678 (announced 12300000, delta +1035 ms) n=529200
ring: fade scheduled at 12345678 n=529200 (read=... written=...)
ring: armed (new track)
ring: dropped 2400 ms of zeros since the boundary
ring: fade re-anchored to 12400000 (was 12345678) outgoing n=... incoming n=529200 k0=...
ring: fade done, read=... written=... incoming_consumed=529200/529200 underruns=0 padding_dropped_ms=...
```

What to look at:

- `delta` on the `boundary: natural` line is how far the predicted boundary was from the eSDK's
  announcement. Beyond 2.5 s the prediction is rejected and the announced frame is used.
- `n` is the fade length in frames. Divide by the sample rate from the `ring: started` line to
  check it against the setting.
- `incoming_consumed=k/n` on `fade done`: `k` short of `n` means the incoming audio ran out, which
  is what a short overlap sounds like.
- `underruns` on `fade done` and `ring: stopped` is a lifetime total for that player, not a count
  for the fade.
- `ring: incoming head not yet delivered` is warned once per fade and means some frames of the
  incoming ramp were rendered as silence.
- `boundary: skip -> cut` or `seek -> cut` should appear for every manual change, and never
  `boundary: natural`.
