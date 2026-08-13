# Python layer

The strategy authoring surface. See [../docs/strategy-api.md](../docs/strategy-api.md) for
the full model.

This layer **configures** the event loop rather than running inside it. Strategies declare
intent — schedules, stops, targets, exit rules — and the native layer evaluates those
declarations. Python is called a handful of times per session, which is why it can be
ordinary, readable Python without costing anything.

Responsibilities:

- Strategy base class, parameter declarations, and sweep expansion
- Chain queries and contract selection (arbitrary predicates; runs ~5–50×/day)
- Custom indicator registration, tiering, and causality validation
- Extension registries — fill models, sizers, exit rules, data adapters
- Reporting, analytics, and run manifests
