# Writing an omotion scene

A scene is one QML file whose root is a visual item (Item, Rectangle). omotion
sets the root's `t` to a time in seconds and takes a picture. That's the whole
renderer, so **everything on screen must be computed from `t`**: the same `t`
must always give the same frame, whatever was rendered before it.

```qml
import QtQuick
import Omotion

Rectangle {
    property real t: 0                 // set by omotion; required
    property real duration: 9          // seconds; required
    property int fps: 30               // optional, 1 to 120
    property string format: "16:9"     // set by omotion: "16:9", "9:16", "1:1", or "4:5"
    property var formats: ["16:9", "9:16"]   // the formats this scene is composed for
    property var beats: [              // the moments a viewer should notice
        { at: 0.0, name: "Hook", purpose: "why the viewer stays",
          enter: "an empty frame", exit: "the claim on screen" },
        { at: 2.2, name: "Problem", purpose: "...", enter: "...", exit: "..." },
        { at: 5.0, name: "Proof", purpose: "...", enter: "...", exit: "..." }
    ]
    property string audio: "track.wav" // optional, beside the scene, starts at t = 0
    property real audioTrim: 0         // optional: seconds cut from the track's start
    property real audioDelay: 0        // optional: seconds of film before the track starts
    property var cues: [               // optional: sounds and moments on the timeline
        { at: 2.2, name: "reveal", sound: "hit", on: "Problem" }
    ]

    width: 1920; height: 1080          // omotion sets the format's size
    color: "#0d0d10"

    Text {
        readonly property real arrive: Motion.spring(parent.t, 0.2)
        text: "Hello"
        opacity: Motion.clamp(arrive, 0, 1)
        y: 400 + (1 - arrive) * 60
    }
}
```

## The rules

- **No clocks.** Animations of any kind (NumberAnimation, Behavior, Transition,
  SequentialAnimation), Timer, FrameAnimation, AnimatedImage, AnimatedSprite,
  and particles all run on the wall clock. Write each value as an expression
  of `t` instead. `omotion check` reports them.
- **No randomness that changes.** `Math.random()`, `Date.now()` and `new Date()`
  differ on every render. Use `Motion.random(seed)` with a fixed seed.
- **No state between frames.** Don't count frames in `onTChanged` or accumulate
  anything; `omotion check` renders the beats forward and backward in two fresh
  copies and reports any frame that differs.
- **Compose each format.** omotion sets `width`, `height` and `format`; lay out
  from them (`height > width` is portrait), with type and spacing scaled to the
  short side, rather than cropping the wide version. List what you've composed
  for in `formats`; `render` makes one video per format.
- **Name the beats.** A beat starts at `at` and lasts until the next one. The
  contact sheet shows the middle of each, labelled, so give each beat a reason
  to exist and enough time to be read.
- **Beats are a contract with the shot list.** Each beat gives its `purpose`,
  and the `enter` and `exit` states it builds between. In a film's folder,
  `check` matches the beats to `shotlist.md` one for one: a beat without a
  purpose, a missing beat, or a different start time is an error; a renamed
  beat or a state that differs from the approved one is a warning.
- **Cues are sounds that belong to something on screen.** `sound` is one of
  `click`, `tick`, `hit`, or `whoosh` (which swells into the cue), made the
  same way on every render and mixed with the track. A cue `on` a beat lands
  on that beat; without `on`, `check` pairs it with the largest change on
  screen within 0.25 s. Either way, more than a frame apart is an error.
- **Cut to the music.** `omotion beats track.wav` writes the track's tempo
  and beats to `track.wav.beats.json`; `check` then warns about any beat that
  cuts more than a frame off the music's.
- Images and fonts load from files beside the scene; refer to them relatively.
  In a film's folder they come from `assets/` and are listed in
  `assets/assets.md`: `check` fails on anything loaded from elsewhere.
  Other QML files beside the scene can be used as components.

## Motion (import Omotion)

