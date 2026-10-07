# Native boot progress

Updated 2026-10-07. This is the current toolkit status; the older workspace-level
`../PROGRESS.md` contains extraction history and the original roadmap.

## Verified milestone: saved character loads into the village

- Stripped v32 RPX discovery uses direct calls, relocated code references, and
  stack-frame prologues. Conditional branch spans are kept in one body; callable
  interior addresses receive entry wrappers. Cross-body unconditional jumps use
  tail calls. These are code partitions, not recovered source function identities.
- Validation rejects gaps, overlaps, and conditional branches crossing discovered
  body boundaries. The symbol-based v0 partition still validates.
- v32 produces 118,671 bodies and 29,493 extra entry points in 80 C files. Remaining
  generator diagnostics are six unresolved calls and six unsupported words.
- Ninja build generation is automatic, including runtime sources and header
  dependencies. Generated data is required to live outside the repository.
- Runtime heap placement now follows the loaded data extent, avoiding overlap
  with v32 BSS/IPC storage. Foreground heap slot 8 matches Cemu's initialization.
- Pre-relocated data-import aliases are initialized, including default heap
  function pointers. v32 has ten resolved data aliases; v0 has eight.
- v32 builds and boots through constructors, main, worker-thread creation, and
  application initialization, asset decompression, software-keyboard initialization,
  and repeated GX2 draw/copy/swap submissions. The experimental offscreen renderer now displays the game’s controller
  detection message, including its text and border. GamePad detection changes
  the message as expected. Intro skipping reaches the rendered MH3U title
  screen (Ver. 1.02, “Press A”), main menu, and New Game character creation.
  The textured character model and reflection render. Replayed directional and
  A-button input navigates character options and changes gender. Native name
  entry is implemented and a replay returns “Hunter” to the menu; gameplay
  now reaches village dialogue and the normal village HUD. Free movement and
  quests are not yet verified. The character-creation
  inversion and large background triangles are fixed in the latest replay.

## Runtime additions

Threads use one pthread per guest thread and a serialized scheduler token. Guest
execution switches on blocking calls, resume/yield, or periodic HLE safe points.
Implemented: create/resume/join/exit, sleep, thread names and specific data,
priority/affinity bookkeeping, recursive mutexes, auto/manual events, event
timeouts, semaphores, message queues, and the global GHS lock. Process `exit`
now exits instead of returning from a stub.

Synchronous FS supports client/command initialization, command user data,
open/read/write/close/flush, positioned reads, seek, stat, merged directory
enumeration, save-directory creation, and removal. Content is an update-over-base
read-only overlay; save files use an explicitly configured host directory.
Paths are confined with Linux `openat2`. Synthetic files and real game asset
loading are exercised; save/load validation remains.

## Verification

- Thirty synthetic regression tests pass: discovery, compiled control flow,
  relocated import aliases, threaded synchronization/reuse, filesystem operations,
  IPC pools, GX2 layouts/registers, and aperture round-trip/slice isolation.
- v32 quick instruction fuzz: 11,330 states over 1,133 encodings, zero unexplained
  mismatches. Paired-single/trap oracle exclusions and known overrides remain.
- All standalone runtime C files pass Clang `-Wall -Wextra -Werror` syntax checks.
- v32 runs through 1,500,000 import calls into repeated rendering submissions
  with base/update content roots configured. This was a bounded diagnostic run
  (`--max-imports 1500000`), not proof of graphical correctness.
- Current generated build/logs: `/tmp/wiiu-recomp-v32/`. The earlier checkpoint
  in `../mh3u/generated-v32/` predates the branch-table and GX2 changes.
- No instruction semantics in `recomp/insn.py` or `runtime/ppc.h` changed.

## Active milestone: graphics submission and rendering

There is no known user-dependent blocker. Remaining engineering work continues.

Completed since the initial GX2 checkpoint:

- Surface/mip layout and format sizes use the isolated Cemu AddrLib and surface
  algorithms, with MPL-2.0 license and source provenance in `runtime/gx2/NOTICE`.
