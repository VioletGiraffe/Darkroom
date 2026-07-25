## Filesystem and catalog consistency

Import, rename, relocation, and catalog updates approximate transactions across filesystem operations and JSON metadata updates. Runtime failures have compensating cleanup, but an abrupt termination between the two can leave catalog state out of sync with the filesystem. The integrity checker mitigates this, but this remains the most consequential risk noted.

## Qt private implementation dependency

The combo-box popup styling in `Style.cpp` identifies and paints Qt's private `QComboBoxPrivateContainer` through a global event filter. This is pragmatic but brittle across Qt upgrades and may fail silently if Qt changes that implementation.

## Slider tick marks suppressed by QSS

Qt's stylesheet style does not render horizontal slider tick marks. The global slider styling therefore removes the per-photo detent ticks from the Photo Compare picker.

## Large UI coordinators

`MainWindow`, `ImportDialog`, and `PhotoCompareWindow` combine many related state transitions and responsibilities.
Main-window browsing and item-management UI is isolated in `MediaBrowserWidget` and small workflow modules, but the
remaining application-level import and library-lifecycle coordination is still broad. The code is
coherent, but this breadth increases regression risk and makes isolated testing difficult. This is a maintainability
concern, not by itself a reason to refactor.

## Photo Compare shutdown latency

Photo Compare cancellation prevents queued reads and decodes from starting, but shutdown still waits for work already in flight. A file operation blocked on slow, removable, or network storage could therefore delay window closure.

## Fixed FFmpeg timeouts

FFmpeg operations use fixed one- and five-minute timeouts. These are reasonable defaults, but unusually slow storage or large media may be reported as failures while still making progress.
