# Darkroom improvement backlog

**Open:** None.

**Decided against or deferred:**

- **Async full-frame ffmpeg:** synchronous extraction was judged acceptable for its explicit workflows.
- **Deduplicate zoom persistence/debounce:** three short consumers remain clearer than an abstraction. Revisit if a
  fourth consumer appears or the behavior grows.