- TV/DRC sizes, texture/color/depth descriptor registers, and CPU tiling apertures
  are implemented. Apertures transfer data in both directions; MSAA apertures and
  nonzero aperture endian modes are explicitly unsupported.
- Callable entries cover branch-table slots even across stripped code partitions.
  This fixed the indirect dispatch at `020a4944` encountered during boot.
- Dynamic exports resolve against their acquired module, including `.rpl` names.
  Initial error-dialog and software-keyboard lifecycles are present. Keyboard
  worker timing follows Cemu's MH3U-specific initialization behavior. Native
  text entry is implemented; broader system dialogs remain incomplete.
- IPC buffer pools now initialize the embedded keyboard library correctly.
- Joined/detached completed thread objects can be recreated at the same address.
  This fixed the repeating decompression loop during texture loading.

Next graphics work:

1. Complete GPU context/state and texture invalidation support.
2. Exercise later menus using reproducible input and the local save service.
3. Add audio and validate
   gameplay. The original roadmap proposes WebGPU; browser support follows a
   working native runtime.

## Remaining correctness limits

- Thread scheduling is cooperative at HLE boundaries. Priorities and affinities
  are recorded, not enforced. Busy-waits without HLE calls can starve other threads.
  Cancellation, suspend/detach APIs, cleanup callbacks, GHS file locks,
  and complete guest-visible thread/context/wait-queue structures remain.
- Host metadata for completed threads is retained until object reuse. Exiting while owning a mutex
  does not yet perform Cafe's automatic mutex cleanup.
- FS asynchronous APIs, additional account/save-service APIs, notifications, full error-mask
  behavior, and shutdown/client lifecycle are incomplete. Stat timestamps and
  quota metadata remain zero; regular synchronous file I/O is implemented.
- Most non-coreinit OS services still return through the generic stub handler.
  Reaching the graphics checkpoint does not prove their semantics.
- Discovery has boot evidence, not whole-game coverage. Relocation-masked v0
  matching and deeper control-flow/lockstep validation remain useful follow-ups.
- Paired-single oracle coverage and Espresso floating-point quirks remain as
  documented in the older progress log. Audio effects/resampling fidelity and the web backend remain unfinished.

## Graphics work in progress

- Ported Cemu's Latte shader translator with standalone compatibility support.
  Six real-game vertex/geometry/pixel shader variants pass GLSL validation.
- Added PM4 register/display-list decoding and `--gpu-validate OUTPUT_DIR`.
- Added experimental `--gpu-render OUTPUT_DIR`: EGL/OpenGL draws, texture detiling,
  uniform/vertex buffers, color clears, copies, and PPM scanout captures. A bounded
  150,000-import boot executes without GL errors and captures the controller
  detection screen. Uniform buffers use GPU little-endian words, following
  Cemu `LatteBufferCache_LoadRemappedUniforms`; swapping these twice caused the
  initial black frames.
- A synthetic translated pixel shader renders the expected red pixels on real
  offscreen OpenGL. This does not establish correctness of game rendering.
- Indexed draw submission and blending/scissor/channel masks are implemented.
  Still implementing full render state, context restoration,
  invalidation and texture formats; input/audio/gameplay validation remain ahead.
  These are engineering tasks, not a request for user action.
- VPADRead returns neutral connected GamePad samples on channel zero, with
  correct button edges and status layout. Optional `--window` presents via SDL
  and maps keyboard input. Dummy-video presentation and VPAD ABI tests pass;
  interactive desktop control and later game screens remain to be exercised.

## Local account and save startup

- One local account (slot 1, persistent ID `80000001`) and synchronous `nn_save`
  wrappers now implement account/common path resolution, directory initialization,
  open/stat/remove, free space and quota flush. No online account is fabricated.
- The game advances from controller detection to save creation and writes system
  and extended data under `/tmp/wiiu-recomp-test-save/80000001`. Save round-trip
  unit tests pass; full in-game save/load validation is still pending.
