# Wii U static recompiler

PowerPC RPX-to-C translator and an experimental Linux runtime. The current native
boot reaches MH3U character creation and loads a saved character into the village
through an experimental OpenGL backend. Village dialogue and the normal HUD are
verified. Free movement, quests, and complete audio remain unverified.
See [PROGRESS.md](PROGRESS.md) for verified milestones and remaining work.

Keep RPX files, extracted content, generated C, and build products outside this
repository. The generator rejects output directories inside the repository.

## Build and boot

Requires Python 3.9+, Clang with C++20, Ninja, zlib, fmt, EGL and OpenGL
development headers/libraries, SDL2, SDL2_ttf and FFmpeg (libavcodec/libavutil/libswscale)
development files, and Linux with
`openat2` support (5.6+). Native text entry also needs DejaVu Sans or Liberation Sans installed. Unicorn and Capstone are needed for instruction fuzzing.

```sh
python3 -m venv .venv
.venv/bin/pip install ninja unicorn capstone

.venv/bin/python -m recomp.program /path/to/update/code/game.rpx /path/to/generated
.venv/bin/ninja -C /path/to/generated -j 8

/path/to/generated/game-native /path/to/update/code/game.rpx \
  --content-base /path/to/base/content \
  --content-update /path/to/update/content \
  --save-dir /path/to/existing-save-directory \
  --stop-before gx2.GX2CalcSurfaceSizeAndAlignment
```

The executable must be run with the **same RPX it was generated from**. It does
not yet check that identity. Output includes `build.ninja`, so separate handwritten
compile/link files are no longer necessary. Ninja tracks header dependencies.

`--stop-before library.function` exits successfully with a `BOOT CHECKPOINT`
message when that import is reached. This is a boot regression check, not proof
that earlier imports are all implemented. `--strict-imports` stops at the first
missing import; otherwise missing imports are logged and return zero.
`--quiet` disables the import trace. An optional hexadecimal entry address is
accepted for debugging.

Content reads prefer the update root and fall back to the base root. Only the
explicit save root permits writes. All roots must already exist. If omitted,
the corresponding filesystem mount is unavailable. Guest paths cannot traverse
outside those roots or follow symlinks.

## Tests

Synthetic regression tests require no game data. Shader tests also use
`glslangValidator` and a working EGL OpenGL 4.3 implementation with
`ARB_clip_control` (or OpenGL 4.5+):

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
```

Instruction verification with a local RPX:

```sh
.venv/bin/python tests/fuzz_insn.py /path/to/game.rpx --per-form 8 --states 10
```

See [AGENTS.md](AGENTS.md) for the thorough fuzz command and repository rules.

`--gpu-validate /outside/repo/shaders` captures translated GLSL from actual draws.
`--gpu-render /outside/repo/frames` enables experimental offscreen rendering and
PPM frame captures. Rendering is incomplete; this is a diagnostic mode. Mesa's
software renderer can run without a display. `--max-imports N` bounds a diagnostic
boot (it exits with a diagnostic error when the limit is reached).

Add `--window` to `--gpu-render` for an SDL window. Keyboard mapping: Z/X/C/V
are A/B/X/Y; arrows are the D-pad; WASD is the left stick; Q/E are L/R;
1/3 are ZL/ZR; Enter is Plus and Backspace is Minus. Closing the window exits.
For MH3U, Enter skips the opening movie, but the title's **Press A** prompt and
menu confirmation require **Z**. Repeated Enter presses do not confirm menus.
Selecting the character’s Name field opens native text entry after the game’s
notice: type normally, press **Enter** to confirm, **Esc** to cancel, and
**Backspace** to delete. Game controls are suppressed while editing.
Presentation prefers an accelerated SDL renderer with software fallback. Key
presses are retained until the next GamePad read, including short taps between
frames. `WIIU_TRACE_INPUT=1` logs delivered button edges; `WIIU_TRACE_GPU=1`
reports cumulative shader, draw, upload, and presentation timings.

Press **F3** to toggle the debug overlay, or add `--debug-overlay` to start it
visible alongside `--window --gpu-render ...`. It shows presented FPS and average/
maximum frame intervals over the last 120 frames, presented-frame and VPAD-read
counts, window focus, input source, text-entry state, held GamePad buttons, stick
position, and the last delivered button press with its age. Timing includes game
execution, rendering, and pacing; it is not a GPU-only measurement. Overlay text
refreshes four times per second and immediately on a new game button press.
Include the overlay in screenshots of a problem. It updates when frames are
presented, so a complete game stall also freezes the overlay.

For deterministic input tests, `--input-replay FILE` replaces keyboard buttons and left-stick input
with lines such as `180 8000` and `181 0` (press A on VPAD sample 180, then
release). Indices are decimal; masks are hexadecimal. Optional x/y values in
`[-1,1]` set the left stick, for example `200 0 0 -1` for Down; omitted axes are
neutral. Each entry persists until the next sample index. Use a separate temporary
`--save-dir` when replaying inputs through save-creation screens.
For headless name-entry tests, `--keyboard-replay FILE` supplies one UTF-8 line
per opened text dialog and confirms it automatically.

`--audio` enables the experimental stereo mixer. Effects and hardware resampling
are incomplete. Video regression tests require `ffmpeg` with the libx264 encoder.
Set `WIIU_TRACE_FS=1` to log opened guest paths while diagnosing asset loading.
