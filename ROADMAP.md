# omotion roadmap

The goal: make omotion the best tool for **motion engineering**, as described in
["Motion Engineering: Build a Video Studio Around Opus 5.5"](https://x.com/0xwhrrari/status/2105643919119696297).
That article's point is that one prompt makes a clip, but a *studio* makes the
next clip better:

```
PROMPT -> CLIP
BRIEF -> SYSTEM -> FILM -> REVIEW -> BETTER SYSTEM
```

omotion wins if **the tenth film made with it is measurably better than the
first**, and if an agent can get there with a person approving at a few gates
instead of supervising every step.

## Principles

These come straight from the article, and every milestone is judged against them.

1. **The model proposes; the system decides what ships.** Approval is a gate a
   person passes, not a command that exited 0.
2. **Evidence over intent.** A film is done when its frames prove it works, not
   when the encoder finished. No new evidence, no new attempt.
3. **Files, not chat.** Every decision lives in a file the next run can read, so
   a run can resume from a completed gate.
4. **Real assets or a blocker.** A missing screenshot stops the run; it's never
   drawn.
5. **Every frame is a deterministic query.** `frame = render(t, shotlist, assets,
   style, seed)`, renderable directly, in any order.
6. **Measure what can be measured; name who judges the rest.** Each checklist
   item is either checked by omotion or assigned to the agent or a person in
   the review file.

## Where we are: v0.1

The article has four layers: Direction, Specification, Rendering, Verification.
v0.1 covers **Rendering** well and **Verification** partly. **Specification**
covers only named beats, and there is no **Direction** layer.

| Article | v0.1 |
| --- | --- |
| Frame function of t, seekable | ✅ QML scenes; `check` catches clocks, randomness, and state carried between frames |
| Contact sheet before export | ✅ Live sheet in the window, `omotion sheet`, manual `--times` strips |
| Variants from the source | ✅ 16:9, 9:16, and 1:1 compositions, one render each |
| Beats | ⚠️ Name and start time only; no entry state, exit state, or purpose |
| Seekability check | ⚠️ Samples beat midpoints and the first and last frames, not every frame |
| Safe area | ⚠️ Only text crossing the frame edge; misses overlaps and non-text elements |
| Motion with weight | ⚠️ `Motion.spring` and guidance; no rules per class, no camera |
| Sound on the timeline | ⚠️ One file muxed from t = 0; no beat grid or cues; playback untested |
| Brief, gates | ⚠️ In the agent skill only, with **no stops for human approval** |
| Reference analysis, style guide | ❌ |
| Review checklist, scoring, critique | ❌ |
| Studio folder, deliverables | ❌ `omotion new` writes one file; no poster frame or storyboard |
| Partial re-render | ❌ Whole-film renders only |

## Milestones

Each milestone ends with a **done when** that can be demonstrated, not argued.

Sizes are relative guesses for one person working with an agent: **S** is
days, **M** about a week, **L** several weeks. Items marked **(spike)** haven't
been shown to be buildable well. Each gets a prototype of a day or two before
its milestone is scheduled, and is cut or redesigned if the prototype fails.

### v0.2: The studio, not the scene (Direction and gates) · M

Closes the largest gap: today an agent can go from prompt to MP4 without a
person approving anything.

- ✅ **`omotion new <dir>` scaffolds the studio** from the article's §9:
  `brief.md`, `style-guide.md`, `shotlist.md`, `assets/` with `assets.md`,
  `scene.qml`, `reviews/`, `renders/`, and `gates.md`. Each file starts as the
  article's template (§1 brief, §2 reference analysis, §3 beat sheet).
- ✅ **The agent skill lives in the studio**, at `.claude/skills/omotion/SKILL.md`
  with an `AGENTS.md` pointing to it, rather than in the home folder. It's
  stamped with the omotion that wrote it, so it travels with the film and
  always matches it; `check` reports a stale one, and `omotion skill install`
  in the folder updates it.
- ✅ **Beats become a contract.** Each beat gains `purpose`, `enter`, and `exit`.
  `check` makes a missing purpose an error, and checks that `shotlist.md` and
  the scene's `beats` agree.
- ✅ **An asset manifest.** `assets/` lists the approved files. `check` flags any
  image or font loaded from outside it. A file named in the brief but missing
  from `assets/` is a blocking error, not a warning.
- ✅ **`omotion status`** reports which gate the project has reached, what each
  gate is missing, and whether each gate was approved. It reads `gates.md`,
  the plain checklist the skill already keeps and a person can edit, so a new
  run resumes where the last one stopped.
- ✅ **The skill is rewritten around the article's six gates**, with real stops:
  1. Approve references and product assets
  2. Approve the style guide and shot list
  3. Inspect representative stills
  4. Rough cut and contact sheet
  5. Repair the three highest-impact defects
  6. Audio, formats, and final deliverables

  The agent stops for the person at gates 1, 2, and 6, and never makes an
  irreversible brand decision on its own.

**Done when:** an agent given only a brief and a folder of real assets stops
for approval at gates 1 and 2 before writing a frame (✅ shown with two test
agents on the skill alone). A second agent session,
started cold, can resume from `omotion status` alone.

### v0.2.5: The studio window · M ✅

The agent keeps working headless in the film's folder; the omotion window
becomes the studio that watches it. Directing film 1 meant relaying notes
through chat and finding moments by hand; the studio makes the person's eye a
direct input to the loop, in files (§9), with evidence (§7).

- **Gates panel** from `gates.md`: passed, current, and waiting for the
  person; each opens the files that are its evidence.
- **Assets panel**: thumbnails of `assets/`, with the ones `assets.md` lists
  as missing flagged.
- **Notes pinned to moments.** Click the preview to write a note at that time,
  format, and point. omotion saves it to `notes.md` with the beat, the text
  under the pointer, and a frame from the render with the point marked, as
  evidence. The skill treats every open note as a defect: fix it locally,
  prove it with a frame or strip, and tick it, or reply and leave it open.
  Notes never pass a gate; only the person does.
- **Activity** from `activity.md`, where the agent writes one line per step
  with its evidence.
- **Reviews and renders** one click away, newest first.
- **Copy a note** for the agent's terminal in one click.

**Done when:** directing a film needs no relaying: the person pins a note in
the window, the agent fixes it with proof, and the tick appears in the window
without the person writing a message beyond "check my notes". (✅ Shown on a
copy of film 1: a note pinned through the window, an agent told only "check
my notes" fixed it with ringed proof frames, reopened gate 6, and asked to
confirm the one value the note left open. Its `fixed:` line, the reopened
gate, and its activity appeared in the window.)

### Checkpoint: prove the loop, then choose the renderer · S

Before v0.3, test whether the studio works, because everything after it is
a bet on the answer.

- **A five-film test.** An agent makes five short films in a row in one
  studio, through the v0.2 gates, from five different briefs. Score each
  against the checklist, and record where the agent struggled: the gates, the
  checks, or writing QML.
- **The renderer decision.** If agents write good QML scenes after the format
  guide and `check` feedback, stay QML-only and continue with v0.3 as
  planned. If they keep struggling with QML, make the studio layers (gates,
  checks, reviews, sheets) work with other renderers first, starting with
  HTML or React rendered through a headless browser, and put the QML-only
  features in v0.5 on hold.

**Done when:** the five films and their scores are in the repository, the
decision is written down with its reasons, and the milestones below are
revised to match it.

**So far** (`docs/checkpoint/films.md`, `docs/checkpoint/renderer.md`): films
1 and 2 are scored. Neither agent struggled with QML; what they missed was
judgment and process: text size at phone width and a blank first frame
(film 1), and in film 2 a price inside a demo slide, an approved shot built
differently without a word, a one-frame ghost at a cut, and gates logged all
at once at the end. Each now has a rule in the skill or a check
(`status`, the beat contract, the asset manifest), except the ghost frame,
which waits for automatic transition strips in v0.3.

### v0.3: Evidence (Verification) · L

Turns the article's §7 checklist and critique into tooling.

- **Every item on the review checklist is either measured or explicitly assigned:**

  | Checklist item | How |
  | --- | --- |
  | Text readable at phone size | Measured: rendered glyph height at phone scale for each format **(spike)**: what size counts as readable, and measuring it reliably from the item tree |
  | No element crossing the safe area | Measured: every visible item against a per-format safe margin |
  | Real product assets | Measured: manifest check (v0.2) |
  | Consistent type and color | Measured: fonts and colors used per beat, compared with the style-guide tokens |
  | No awkward mid-transition frames | Strips found automatically (below); judged by the agent and a person |
  | Cuts and sound cues land together | Measured in v0.4 |
  | First two seconds contain a real hook | Measured: visible change before 2 s; quality judged by the agent |
  | Last frame strong enough to be a poster | Last frame exported as `poster.png`; judged |

- ✅ **Overlap detection**: text over content that isn't background. This would
  have caught the chip running into the screenshot in our launch film.
- ✅ **On-screen time per text**: words shown versus seconds held, so headlines
  "hold long enough to be read" (§5).
- ✅ **Automatic transition strips.** Score how fast each frame changes from the
  last at low resolution, and put the fastest transitions on the sheet without
  being asked.
- ✅ **`check --thorough`** asks for every frame in a shuffled order, not just
  sampled moments.
- ✅ **`omotion review`** writes `reviews/NNN.md`: contact sheets, measured
  results, and the article's critique format, with **the three largest
  defects, each with a timestamp, a cropped image as evidence, and a local
  fix**. It also scores hook, readability, continuity, motion, and sound sync,
  then accepts or sends back.
- ✅ **Partial re-render.** Cache frames by a hash of the scene and its inputs,
  and re-render only the ranges a patch changed, as §7 asks ("render the
  affected frames again").

**Done when:** the defects found by hand in the launch film are reported
automatically, with timestamps. After a patch to one beat, re-rendering
touches only that beat's frames, and checking and rendering a sheet for a
15-second film take under 10 seconds.

**Status:** built. On film 1, `check` reports the defects people found by
hand: the 16:9 supporting text at 6-8 px on a phone (the peer review) and
the empty frame 0 (the person's note 001), each with its time and place. A
one-line patch to one beat re-encodes 96 of 450 frames, and the result
matches a fresh render exactly. Checking and sheeting film 1 in both formats
takes 6.0 s of CPU; on this machine, with a load average of 16 on 8 cores,
that was 11.9 s of wall time, so the 10-second target still wants a run on
an idle machine. Not measured: words inside pictures, and the price in film
2's demo slide, which only a reviewer reading the frames can catch.

### v0.4: Sound on the same timeline · L

From §6: "If it has no shape without the picture, the sound design is
probably not finished."

- ✅ **Cues** declared in the scene (`{ at: 1.5, name: "click" }`) and shown on
  the timeline beside the beats.
- ✅ **`omotion beats track.wav`** measures a track's tempo and beats (onset
  detection on ffmpeg-decoded audio) and writes a beat grid the scene can cut
  against. **(spike)**: whether simple onset detection is accurate enough on
  real music without a dedicated library.
- ✅ **A waveform on the timeline**, scrubbed audio, and an offset and trim for the track.
- ✅ **Synthesized cues**: deterministic clicks and hits placed at cue times and
  mixed at render, for films without a licensed track.
- ✅ **Sync check**: each cue paired with the visual event it belongs to (a beat,
  or a named moment), with drift reported in frames.
- ✅ Loudness normalized on export.

**Done when:** a film's declared cues land within one frame of their visual
events, verified by `check`, and audio playback in the window is covered by a test.

**Status:** built. `check` reports a cue more than a frame from its beat or
from the change on screen nearest it, and a cut more than a frame from the
music's beat; the window plays the track, trimmed and delayed, with the
cues' sounds, as the render mixes them, and a test plays it. The spike's
answer so far: on a drum pattern at 97 BPM with off-beat ticks, energy-onset
detection finds the tempo within a beat a minute and every kick within a
frame. There's no music on this machine to try it on, so real tracks, with
soft attacks and tempo changes, are still to test.

### v0.5: Craft (motion, continuity, style) · M–L

From §3 and §5: motion that carries mass and intent, and objects that stay
recognizably the same across states.

- ✅ **Motion rules per class**, defined in the style guide (micro UI, panels,
  camera, headlines, mascot) and used as `Motion.rule("panel", t, start)`.
  `check` warns about springs that ignore the rules.
- ✅ **A `Camera`**: deterministic pan, zoom, and parallax over the scene, slow
  enough to keep the viewer oriented.
- ✅ **States and continuity**: a state list (`SEARCH -> RESULT -> DETAIL`) with
  `Motion.between(states, t)`, interpolating one object's size, color,
  content, and position across states.
- ✅ **Style tokens**: palette and type from `style-guide.md` exposed as a `Style`
  object, so a film can't drift from its own guide.
- ✅ **4:5**, the Instagram feed's format (1080×1350), beside 16:9, 9:16, and 1:1;
  the first test agent asked for an Instagram film couldn't offer it. Moved
  ahead of the checkpoint after a second agent hit the same gap.
- ✅ **First-class formats**: per-format timing (1:1 gets "tighter typography and
  shorter holds", §8), layout helpers, and a starter composed for all three.
- ✅ **A global seed** (`render --seed`) for repeatable variations.

**Done when:** the article's five-beat example film (hook, problem, reveal,
proof, close) is written using only rules, states, and tokens: `check` finds
no spring outside a rule and no color or font outside the style guide. It
passes every v0.3 check in all three formats.

**Status:** built. `examples/launch/` is that film: hook, problem, reveal,
proof, and close, 15 seconds in 16:9, 9:16, 1:1, and 4:5, using only the
guide's tokens, `Motion.rule`, `Motion.between`, and `Camera`. `check`
reports no errors and no warnings in all four formats; on the way, it caught
the headline running into the card in 1:1 and 4:5. A person still has to
judge it: its gates 1, 2, and 6 wait.

### v0.6: References and footage (Specification) · L

From §2: "If you do not supply a visual grammar, the model supplies its own."

- ✅ **`omotion reference <video or images>`** extracts frames and measures what
  can be measured: palette, pacing (time between scene changes), and motion
  energy. It writes the numbers into `style-guide.md` for the agent to
  complete, with the article's "do not copy" section.
- ✅ **Asset capture** from a real product site into `assets/`, using a browser
  if one is installed, then listed for approval at gate 1. **(spike)**:
  driving a headless browser from omotion, and handling sites that need a login.
- ✅ **Seekable footage**: a `Clip` item that shows a video file's frame at t,
  for the photoreal and organic shots the article says code-made motion
  can't do. **(spike)**: exact frame-accurate seeking, and whether decoding
  each frame on demand is fast enough to render.

**Done when:** for three reference films, the style guide's measured palette
is within ΔE 5 of colors sampled from the reference, and its average time
between scene changes is within 20% of the reference's. A film made from that
guide also lands within those same tolerances when measured the same way.

**Status:** built. A test renders three reference films with known colors and
shot lengths, measures each within ΔE 5 and 20%, then makes a film from each
measured guide and measures that within the same tolerances. On omotion's
own films, scene changes land within 0.2 s of the beats. The spikes'
answers: capture drives the installed Chromium headless in a profile of its
own, so pages behind a login are left to the person's own screenshot; and a
clip decoded once into numbered frames is exactly frame-accurate, 60 frames
asked for out of order in under 2 s, decoding included.

### v1.0: Production · L

- ✅ **Deliverables from §1 and the final prompt**: final videos, poster frames,
  contact sheets, storyboard PDF, source, asset list, and a short "what still
  needs human review" note.
- ✅ **Speed**: parallel rendering across processes by frame range; progress and
  cancellation in the window and the CLI.
- ✅ **Reproducibility**: fonts and assets bundled with the project, so a film
  renders identically on another machine.
- ✅ **Studio templates** (product launch, UI walkthrough, data story) so "the next
  video starts with a real base": swap the brief and assets, keep the render
  contract and checklist.
- **Release**: tagged releases, the `omacom/omarchy-pkgs` package, all 50 Omarchy
  languages, and a gallery of films with their studios.

**So far:** the window speaks all 50 Omarchy languages; 42 of the catalogs
are drafts that a native speaker should review, especially the words for
gates, beats, assets, and contact sheets.

**Done when:** one command produces every deliverable for a template studio.
The same studio renders on a second machine with every frame matching within
the seek check's tolerance. omotion installs from `omarchy-pkgs` on a fresh
Omarchy machine.

## How we'll know it's number one

The claim is about getting better, so measure exactly that:

- **The tenth-film test.** Run an agent through ten briefs in sequence on one
  studio. Score each film against the checklist. The scores should rise, and
  defects should be caught by omotion before a person sees them.
- **Against the one-prompt baseline.** For the same briefs, compare a one-shot
  film with the gated omotion film, scored blind by people on the article's
  five dimensions (hook, readability, continuity, motion, sound sync).
- **Time to an approved film**, and how many human decisions it took (the
  target is three gates, not dozens of corrections).
- **Coverage**: the share of checklist items measured rather than left to judgment.

## Risks and open questions

- **Models know React far better than QML.** The article names Remotion (React
  components at known frame numbers) and HyperFrames (an HTML composition). An
  agent is likelier to write good Remotion code on the first try.
  Mitigations: a strong format guide, worked examples, and `check` feedback
  that teaches. Whether omotion should also accept HTML or React scenes is
  decided at the checkpoint after v0.2, from evidence rather than guesswork.
- **"Number one" may need reach beyond Omarchy.** omotion is Qt and runs on
  other Linux distributions and macOS, but packaging, the portal file picker,
  and theme following are Omarchy-specific today. A decision for after v1.0.
- **GPU differences across machines.** Seek checks already tolerate glyph
  antialiasing; reproducible renders on other hardware need testing (v1.0).
- **Judgment can't be fully measured.** Whether the hook is good stays with the
  agent and the person. omotion's job is to put the evidence in front of them
  and record the decision.

## Not doing

- A general video editor (multi-track editing, effects browsers). Scenes are code.
- A generative video model. Footage comes in as assets.
- Cloud rendering, before local rendering is fast and reproducible.

## First steps

The three cheapest changes with the most effect, in order:

1. **Rewrite the skill around the six gates**, with stops for approval.
2. **`omotion new <dir>` scaffolds the studio** with the article's templates.
3. **Overlap and safe-area checks**, so the defect we found by eye is found
   automatically.

Then the studio window (v0.2.5), which film 1 showed is what directing needs
most, and then the checkpoint: the five-film test and the renderer decision.
Film 1 is the first of the five.