- `--input-replay FILE` consumes ordered decimal sample index / hexadecimal button
  mask pairs, with held masks persisting until the next entry. This explicitly
  selected diagnostic input source enables reproducible menu testing.

## Startup services and media

- OS alarms support one-shot/periodic deadlines, cancellation, and user data on
  a dedicated serialized service thread. Deadline/cancellation tests pass.
- AX frame callbacks run every 3 ms. Basic 96-voice allocation, PCM8/PCM16/DSP
  ADPCM decoding, loop/end handling, source-rate advancement and stereo direct
  mixing are implemented; `--audio` enables SDL output. Synthetic PCM/ADPCM,
  looping/completion tests pass. This is not a complete DSP implementation:
  priority stealing, auxiliary effects, filters, and accurate resampling remain.
- GX2 flush/draw-done/timestamps and CPU texture invalidation are implemented.
- File tracing (`WIIU_TRACE_FS=1`) identified the later black stage as movie
  startup. H264 decoder APIs now use FFmpeg with NV12 conversion, aligned rows,
  frame timestamps, and output callbacks. A generated red-video test passes;
  the game decodes real movie frames and accepts intro skipping. Continuous
  playback still needs visual verification. Per-frame mode uses FFmpeg’s public
  decoded-band callback to deliver decode order without waiting for presentation
  reordering.
- Full synthetic suite: 26 tests pass.
- GX2 swap status now writes both flip counters and timestamps. Leaving these
  outputs uninitialized caused the game to request a very long sleep. With this
  fixed, the native renderer reaches the title screen; capture:
  `/tmp/wiiu-recomp-frames-swap/frame-1000.ppm`.

## Character creation checkpoint

- Replayed menu input selects New Game and reaches character creation. A bounded
  2,500,000-import run ends at its requested diagnostic limit without GPU errors.
  Capture: `/tmp/wiiu-recomp-frames-banks/frame-latest.ppm`.
- Added 1D R32F lookup textures, full uniform-bank uploads for dynamic shader
  indexing, depth surfaces/clears/testing, face culling, and blend constants.
  Missing uniform banks had made the character model invisible; it now renders
  with textures and reflection.
- A synthetic backend test verifies real GL pixel results for uniform-bank data,
  1D float sampling, depth rejection/clear, CPU texture invalidation, and a color
  copy after depth attachment. This complements the shader translation tests.
- Next milestone: completing character creation and validating in-game
  rendering/save-load/audio. Broader GPU state
  and OS fidelity work remains. No user-dependent blocker is currently known.

## Desktop presentation crash fixed

- Reproduced the user's `--window --audio` crash under GDB: the first SDL window
  presentation entered NVIDIA EGL with the runtime's offscreen context still
  current. The crashing stack passed through SDL3 (SDL2 compatibility) and
  `libnvidia-eglcore.so.610.57.04`.
- Scope offscreen readback so its EGL context is released before SDL presentation.
  Rebuilt `/tmp/wiiu-recomp-v32/game-native`; the original desktop/audio launch
  now runs through the title and main menu with replayed input. The 55-second
  bounded window check captured `/tmp/wiiu-window-fixed/frame-700.ppm` without
  a signal or GPU error. Desktop character creation was not reached in that run.
- The backend regression asserts that no EGL context is current at the window
  handoff and verifies subsequent rendering reacquires it. All 26 tests pass.

### Follow-up: visible window and audio timing

- Simply unbinding EGL avoided the crash but did not preserve SDL's context.
  Offscreen operations now save and restore the previous display/context/read
  and draw surfaces. The backend test exercises a second context across two
  presentations and intervening rendering. A compositor screenshot of the
  actual game window confirms visible output, also confirmed by the user.
- The automated desktop run intentionally used input replay and no audio;
  keyboard buttons were overridden and silence was expected in that run. It
  finished at the 2,500,000-import diagnostic limit. Manual launches must omit
  `--input-replay` and include `--audio` to test sound.
