# omotion

Cut a picture into layers and bring it to life. Open one image, trace around
the parts that should move, and each one becomes its own layer that you can
move, turn, scale, stretch and fade with keyframes. Export to MP4, WebM, GIF,
or a folder of PNG frames.

The whole animation is a plain `scene.json` in the project folder. omotion
saves every edit there as you make it, and when anything else changes that
file (a text editor, a script, an AI agent), the window shows the change at
once. So you can rough out motion by hand, have an agent polish the timing
in the file, and watch it play back live.

## Getting started

```sh
omotion photo.png          # makes photo.omotion/ beside the image and opens it
omotion my-animation/      # opens a project folder, or makes one from a folder of images
```

Your original image is copied into the project and never touched.

1. Press **C**, click around a part of the picture (an arm, a cloud, a
   logo), and press **Enter**. The part becomes its own layer and leaves a
   transparent hole behind (**Shift+Enter** copies it out instead).
2. Press **P** and click where the new layer should turn, like a shoulder.
3. Press **K** to set a keyframe, step ahead with **.**, change the layer
   (drag it, **[ ]** to rotate, **- =** to scale), and keep going.
4. **E** changes how a keyframe eases into the next one.
5. **Ctrl+E** exports into `renders/` in the project folder.

## Shortcuts

| Key | Does |
| --- | --- |
| `O` / `Shift+O` | Animate an image / open an animation folder |
| `I` | Add an image as a layer |
| `Ctrl+E` | Export MP4, WebM, GIF or PNG frames |
| `Space` | Play / pause |
| `,` `.` | Previous / next frame |
| `<` `>` | Previous / next keyframe |
| `Home` `End` | Start / end |
| Click, drag | Select the layer under the mouse and move it (Shift+drag keeps the selection) |
| Mouse wheel | Scale the selected layer (Ctrl+wheel rotates) |
| `Tab` / `Shift+Tab` | Select the layer below / above |
| `Ctrl+↑` `Ctrl+↓` | Move the layer up / down the stack |
| `←` `→` `↑` `↓` | Nudge 1px (Shift: 10px) |
| `[` `]` | Rotate 1° (`{` `}`: 15°) |
| `-` `=` | Scale down / up |
| `9` `0` | Opacity down / up |
| `K` | Keyframe every property at the playhead |
| `X` | Remove the keyframes at the playhead |
| `E` | Change the easing out of the keyframe at the playhead |
| `C` | Cut tool: click around a piece, `Enter` cuts it out, `Shift+Enter` copies it, `Backspace` drops the last point |
| `P` | Pivot tool: click where the layer turns and scales around |
| `H` | Hide / show the layer |
| `F2` | Rename the layer |
| `Delete` | Delete the layer |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `Esc` | Cancel, close, or deselect |
| `?` | Every shortcut |
| `Q` | Quit |

In the inspector, the ◆ beside a property means it's animated. Clicking ◇
starts animating just that property at the playhead.

## The project folder

```text
photo.omotion/
  scene.json        the animation
  assets/           every image a layer uses; cuts add new files here, never overwrite
  renders/          exports
```

### scene.json

```json
{
  "width": 1280, "height": 720, "fps": 30, "duration": 4,
  "background": "#000000",
  "layers": [
    { "name": "body", "source": "assets/robot.png", "x": 640, "y": 400 },
    {
      "name": "arm", "source": "assets/arm.png", "parent": "body",
      "anchor": [0.1, 0.5],
      "x": 210, "y": 120,
      "keys": {
        "rotation": [
          { "t": 0, "v": 0, "ease": "inOut" },
          { "t": 1, "v": -40, "ease": "outBack" },
          { "t": 2, "v": 0 }
        ]
      }
    }
  ]
}
```

- **Layers** are drawn in order, first at the back. Names must be unique.
- **Properties** are `x`, `y`, `scale`, `scaleX`, `scaleY`, `rotation`
  (degrees, clockwise) and `opacity` (0–1). A property is either a number
  or animated under `keys`; keys win.
- **`x`, `y`** place the layer's `anchor`, its pivot as a fraction of the
  image (`[0.5, 0.5]` is the centre). Without a parent that's in canvas
  pixels. With a `parent` it's in the parent image's pixels, and the layer
  moves, turns and scales with its parent. Opacity isn't inherited.
- **Keys** have a time `t` in seconds, a value `v`, and an `ease` for the
  way *out* of that key: `linear`, `in`, `out`, `inOut`, `outBack`
  (overshoots and settles), or `hold` (jumps at the next key).
- **`background`** can be see-through: `#00000000`, as `#AARRGGBB`. WebM
  and GIF keep the transparency, MP4 can't.
- Fields omotion doesn't know about are kept as they are, so notes and
  metadata are safe to add.

If the file stops parsing, omotion says where in the window and leaves the
file alone until it parses again.

## Command line

The command line uses the same renderer as the window, without opening one:

```sh
omotion render photo.omotion out.mp4      # or .webm, .gif, or a folder for PNG frames
omotion frame photo.omotion 1.5 f.png     # one frame, to check a moment
omotion cut photo.omotion photo 0,640 900,800 1920,455 1920,1080 0,1080 --copy --name foreground
                                          # what the C tool does, with points in canvas pixels
```

## Building

omotion is C++ and Qt Quick, built with qmake and make into one binary.

```sh
bin/build      # build/omotion
bin/test       # the Qt Test suite, offscreen
bin/install    # builds the Arch package and installs it, so it's in the app launcher
```

It follows the Omarchy theme's accent color live, and needs `ffmpeg` to
export video.
