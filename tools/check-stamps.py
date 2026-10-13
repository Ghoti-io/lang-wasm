#!/usr/bin/env python3
"""Fail if a compile rule names no flags stamp, or a stamp omits a variable.

Each build tree keeps a .flags file holding the flag string it was built
with, and the object rules depend on it, so a flag change - including one
that arrives on the command line and touches no file - moves an mtime and
forces a rebuild.  That only works if the stamp records the variables the
recipes actually expand.  It did not, in model: the release stamp recorded
$(CFLAGS) while the library objects compile with $(LIB_CFLAGS), so changing a
flag that lives only in LIB_CFLAGS rebuilt nothing at all.  A rebuild that
does not happen is invisible - it looks exactly like a build already current
- which is why this is checked here rather than left to a comment.

This is the flag-stamp half of model's tools/check-lists.py, unchanged; the
other half of that script checks lists that only model has.

Usage: check-stamps.py [Makefile]
"""

import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
makefile_path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "Makefile"


def fail(message):
    print("check-stamps: %s" % message, file=sys.stderr)
    sys.exit(1)


problems = []

# Join backslash continuations first. A wrapped prerequisite list is
# indented with tabs, so without this the continuation lines read as
# recipe lines and the rule appears to expand whatever appears in them -
# order-only prerequisites like $(BUILD_DIR), which are paths and not
# flags.
makefile = re.sub(r"\\\n", " ", makefile_path.read_text())

stamp_recipes = {}
for name, body in re.findall(
        r"^\$\((\w*FLAGS_STAMP)\): force-flags\n((?:\t.*\n)+)",
        makefile, re.M):
    # Find the printf line, then harvest every $(VAR) on it - rather than
    # matching the format and the flag string as one adjacent pattern.
    # Adjacency is an assumption about spelling, and it was wrong twice in
    # model; scanning the whole line has no adjacency to get wrong.
    printf_lines = [l for l in body.splitlines() if "printf" in l]
    if not printf_lines:
        fail("the %s recipe does not printf a flag string; this gate is "
             "measuring nothing" % name)
    stamp_recipes[name] = set(
        re.findall(r"\$\((\w+)\)", " ".join(printf_lines)))
if not stamp_recipes:
    fail("found no flag stamps at all; the pattern must have rotted")
recorded_anywhere = set().union(*stamp_recipes.values())

# Find the compile rules by what their recipes DO, not by how their targets
# are spelled: a compiler variable plus any sign of a source, `-c` or not.
# A pure link names its inputs with $^ or an object list and stays out.
# A rule's recipe does not have to start on the line after its target -
# the house style for a gate is `name: ## help`, a comment block, then the
# recipe - so comment lines between are allowed. .PHONY targets are then
# excluded on purpose: a phony target runs every time and has no object
# that can go stale.
RULE = (r"^([^\s#][^\n:=]*):([^\n]*)\n(?:(?:#[^\n]*|[ \t]*)\n)*"
        r"((?:\t.*\n)+)")
phony = set()
for names in re.findall(r"^\.PHONY:([^\n]*)$", makefile, re.M):
    phony.update(names.split())

compile_rules = [
    (target, prereqs, body)
    for target, prereqs, body in re.findall(RULE, makefile, re.M)
    if target.strip() not in phony
    if re.search(r"^\t@?\$+\(\w*C(?:C|XX)\)[^\n]*(?:\s-c\s|\$<|\.c\b|\.cpp\b)",
                 body, re.M)]
if not compile_rules:
    fail("found no compile rules at all; the pattern must have rotted")

guarded = []
for target, prereqs, body in compile_rules:
    stamp = re.search(r"\$\((\w*FLAGS_STAMP)\)", prereqs)
    if stamp:
        guarded.append((stamp.group(1), prereqs, body))
    else:
        problems.append(
            "the rule for %s compiles but names no flag stamp, so it keeps "
            "whatever flags it was first built with and never rebuilds when "
            "they change. Partial coverage is worse than none: the rules that "
            "do rebuild make it look as though the flag change rebuilt "
            "everything" % target.strip())

# $(@D), $@ and $< are make's own automatic variables, not flags.
AUTOMATIC = {"@D", "@", "<", "CURDIR", "MAKE"}
for stamp, prereqs, body in guarded:
    # Only the compiler's own command line.  A recipe's mkdir and its shell
    # guards mention variables that are paths and probes, not flags, and a
    # compile-and-link line names its link inputs - which are already file
    # prerequisites, so make's mtimes cover them and the stamp need not.
    used = set()
    for line in body.splitlines():
        if re.match(r"\t@?\$+\(\w*C(?:C|XX)\)", line):
            used |= set(re.findall(r"\$\((\w+)\)", line))
    used -= AUTOMATIC
    used -= set(re.findall(r"\$\((\w+)\)", prereqs))
    missing = sorted(used - stamp_recipes.get(stamp, set()))
    if missing:
        problems.append(
            "a rule guarded by %s expands %s, which %s does not record, so "
            "changing %s rebuilds nothing"
            % (stamp, ", ".join("$(%s)" % m for m in missing), stamp,
               "them" if len(missing) > 1 else "it"))

# The link lines, which the population above deliberately excludes.
#
# A link rule cannot always take a stamp as a prerequisite: where its recipe
# uses $^ the stamp is handed to the linker as an input and the build fails
# with "file format not recognized".  So link rules are covered transitively
# instead - a stamp change rebuilds the objects, and rebuilt objects relink
# whatever uses them.  That only works if the link line's flags are recorded
# in some stamp at all.  This check is deliberately weaker than the one
# above: it asks whether a variable is in ANY stamp, not the right one.
link_problems = []
link_lines = 0
for target, prereqs, body in re.findall(RULE, makefile, re.M):
    if target.strip() in phony:
        continue
    rule_missing = set()
    for line in body.splitlines():
        if not re.match(r"\t@?\$+\(\w*C(?:C|XX)\)", line):
            continue
        if re.search(r"\s-c\s|\$<|\.c\b|\.cpp\b", line):
            continue                      # a compile; checked above
        link_lines += 1
        used = set(re.findall(r"\$\((\w+)\)", line)) - AUTOMATIC - {"^"}
        used -= set(re.findall(r"\$\((\w+)\)", prereqs))
        rule_missing |= used - recorded_anywhere
    # Report ownership by rule: a rule with two link commands sharing one
    # unrecorded variable is one defect with one fix.
    if rule_missing:
        missing = sorted(rule_missing)
        link_problems.append(
            "the link line for %s expands %s, which no flag stamp "
            "records, so changing %s relinks nothing"
            % (target.strip(), ", ".join("$(%s)" % m for m in missing),
               "them" if len(missing) > 1 else "it"))
problems.extend(link_problems)
if not link_lines:
    fail("found no link lines at all; this check would pass vacuously")

if problems:
    for problem in problems:
        print("check-stamps: %s" % problem, file=sys.stderr)
    sys.exit(1)

print("check-stamps: %d flag stamps guarding %d compile rules, each recording "
      "every variable its recipes expand; %d link lines, every flag on them "
      "recorded somewhere"
      % (len(stamp_recipes), len(guarded), link_lines))
