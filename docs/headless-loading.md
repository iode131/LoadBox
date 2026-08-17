# Loading IRs and NAM models in the headless VST3 build

Investigation notes for `LoadBoxVST3Headless` — how the GUI build loads files, what
survives without the GUI, and which mechanisms are candidates for replacing the
file picker.

Reference points: this repo at commit `4f657d1` ("removing gui"), and Sushi at
`~/Work/sushi` commit `08f24e9c`.

## 1. How the GUI build does it

The whole thing funnels through two `std::string`s on the engine plus one non-RT
worker thread. The GUI is a thin front end over that:

1. Xputty file dialog / combobox writes the chosen path into
   `ModelPicker.filename` (`LoadBox/gui/widgets.h:33`; dialog opened at
   `LoadBox/gui/widgets.cc:1229`).
2. The C callback `sendFileName(X11_UI*, ModelPicker*)` calls
   `LoadBox::sendFileName()` (`LoadBox/plugdef/LoadBox.cc:316`). It filters on
   `.nam` / `.wav` / `.WAV`, writes the path into `engine.ir_file` (left,
   `m->model == 3`) or `engine.ir_file1` (right, `== 4`), bumps `engine._cd` by
   1 or 2 to mark which slot is dirty, and sets `workToDo`.
3. A 60 ms timer thread (`fetch.startTimeout(60)`, `LoadBox.cc:101`) runs
   `runGui()` → `checkEngine()` (`LoadBox.cc:189`), which sees `workToDo` and
   hands the job to the loader thread: `engine.xrworker.runProcess()`.
4. `Engine::do_work_mono()` (`LoadBox/engine/engine.h:283`) reads `_cd` and calls
   `setIRFile()` on the dirty slot(s) — loading a NAM through
   `NeuralModelLoader` or configuring the FFT convolver from a WAV — then sets
   `_notify_ui` so the GUI can echo the loaded path back.

**The contract for a load is therefore: write a path string, set `_cd`, kick
`xrworker`.** Everything else in the GUI is decoration.

## 2. What still works headlessly today

`readState()` (`LoadBox.cc:356`) performs the same three steps *and kicks
`xrworker` itself* (`LoadBox.cc:397-400`), so it never depended on the GUI timer.
Its format is:

```
[CONTROLS] <gainL> <gainR> <normL> <normR> <bypass> <irmode> <irmix> <master> |[IrFile] /path/a.wav|[IrFile1] /path/b.nam|
```

The `[IrFile]` / `[IrFile1]` entries alone are enough — the parser skips missing
keys. Paths with spaces are fine (`remove_sub` on the whole line, not a
whitespace split). The `|` terminators are required.

`readState` is wired to VST3 `IComponent::setState` and
`IEditController::setComponentState` (`LoadBox/wrapper/vst3/VST3Wrapper.cpp:650`
and `:410`), and Sushi calls both (`sushi/src/library/vst3x/vst3x_wrapper.cpp:1147`,
`_set_binary_state`). Sushi exposes that as `AudioGraphController.SetProcessorState`
with the `binary_data` field (`sushi/rpc_interface/protos/sushi_rpc.proto:579`),
i.e. `elkpy.audio_graph.set_processor_state()`.

**So there is already a working load path with zero plugin changes**: read the
state back with `get_processor_state`, replace `binary_data`, push it with
`set_processor_state`. (elkpy's `ProcessorState` is constructed from a gRPC
object, so round-tripping an existing state is easier than building one from
scratch.)

Important gap: `checkEngine()` lives inside `#ifndef HEADLESS`, so in the
headless build **nothing services `workToDo` any more**. `readState` is the only
thing that can currently trigger a load. Any new mechanism must kick `xrworker`
itself, or re-add a low-priority timer thread — `ParallelThread::startTimeout`
(`LoadBox/engine/ParallelThread.h:175`) needs no X11.

## 3. Candidate mechanisms

### 3.1 Elk VST3 string properties — best fit for Sushi

Sushi supports string-valued controls for VST3 through Elk's own extension
interface `IElkControllerExtension`
(`sushi/third-party/elk-plugin-extensions/include/elk_vst3_extensions/elk_vst3_extensions.h:87`).
Implement `getPropertyCount` / `getPropertyInfo` / `getPropertyValue` /
`setPropertyValue` on the edit controller and Sushi's `_register_properties()`
(`vst3x_wrapper.cpp:908`) picks them up automatically, exposing them as named
properties over:

