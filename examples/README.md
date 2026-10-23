# Example films

Each folder is a whole studio, made with omotion by the agent that built it:
the brief, style guide, shot list, assets, gates, reviews, and renders. None
is approved by a person; their gates 1, 2, and 6 wait. Open one with
`omotion open examples/<name>/scene.qml`.

| Film | Made from | What it shows |
| --- | --- | --- |
| [launch](launch/) | the five beats by hand | omotion itself: hook, problem, reveal, proof, close |
| [walkthrough](walkthrough/) | `--template walkthrough` | directing a film from the studio window, in three steps on the real window |
| [data](data/) | `--template data` | how many frames a patch renders again, from a real measurement |

Every film is 15 seconds in 16:9, 9:16, 1:1, and 4:5, passes `omotion check`
in all four, and is built only from its style guide's tokens, motion rules,
and states. The renders are in each film's `renders/`, with a poster per
format.