- Audio tracing exposed service-clock drift: 900 ms of generated PCM took
  3–4 seconds of wall time. Service deadlines now advance on a fixed timeline
  instead of sleeping a fresh period after each callback. Audio also buffers
  48 ms before starting the device. A dummy-device run now generates 12,600 ms
  of PCM in 12,595 ms; occasional empty queues remain under rendering load.
  Audible quality is not yet verified. `WIIU_TRACE_AUDIO=1` reports queue timing.
- All 26 regression tests and strict standalone C syntax checks pass. Window
  performance remains slow; repeated shader translation and synchronous GPU
  readback/upload paths still need profiling and optimization.

## Intro movie visual loop fixed

- Reproduced repeated opening movie images while decoder timestamps advanced
  beyond 90 seconds. The game was not restarting: cached GPU textures retained
  the initial contents of recycled H264 output buffers.
- Completed NV12 decoder writes now invalidate overlapping cached textures
  before notifying the guest. Regression coverage checks the decoded buffer
  notification address/size; the render backend already tests invalidation and
  replacement pixels. All 26 tests pass.
- A 45-second unskipped playback check decodes over 1,050 frames (35 seconds
  of movie time) and visibly progresses through later underwater scenes rather
  than repeating the opening fish. Full 129-second movie completion remains
  unverified. Capture: `/tmp/wiiu-intro-fixed/frame-latest.ppm`.
- `WIIU_TRACE_VIDEO=1` logs periodic decoded timestamps and decoder errors.
  Rebuilt `/tmp/wiiu-recomp-v32/game-native`; existing processes must restart
  to use this fix.

## Keyboard delivery and movie performance

- SDL event polling is confined to the window's owner thread. Other guest
  threads read its serialized input snapshot. Button presses are retained for
  at least one VPAD sample, preventing a press/release between slow frames from
  disappearing. Focus loss clears held state. A real SDL event-queue test covers
  quick taps, repeats, held buttons/sticks, release, and a guest reader thread.
- The SDL window now prefers hardware-accelerated presentation, falling back to
  software. An offscreen SDL/OpenGL integration test verifies repeated
  presentation and restoration of its EGL context with the actual presenter.
  Physical desktop keyboard interaction still requires user confirmation.
- Linear video surface reads copy rows rather than calculating each byte's
  address. Tests cover row padding, array slices, formats, and bounds. Opt-in
  `WIIU_TRACE_GPU` profiling measured average uploads falling from roughly
  4.9 ms to 0.8 ms; 1,320 offscreen presentations took 15.83 seconds
  versus 20.67 before. These are local runs, not a desktop FPS guarantee.
- `WIIU_TRACE_INPUT=1` logs delivered held/pressed/released masks. The executable
  at `/tmp/wiiu-recomp-v32/game-native` has been rebuilt with these changes.
- All 29 regression tests and strict standalone C syntax checks pass.

## Focus-change follow-up

- Initially reported as focus-related; the user subsequently confirmed that
  controls fail on entering character creation even without switching focus.
- Clear pending presses as well as held keys on both focus loss and gain.
  Regression coverage now verifies fresh input after refocusing and discarding
  a press interrupted by focus loss. Focus transitions are logged with
  `WIIU_TRACE_INPUT=1`. Rebuilt the native executable; all 29 tests pass.
- The user's character-creation screenshot also shows rendering corruption;
  matching corruption exists in the internal PPM capture, ruling out an
  SDL-only presentation issue. That rendering issue remains unresolved.

## Character-creation input lockup fixed

- Reproduced with recorded A and D-pad input, without focus changes or selecting
  Name. Debugger checks confirmed VPAD presses reached the game's input buffers.
- `VPADGetTPCalibratedPoint` had been a return-only stub. The game passed stack
  storage as its output and subsequently consumed unwritten touch coordinates,
  touch state, and validity. Implemented Cemu's
  `vpadExport_VPADGetTPCalibratedPoint` / `_tpRawToResolution` behavior, including
  in-place input/output. This restores menu navigation in the same replay.
