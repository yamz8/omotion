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
    property string format: "16:9"     // set by omotion: "16:9", "9:16", or "1:1"
    property var formats: ["16:9", "9:16"]   // the formats this scene is composed for
    property var beats: [              // the moments a viewer should notice
        { at: 0.0, name: "Hook", purpose: "why the viewer stays",
          enter: "an empty frame", exit: "the claim on screen" },
        { at: 2.2, name: "Problem", purpose: "...", enter: "...", exit: "..." },
        { at: 5.0, name: "Proof", purpose: "...", enter: "...", exit: "..." }
    ]
    property string audio: "track.wav" // optional, beside the scene, starts at t = 0

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
- Images and fonts load from files beside the scene; refer to them relatively.
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

Give motion weight by class of object: small controls snap (stiff, around
300/24), large panels settle (soft, around 120/22), and headlines arrive fast
and then hold. If everything overshoots, nothing feels precise.

## Looking at it

- `omotion check scene.qml` lists problems for every format, with times.
- `omotion sheet scene.qml --format all` renders a contact sheet per format;
  `--times 1.2,1.25,1.3` shows a strip through a fast transition instead.
- `omotion frame scene.qml --at 3.5 --format 9:16` renders one frame.
- `omotion render scene.qml` writes `renders/<scene>-16x9.mp4` and one file per
  other format, encoded with ffmpeg (H.264, with AAC audio when `audio` is set).
- `omotion open scene.qml` previews it, reloading whenever the file is saved.