| Function | Gives |
| --- | --- |
| `Motion.progress(t, start, end)` | 0 before start, 1 after end, linear between |
| `Motion.spring(t, start, stiffness = 170, damping = 26)` | 0 toward 1 from `start`, may overshoot; a closed-form spring |
| `Motion.easeIn(p)`, `easeOut(p)`, `easeInOut(p)` | cubic curves on 0..1 |
| `Motion.mix(from, to, amount)` | linear interpolation |
| `Motion.clamp(x, low, high)` | x kept within low..high |
| `Motion.random(seed)` | a repeatable number in [0, 1) |
| `Motion.beatAt(beats, t)` | index of the beat playing at t, or -1 |
| `Motion.rule(name, t, start)` | a spring by the style guide's motion rule for that class of object |
| `Motion.between(states, t)` | one object across states `[{ at, x, color, ... }]`, each eased in over `over` s (0.4) |
| `Motion.per(format, { "1:1": 56 }, 64)` | a value for this format, or the fallback |
| `Motion.safe(format, width, height)` | the rectangle the platform's controls leave clear |

**Style** holds the style guide's `TOKENS`: `Style.colors.ink`,
`Style.fonts.headline`, and the rules `Motion.rule` uses (without a guide,
`micro`, `panel`, `camera`, and `headline` move as a new film's guide has them). `check` warns about
a `Motion.spring` that matches no rule and a color or font the guide doesn't
name, so the film can't drift from its guide.

**Camera** pans and zooms over what's inside it, from `keys: [{ at, x, y,
zoom, over }]`: the point of the content at the frame's center by `at`,
eased in over `over` seconds (1). `camera.plane(depth)` gives `{ x, y, scale }`
for a layer the camera moves less (below 1; 0 stays with the frame) or more
(above 1) than the content, set on an Item with `transformOrigin: Item.TopLeft`:

```qml
Camera {
    id: camera
    anchors.fill: parent
    t: scene.t
    keys: [{ at: 0, zoom: 1 }, { at: 4, x: 1300, y: 500, zoom: 1.4, over: 1.5 }]
    Image { source: "assets/screen.png" }
}
```

**Clip** shows footage, a frame at a time, chosen by number: `Clip { video:
"assets/footage.mp4"; time: scene.t - 2; fps: scene.fps; width: 960; height:
540 }` shows the clip from 2 s, its first frame before and its last after.
The clip is decoded once into `renders/.cache/clips`, so any frame can be
asked for in any order; `length` is how long it runs.

A scene that declares `property int seed` gets the seed from `--seed N` on
any command; `Motion.random(seed * 100 + i)` then makes repeatable
variations, and `render --seed 3` writes `renders/scene-16x9-seed3.mp4`.

Give motion weight by class of object: small controls snap (stiff, around
300/24), large panels settle (soft, around 120/22), and headlines arrive fast
and then hold. If everything overshoots, nothing feels precise.

## Looking at it

- `omotion check scene.qml` lists problems for every format, with times. It
  measures every frame: text under 10 px at phone width (390 px wide), text
  held less than 0.6 s or one second per 25 characters, text outside the safe
  area or running into other things, and an empty frame 0. `--thorough` also
  asks for every frame, in order and shuffled, to find state between frames
  that the sampled moments miss.
- `omotion sheet scene.qml --format all` renders a contact sheet per format,
  with strips of the film's fastest changes and any frame unlike its
  neighbors below the beats; `--times 1.2,1.25,1.3` shows those moments instead.
- `omotion frame scene.qml --at 3.5 --format 9:16` renders one frame.
- `omotion review scene.qml` writes the next `reviews/NNN.md`: sheets,
  measures, scores, and the three largest defects with cropped evidence.
- `omotion render scene.qml` writes `renders/<scene>-16x9.mp4` and one file per
  other format, encoded with ffmpeg (H.264, with AAC audio when there's a track
  or a cue sound, brought to -14 LUFS, the loudness feeds play at).
  Each beat is encoded on its own and kept in `renders/.cache`, so after a
  patch only the beats whose frames changed are encoded again (`--fresh`
  encodes them all).
- `omotion open scene.qml` previews it, reloading whenever the file is saved.
