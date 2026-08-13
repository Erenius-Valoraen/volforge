# miniz

Vendored, not fetched: this is a build dependency on a machine with limited disk
and no package manager, and two files that never change are cheaper to carry
than a submodule or a configure-time download.

- Version 3.1.2, from https://github.com/richgel999/miniz/releases
- MIT licensed; see LICENSE
- Unmodified

Used only for reading vendor `.zip` archives. Nothing in volforge's own storage
format depends on it — the day store has its own codec and no external
compression library.
