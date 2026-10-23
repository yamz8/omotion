# Tests of the agent skill

`evals/` holds the cases a fresh agent runs with the skill (skill-creator's
layout, also what `claude plugin eval` reads): rough-cut-review, gate1-stop,
notes-round, and full-film. All four passed with the current skill on
2026-10-23 (7/7, 4/4, 6/6, 6/6).

`trigger-eval.json` holds 20 queries for tuning the skill's description, 10
that should load it and 10 near misses. They were written by the agent and
not yet reviewed by a person. Skill-creator's loop ran them on 2026-10-23
(five rewrites, three runs a query, 8 held out): every description, the
original included, scored precision 100% and recall 0-8%, because `claude
-p` in that harness almost never invokes the skill whatever its description
says. The loop couldn't tell descriptions apart, so the original stays.
Rerun it after reviewing the queries, in a setup where the skill does
trigger (from inside a film's folder).
