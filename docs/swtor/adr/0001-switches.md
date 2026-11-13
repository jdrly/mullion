# A fork switch is declared once, by the module that reads it

Status: accepted, 2026-11-13.

## Context

Mullion reads about fifty `MULLION_*` variables, each where it is used, through `env::getEnvVar`
(`src/util/util_env.hpp`) or `getenv`. Nothing lists the ones in effect, so a log can't say which switches a session
ran with. This fork adds more, one per behaviour change ([CONTRIBUTING](../../../.github/CONTRIBUTING.md#definition-of-done)),
and several lanes add them at the same time.

A central settings file that lists every switch would fix the first problem and cause a second. Every pull request
that adds a switch would edit it, and parallel lanes would conflict there. `jdrly/dxmt` had exactly that: nine
swap chain commits that only added a field to the startup line.

## Decision

- A fork switch is declared once, at namespace scope, in the module that reads it, through one small registry. The
  declaration gives the name, the default and the grammar of its value. Declaring registers the switch while the
  DLL loads and reads nothing.
- The session header prints every registered switch with its effective value, sorted by name. A new switch appears
  there by being declared, without editing the header's code.
- A read parses the environment when it happens, as `getEnvVar` does. The switch keeps no cache, and a caller keeps
  whatever cache it had, so a read's timing doesn't change.
- Grammars aren't unified. Each switch keeps the one it has (`!= "0"`, `== "1"`, a number). Changing what a switch
  accepts is a behaviour change of its own.
- Upstream's variables stay as they are, read where they are used. Moving them into the registry would edit
  upstream's code at every read and conflict with each upstream batch. The header lists the `MULLION_*` variables
  that are set on a line of their own, so upstream's are visible too. That line keeps the log rules: a value that is
  a path, or could name the player, never prints as it is. The session header's work sets how such a value prints.
- The registry is header-only, pure C++ without Windows or Metal, and has a host test.
- A fix needs no switch when it only removes undefined behaviour, or makes code do what its own comment and its
  callers already say it does, such as a check that an operator precedence mistake makes always false. Its off value
  would bring the defect back, and nobody would choose that. The pull request says which of the two the fix is, and
  why. Such a fix merges on; on a high path the runner can't exercise it still says "Needs the Mac run". A change that
  makes code do something it never said it does is a behaviour change, and has a switch.

The worked example is `jdrly/dxmt`'s `src/dxmt/dxmt_gw2_switches.hpp` and its ADR 0004. The implementation here is
the session header's work (private ticket 03), which also picks the file.

## Consequences

- Each DLL that links the registry has its own copy of it. The header prints the copy in the DLL that writes it.
- A switch declared outside the registry doesn't show on the header. Review treats that as a missing switch.
- Every switch still needs its row in [docs/swtor/README.md](../README.md).

## Alternatives considered

- **One central list of switches.** Every new switch would edit it, and parallel pull requests would conflict there.
- **Moving upstream's variables into the registry too.** It would show them with their defaults, at the price of a
  conflict in every upstream batch that touches a read. The line of set variables shows the ones that matter for a
  session, which is what a log needs.
- **Parsing each switch once and caching it.** It would change when values are read. That is a measured change of its
  own if it is ever wanted.