- Verified offscreen captures: repeated Down presses reach Voice; a separate
  Down/A/Right/A sequence changes the character from male to female. The native
  executable at `/tmp/wiiu-recomp-v32/game-native` is rebuilt. Name entry remains
  a separate unfinished software-keyboard feature, and graphics corruption is
  still present.
- WPADProbe/KPADReadEx now report absent Wii controllers rather than leaving
  type/error outputs untouched (Cemu `padscoreExport_WPADProbe` / `_KPADRead`).
  This correction alone did not fix the character-creation lockup.
- Input replay accepts optional left-stick x/y values after the button mask.
  Regression tests cover these samples, stale touch output, in-place calibration,
  disconnected controllers, and SDL focus transitions. All 29 tests and strict
  standalone C syntax checks pass.

## Character-creation rendering orientation fixed

- The offscreen backend rendered to textures in OpenGL bottom-up order while
  uploaded guest textures used top-down order. Sampling a rendered surface
  therefore inverted it; flipping only the final presentation could not correct
  intermediate passes. Follow Cemu `LatteRenderTarget_updateViewport` and
  `OpenGLRenderer::renderTarget_setViewport`: use clip-control origin according
  to viewport Y scale, guest viewport/scissor coordinates, and the guest clip
  depth convention. SDL presentation and PPM output now consume guest row order.
- The same character-creation replay now displays a single upright model and
  intact background. Verification capture:
  `/tmp/wiiu-character-origin/frame-latest.ppm`. This confirms the reported
  corruption is fixed in that scene, not overall graphics completeness.
- Also implemented the previously missing GX2SetDepthStencilControl and
  GX2SetStencilMask calls, stencil comparisons/operations and write masks,
  back-face state fallback, stencil-only attachments, and disabled color writes.
  These follow Cemu `GX2_State.cpp` and OpenGL render-state handling. Stencil
  support alone did not resolve the reported inversion.
- Synthetic pixel tests cover stencil masking, color-write suppression,
  independent back-face comparison, and an asymmetric render-to-texture round
  trip that catches vertical inversion. All 29 tests pass, including accelerated
  SDL/EGL context handoff. Rebuilt `/tmp/wiiu-recomp-v32/game-native`.
- Rendering now requires ARB_clip_control or OpenGL 4.5+, checked explicitly at
  initialization. Name entry remains the next unfinished interaction milestone.


## Name selection exit fixed; native text entry implemented

- Selecting Name called unresolved `SwkbdSetReceiver` at guest LR `0293bb04`.
  The keyboard path also required `SwkbdGetKeyboardCondition`. Implemented
  these exports and the form/keyboard lifecycle, UTF-16 guest buffers, receiver
  change callback, confirmation and cancellation, following Cemu’s swkbd.cpp.
- An SDL text overlay accepts hardware typing, Enter to confirm, Esc to cancel,
  and Backspace to delete a Unicode code point. It respects the game’s name
  length limit and disables game controls during editing. SDL2_ttf plus a
  DejaVu Sans or Liberation Sans font are now build/runtime dependencies.
- `--keyboard-replay FILE` supplies UTF-8 names to headless regression runs.
  The v32 replay accepted “Hunter” and displayed it in character creation
  without the unknown-target fatal error. A subsequent Down press selected
  gender with “Hunter” retained and the dialog closed. Capture:
  `/tmp/wiiu-name-verified/frame-latest.ppm`. This verifies the guest name handoff;
  completing creation, gameplay, and save/load still need validation.
- Thirty regression tests pass, including receiver callbacks and register
  preservation, guest buffer termination, confirm/cancel state, UTF-16 limits,
  surrogate-pair deletion, disabled confirmation, and game-input suppression.
  All standalone C sources pass strict Clang syntax checks. The native binary
  at `/tmp/wiiu-recomp-v32/game-native` is rebuilt.
- Text editing currently supports append/backspace; guest cursor/selection
  APIs and IME composition display are not implemented.


## Debug overlay and user-confirmed character creation

- User confirms character creation completed. In-game play and save reload are
  not yet verified.
