# Native layer

Everything here runs at native speed because it sits in the hot path. See the frequency
table in [../docs/design.md](../docs/design.md) for why these specific components — and
only these — must be native.

## `core/`

Event loop, portfolio and position model, fill models, margin (SPAN + exposure), and the
Greeks/IV engine.

Position declarations — stops, targets, trailing rules, scheduled exits — are evaluated
here, roughly 22,500 times per session, rather than in Python.

## `ta/`

Streaming indicator primitives, and the execution backend for composed indicator
expressions.

Note that most custom indicators do **not** land here. Because indicators are pure
functions of the price series, they are computed vectorized ahead of replay; this directory
holds the built-in library and the primitives that user expressions compose against.

## Dependency policy

Dependencies are kept minimal and vendorable. The target machine is Windows/MSVC with
limited free disk, and a heavyweight dependency tree is a real cost rather than a
theoretical one.
