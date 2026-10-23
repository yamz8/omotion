# The five-film test

Each film is made by an agent in its own studio, through the six gates, from
a different brief. Each record says what the agent caught itself, what
omotion caught, what a person or a reviewer caught after them, and where the
agent struggled: with the gates, the checks, or writing QML. Scores are the
agent's own from `reviews/001.md`, then the checklist as the person's side
found it.

| Film | Brief | Formats | Gates | Agent's scores (hook, read, cont., motion) | Defects missed by the agent |
| --- | --- | --- | --- | --- | --- |
| 1 | omotion itself | 16:9, 1:1 | all six, approved | 4, 4, 4, 3 | 2 |
| 2 | Hype, Markdown slides | 16:9, 1:1 | 1-5, gate 6 waiting | 4, 3, 4, 4 | 3, and 2 in process |
| 3 | | | | | |
| 4 | | | | | |
| 5 | | | | | |

## Film 1: omotion (`~/Videos/my-first-film`)

15 s, silent, 6 beats. Approved at gate 6 on 2026-10-23.

**Caught by the agent** (review 001, all fixed with proof): a crossfade that
let the background show through at 13.95-14.05 s; a dot entering from
outside the 1:1 frame at 1.65 s; an almost empty frame at 5.00-5.30 s.

**Caught after the agent:**
- A peer review found the 16:9 supporting text at 6-8 px on a phone-width
  feed. The agent hadn't measured size at phone width. **Response:** the
  skill's gate 4 now measures it (a377ee3), with the arithmetic per format.
- The person's note 001: the film opens on 0.2 s of black, so the
  thumbnail is empty. Still open. **Response:** gate 4 now checks the first
  frame.

**Struggled with:** the checks it had no numbers for (size, holds), not the
gates and not QML. Its scene passed `check` in both formats.

## Film 2: Hype (`~/Videos/hype-film`)

20 s, silent, 6 beats. Gates 1-5 passed; gate 6 waits for the person.

**Caught by the agent** (review 001, all fixed with proof): half-drawn
captions at the cuts at 13.0 and 14.35 s; an empty frame and a vanishing
logo at 16.5 s; the editor fading in as a ghost at 6.05 s. It measured every
text size and hold, which film 1 hadn't.

**Caught after the agent** (one-per-second sheets and strips of the MP4s):
- 14.35-16.5 s: a demo slide shows a $9 / $29 price table under "Present
  fullscreen. Export to PDF or PowerPoint.", which reads as Hype's pricing.
  The brief has no price. **Response:** the skill now treats words inside
  assets as claims (gate 2 and gate 4).
- 3-6 s: the slide never fills the frame, against the approved shot "scales
  up to fill". **Response:** beats now carry their `enter` and `exit` states,
  and `check` matches them to `shotlist.md`; the skill says to raise a change
  to an approved shot instead of making it quietly.
- 3.48 s: a one-frame ghost at the cut. **Response:** none yet; automatic
  transition strips (v0.3) would put it on the sheet.

**In process:**
- Gates 3 to 6 were all logged at 06:52, written at the end rather than as
  each happened. **Response:** the skill now says to log each step as it
  finishes.
- Its hand-off listed fixable problems as "needs human review".
  **Response:** gate 6 now says a problem it could fix belongs in gate 5.
- Its asset list names "slides/slide-00N.png + slides.json" and leaves out
  the three fonts the film loads. **Response:** the asset manifest check
  (5bcbcd8) now fails on both.

**Struggled with:** holding to approved shots, and the claims inside real
assets. Again not QML: the scene was clean in both formats, and its motion
chain (Markdown into slide into editor) was its strongest beat.

## What the tests of the skill show

Three test studios, each run by a fresh agent with the skill (in
`tests/skill/evals/`, results in `omotion-skill-workspace/`). Iteration 2 ran
with `status`, the beat contract, and the asset manifest:

| Case | Checks passed | What it shows |
| --- | --- | --- |
| rough-cut-review | 7 of 7 | Caught the planted short hold, small caption, drifted screenshot and price; kept each round's proof in its own folder |
| gate1-stop | 4 of 4 | Asked for the eight product facts, proposed the rest, stopped |
| notes-round | 6 of 6 | Fixed with proof, left an open value open, treated "looks good!" as no approval, reopened gate 6 |
