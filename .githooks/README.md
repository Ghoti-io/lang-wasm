# Git hooks

Tracked hooks for this repository, enabled via `core.hooksPath`.

## Setup after cloning

Git deliberately does not install hooks on clone, so run this once per clone:

```bash
./.githooks/install.sh
```

That points `core.hooksPath` at this directory. Because the hooks are tracked,
later changes to them take effect on `git pull` with no reinstall.

To disable: `git config --unset core.hooksPath`

## Hooks

### `commit-msg`

Strips AI-assistant attribution that coding tools append to commit messages —
`Co-Authored-By: Claude <noreply@anthropic.com>`, `🤖 Generated with [Claude
Code]`, Cursor/Copilot/Codex equivalents, and session-id trailers — then tidies
up the blank lines left behind.

Two deliberate safety properties:

- **Human co-authors are preserved.** Only `Co-authored-by:` lines naming a
  known assistant or vendor are removed, so real collaborators and your own
  `Signed-off-by:` survive.
- **Legitimate prose is preserved.** A commit like `feat: add Claude API
  client` and a body that discusses Cursor or session ids pass through
  untouched; only recognised trailer and banner *forms* are matched.

If a message consists of nothing but attribution, the hook leaves it alone
rather than silently emptying it, so git's own empty-message check fires.

To extend the list of tools, edit the `AGENTS` pattern near the top of
`commit-msg`.

### Testing a change to the hook

```bash
printf 'subject\n\n🤖 Generated with [Claude Code](https://claude.ai/code)\n' > /tmp/msg
./.githooks/commit-msg /tmp/msg && cat /tmp/msg
```
