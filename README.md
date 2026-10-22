# omotion

![omotion previewing the starter scene's Review beat](screenshot.png)

omotion previews, reviews, and renders motion scenes for Omarchy. A scene is
one QML file in which everything on screen is computed from the time `t`, so
any frame can be asked for on its own, in any order. That makes revisions
local: a title that enters too early is one number, a shape that overshoots
is one spring.

It's built for the way agents make video. You (or your coding agent) write
the scene, and omotion:

- **previews it live**, reloading whenever the file is saved and staying on
  the moment you were looking at;
- **shows a contact sheet**, every beat at once, so you look at the frames
  before anyone watches the film;
- **checks it**: no wall clocks, no randomness that changes, no state carried
  between frames, no text outside the frame in any format;
- **renders each format from the source**: 16:9, 9:16, and 1:1 are composed
  by the scene, not cropped from one another, and encoded with ffmpeg.

## A scene

```qml
import QtQuick
import Omotion

Rectangle {
    property real t: 0
    property real duration: 6
    property var formats: ["16:9", "9:16"]
    property var beats: [{ at: 0, name: "Hook" }, { at: 3, name: "Proof" }]

    color: "#0d0d10"

    Text {
        id: title
        readonly property real arrive: Motion.spring(parent.t, 0.2)
        anchors.centerIn: parent
        text: "Hello"
        color: "white"
        font.pixelSize: 140
        opacity: Motion.clamp(arrive, 0, 1)
        transform: Translate { y: (1 - title.arrive) * 60 }
    }
}
```

`omotion new film/scene.qml` writes a fuller starter, and `omotion help
format` prints the whole format and the `Motion` helpers.

## Shortcuts

| Key | Action |
| --- | --- |
| Space | Play or pause |
| ← → | Step one frame |
| Shift ← → | Jump one second |
| [ ] | Previous or next beat |
| Home End | Start or end |
| F | Next format |
| C | Contact sheet |
| E | Export this format |
| Shift E | Export every format |
| O | Open a scene |
| N | New scene |
| R | Reload the scene |
| ? | Show shortcuts |
| Q | Quit |

## Commands

None of these need a display. Each takes `--json`, exits 0 or 1, and writes
errors to stderr.

```sh
omotion new film/scene.qml                   # start from the starter
omotion check film/scene.qml                 # every problem, in every format
omotion sheet film/scene.qml --format all    # contact sheets as PNG
omotion frame film/scene.qml --at 3.5        # one frame as PNG
omotion render film/scene.qml                # renders/scene-16x9.mp4, scene-9x16.mp4
omotion open film/scene.qml                  # the window
omotion skill install                        # teach your coding agent the workflow
```

`omotion skill install` puts a short skill in `~/.agents/skills/omotion` and
links it into `~/.claude/skills`. It walks an agent through brief, beats,
check, contact sheet, then render, stopping at each gate.

## Building

omotion is C++ and Qt Quick, built with qmake6 and make. Everything it needs
ships with Omarchy.

```sh
bin/build      # build/omotion
bin/test       # the Qt Test suite, offscreen
bin/install    # makepkg -fsi, so omotion shows up in the app launcher
```

## License

MIT. See [LICENSE](LICENSE).
