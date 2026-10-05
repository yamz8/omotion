# omotion

An Omarchy desktop app that turns a picture into an animation: cut it into
layers (by colour or AI select), fill the holes behind, keyframe the layers
and a camera, export mp4/webm/gif/frames. Built to the omarchy-app skill:
C++17 + Qt 6 Quick (QML, Material), qmake6, one binary, an Arch package.
It is also meant to be driven by an agent: `scene.json` in a project folder
is the source of truth, the window reloads it live, and the CLI covers the
same work (`omotion --help`).

## Loop

- `bin/build` builds `build/omotion`; run it with a project folder or image.
- `bin/test` builds and runs the Qt Test suite offscreen. Read the `Totals`
  line before committing; AI tests skip when the models aren't downloaded.
- `bin/install` runs `makepkg -fsi` and needs sudo. Never hand it back to the
  user: run `omarchy-launch-floating-terminal-with-presentation "bin/install"`
  (absolute path) so they type the password in an Omarchy terminal. Reinstall
  after each finished feature.
- Try renders with the CLI (`omotion render`, `omotion frame`) and look at
  the frames before calling something done.

## Where things are

- `src/scene.*` the model and renderer: layers, keyframes and eases, camera,
  parenting, looks, blend modes, particles, puppet pins, `AssetCache`.
- `src/project.*` loading/saving `scene.json`, cuts and hole filling,
  depth split, parallax, presets, saved motions, sound, cleanup.
- `src/select.*` colour selection and outline tracing.
- `src/ai.*` optional ONNX Runtime models (depth, MobileSAM, LaMa), loaded
  at runtime; models in `~/.local/share/omotion/models`.
- `src/backend.*` everything the window does; `src/sceneview.*` the canvas;
  `src/*.qml` layout and keys only; `src/main.cpp` app setup and the CLI.
- `src/exporter.*` frames piped to ffmpeg.
- `docs/roadmap.md` what's next.

## Conventions

- Logic in C++, layout and keys in QML; every key is a `Shortcut`, listed in
  `HelpOverlay.qml` and the README table.
- Keep `scene.json` readable and backward compatible; unknown fields are
  preserved. Document format changes in the README.
- Heavy work runs on QtConcurrent; drop a result if the scene changed.
- Never modify the user's originals; save atomically.
- Comments explain why, in full sentences. Commit messages short and
  imperative. Add tests for new engine behaviour.
- Don't change Omarchy or OS settings without asking (wallpaper integration
  is to be discussed with the user).