- Added F3 to toggle an SDL debug overlay and `--debug-overlay` to enable it at
  startup. Shows presented FPS, rolling 120-frame average/max frame intervals,
  presentation and VPAD read counts, focus, input source, text-entry state,
  held buttons/stick, and the last delivered game press with elapsed time.
- Text textures are cached, refreshed at 4 Hz or on a new press. F3 remains
  available during text entry and never becomes a guest button. Timing measures
  presentation intervals, not GPU execution. A stalled presenter also stalls
  the overlay; it is not a watchdog.
- Regression coverage checks actual overlay pixels, F3 on/off, key repeat and
  key-up handling, text-entry isolation, startup enablement, and accelerated
  SDL/EGL context handoff with the overlay enabled.
- User also reported an OS “Application Not Responding” dialog. The process had
  already exited when inspected; the launch log ends in input/focus events with
  no fatal diagnostic. Cause remains unconfirmed pending the triggering action
  or a live stalled-process capture.


## Village transition: render-target reuse fixed

- User narrowed the nonresponse to the end of the village introduction after
  character creation. A copied `user1` save loads into Character Select; replay
  then reproduced a renderer fatal during village loading, with guest LR
  `02bc0044`: framebuffer status `0x8cd6` (incomplete attachment).
- The recorded draw selected address `15bd0000` as a 480×270 RGBA color target
  (`CB_COLOR0_INFO=08100468`, `CB_COLOR0_SIZE=001fdc3b`), while the descriptor
  cache still associated it with a 1024×1024 D24S8 depth image. No new host
  color-buffer registration accompanied the recorded command's address change.
- Color draws now resolve the active view from color registers, following Cemu
  `LatteMRT::GetColorAttachmentTexture`. Incompatible descriptor changes invalidate
  the cached GL image, including switching an allocation back to depth. Deletion
  is deferred until the GL context is current.
- Color attachment activation now combines pixel shader outputs, channel masks,
  and SPECIAL_OP as in Cemu `LatteMRT::GetActiveColorBufferMask`. Depth/stencil-only
  passes attach no color image; subsequent color passes restore read/draw buffers.
- Regression tests verify depth/stencil-only rendering, register-only selection
  of an allocation previously used for a larger depth image, real color pixel
  results, and conversion of that allocation back into a valid depth target.
  All 30 regression tests pass. The rebuilt native executable passes the earlier
  fatal and renders the village with the character and Village Chief dialogue.
- Diagnostic saves and captures remain under `/tmp/wiiu-village-*`; the user's
  `/tmp/wiiu-recomp-test-save` was only read and copied for these replays.

- The final bounded replay advanced the Village Chief's dialogue and dismissed
  it into the normal village HUD with NPC interaction prompts. Capture:
  `/tmp/wiiu-village-move/frame-latest.ppm`. It ran for its configured 240 seconds
  without a runtime fatal; the timeout stopped it before the scheduled stick
  movement, so free movement remains unverified. Thirty tests pass, including
  the strengthened 32×32 depth → 16×16 color → 32×32 depth reuse test.


## End-of-session checkpoint

- Native executable: `/tmp/wiiu-recomp-v32/game-native`.
- User save: `/tmp/wiiu-recomp-test-save`; diagnostic runs used copied saves.
- F3 toggles the debug overlay; `--debug-overlay` starts it visible.
- Next verification: free movement after Village Chief dialogue, quests, saving
  and reloading after play, and audible in-game output. Longer play may expose
  further missing GPU or OS behavior.
- Latest checks: all 30 regression tests passed, with a subsequent targeted pass
  for the strengthened render-target reuse test. No instruction semantics changed.


## User performance baseline

- User screenshot after the village transition shows 8.3 presented FPS at
  1920×1080, with 120.7 ms average and 146.1 ms maximum frame interval.
  Frame counter 2019, VPAD reads 2020. The window was unfocused at capture.
- The character is visible at a different village position with the normal HUD.
  Next session should profile active play before optimizing. Shader translation
  currently runs for each draw in `validate_draw`; measure its share alongside
  texture upload, draw, and presentation costs using `WIIU_TRACE_GPU=1`.
