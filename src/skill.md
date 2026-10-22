---
name: omotion
description: |
  Direct and engineer motion videos (product films, launch clips, UI motion,
  data stories, kinetic typography) with omotion: a studio folder holding the
  brief, style guide, and shot list, and a QML scene in which every frame is a
  function of time, approved at gates by the person, checked, reviewed on
  contact sheets, and rendered to MP4 in 16:9, 9:16, and 1:1. Use for ANY
  request to make, change, review, or export a video or animation with omotion,
  and for any .qml file that is an omotion scene.
---

# omotion

You direct the film; the person approves it. Your work moves through six
gates, and each gate leaves files that prove it was passed. **The model
proposes the work; the person decides what ships.** A command that exited 0
is not an approval, and the first MP4 is a prototype.

Every command takes `--json`, exits 0 or 1, and writes errors to stderr;
`omotion help <command>` lists its options. No command needs a display.

## The studio

One folder per film, named for it (`jotter-sync/`; `film/` below stands for
that name). Keep every decision in it, never only in the chat, so a later
session can pick up where this one stopped.

```
film/
  brief.md          what the film says, and the facts about the product
  style-guide.md    palette, type, motion rules, and banned defaults
  shotlist.md       beats with timestamps, entry and exit states, and purpose
  assets/           approved logos, screens, footage, and sound, listed in assets.md
  scene.qml         the film (start it with: omotion new film/scene.qml)
  reviews/          contact sheets, stills, and defect notes
  renders/          the exports, one per format
  gates.md          which gates are passed, and who approved them
```

**At every stop**, your message carries the proposals the person should
approve and the questions only they can answer, with the files' paths for
the rest; don't paste whole files.

**Resuming:** read `gates.md` first. Continue from the first gate not marked
passed, and say which file is missing if a passed gate's evidence is gone.

`gates.md` looks like this, and you update it as you go. A gate you've
finished but the person hasn't approved stays unchecked and says what it's
waiting for:

```
- [x] 1 References and assets: approved by the person, 2026-10-23
- [ ] 2 Style guide and shot list: waiting for the person's approval since 2026-10-23
- [ ] 3 Representative stills
- [ ] 4 Rough cut and contact sheet
- [ ] 5 Three defects repaired
- [ ] 6 Audio, formats, and deliverables: approved by the person
```

## Gate 1: References and assets (the person approves)

Write `brief.md`. Separate the two kinds of line in it:

- **Facts about the product** (what it does, which devices, the price, the
  launch date, the call to action and link): ask for each one you weren't
  told. Never guess them, and never let a guess reach the screen.
- **Choices about the film** (the one sentence, the formats, the length):
  draft them yourself and mark them as proposals for the person to approve.
  Propose formats from where the film will be posted, such as 16:9 and 1:1
  for an X feed, or 9:16 for Stories, Reels, and TikTok.

```
FILM
One sentence: the idea the viewer should remember
Audience:
Duration: seconds
Formats: 16:9 / 9:16 / 1:1

ASSETS
Real logo: path
Real UI screenshots: paths
Brand type and colors:
Music or audio reference: path

VISUAL RULES
Keep: specific traits from the reference
Avoid: specific clichés (big centered text on a gradient, everything fading in, logo only at the end)
Never invent product screens, metrics, or logos

DELIVERABLES
Contact sheets, final videos, poster frames, source, asset list
```

Collect the real assets into `assets/` and list each in `assets/assets.md`
with where it came from. List the ones you still need there too, marked
missing, so the list is the request. **A missing product asset (a logo, a
screen, a product photo) is a blocker:** stop and ask for it, and never draw
a convincing fake. The film may invent camera moves and openings; it may not
invent the dashboard or replace the logo. Brand colors and fonts aren't
blockers: without them, you propose a style at gate 2. Without a logo, the
product's name set in type can stand in for it, but say it's a stand-in, not
a logo, and ask the person to approve it.

If the person has no such asset at all (a product that hasn't launched, a
logo still being designed), offer what the film can honestly do without it,
such as a typographic film with no product imagery, or waiting for the
asset. Let them choose, and write their choice in the brief.

**Stop.** Show the person the brief and the asset list, and wait for
approval before going on.

## Gate 2: Style guide and shot list (the person approves)

If you were given a reference (a frame, a film, or earlier work), look at it
and write down what you learned in `style-guide.md`, in measurable terms:

```
REFERENCE ANALYSIS
Palette: actual colors, not "premium"
Typography: family, width, weight, scale
Composition: where the eye goes first
Pacing: average time between meaningful changes
Motion: snap, glide, overshoot, cut, hold
Texture: grain, paper, glass, none

FONTS
Family and weight for each use; a file in assets/, or installed (fc-list)

MOTION RULES
Micro UI: quick, minimal overshoot
Panels: controlled settle
Camera: smooth, almost invisible
Headlines: strong entrance, stable hold

DO NOT COPY
Subjects, logos, copy, or product-specific imagery
```

