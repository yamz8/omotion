# Style guide

Status: draft (gate 2)

REFERENCE ANALYSIS
Write "none provided" without a reference, and fill the headings below with
the style you propose instead.
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

TOKENS
The values scene.qml uses, as Style.colors.<name>, Style.fonts.<name>, and
Motion.rule("<name>", t, start) for a spring of stiffness and damping;
omotion check holds the film to them. Replace these with the approved style.
color background #0d0d10
color ink #f4f1ea
color accent #ffd60a
font headline Adwaita Sans
rule micro 260 24
rule panel 150 20
rule camera 60 16
rule headline 210 22

DO NOT COPY
Subjects, logos, copy, or product-specific imagery
