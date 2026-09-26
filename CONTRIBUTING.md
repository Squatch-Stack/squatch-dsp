<!-- SPDX-License-Identifier: MIT -->
# Contributing to squatch-dsp

Thank you for helping. Every change comes in as a pull request and is reviewed before it lands.
This page says what belongs here, how to shape a pull request, and what the checks look at.

## What belongs here

squatch-dsp is a **library**: a real-time sound engine in C++17. It holds:

- the block core (`Block`, `Arena`, parameters, control contracts) and the blocks built on it;
- their tests, references and golden renders, and the benchmark;
- the Squatch Tuner's source (`blocks/tuner/`, also published on its own as squatch-tuner);
- thin reference hosts that show the library in use (the web demo, plugin adapters).

It does not hold applications, products, device firmware, bench scripts or anything that needs
an account, a key or a download to build. If you are unsure whether something fits, open an
issue first.

## A good pull request

- **One change.** A fix, a feature, a refactor or a documentation change, not several. Refactors
  go in their own pull request, ahead of the change that needs them.
- **Small.** Aim for under 400 changed lines. Over 1000 needs a reason in the description (a
  maintainer then adds the `large-change` label); generated files are not counted.
- **Tested.** A behaviour change comes with a test that fails without it. A new block follows
  "Adding a block" in the [README](README.md).
- **Checked locally.** `make ci` runs what CI runs: `lint headers lib test`.
- **Commit subjects** follow [Conventional Commits](https://www.conventionalcommits.org/):
  `type(scope): summary`, at most 72 characters, with type one of `feat fix docs chore ci build
  test refactor perf style revert`. Say why in the body when it is not obvious. No `fixup!`,
  `squash!` or `WIP` commits in the final branch.

## What the checks look at

On every pull request (`.github/workflows/quality.yml` and `ci.yml`):

| Check | Limit |
|---|---|
| Cyclomatic complexity per function (`make lint`, lizard) | at most 10 |
| Function length | at most 60 lines |
| Parameters per function | at most 5 |
| Shell scripts (shellcheck) | no findings |
| Commit subjects (`tools/pr-shape`) | the format above |
| Pull request size (`tools/pr-shape`) | notice over 400 lines, fails over 1000 without `large-change` |
| Standalone headers, library, tests | pass under ASan + UBSan and TSan, with the real-time allocation and lock watch |

Nothing here is lowered to make a pull request pass: fix the code instead.

## Your machine

This repository installs nothing on your machine and changes no configuration: there are no hooks
to install, no package managers and no downloads at build time. The checks above run in CI; run
`make ci` yourself when you want them locally (it needs a C++17 compiler, make, and `lizard` for
the lint step).

## How a pull request lands

squatch-dsp is published from a development tree that also holds work that is not public yet.
Once a pull request is approved, a maintainer applies it there and it comes back here in the
next publish, credited to you. The `EXPORTED` file lists every published file; a pull request
may edit them, and CI reports which, but it only enforces `EXPORTED` on `main`.

Contributions are accepted under the MIT licence of this repository.
