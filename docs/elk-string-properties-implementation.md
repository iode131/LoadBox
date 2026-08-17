# Implementation summary: IR/NAM loading via Elk VST3 string properties

Implemented 2026-08-17. Uncommitted at the time of writing.

Adds two writable string properties, `ir_file_left` (id 100) and `ir_file_right`
(id 101), so `LoadBoxVST3Headless` can be given IR/NAM files over gRPC or OSC
without a GUI. Design rationale and the survey of alternatives are in
[headless-loading.md](headless-loading.md); usage is in section 4a of that file.

## What's in

| File | |
|---|---|
| `LoadBox/wrapper/vst3/elk_extensions.h` | **new** — travesty-style declarations of `IElkControllerExtension` + `IElkComponentHandlerExtension`. No Steinberg SDK, no makefile change. |
| `LoadBox/wrapper/PluginAPI.h` | `StringPropertyInfo`, `IStringPropertyListener`, five defaulted `IPluginClient` virtuals — VST2/CLAP untouched. |
| `LoadBox/wrapper/vst3/VST3Wrapper.cpp` | `wrap_elk_controller_ext` boxed off the controller; `set_component_handler` now stores the handler and queries it for the notification interface. |
| `LoadBox/plugdef/LoadBox.cc` | Staging + `pumpPendingFiles()` + service thread + publish loop + property accessors. |
| `LoadBox/plugdef/PluginClient.cc` | Forwarding. |
| `sushi_configs/empty.json` | Fixed the stale `/home/max/Work/LoadBox/` path. |
| `docs/headless-loading.md` | Usage section and the host-side limitations below. |

All four formats build clean (`vst3`, `vst3headless`, `clap`, `vst2`).

## Verified against Sushi

Run as `sushi -d -c sushi_configs/empty.json --log-level=info --log-flush-interval=1`,
driven with elkpy. Note the log needs a flush interval or the interesting lines
stay buffered.

- `Plugin supports Elk Controller Extension`, then
  `Registered string property "ir_file_left"` / `"ir_file_right"` — ids 100/101,
  no collision with parameters 0–5.
- gRPC round-trip: load, read back the confirmed path, both slots independently,
  `"None"` clears.
- Bad paths (missing file, wrong extension) rejected — value unchanged.
- **State restore** via `SetProcessorState` binary_data loads both slots,
  confirming the `readState` re-routing.
- **Notifications** arrive with the confirmed path (20 over the test run).
- 12 rapid alternating sets settle on the last request with nothing dropped — the
  busy-loader retry works.
- **Audio actually changes**: with both slots loaded, the processor's cost in
  `process()` goes from 0.029% to 0.757% of the buffer, a 26× jump. That's the
  convolver running. Measured with `ProgramController`-adjacent timing calls
  (`c.timings.get_processor_timings`) under `--timing-statistics`, using a
  24000-sample IR.

## Two deviations from the plan

**Loads now apply immediately.** `setFileName` attempts the handover inline
instead of waiting up to 100 ms for the service tick, with a `pumpMutex` so only
one thread pumps at a time. The claim-idle window between
`xrworker.getProcess()` and `runProcess()` is what makes writing the engine's
strings safe there, regardless of caller. The service thread is now purely
retry-and-publish.

**Plan step 5 was checking the wrong field.**
`AudioGraphController::get_processor_state` never calls the plugin's
`save_state()` — it builds its reply from bypass, program, parameters and
properties. `binary_data` comes back empty for *any* plugin; confirmed against
the pre-change binary from git, so it is not a regression. The IR paths do appear
in that reply under `properties`.

## Two host-side limitations worth knowing

- **You cannot set the IR in the JSON config.** `Vst3xWrapper::set_state` handles
  `binary_data`, `program`, `bypassed` and `parameters` but never reads
  `state->properties()`, so `initial_state` properties are parsed and silently
  dropped for VST3. The IR has to be set over gRPC/OSC after Sushi is up. This is
  also why an offline-render A/B test comes back bit-identical.
- **A rejected path is invisible to the client.**
  `ParameterController::set_property_value` queues an event and returns `OK`
  before the plugin is ever consulted, so the VST3-level rejection only shows up
  in the Sushi log and via read-back. The plan called this a "synchronous error" —
  true at the VST3 layer, not observable through Sushi.

## Repo hygiene

Tracked build artifacts (`.so`, `.a`, `.o` under `LoadBox/` and `bin/`) show as
modified, since this repo commits them. `make clap` / `make vst2` also leave new
untracked binaries from verification.
