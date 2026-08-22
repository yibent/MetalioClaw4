# Unattended Lua self-test

Enable `CONFIG_LUA_SELF_TEST_ON_BOOT` for a diagnostic firmware. The task is
started after the display and audio backends are ready, waits past the normal
2-second Boot-to-Home transition, then runs once and returns its Lua-owned
screen to the normal Home screen. It does not require touch input or an app
launch.

The serial tag is `LuaSelfTest`. The useful markers are:

```text
LuaSelfTest: ===== unattended Lua self-test BEGIN =====
LuaSelfTest: CASE_PASS name=...
LuaSelfTest: CASE_SKIP name=uart_loopback ...
LuaSelfTest: ===== unattended Lua self-test PASS: failed=0 skipped=1 ... =====
```

The suite covers the public `runtime`, `ui`, `audio`, `http`, `camera`, `alert`, and `device` Lua modules,
JSON arguments, calling a `main` entry function and encoding its return value
as JSON, standard Lua libraries, timeout/cancellation, job output
truncation, UI object creation/update/delete, synthetic touch queue events,
the UART capability/policy gates, and the HTTP capability/argument gates.
Live HTTP I/O is not part of the unattended suite. It also runs virtual app scenarios named
`virtual_lifecycle`, `virtual_lifecycle_reenter`, `virtual_frame_loop`,
`virtual_invalid_order`, `virtual_crash_cleanup`, `virtual_cleanup_probe`,
`virtual_audio_race`, and `virtual_ui_contention`. These cover page
lifecycle/re-entry, fixed-rate frame loops, invalid call-order recovery,
uncaught-error cleanup, audio channel contention, and two-app display ownership
contention. UART byte-level I/O is intentionally
reported as skipped: the current board wiring has no board-approved unused UART
with a loopback, and claiming a pass would hide a hardware coverage gap.

The virtual-app soak depth is controlled by
`CONFIG_LUA_SELF_TEST_STRESS_ROUNDS`. The diagnostic `sdkconfig` currently uses
20 rounds. Job timeouts scale with this value, so increasing it does not turn a
healthy long run into a false timeout. During long cases, `PROGRESS` lines show
the current page, frame, cycle, or event burst. Every case summary also reports
`heap_delta` to make gradual leaks easier to spot across repeated boots.

The extended scenarios also include:

- `virtual_navigation`: repeated page construction, input, audio, and parent
  teardown while retaining the app's root screen.
- `virtual_event_storm`: bounded input-queue overflow, oldest-event eviction,
  and ordered draining under UI updates.
- `virtual_hierarchy`: nested object trees, cascading parent deletion, stale
  handle rejection, and immediate reconstruction.
- `virtual_audio_interleave`: partial stop, handle-slot reuse, and interleaved
  playback cleanup.
- `virtual_stop_cleanup`: external cancellation while UI and audio are live,
  followed immediately by `virtual_stop_cleanup_probe` to prove both resources
  can be acquired again.
- `virtual_ui_contention`: repeated two-job display ownership races with varied
  entry delays and ownership durations.
- `virtual_job_saturation`: fills the runtime's concurrent job slots, verifies
  deterministic rejection at capacity, cancels every worker, and then lets the
  following cases prove the slots are reusable.

The self-test adds a test-only UI event injector and a test-only live `print`
mirror. Both are capability-gated and are unavailable to ordinary Lua jobs
unless the caller explicitly grants the corresponding capability.

For a test build, configure the correct board first, then run the usual IDF
5.5.4 build/flash commands from an IDF shell. Capture the serial output and
search for `LuaSelfTest`. Do not use app-only flashing if the selected board's
partition/assets layout requires a full flash.