- gRPC — `ParameterController.SetPropertyValue` / `GetPropertyValue`,
  `elkpy.parameter.set_property_value()` (`elkpy/src/elkpy/parametercontroller.py:471`)
- OSC — `/property/<processor>/<property>` (`sushi/src/control_frontends/osc_frontend.cpp:428`)
- change notifications back out to both

Declare e.g. `ir_file_left` and `ir_file_right`. `setPropertyValue` is documented
as being called from a non-RT thread, so it can set `ir_file`, bump `_cd` and
kick `xrworker` directly.

Implementation notes:

- The plugin uses `travesty`, not the Steinberg SDK, so the vtable struct is
  hand-rolled rather than inherited. The pattern already exists: boxed
  sub-objects returned from `query_interface_component`
  (`VST3Wrapper.cpp:587-604`). Copy that shape and return the new object from
  `query_interface_controller` (`VST3Wrapper.cpp:385`).
- IID: `V3_ID(0x1016CCA4, 0x930B4F58, 0x83918C4F, 0x8C4F99AB)` — travesty's
  `V3_ID` takes the same four words as Steinberg's `DECLARE_CLASS_IID` and
  handles platform byte order (`travesty/base.h:75`).
- Sushi queries the extension off the *controller* object
  (`sushi/src/library/vst3x/vst3x_host_app.cpp:366`).
- Property IDs must not collide with parameter IDs (0–5 are in use); use e.g.
  100 / 101.
- Roughly 100 lines in the wrapper plus two methods on `IPluginClient`. Hosts
  that don't know the interface simply fail the `queryInterface` and ignore it.

### 3.2 VST3 state as-is

