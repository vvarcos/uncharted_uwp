# AI disclosure

**The work in this repository was produced by artificial intelligence.**

Every porting change, script, build step, audit, documentation page and commit in this repository
was written by AI agents running in an automated coding harness. No part of the UWP port was
hand-written by a person.

## What the human did

- Set the requirements, priorities and the console constraints (5120 MiB ceiling, three 512 MiB
  arenas, Dozen/Vulkan 1.2).
- Supplied the legally obtained game dump, the artwork and the hardware.
- Built, deployed and tested on the console, and reported the logs that count as evidence.
- Reviewed and approved (or rejected) each change.

## What the AI did

- Reading the upstream sources and writing the host, bridge, scripts, packaging and documentation.
- Auditing the tree, cleaning what must not be published, scanning for secrets and oversized files.
- Preparing and maintaining the Git history of this repository.

## What this means for you

- **Review before you trust.** AI-written code can be plausible and still wrong; the claims in the
  documentation are only as good as the console logs behind them.
- **No warranty.** See [LICENSE](LICENSE). This is an experimental, incomplete port.
- **Upstream credit is not AI work.** *uncharted_pc*, *bloodborne_pc* (bbport), *gow3_xbox*,
  *SpartanPort*, *shadPS4*, KytyPS5-for-Xbox, Mesa/Dozen and every vendored library listed in
  [CREDITS](CREDITS.md) are human work that this repository builds on.
- **No game data or credentials** were used to train or produce this export; the repository ships
  none.

If you fork this project, keep this notice: anyone reading your fork deserves to know how it was
made.
