# The renderer decision: the case so far

**Status:** a draft from two of the five films. The decision is the
person's, after films 3 to 5.

The question from the roadmap: do agents write good QML scenes after the
format guide and `check` feedback, or should the studio layers (gates,
checks, reviews, sheets) also work with HTML or React rendered in a browser?

## The evidence

**For staying QML-only:**
- Both films' scenes passed `check` in every format on the agent's first
  complete pass: no wall clocks, no state between frames, no text outside the
  frame.
- Neither agent asked about QML or fought it. Film 2's strongest beat was a
  continuous chain of motion between three states, built from
  `Motion.spring` and `Motion.progress` alone.
- Every defect found after the agent, in both films, was about judgment or
  process: text size at phone width, a blank first frame, an approved shot
  changed, a price inside an asset, gates logged late. None would change
  with another renderer, and each now has a rule in the skill or a check in
  omotion.
- The three skill tests ran end to end in 1-3 minutes each, including
  editing scenes, without a QML error reaching the person.

**For adding a browser renderer:**
- Models still know React and HTML better, which matters most for films
  this test hasn't tried: data stories with charts, long kinetic type, and
  anything that wants an existing web component.
- QML is tied to Qt's renderer, so "the same frame on another machine" still
  has to be proven (v1.0).

**Not yet tested:** a film without real product assets (type-only), a 9:16
film, a film with audio, a data story. Films 3 to 5 should cover these, or
the evidence only speaks for product films.

## Leaning

Stay QML-only, and keep v0.3 as planned, unless films 3 to 5 show agents
struggling with QML itself: scenes that fail `check` repeatedly, or effects
an agent says it can't build. Two films are not enough to close it.
