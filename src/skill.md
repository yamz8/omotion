---
name: omotion
description: |
  Direct and engineer motion videos (product films, launch clips, UI motion,
  data stories, kinetic typography) with omotion: a studio folder holding the
  brief, style guide, and shot list, and a QML scene in which every frame is a
  function of time, approved at gates by the person, checked, reviewed on
  contact sheets, and rendered to MP4 in 16:9, 9:16, 1:1, and 4:5. Use for ANY
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

You're working in a film's folder, made by `omotion new <folder>`, and this
skill came with it. Keep every decision in its files, never only in the chat,
so a later session can pick up where this one stopped. The files start as
templates: fill them in and keep their headings. If you were asked for a film
and there's no studio yet, run `omotion new <folder>`, named for the film, and
work in it. (`film/` below stands for the folder's path.)

```
film/
  brief.md          what the film says, and the facts about the product
  style-guide.md    palette, type, motion rules, and banned defaults
  shotlist.md       beats, states, timestamps, and every word on screen
  assets/           the real logo, screens, footage, and sound, listed in assets.md
  scene.qml         the film; a starter until gate 3
  reviews/          stills, contact sheets, strips, and defect notes
  renders/          the exports, one per format
  gates.md          which gates are passed, and who approved them
  notes.md          the person's notes, each pinned to a moment of the film
  activity.md       your steps, which the person watches
  AGENTS.md         points agents to this skill
```

**Start every turn** with `omotion status film/ --json`: the gate you're at,
what each gate's evidence lacks, and the open notes, which you then read in
`notes.md`. Continue from the first gate not passed. If a passed gate's
evidence is gone, `status` fails and names the file: tell the person. A gate you've finished but the person hasn't
approved stays unchecked and says what it waits for:

```
- [x] 1 References and assets: approved by the person, 2026-10-23
- [ ] 2 Style guide and shot list: waiting for the person's approval since 2026-10-23
```

**At every stop**, your message gives the proposals to approve and the
questions only the person can answer, with the files' paths for the rest;
don't paste whole files. If `omotion check` says this skill is out of date,
tell the person; `omotion skill install` in the folder updates it.

## Gate 1: References and assets (the person approves)

Fill in `brief.md`, keeping two kinds of line apart:

- **Facts about the product** (what it does, which devices, the price, the
  launch date, the call to action and link): ask for each one you weren't
  told. Never guess them, and never let a guess reach the screen.
- **Choices about the film** (the one sentence, the formats, the length):
  draft them and mark them as proposals. A proposal states only facts the
  person gave you, with a visible gap for anything unconfirmed. Propose
  formats from where the film will be posted: 16:9 and 1:1 for an X feed,
  4:5 for the Instagram and Facebook feeds, 9:16 for Stories, Reels, and
  TikTok.

Collect the real assets into `assets/` and list each in `assets/assets.md`
with where it came from, naming its file by its path in `assets/`
(`screens/editor.png`, or `slides/*.png` for a set); list the ones you still
need there too, marked missing. Fonts and sound are assets too. `check`
fails on a file the film loads from outside `assets/` and on a listed file
that isn't there, and warns about one that isn't listed. **A missing product asset (a logo, a screen, a product photo) is a
blocker:** ask for it, and never draw a convincing fake. The film may invent
camera moves and openings, never the dashboard or the logo. Whether the film
shows the product at all is a choice you can offer: without a logo, the name
set in type can stand in, said to be a stand-in; for a product that hasn't
launched, a type-only film. Let the person choose, and write it in the brief.
Brand colors and fonts don't block: without them you propose a style at gate 2.

**Stop.** Show the brief and the asset list, and wait for approval.

## Gate 2: Style guide and shot list (the person approves)

Given a reference (a frame, a film, earlier work), write what you learned in
`style-guide.md` in measurable terms: actual colors, not "premium";
typefaces and weights; where the eye goes first; the time between meaningful
changes. Without one, write "none provided" and fill the same headings with
the style you propose, marked as a proposal. If a reference is unclear, ask
or render one test frame rather than build on a guess.

Write the palette, fonts, and motion rules as `TOKENS` too (`color ink
#f4f1ea`, `font headline Adwaita Sans`, `rule panel 120 22`); the scene uses
them as `Style.colors.ink` and `Motion.rule("panel", t, start)`, and `check`
reports anything outside them. Propose only fonts that are installed
(`fc-list`) or that can go in `assets/`. Once approved, copy the font files into `assets/` and load them
with `FontLoader`, setting the weight and style to match the file, so the
film renders the same elsewhere.

Fill in `shotlist.md`: what the viewer knows at each moment, not a list of
effects. Every beat has an entry state, an exit state, and a reason to
exist; cut any beat you can't justify. For UI motion, list the states first
(SEARCH -> RESULT -> DETAIL). The shot list holds **every word on screen**,
verbatim; the person approves the copy here, and every claim in it comes from
the brief's facts. Words inside your assets are on screen too: a screenshot
or demo slide showing a price, a plan, or a number the brief doesn't have
makes that claim to the viewer. Replace that sample content, or ask.

Compose each format on purpose: 16:9 has more spatial context, 9:16 fewer
elements and a stronger vertical hierarchy, 4:5 a tall feed post between the
two, 1:1 tighter type and shorter holds. Never crop one from another. Beats start at the same times in every
format; within a beat, timing may differ.

**Stop.** Show the style guide and shot list, and wait for approval before
writing the scene.

## Gate 3: Representative stills

Run `omotion help format` for the scene format and the Motion helpers. Replace
the starter in `scene.qml` with the film from the shot list: its `beats`
match `shotlist.md` one for one, with the same names and start times, each
with its `purpose` and its `enter` and `exit` states copied from the shot
list, its `formats` list every format in the
brief, and it uses only files from `assets/`. Build each beat to the entry
and exit states the person approved. If one can't be built as written, or you
see a better way, say so at the next stop instead of quietly changing it:
the approval was for those shots.

Run `omotion check film/scene.qml --json` after every edit and fix every
error: a clock or a stateful frame breaks rendering, and text outside the
frame is a layout bug in that format.

Render one still per beat in every format (`omotion frame film/scene.qml --at
<s> --format <f> -o film/reviews/`) and look at each. Type, color, and
composition are cheapest to fix here.

## Gate 4: Rough cut and contact sheet

Render `omotion sheet film/scene.qml --format all -o film/reviews/` and look at
every PNG. Judge only the rendered frames, not what you meant to make.

**`omotion check` measures what stills miss**, at every frame and in every
format; fix each error it reports:

- **Text size at phone width.** In a feed the film plays about 390 px wide,
  and text must be at least 10 px there: about 50 px in a 1920-wide frame,
  28 px in a 1080-wide one.
- **Holds.** Each text is fully on screen for at least 0.6 s, or one second
  per 25 characters if that's longer.
- **The safe area**, text running into other text or across the edge of a
  picture, colors and fonts outside the style guide, a one-color frame 0
  (often the thumbnail), and nothing changing in the first two seconds.

**Measure what `check` can't read:** a mark or badge that means something
needs the same hold as text; find it with a strip across its entrance and
exit (`omotion sheet ... --times 5.1,5.2,5.3,5.4,5.5 -o
film/reviews/strip-badge.png`). The sheet adds strips of the film's fastest
changes; look at each, and at a strip across every cut, for empty or
half-drawn frames. Words inside pictures are too small for `check` to see:
judge them at phone width.

**Then judge the rest:** each beat reaching the exit state in
`shotlist.md`; real product assets, never replacements, and no claim inside
them the brief doesn't make; cuts and sound cues landing together; whether
the hook is a real one; a last frame strong enough to be the poster; and
after each move, the viewer knows where to look.

## Gate 5: Repair the three largest defects

The person's open notes come first. Then run `omotion review
film/scene.qml --json`. It writes the next `reviews/NNN.md` with its
evidence in `reviews/NNN/`: sheets with strips, everything `check` measures,
scores as a floor, and the three largest measured defects with cropped
frames. It sends the film back while `check` has errors. Complete each ✎:
your own scores, where judgment is lower; **the three largest defects** you
see, which may outrank the measured ones, each with a timestamp, format,
evidence, and a local fix.

Patch only those sections, run `check`, and render the affected moments again
to show each fix, into a folder of the round's own (`-o
film/reviews/round2/`), so it never replaces the evidence it's fixing. The
next `review` starts the next round. Repeat gates 4 and 5 until no defect is
worth fixing. No new
evidence, no new attempt: never re-render to "see if it's better" without a
defect to fix.

## Gate 6: Audio, formats, and deliverables (the person approves)

- **Audio:** if the brief has a track, put it in `assets/`, set `audio` in
  the scene, run `omotion beats` on it, and cut to its beats; `check` holds
  the cuts to them. Give each moment that should be heard a cue with its
  sound, `on` the beat it belongs to. If the sound has no shape without the
  picture, it isn't finished. The music's license is the person's decision.
- **Render:** `omotion render film/scene.qml --json` writes one MP4 per format
  into `renders/`.
- **Posters:** the strongest frame in each format (`omotion frame ... -o
  film/renders/`).
- **Hand-off:** the videos, posters, contact sheets, and a short note on
  **what still needs human review**: anything you couldn't measure, or chose
  without approval. A problem you could fix belongs in gate 5, not here.

**Stop.** Mark gate 6 only when the person approves the film.

## The person's notes

The person watches the studio in the omotion window (the gates, assets,
evidence, your activity, and the film, which reloads each time you save) and
answers by pinning notes to moments of it. Each open note in `notes.md` gives
a time, a format, a beat, a point, often the text under it, and a frame with
the point ringed. **Each one is a defect:** fix it locally, render that moment
again to prove it, name the proof after the note, never over existing
evidence, then tick it with a `fixed:` line:

```
- [x] 004 · 12.60s · 16:9 · Gates · (1152, 446) · on "you approve"
  evidence: reviews/notes/004.png
  note: these marks leave too soon
  fixed: marks arrive at 11.2–11.4 s and hold to 13.05 s; proof reviews/fix-note004-16x9.png
```

Never tick a note without that proof.

- A note that leaves a value open ("make it pink"): choose one, fix it, add a
  `reply:` asking the person to confirm, and leave it open.
- A note you think is wrong, or that needs the person's decision: add a
  `reply:` and leave it open.
- A note that changes something approved (style guide, shot list, copy) is
  the person's decision: apply it, update that file, and add "amended by note
  004" to that gate's line in `gates.md`.
- A note on a film approved at gate 6 reopens it: re-render, untick gate 6,
  and wait for the person.

A note is never an approval, even "looks good". Say in your message which
notes you ticked and which you left open, and why.

**Activity:** the person follows your work live in the window, so as you
finish each step that changes the studio, not at the end, add a line to
`activity.md` (create it if missing): the local time as HH:MM (`date
+%H:%M`), what changed, and its evidence, such as `- 14:02 rebuilt the gates
beat; strip in reviews/strip-gates-16x9.png`.

## Working with the person

- Never make an irreversible brand decision for them: the logo, the claim,
  product facts, or the music license.
- If they ask to skip a gate, do it, and record in `gates.md` that it was
  skipped and by whom.
- `omotion open film/scene.qml` opens the studio window for them; it needs
  their desktop, so don't wait on it.
