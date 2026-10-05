# omotion

Cut a picture into layers and bring it to life. Open one image, split it
into layers by depth or pick out the parts that should move, and each one
becomes its own layer that you can move, turn, scale, stretch, fade and
restyle with keyframes. Export to MP4, WebM, GIF, or a folder of PNG frames.

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

**The quick way:** press **D** to split the picture into layers by depth,
then **A** to give it a slow, looping camera move where near things slide
past far ones. **Space** plays it, **Ctrl+E** exports it. (D and A use the
[AI](#ai); omotion offers to set it up the first time.)

Things that should move on their own, like a tree that sways, are best cut
out first (**W**, click, **Enter**), before **D**: each goes with the depth
layer it stands in, so it rides the camera move with its surroundings, and
**N** can still give it its own motion on top.

**By hand:**

1. Press **W** and click a part of the picture (an arm, a cloud, a logo);
   the AI selects the thing you clicked. Shift+click takes more of it,
   right-click leaves a part out, and **T** tries its other guesses. **M**
   switches to colour select, which takes the area of similar colour around
   the click and needs no AI. Or press **C** and drag around the part, or
   click around it point by point. Drag the points to adjust the shape, and
   press **F** to soften its edge.
2. **Enter** makes the part its own layer, and the AI paints in what was
   behind it, so it can move away without leaving a gap (**G** switches to
   a smooth fill, or none; **Shift+Enter** copies the part out instead). **R** reshapes a piece later, keeping its animation.
3. Press **P** and click where the new layer should turn, like a shoulder,
   or the foot of a tree. **N** gives it a ready-made motion: sway, float,
   pulse, bounce or spin, which fit the scene so they loop with it, or a
   fade in from the playhead.
4. Press **K** to set a keyframe, step ahead with **.**, change the layer
   (drag it, **[ ]** to rotate, **- =** to scale), and keep going. To bend
   it instead, press **J**, click to pin it in a few places, and drag the
   pins.
5. Drag keyframes (◆) along the timeline to fix the timing, and press **E**
   to change how they ease into the next one, or drag the handles of the
   curve in the inspector to shape it yourself. **Ctrl+C** and **Ctrl+V**
   reuse a motion on another layer.
6. Press **V** for the camera, and animate it like a layer: pan, zoom and
   turn the whole scene. Each layer's **Follow camera** sets how much the
   camera moves it, for depth.
7. Tie one thing to another with the ⛓ beside a property in the inspector:
   `car.x × 0.8` turns a wheel as its car drives, `speed of car.x × 0.002`
   thickens dust as it speeds up, `time × 90` spins forever. The link adds
   to the property's own value, which still keyframes as usual.
8. Give a layer a look in the inspector: brightness, contrast, saturation,
   blur, glow, and a blend (**B**). They animate like everything else. A
   glowing light is a soft copy-cut of it (**Shift+Enter**), set to *add*,
   with the contrast up, the brightness down and some glow.
9. **Ctrl+E** exports into `renders/` in the project folder, at full size or
   half (**H** in the export menu). GIFs play at 15 fps to stay small.

## Shortcuts

| Key | Does |
| --- | --- |
| `O` / `Shift+O` | Animate an image / open an animation folder |
| `I` | Add an image as a layer |
| `Ctrl+M` | Add music or sound: it plays along, shows as a waveform on the timeline, and goes into MP4 and WebM |
| `Shift+I` | Add particles: snow, rain, dust, fireflies, leaves or embers, looping with the scene |
| `Ctrl+E` | Export MP4, WebM, GIF or PNG frames (`H` there for half size) |
| `Ctrl+Shift+K` | Move images nothing uses any more (not even undo) to the trash |
| `Space` | Play / pause |
| `,` `.` | Previous / next frame |
| `<` `>` | Previous / next keyframe |
| `Home` `End` | Start / end |
| `L` | Show the selected layer's path through the scene, and faint ghosts of it at the keyframes either side |
| Click, drag | Select the layer under the mouse and move it (Shift+drag keeps the selection) |
| Mouse wheel | Scale the selected layer (Shift+wheel turns it) |
| Ctrl+wheel, middle-drag | Look closer about the mouse, and move the view; `Ctrl+=` `Ctrl+-` zoom, `Ctrl+0` fits it again |
| `Tab` / `Shift+Tab` | Select the layer below / above |
| `Ctrl+↑` `Ctrl+↓` | Move the layer up / down the stack |
| `←` `→` `↑` `↓` | Nudge 1px (Shift: 10px) |
| `[` `]` | Rotate 1° (`{` `}`: 15°) |
| `-` `=` | Scale down / up |
| `9` `0` | Opacity down / up |
| `Ctrl+Shift+S` | Save the layer's animation (or the picked keyframes) as a motion, for any layer in any project |
| `J` | Pins: click the layer to pin it, drag a pin to bend it (an arm at the elbow, a tail), right-click to unpin; `K` keyframes the bend |
| `N` | A ready-made motion for the layer: sway, float, pulse, bounce or spin (looping with the scene), or fade in |
| `K` | Keyframe every property at the playhead |
| Drag ◆ | Retime keyframes on the timeline; Shift+click picks several to move together |
| `X` | Remove the picked keyframes, or those at the playhead |
| `E` | Change the easing out of the picked keyframes, or those at the playhead |
| `Ctrl+C` / `Ctrl+V` | Copy keyframes, and paste them at the playhead on the selected layer (any layer) |
| `V` | Select the camera (or its row at the top of the timeline): drag the canvas to pan, wheel to zoom, `[ ]` to turn, `K` to keyframe |
| `D` | Split the picture into layers by depth (AI) |
| `A` | Make a depth split move: the camera drifts and pushes in, and each depth layer follows it by its depth |
| `W` | Select: click a thing (AI), Shift+click adds, right-click leaves out; `C` then fine-tunes its points |
| `M` | In `W`: switch between AI select and colour select |
| `T` | In `W`: the AI's other guesses, or colour select's tolerance (also the mouse wheel) |
| `C` | Cut tool: drag around a piece or click points, `Enter` cuts it out, `Shift+Enter` copies it, `Backspace` drops the last point |
| Drag a point | Move it; click an edge to add a point, right-click a point to remove it |
| `F` | Soft edge for the cut: 0, 1, 3, 8 or 20 px |
| `G` | How the hole a cut leaves is filled: AI (the default, where it's set up), smooth, or not at all |
| `R` | Reshape the selected piece by its points; its animation and pivot stay (a depth layer opens the brush) |
| `Shift+R` | Paint a piece's shape with a brush: drag adds, right-drag takes away, the wheel sizes it, `Enter` applies |
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

Three things are better with a little AI, which runs on your own computer:

- **D splits a picture into layers by depth**: near, middle, far, and what's
  left as the background. Each layer carries a filled-in background behind
  the ones in front of it, so nothing opens up when they move apart. **A**
  then makes it move: a slow camera drift and push-in, where nearer layers
  slide further, looping.
- **W selects things with AI**: click the thing, Shift+click to add more of
  it, right-click to leave a part out; **T** steps through its guesses (a
  part, the thing, the whole). **M** switches to colour select.
- **Cuts are filled in with AI**: what's behind a piece you cut out is
  painted in to match its surroundings (the hillside behind a tree, the
  slope behind a deer), not just smoothed over. **G** in the cut tools
  switches between AI, smooth and no fill. It takes a few seconds a cut,
  in the background; `OMOTION_FILL=smooth` makes smooth the default.

It needs ONNX Runtime (`omarchy-pkg-add onnxruntime-cpu`) and three models,
which omotion offers to download the first time (about 350 MB, checked
against known hashes, kept in `~/.local/share/omotion/models`):

| Model | For | License |
| --- | --- | --- |
| [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small) | depth | Apache-2.0 |
| [MobileSAM](https://huggingface.co/Acly/MobileSAM) | selecting | MIT |
| [LaMa](https://huggingface.co/Carve/LaMa-ONNX) | filling | Apache-2.0 |

Without them, everything else works as before, and omotion says what's
missing when you ask for an AI feature.

## The project folder

```text
photo.omotion/
  scene.json        the animation
  assets/           every image a layer uses; cuts add new files here, never overwrite,
                    so undo works; Ctrl+Shift+K clears out the ones nothing uses
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
- **`pins`** bend a layer: points on its image (in its pixels), each with
  an offset `pin<n>x` and `pin<n>y` (constant or keyed, like any property).
  The image follows them as rigidly as it can, so a pin at the shoulder and
  one at the hand bend an arm at the elbow. A pin added to a layer that's
  already bent holds the point it's put on; the rest can settle a little,
  as every pin has a say in the bend.
- **`audio`** is the scene's sound: its `source`, the `offset` in seconds
  into it where the scene starts, and `volume` (1 as it is).
- **`particles`** makes a layer of rain, snow and the like instead of an
  image: `kind` (`snow`, `rain`, `dust`, `fireflies`, `leaves`, `embers`),
  `count`, `size`, `speed` (canvas pixels a second), `direction` (degrees,
  90 is down), `spread`, `sway`, `twinkle` (0–1), `color` and `seed`. They're
  fitted to loop with the scene, and take the layer's opacity, blend, blur,
  glow and follow.
- **`blend`** is how a layer mixes with the ones below: `normal`, `add`,
  `screen`, `multiply`, `overlay`, `lighten` or `darken`.
- **`camera`** is what part of the world the canvas shows: `x`, `y` (the
  world point at the canvas centre), `zoom` and `rotation`, constant or
  under `keys` like a layer's. A layer's **`follow`** says how much the
  camera moves it, from 1 (fully; the default) to 0 (stuck to the screen);
  depth layers follow less the farther they are, which is the parallax.
  Without its own `follow`, a layer takes its parent's.
- **`x`, `y`** place the layer's `anchor`, its pivot as a fraction of the
  image (`[0.5, 0.5]` is the centre). Without a parent that's in canvas
  pixels. With a `parent` it's in the parent image's pixels, and the layer
  moves, turns and scales with its parent. Opacity isn't inherited.
- **Keys** have a time `t` in seconds, a value `v`, and an `ease` for the
  way *out* of that key: `linear`, `in`, `out`, `inOut`, `outBack`
  (overshoots and settles), `hold` (jumps at the next key), or a curve of
  your own, `cubic(x1,y1,x2,y2)`, as in CSS (y past 0–1 overshoots).
- **`background`** can be see-through: `#00000000`, as `#AARRGGBB`. WebM
  and GIF keep the transparency, MP4 can't.
- **`links`** drive a property from another, so a scene says how things
  relate instead of keyframing each one. Under the property's name: `from`
  (`"layer.property"`, `"camera.property"`, or `"time"` in seconds), `of`
  (`value`, the default; `speed`, its change a second; or `acceleration`),
  `times` (1 if left out), and optionally `lag` (seconds behind the
  source), `smooth` (seconds `speed` and `acceleration` are measured over,
  a frame if left out), `abs` (`true` counts backwards as forwards), and
  `min`/`max` to hold it within. What the link gives is added to the
  property's own value or keys. One link per property; links that loop
  back on themselves are refused. The camera takes them too.

  ```json
  "links": {
    "rotation": { "from": "car.x", "times": 0.8 },
    "opacity":  { "from": "car.x", "of": "speed", "times": 0.002, "abs": true, "max": 1 }
  }
  ```
  Properties are read as stored: a parented layer's `x` is in its parent's
  pixels.
- **`cut`** on a piece remembers how it was made: the layer it came `from`,
  its `polygon` (or for depth layers, a `mask` image placed at
  `maskOrigin`) and `feather` in that layer's image pixels, whether it
  `punch`ed a hole, whether that hole is `fill`ed and how (`fillWith`: `ai`
  or `smooth`), its `order` among
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
omotion render photo.omotion out.mp4      # or .webm, .gif, or a folder for PNG frames;
                                          # --scale 0.5 for half size
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
omotion info photo.omotion                # every layer's place on the canvas, as JSON
omotion pivot photo.omotion pine 1460,780 # P: where it turns, here the foot of the tree
omotion preset photo.omotion pine sway    # N: a ready-made motion (--amount 2 for twice as much)
omotion link photo.omotion wheel rotation "car.x * 0.8"
                                          # ⛓: drive a property from another; extras
                                          # after commas: lag, smooth, min, max, abs;
                                          # --none unlinks
omotion particles photo.omotion snow --count 400
                                          # Shift+I: a particle layer (any setting as --name value)
omotion audio photo.omotion song.mp3 --offset 12
                                          # Ctrl+M: the scene's sound, from 12 s in (--none takes it away)
omotion motion save photo.omotion pine sway-gently
omotion motion apply other.omotion tree sway-gently --at 2
                                          # saved motions, as with Ctrl+Shift+S and N
omotion clean photo.omotion               # unused images to the trash (--dry-run lists them)
```

## Building

omotion is C++ and Qt Quick, built with qmake and make into one binary.

```sh
bin/build      # build/omotion
bin/test       # the Qt Test suite, offscreen
bin/install    # builds the Arch package and installs it, so it's in the app launcher
```

It follows the Omarchy theme's accent color live, and needs `ffmpeg` to
export video. The AI is built in when ONNX Runtime's headers are there
(`onnxruntime-cpu`); the library itself is only loaded when an AI feature is
used, so the same build runs without it.