Without a reference, write "none provided" under REFERENCE ANALYSIS, fill
the same headings with the style you propose, and say it's a proposal. If the
reference is unclear, ask, or render one small test frame; don't build the
film on a guess.

Propose only fonts that are installed (check with `fc-list`) or that you can
put in `assets/`. Once the style is approved, copy the font files into
`assets/` and load them in the scene with `FontLoader`, so the film renders
the same on another machine. Set `font.family` from the loader and the
weight and style to match the file; the family alone can pick an installed
face instead.

Write `shotlist.md`: what the viewer knows at each moment, not a list of
effects. It holds **every word that will appear on screen**, verbatim; the
person approves the copy here, word for word, and every claim in it must come
from the brief's product facts. Every beat has an entry state, an exit state, and a reason to exist;
if you can't say why a beat exists, it doesn't belong in the film. For UI
motion, list the states first (SEARCH -> RESULT -> DETAIL -> ACTION).

```
00-02s   HOOK      enters: empty   exits: the claim is on screen   why: earn attention
02-05s   PROBLEM   ...
```

Lay out each format on purpose: 16:9 has more spatial context, 9:16 fewer
elements and a stronger vertical hierarchy, 1:1 tighter type and shorter
holds. Never plan one wide film and crop it. The beats start at the same times in
every format; within a beat, timing may differ by format.

**Stop.** Show the person the style guide and shot list, and wait for
approval before writing the scene.

## Gate 3: Representative stills

`omotion new film/scene.qml`, then run `omotion help format`, which prints
the scene format and the Motion helpers for this version of omotion. Build
the scene from the shot list. Its
`beats` match `shotlist.md` one for one, and its `formats` list every format
in the brief. Use only files from `assets/`.

Run `omotion check film/scene.qml --json` after every edit and fix every
error. A clock or a stateful frame breaks rendering, and text outside the
frame is a layout bug in that format.

Render one still per beat in every format, `omotion frame film/scene.qml --at
<s> --format <f> -o film/reviews/`, and look at each before you go on. Wrong
type, color, or composition is cheapest to fix here.

## Gate 4: Rough cut and contact sheet

`omotion sheet film/scene.qml --format all -o film/reviews/`, then look at
every PNG. For each fast transition, render a strip of close moments:
`omotion sheet film/scene.qml --times 2.0,2.1,2.2,2.3 -o film/reviews/strip.png`.

Judge only the rendered frames, not what you meant to make, against this
checklist:

```
[ ] Text readable at phone size
[ ] No element crossing the safe area, or overlapping another
[ ] Real product assets, not invented replacements
[ ] Consistent type and color across beats
[ ] No awkward mid-transition frames
[ ] Cuts and sound cues land together
[ ] The first two seconds contain a real hook
[ ] The last frame is strong enough to be a poster
[ ] After each move, the viewer knows where to look
```

## Gate 5: Repair the three largest defects

Write `reviews/NNN.md` (001, 002, and so on). Score hook, readability,
continuity, motion, and sound sync from 1 to 5, then list **the three largest
defects**, each with:

- a timestamp and format,
- the evidence (the sheet, strip, or still that shows it),
- a local fix.

Patch only those sections, run `check`, then render the affected moments
again with `frame` or `sheet --times` to show each one is fixed. Repeat
gates 4 and 5 until no defect is worth fixing. No new evidence, no new
attempt: never re-render to "see if it's better" without a defect to fix.

## Gate 6: Audio, formats, and deliverables (the person approves)

- **Audio**: if the brief has a track, set `audio` in the scene and cut
  against its beats. Close your eyes and listen: if the sound has no shape
  without the picture, it isn't finished.
- **Render**: `omotion render film/scene.qml --json` writes one MP4 per
  format into `renders/`.
- **Poster frames**: the strongest frame in each format,
  `omotion frame film/scene.qml --at <s> --format <f> -o film/renders/`.
- **Hand-off**: give the person the videos, posters, contact sheets, and a
  short note on **what still needs human review**: anything you couldn't
  measure, or chose without their approval.

**Stop.** The person approves the film; mark gate 6 only when they do.

## Working with the person

- Never make an irreversible brand decision on their behalf: the logo, the
  claim, product facts, or the music license.
- If they ask to skip a gate, do it, and record in `gates.md` that it was
  skipped and by whom.
- If they have the scene open in the omotion window, your saved edits appear
  there at once. `omotion open film/scene.qml` opens it for them; it needs
  their desktop, so don't wait on it.
