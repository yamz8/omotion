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
   curve in the inspector to shape it yourself. For weight, type a physical
   easing in the inspector: `spring(3)` overshoots and settles, `bounce(2)`
   lands like a dropped ball, `arc(300)` jumps 300 px high on the way to
   the next key. **Ctrl+C** and **Ctrl+V**
   reuse a motion on another layer.
6. To bring something from the distance, key its **Depth z** in the
   inspector from far (1500) to 0: it shrinks toward the vanishing point,
   which you set under Scene where the road meets the horizon.
7. Press **V** for the camera, and animate it like a layer: pan, zoom and
   turn the whole scene. Each layer's **Follow camera** sets how much the
   camera moves it, for depth.
8. Tie one thing to another with the ⛓ beside a property in the inspector:
   `car.x × 0.8` turns a wheel as its car drives, `speed of car.x × 0.002`
   thickens dust as it speeds up, `time × 90` spins forever. The link adds
   to the property's own value, which still keyframes as usual.
9. Give a layer a look in the inspector: brightness, contrast, saturation,
   blur, glow, and a blend (**B**). They animate like everything else. A
   glowing light is a soft copy-cut of it (**Shift+Enter**), set to *add*,
   with the contrast up, the brightness down and some glow.
10. **Ctrl+E** exports into `renders/` in the project folder, at full size or
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
| `Shift+D` | Make the picture a 3D world: the AI reads its distances, lens and ground (see [Worlds](#worlds)) |
| `S` | Give the selected piece of the world a 3D shape |
| Drag in a world | With the camera selected (`V`): turn it, Shift+drag moves it. A piece: move it in 3D, Shift+drag nearer / farther |
| Click a piece of a world | Pick it where it is in 3D: a box shows round it, arrows drag it along x (red), y (green) and z (blue), its ring turns it |
| `G` | Grab the selected piece of a world: it follows the mouse, `X` `Y` `Z` keep it to an axis, a click or `Enter` places it, right-click or `Esc` puts it back |
| Steps | In the inspector, how the selected layer came to be: cut from what, how the hole behind it was filled, the world, its 3D shape. Each can be done again or differently (fill by the AI, smoothly, or leave open); ⚠ marks one made from something that has changed since, and the top bar's button redoes all of those in order. `omotion steps` and `omotion redo` do the same |
| Outliner | Top right: the camera, the world with each piece under what it was cut from (▦ the picture, ▲ a 3D shape, ▱ flat), then the layers over it. Click selects, ● hides, double-click renames; World shows its haze, grain, shutter and dust colour |
| `` ` `` | Free view: look at the world from anywhere (middle-drag or Alt+drag orbits, Shift pans, the wheel or `-` `=` comes closer, arrows orbit, `F` looks at the selected); the shot's camera shows in it and can be moved too. Nothing in the shot changes |
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
| `Ctrl+Tab` | The graph editor (or Curves on the timeline): the selected layer's or camera's animated properties as curves over the scene, each fitted to the height; drag a key in time and value (Shift: value only), click a property to see it alone with its values |
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

## Worlds

A world turns the picture into a 3D place you direct: its ground, the
things in it and a camera that can move. Everything still starts from the
picture, and from its own camera the world looks exactly like it.

```sh
omotion world photo.omotion --horizon 0.59   # the AI reads distances, the lens and the ground (~1–2 min the first time)
omotion shape photo.omotion car              # a cut-out piece becomes a 3D object (~20 s)
omotion size photo.omotion car 1.4           # the world to scale: that piece is 1.4 m tall
omotion drive photo.omotion car --from 8,22 --to -5,-1 --speed 50
                                             # over the ground at 50 km/h: off crests, landing, dust
omotion link photo.omotion camera yaw "car.worldX * 1.2, spring 1.2"
                                             # a camera that follows
```

- **world** reads where every pixel is in meters, the lens it was seen
  through and the ground, and builds a mesh of the picture (with what's
  behind cut pieces filled in, and its edges painted on past the frame, so
  the camera can turn a little). The ground in sight may slope (a road up
  to a crest); `--horizon`, how far down the picture the horizon is, says
  which way is level.
- **shape** gives a piece a closed 3D shape (TripoSR), fitted to its
  cut-out and wearing the picture, so it can turn and has a back. The side
  the picture never showed is a guess, and shows when it turns far.
- **size** sets the scale: what the AI reads from a picture, and a
  painting above all, is only roughly to scale, and gravity looks right
  only at the right size.
- **drive** moves a piece over the ground at a speed: it keeps to the
  ground, flies where the ground falls away faster than gravity, lands
  where the ground is and pitches with its path, baked as keyframes. It
  keeps facing as the picture shows it (`--face` turns it to its way).
  Where it takes off and lands, dust is thrown up; under a shaped piece,
  a shadow falls on the ground. Ground hidden behind a crest is taken to
  fall gently away under the line of sight.
- **ground** tells the ground's height at places; **info** gives each
  world piece's pivot, the box it fills and its heading, in meters.

In the window: `Shift+D` makes the world and `S` gives the selected piece
a shape (both in the background). The world is shown in 3D under the
canvas. With the camera selected (`V`), dragging turns it and Shift+drag
moves it; dragging a piece moves it in 3D, Shift+drag nearer and farther.
The inspector has the world properties of the camera and of each piece,
its real height, and a drive over the ground from the playhead.

A world can't show what the picture never did: turned far, a shape's
unseen side is the AI's guess; the camera can turn a little past the
frame; moving it opens gaps behind near things, which show what the AI
painted there. Moves of a meter or so in a scene a few meters deep look
right; much more and flat things (a cut-out of dust) show they're flat.

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
- **Properties** are `x`, `y`, `z`, `scale`, `scaleX`, `scaleY`, `rotation`
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
- **`z`** sets a layer back from where it's placed, in pixels (0, the
  default, changes nothing; less than 0 brings it closer). Things further
  back shrink toward the vanishing point by `distance / (distance + z)`, as
  a camera sees them, so a car keyed from far to near comes down the road
  and grows the way a real one does, with no scale keys. Children are as far
  back as their parent, plus their own `z`. **`perspective`** at the top of
  the file sets the `vanish`ing point, `[x, y]` on the canvas where the road
  meets the horizon (the canvas centre if left out), and the focal
  `distance` in pixels (the canvas width if left out; a layer that far back
  is half size).
- **`world`**, made by `omotion world`: the `picture` (layer) it was made
  from, its `depth` and the depth `behind` its pieces (16-bit images,
  logarithmic from 5 cm to 2 km), the lens `focal` in the picture's pixels,
  the `ground` (`up`, which way is up as the picture's camera sees it, x
  right, y down, z ahead; and the camera's `height` above it in meters),
  the picture carried past its edges (`wide`, by `margin` pixels each side),
  the layer `far` behind the near side of jumps in distance (its `depth`
  and `picture`, where the picture's own mesh tears so a moving camera sees
  behind a hill or a sign),
  its `scale`, the `dust` colour and the camera's `shutter` in degrees (180
  unless set; 0 for none): what moves fast smears across that much of each
  frame, as on film. Frames where little moves take fewer moments across
  the shutter, so they export faster. `haze` is the air: its `color` (from the farthest land
  the picture shows) and `density`, how much of a piece it hides a meter
  farther than the picture has it (0.02). `grain` is the film grain over a
  world's frames (0.025; 0 for none). `pieces` (a code for each piece cut
  out of the picture), `made` (when, in ms) and `horizon` say what it was
  made from, so it's known when it's out of date; made again, a world keeps
  its `scale`, `grain`, `shutter`, `haze` and `dust`. A shape's sides the picture never
  showed catch the light round it at their edges. A world layer's **world properties**
  move it from where it is in the picture: `worldX` (right), `worldY` (up)
  and `worldZ` (away), in meters, and `yaw` (right), `pitch` (nose up) and
  `roll` (lean right), in degrees, turning about its pivot. The camera has
  the same but `roll` (its `rotation` is that), and its `zoom` is the lens.
  A layer's `shape` names its 3D shape and which `quarter` turn faces it,
  and what it was made `from` (its cut's code) in which `world` (its `made`);
  `landings` and `takeoffs` (seconds) throw up dust.
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
  your own, `cubic(x1,y1,x2,y2)`, as in CSS (y past 0–1 overshoots). The
  physical ones: `spring(n)` overshoots and rings down in `n` wobbles
  (`spring` is 2); `bounce(n)` falls under gravity and bounces `n` times,
  each a quarter as high (`bounce` is 3; `bounce(0)` is a plain fall);
  `arc(h)` is a jump, straight across with `y` rising `h` pixels at the
  middle along a gravity parabola (other properties go straight, so an arc
  on every property's key is fine).
- **`shutter`** (degrees, 0 unless set) blurs what moves fast in a flat
  scene's exports, as film does: 180 is a film camera's. Worlds blur by
  default. Layers that hold still under the shutter cost nothing extra.
- **`background`** can be see-through: `#00000000`, as `#AARRGGBB`. WebM
  and GIF keep the transparency, MP4 can't.
- **`links`** drive a property from another, so a scene says how things
  relate instead of keyframing each one. Under the property's name: `from`
  (`"layer.property"`, `"camera.property"`, or `"time"` in seconds), `of`
  (`value`, the default; `speed`, its change a second; or `acceleration`),
  `times` (1 if left out), and optionally `lag` (seconds behind the
  source), `smooth` (seconds `speed` and `acceleration` are measured over,
  a frame if left out), `abs` (`true` counts backwards as forwards), and
  `min`/`max` to hold it within. `spring` (times a second) makes it follow
  on a spring, lagging, overshooting and settling, with `damping` (0.5 if
  left out; 1 settles without overshoot, near 0 rings): a body on its
  suspension, a lean that wobbles, a camera that's carried. What the link gives is added to the
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
                                          # after commas: lag, smooth, spring, damping,
                                          # min, max, abs;
                                          # --none unlinks
omotion world photo.omotion --horizon 0.59   # see Worlds: world, shape, size, drive, ground
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
