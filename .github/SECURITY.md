# Security policy of jdrly/mullion

This repository is the SWTOR on Mac fork of Mullion: the build of Mullion that runs the DirectX 12 client of Star
Wars: The Old Republic on Apple Silicon. This file is the fork's own, and it takes the place of upstream's root
`SECURITY.md` for this repository: what you report here is about this fork, and reaches me, its maintainer, not
upstream.

## Reporting a vulnerability

Report it privately, through GitHub's private vulnerability reporting: the repository's **Security** tab, then
**Report a vulnerability**. Please don't open a public issue, pull request or discussion for it.

Say which commit or build you looked at (a build's `VERSION` file names its commit), what an attacker can do, and how
to reproduce it. The answer comes in the report.

## What is in scope

What this fork builds and changes, as it is built here:

- the fork's switches and log lines (`docs/swtor/README.md`), and everything else the fork adds;
- the Wine-to-Metal bridge (`src/winemetal`), on both its Windows and its Unix side;
- the shader converter (`src/airconv`) and the shader cache, as this repository builds them;
- the builds this repository's `SWTOR` workflow publishes.

Out of scope: SWTOR itself, the SWTOR on Mac app, Wine or CrossOver, macOS, and Apple's Metal. A defect that sits in
upstream Mullion's code is in scope here as far as this fork ships it.

## Upstream

Nothing reported here is forwarded to upstream Mullion (`dttdrv/mullion`), DXMT or any other project without my
decision. The fork fixes such defects itself, in its own pull requests ([ADR 0005](../docs/swtor/adr/0005-the-fork-fixes-upstreams-defects-itself.md)).
Upstream's root `SECURITY.md` and `CODE_OF_CONDUCT.md` stay as upstream has them; for upstream Mullion itself, its own
policy applies.
