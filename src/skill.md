---
name: omotion
description: |
  Make motion videos (product films, launch clips, UI motion, data stories,
  kinetic typography) as omotion scenes: QML files where every frame is a
  function of time, checked, reviewed on contact sheets, and rendered to MP4 in
  16:9, 9:16, and 1:1. Use for ANY request to make, change, review, or export
  a video or animation with omotion, and for any .qml file that is an omotion scene.
---

# omotion

An omotion scene is one QML file that you write with your ordinary editing
tools. The `omotion` command starts, checks, pictures, and renders it. No
command needs a display.

Run `omotion help format` first. It prints the whole scene format and the
Motion helpers for this version of omotion.

## Workflow

Treat each step as a gate: don't start the next until this one has left its
evidence.

1. **Brief.** Before any code, write down what the film is for, the one thing
   the viewer should remember, which assets must be real (screenshots, logos),
   and what would make it unacceptable. If a required real asset is missing,
   stop and ask for it; never draw a convincing fake.
2. **Beats.** Plan the beats: what the viewer knows at each one, and why it
   exists. They become the scene's `beats` list.
3. **Start.** `omotion new film/scene.qml` writes a working starter to adapt.
4. **Check.** `omotion check film/scene.qml --json` after every edit. Fix every
   error: a clock or stateful frame breaks rendering. Treat text outside the
   frame as a layout bug in that format.
5. **Look.** `omotion sheet film/scene.qml --format all`, then view the PNGs.
   Ask: does the viewer know where to look in every tile? Is text readable and
   on screen long enough? Did the vertical version survive, or is it a crop?
   For a fast transition, `--times` with a few close moments shows a strip.
   Repair what you see and look again before you render.
6. **Render** when the sheets hold up: `omotion render film/scene.qml --json`
   writes one MP4 per format into `renders/`. The render isn't the proof; the
   sheets are.

## Working with the person

- If they have the scene open in the omotion window, your saved edits appear
  there at once, at the moment they were looking at. `omotion open <file>`
  opens the window for them; it needs their desktop, so don't wait on it.
- Every command takes `--json`, exits 0 on success and 1 on failure, and writes
  errors to stderr. `omotion help <command>` lists a command's options.
