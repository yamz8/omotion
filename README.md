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

1. Press **W** and click a part of the picture (an arm, a cloud, a logo):
   smart select takes the area of similar colour around the click, picking
   how similar by itself (the mouse wheel or **T** changes it, Shift+click
   adds more). Or press **C** and drag around the part, or click around it
   point by point. Drag the points to adjust the
   shape, and press **F** to soften its edge. **Enter** makes the part its
   own layer, and paints the hole it leaves from the colours around it, so
   the piece can move away without leaving a gap (**G** leaves the hole
   empty; **Shift+Enter** copies the part out instead). **R** reshapes a
   piece later, keeping its animation.
2. Press **P** and click where the new layer should turn, like a shoulder.
3. Press **K** to set a keyframe, step ahead with **.**, change the layer
   (drag it, **[ ]** to rotate, **- =** to scale), and keep going.
4. Drag keyframes (◆) along the timeline to fix the timing, and press **E**
   to change how they ease into the next one. **Ctrl+C** and **Ctrl+V**
   reuse a motion on another layer.
5. Give a layer a look in the inspector: brightness, contrast, saturation,
   blur, glow, and a blend (**B**). They animate like everything else. A
   glowing light is a soft copy-cut of it (**Shift+Enter**), set to *add*,
   with the contrast up, the brightness down and some glow.
6. **Ctrl+E** exports into `renders/` in the project folder.

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
| Drag ◆ | Retime keyframes on the timeline; Shift+click picks several to move together |
| `X` | Remove the picked keyframes, or those at the playhead |
| `E` | Change the easing out of the picked keyframes, or those at the playhead |
| `Ctrl+C` / `Ctrl+V` | Copy keyframes, and paste them at the playhead on the selected layer (any layer) |
| `D` | Split the picture into layers by depth (AI) |
| `A` | Make a depth split move: a looping parallax camera |
| `W` | Select: click a thing (AI), Shift+click adds, right-click leaves out; `C` then fine-tunes its points |
| `M` | In `W`: switch between AI select and colour select |
| `T` | In `W`: the AI's other guesses, or colour select's tolerance (also the mouse wheel) |
| `C` | Cut tool: drag around a piece or click points, `Enter` cuts it out, `Shift+Enter` copies it, `Backspace` drops the last point |
| Drag a point | Move it; click an edge to add a point, right-click a point to remove it |
| `F` | Soft edge for the cut: 0, 1, 3, 8 or 20 px |
| `G` | Fill the hole the cut leaves from the colours around it: on (the default) / off |
| `R` | Reshape the selected piece; its animation and pivot stay |
| `P` | Pivot tool: click where the layer turns and scales around |
| `B` | Blend the layer: normal, add, screen, multiply, overlay, lighten, darken |
| `Ctrl+D` | Duplicate the layer |
| `H` | Hide / show the layer |
| `F2` | Rename the layer |
| `Delete` | Delete the layer |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `Esc` | Cancel, close, or deselect |
| `?` | Every shortcut |
| `Q` | Quit |

In the inspector, the ◆ beside a property means it's animated. Clicking ◇
starts animating just that property at the playhead.

## AI

Two things are better with a little AI, which runs on your own computer:

- **D splits a picture into layers by depth**: near, middle, far, and what's
  left as the background. Each layer carries a filled-in background behind
  the ones in front of it, so nothing opens up when they move apart. **A**
  then makes it move: a slow camera drift and push-in, where nearer layers
  slide further, looping.
- **W selects things with AI**: click the thing, Shift+click to add more of
  it, right-click to leave a part out; **T** steps through its guesses (a
  part, the thing, the whole). **M** switches to colour select.

It needs ONNX Runtime (`omarchy-pkg-add onnxruntime-cpu`) and two models,
which omotion offers to download the first time (about 145 MB, checked
against known hashes, kept in `~/.local/share/omotion/models`):

| Model | For | License |
| --- | --- | --- |
| [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small) | depth | Apache-2.0 |
| [MobileSAM](https://huggingface.co/Acly/MobileSAM) | selecting | MIT |

Without them, everything else works as before, and omotion says what's
missing when you ask for an AI feature.

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
  (degrees, clockwise) and `opacity` (0–1), and for the layer's look
  `brightness` (−1 to 1, default 0), `contrast` and `saturation` (1 is
  unchanged), `blur` and `glow` (in canvas pixels, default 0). A property is
  either a number or animated under `keys`; keys win.
- **`blend`** is how a layer mixes with the ones below: `normal`, `add`,
  `screen`, `multiply`, `overlay`, `lighten` or `darken`.
- **`x`, `y`** place the layer's `anchor`, its pivot as a fraction of the
  image (`[0.5, 0.5]` is the centre). Without a parent that's in canvas
  pixels. With a `parent` it's in the parent image's pixels, and the layer
  moves, turns and scales with its parent. Opacity isn't inherited.
- **Keys** have a time `t` in seconds, a value `v`, and an `ease` for the
  way *out* of that key: `linear`, `in`, `out`, `inOut`, `outBack`
  (overshoots and settles), or `hold` (jumps at the next key).
- **`background`** can be see-through: `#00000000`, as `#AARRGGBB`. WebM
  and GIF keep the transparency, MP4 can't.
- **`cut`** on a piece remembers how it was made: the layer it came `from`,
  its `polygon` (or for depth layers, a `mask` image placed at
  `maskOrigin`) and `feather` in that layer's image pixels, whether it
  `punch`ed a hole and whether that hole is `fill`ed, its `order` among
  that layer's pieces, and for depth layers its `depth` (0 far, 1 near),
  which **A** moves it by. **`uncut`**
  on a layer that has holes is its image before any cuts. omotion redraws
  pieces and holes from these when one is reshaped; edit the polygon by
  hand and press `R` then `Enter` to apply it.
- Fields omotion doesn't know about are kept as they are, so notes and
  metadata are safe to add.

If the file stops parsing, omotion says where in the window and leaves the
file alone until it parses again.

## Command line

The command line uses the same renderer as the window, without opening one:

```sh
omotion render photo.omotion out.mp4      # or .webm, .gif, or a folder for PNG frames
omotion frame photo.omotion 1.5 f.png     # one frame, to check a moment
omotion cut photo.omotion photo 0,640 900,800 1920,455 1920,1080 0,1080 --copy --soft 3 --name foreground
                                          # what the C tool does, with points in canvas pixels
omotion cut photo.omotion photo --pick 316,703 --name deer
                                          # what colour select does: the area under the point
omotion cut photo.omotion photo --ai-pick 316,703 --name deer
                                          # what AI select does
omotion split photo.omotion --layers 4 --animate
                                          # D then A: depth layers, moving
omotion models --download                 # fetch the AI models
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
