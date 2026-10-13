# Contributing

**Patches are not being accepted at this time.**

Bug reports, reproducers and questions are welcome and genuinely useful.
Please open an issue.

## Why

This is a single-author project and the licensing is not finished being
settled. The libraries are LGPL-3.0-only, and a commercial license may be
offered alongside that. Taking a patch before that is decided would mean
either asking you to sign a contributor agreement drafted for terms that do
not exist yet, or quietly foreclosing the option. Neither is a reasonable
thing to ask of somebody who just wanted to fix a bug.

"At this time" is meant literally rather than as a polite no. If
contributions open, this file will say so, and it will say what the terms
are before you write any code.

## What is useful right now

- **Bug reports, with a reproducer if you can.** Every library has a test
  suite and most have fuzzing targets, so a failing input is the single most
  valuable thing you can send.
- **Portability reports.** Linux is the only platform any of this has been
  verified on. Build failures and behavioural differences elsewhere are worth
  knowing about, and there is a standing list of what has never been run on
  Windows.
- **Specification disagreements.** If the code and a standard disagree, say
  which standard and which clause. Several defects have been found exactly
  that way, and it is the kind of report that is hard to get and easy to act
  on.
