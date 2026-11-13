# The shader cache is keyed on a codegen id the build computes from the converter's sources, and on the macOS build

Status: accepted, 2026-11-14.

## Context

Upstream versions its converted-shader cache with `AIRCONV_VERSION`, a number raised by hand. On upstream's `main`,
53 of 106 changes that touched the converter merged without raising it. A player with a warm cache then keeps the
old shader: it loads and draws wrong, or keeps a bug the release fixed. This fork takes upstream's changes in batches,
so it can't rely on that discipline. The key also lacked the macOS build: after a macOS update the same blobs were
served. Old tables were never dropped.

d9mt solved the same problem (its ADR-0006) with a hash of the codegen's object code, read back from the built
library. This fork is built on GitHub's runner; an id that only one toolchain on one Mac reproduces would make the
runner's id meaningless.

## Decision

- **The id is a hash of the converter's sources and toolchain versions** (`tools/codegen_id.py`): every file under
  `src/airconv` and `libs/DXBCParser`, every file of the tree they include with quotes, transitively, LLVM's version,
  the Metal compiler's version and `AIRCONV_VERSION`. The build computes it every time and compiles it in.
- **Includes are followed, not listed.** A hand-kept list of headers is what goes stale; following quoted includes
  catches a header the converter starts to include.
- **The macOS build joins the identity.** The key starts with both; the table, the pipeline archives and a game's
  pipeline library are named or versioned by a tag of both.
- **Other identities are dropped when the cache opens**, under the writers' lock, with one log line.
- **No switch.** A key has no safe old value to fall back to.
- **The build checks itself.** CI computes the id again from the checkout and requires it in the DLLs and `VERSION`.

## Consequences

- No converter change can serve a stale shader, whether or not upstream raised `AIRCONV_VERSION`.
- Any byte of an input changes the id: a comment, and every new bridge call (`winemetal.h` is an input). Each such
  build, and each macOS update, makes every player convert every shader once; the release note says so
  ([caches.md](../caches.md)).
- A new Xcode whose Metal compiler reports a new version changes the id, even when the helpers compile the same.
- Unlike d9mt, there is no lineage: a build can't carry another id's cache over. One can be added if a recompile
  ever costs too much.
