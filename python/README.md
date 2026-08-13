# Python layer

The strategy authoring surface. See [../docs/strategy-api.md](../docs/strategy-api.md) for
the full model.

This layer **configures** the event loop rather than running inside it. Strategies are
coroutines that suspend on `await <condition>`; the condition compiles to a native predicate
and Python resumes only when it fires. A strategy wakes a handful of times per session,
which is why it can be ordinary, readable Python without costing anything.

Responsibilities:

- The `@strategy` decorator, the coroutine driver, and condition compilation
- Position and leg model, with risk rules resolving at the level they are applied
- Chain queries and contract selection (arbitrary predicates; runs ~5–50×/day)
- Custom indicator registration, tiering, and causality validation
- Extension registries — fill models, sizers, exit rules, data adapters
- Sweep expansion and reporting, kept strictly outside strategy definitions
- Run manifests