Free, works today, and survives session save/restore. Downsides: opaque blob to
any UI; all-or-nothing (can't set just the left IR without also pushing gains);
the wrapper's read buffer is a fixed 4096 bytes (`VST3Wrapper.cpp:414`, `:654`).
Good as the immediate unblock and as the persistence layer regardless of what
else gets added.

### 3.3 Stepped parameter over a scanned directory

Scan e.g. `~/.config/LoadBox/irs/` at instantiation and register `IR Select L/R`
as stepped parameters with `step_count = n-1`, returning file names from
`get_parameter_string_for_value` (already implemented,
`VST3Wrapper.cpp:468`). Works in every VST3 host with no new interfaces,
automatable, readable as text via `GetParameterValueAsString`. Downside: the
list is frozen at instantiation (hosts cache parameter info) and files outside
the scanned directory are unreachable.

### 3.4 VST3 programs via `IUnitInfo` (+ MIDI Program Change)

Same directory-scan idea exposed as a program list. Sushi's
`_setup_internal_program_handling()` (`vst3x_wrapper.cpp:903`) requires a
parameter flagged `kIsProgramChange` plus `IUnitInfo`; you then get
`ProgramController.SetProcessorProgram` over gRPC/OSC **and** MIDI PC routing for
free — relevant for footswitch/pedal control. `travesty/unit.h` is already in
the tree.

### 3.5 `.vstpreset` files

If a plugin exposes no internal programs, Sushi falls back to scanning
`~/.vst3/presets/<vendor>/<plugin>/*.vstpreset` and exposes each file as a
program (`vst3x_wrapper.cpp:939`; load at `:499`). Zero plugin code, but it
requires emitting valid VST3 preset containers and gives *presets*, not
arbitrary file loading.

### 3.6 Host-independent side channel inside the plugin

Restart a `ParallelThread::startTimeout(60)` worker that polls a control file
(e.g. `~/.config/LoadBox/<instance>.load`) or listens on a Unix socket / OSC
port, and drive the same three-step contract. Literally "replace the GUI with a
file"; works in any host, including bare `sushi` with no gRPC client. Costs:
instance addressing when more than one plugin is loaded, and it sits outside
host state — though since the engine owns `ir_file`, `saveState` picks the path
up anyway, so recall still works.

## 4. Recommendation

Use **3.1 (Elk string properties)** as the real mechanism and keep **3.2 (VST3
state)** as the persistence path. That combination is native to the deployment
host, gives human-readable named controls over both gRPC and OSC, and needs no
directory pre-scan or fixed file list. Add **3.4** later if MIDI/footswitch
program stepping is wanted.

## 4a. Implemented: `ir_file_left` / `ir_file_right`

Option 3.1 is in the tree. Two writable string properties:

| property | id | slot |
|---|---|---|
| `ir_file_left`  | 100 | `engine.ir_file`  (left) |
| `ir_file_right` | 101 | `engine.ir_file1` (right) |

Set either to an absolute path ending in `.nam` / `.wav` / `.WAV`, or to `"None"`
to clear the slot. Reading a property reports the name the **loader thread
confirmed**, so a load that failed reads back as `"None"`.

```python
from elkpy import sushicontroller as sc
c = sc.SushiController('localhost:51051', '<path>/sushi_rpc.proto')
pid = c.audio_graph.get_processor_id('LoadBoxVST3Headless')
props = {p.name: p.id for p in c.parameters.get_processor_properties(pid)}
c.parameters.set_property_value(pid, props['ir_file_left'], '/path/to/ir.wav')
c.parameters.get_property_value(pid, props['ir_file_left'])   # confirmed path, or 'None'
```

Over OSC the same control is `/property/LoadBoxVST3Headless/ir_file_left <path>`
(the processor name comes from the JSON config). `SubscribeToPropertyUpdates`
receives the confirmed value once the load completes.

Files touched: `LoadBox/wrapper/vst3/elk_extensions.h` (new, travesty-style
declarations of the two Elk interfaces), `wrapper/PluginAPI.h` (format-agnostic
`StringPropertyInfo` / `IStringPropertyListener` contract),
`wrapper/vst3/VST3Wrapper.cpp` (`wrap_elk_controller_ext`, plus
`set_component_handler` now storing the handler), `plugdef/LoadBox.cc`
(staging + `pumpPendingFiles` + service thread), `plugdef/PluginClient.cc`.

Behaviour worth knowing:

- A set is **accepted, not applied**. The load runs on the engine's existing
  `xrworker` thread. It is normally handed over immediately, but if the loader is
  mid-load the request stays staged and a 100 ms service thread retries.
- A rejected path (bad suffix, unreadable file) fails at the VST3 layer, but
  **Sushi cannot report that to the client**: `ParameterController::set_property_value`
  queues an event and returns `OK` before the plugin is consulted. The rejection
  is visible only in the Sushi log and through the read-back.
- The service thread also revives the normalisation and clear-slot paths in
  `sendValueChanged`, which had been dead since the GUI was removed.

### Host-side limitations found while testing

- **`initial_state` cannot set a property on a VST3 plugin.**
  `Vst3xWrapper::set_state` handles `binary_data`, `program`, `bypassed` and
  `parameters`, but never reads `state->properties()`
  (`sushi/src/library/vst3x/vst3x_wrapper.cpp:534`). The JSON config parses the
  section happily and the values are silently dropped, so the IR cannot be
  configured at startup — it has to be set over gRPC/OSC once Sushi is up.
- **`GetProcessorState` never returns the plugin's own state blob.**
  `AudioGraphController::get_processor_state` builds its reply from bypass,
  program, parameters and properties, and does not call the processor's
  `save_state()` (`sushi/src/engine/controller/audio_graph_controller.cpp:164`).
  So `binary_data` comes back empty regardless of the plugin — verified against
  the pre-change binary too. The IR paths do show up in that reply, under
  `properties`. `SetProcessorState` with `binary_data` does work, and still
  loads both slots.

## 5. Defects and rough edges found along the way

- ~~`checkEngine()` is compiled out in headless, so `workToDo` is dead.~~
  **Fixed** in 4a: `serviceEngine()` is now unconditional and driven by a service
  thread in the headless build, so the normalisation and clear-slot paths in
  `sendValueChanged` work again.
- Sushi calls `setComponentState` *and* `setState` with the same blob
  (`vst3x_wrapper.cpp:1158-1162`), and both map to `readState`
  (`VST3Wrapper.cpp:410`, `:650`). So `readState` runs twice per state set and
  `_cd` accumulates past 2, reloading both slots every time. Harmless but it
  doubles load time on large IRs/NAMs.
- `setIRFile` uses `bufsize`, which is 0 until the first `process()` call. A
  state set arriving before audio starts configures the convolver with
  `_head = 1` (`LoadBox/engine/fftconvolver.cpp:250-253`). Worth gating the load
  on `bufsize > 0` and re-triggering once it is known.
- Only six parameters are registered (`LoadBox.cc:69`): Enable, IR Out Gain L/R,
  IR Mode, IR Mix, Master. IR normalisation and slot-clear are not parameters,
  so they are unreachable headlessly through any mechanism until they are
  registered or covered by a property.
- `LoadBoxClient` does not override `onParameterChanged`
  (`LoadBox/wrapper/PluginAPI.h:112`), so there is currently no hook for
  parameter-driven side effects such as a file-select parameter.
