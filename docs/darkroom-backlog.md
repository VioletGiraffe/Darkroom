# Darkroom improvement backlog

**Open:**

- **Named label chips on cards:** cards show display-only label dots; removal goes through the context-menu Labels
  checklist. Named chips would be removed by dragging one off the card: a deliberate gesture, so a misclick cannot
  remove a label.
- **Owned/referenced model for videos:** a photo is either owned (stored under `<root>/Photos/<label>`) or referenced at
  its external path; a video's source stays wherever import put it, and only its frame folder is library-owned.
  Extending the split to videos needs decisions on:
  - where owned sources live; label rename, delete and relocation would then move source files, not only frame folders
  - migrating existing libraries with scattered sources
  - whether Copy/Move to a chosen folder (`anySourceDir`) survives
  - Delete and Rename of referenced items: both act on the external file today, for photos too
  - failure UX for a missing referenced source: frame extraction, re-export and ghost re-import need it
  - integrity-scan drift kinds: referenced source missing, owned source outside the library
  - name collisions for sources moving into the library; photo import already auto-renames
- **Portable libraries:** label-to-folder matching is case-insensitive (`ordinaryLabelIdByName`, the `renameLabel` path
  rewrite). On a case-sensitive file system it misses a folder whose case differs from its label's display name, as
  after moving a library from Windows.

**Decided against or deferred:**

- **Async full-frame ffmpeg:** synchronous extraction was judged acceptable for its explicit workflows.
- **Deduplicate zoom persistence/debounce:** three short consumers remain clearer than an abstraction. Revisit if a
  fourth consumer appears or the behavior grows.
