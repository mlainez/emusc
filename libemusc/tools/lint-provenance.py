#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# This file was produced with the assistance of an AI coding agent as part of
# this fork's own tooling; it has no upstream libEmuSC lineage.
#
# Check the `@provenance` tags in the C/C++ sources, and list the comments
# that still state their evidence class in free text instead of a tag.
#
#   libemusc/tools/lint-provenance.py [--strict] [--summary] [PATH...]
#
# PATH defaults to libemusc/ and emuscd/ under the repository root. The tag
# format is specified in AGENTS.md, "Provenance annotations":
#
#   // @provenance class=MEASURED devices=JV-1080 ref=M-018
#
# What is checked, per comment block (a /* */ comment, or a run of // lines
# with nothing but whitespace between them):
#
#   error   a tag with a missing, unknown or repeated key, a `class` outside
#           the closed set, or a `devices` list that is empty, repeats a name
#           or holds something that is not spelled like a Roland model
#           (SC-55, SC-55mkII, JV-1080, ...);
#   warning a tag whose `ref` is a placeholder (`P-xxxx`);
#   legacy  a block that states an evidence class in free text (FW-EXACT,
#           MEASURED, FITTED, `FIT`, CREDITED, UNVERIFIED, [DOCUMENT], ...)
#           and carries no tag at all, so it still needs migrating.
#
# Legacy detection is a heuristic over upper-case words: it has no false
# negatives for the vocabulary listed in LEGACY_MARKERS, and it will also
# flag an all-caps heading such as "WHY THIS AND NOT A FITTED CURVE", which
# is left for whoever migrates the block to judge. A block that has any tag
# is considered migrated as a whole.
#
# `ref` is checked for form only, never resolved. The ledgers behind the IDs
# live outside this repository, in the sibling git repos ../scdb and
# ../emusc-match relative to its root. Those are not submodules, so a
# checkout may not have them:
#   scdb/devices/<device>/11_validation/measurements.md         M-NNN, per device
#   scdb/devices/<device>/12_implementation/implementation_divergences.md  D-NN
#   emusc-match/PROVENANCE.md, TAINT-REGISTER.md, backlog/tasks/   P-, T-, TASK-
# The link between those ledgers and this repo's comments is partial and still
# being cross-checked. An ID with no ledger entry, or with an entry on another
# subject, is not yet confirmed. It is not proven wrong. See AGENTS.md,
# "Provenance annotations".
#
# Exit status is 1 on any error, and with --strict on any legacy block too.

import argparse
import os
import re
import sys
from collections import Counter

CLASSES = ("FW-EXACT", "DOCUMENTED", "MEASURED", "FITTED", "CREDITED",
           "UNVERIFIED")
KEYS_REQUIRED = ("class", "devices")
KEYS_OPTIONAL = ("ref",)
DEVICE_NAME = re.compile(r"^[A-Z]{2,4}-\d+[A-Za-z]*$")
TAG = re.compile(r"@provenance\b(.*)$")
PLACEHOLDER_REF = re.compile(r"x{3,}", re.IGNORECASE)
LEGACY_MARKERS = re.compile(
    r"\bFW-EXACT\b|\bFIRMWARE-EXACT\b|\bMEASURED\b|\bFITTED\b|`FIT`"
    r"|\bCREDITED\b|\bUNVERIFIED\b|\[DOCUMENT\]")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hh", ".hpp")


def comment_blocks(text):
    """Yield (first_line, [(line_no, comment_text), ...]) per comment block.

    A small C/C++ lexer: enough to keep string and character literals from
    being read as comments. Raw string literals are not special-cased; none
    of the scanned sources uses one that contains a comment opener."""
    i, n, line = 0, len(text), 1
    run = None            # the // block being accumulated
    run_end_line = 0
    code_since_run = False

    def flush():
        nonlocal run
        if run:
            yield_list.append(run)
        run = None

    yield_list = []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            body = text[i + 2:j]
            if run is not None and not code_since_run and line == run_end_line + 1:
                run.append((line, body))
            else:
                flush()
                run = [(line, body)]
            run_end_line = line
            code_since_run = False
            i = j
            continue
        if text.startswith("/*", i):
            flush()
            j = text.find("*/", i + 2)
            j = n if j < 0 else j
            body = text[i + 2:j]
            lines = []
            for k, part in enumerate(body.split("\n")):
                lines.append((line + k, part))
            yield_list.append(lines)
            line += body.count("\n")
            i = j + 2
            continue
        # Code: any non-comment token ends a // run.
        if run is not None:
            flush()
        code_since_run = True
        if c in "\"'":
            q = c
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    i += 1
                elif text[i] == "\n":
                    line += 1
                i += 1
            i += 1
            continue
        i += 1
    flush()
    return yield_list


def check_tag(rest):
    """Return (errors, warnings) for the text after `@provenance`."""
    errors, warnings = [], []
    fields = {}
    for token in rest.split():
        if token in ("*/", "*"):
            continue
        if "=" not in token:
            errors.append(f"malformed field '{token}' (expected key=value)")
            continue
        key, value = token.split("=", 1)
        if key not in KEYS_REQUIRED + KEYS_OPTIONAL:
            errors.append(f"unknown key '{key}'")
            continue
        if key in fields:
            errors.append(f"repeated key '{key}'")
            continue
        fields[key] = value
    for key in KEYS_REQUIRED:
        if key not in fields:
            errors.append(f"missing '{key}='")
    cls = fields.get("class")
    if cls is not None and cls not in CLASSES:
        errors.append(f"class '{cls}' is not one of {', '.join(CLASSES)}")
    devices = fields.get("devices")
    if devices is not None:
        names = [d for d in devices.split(",")]
        if not devices or any(not d for d in names):
            errors.append("'devices' is empty or has an empty entry")
        else:
            for d in names:
                if not DEVICE_NAME.match(d):
                    errors.append(f"device '{d}' is not spelled like a model "
                                  "name (e.g. SC-55mkII, JV-1080)")
            dup = [d for d, k in Counter(names).items() if k > 1]
            if dup:
                errors.append(f"device(s) listed twice: {', '.join(dup)}")
    ref = fields.get("ref")
    if ref is not None:
        if not ref:
            errors.append("'ref=' is present but empty")
        elif PLACEHOLDER_REF.search(ref):
            warnings.append(f"ref '{ref}' is a placeholder")
    return errors, warnings


def lint_file(path, report):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        report["errors"].append(f"{path}: error: {e}")
        return
    for block in comment_blocks(text):
        tagged = False
        for line_no, body in block:
            m = TAG.search(body)
            if not m:
                continue
            tagged = True
            report["tags"] += 1
            errors, warnings = check_tag(m.group(1))
            for e in errors:
                report["errors"].append(f"{path}:{line_no}: error: {e}")
            for w in warnings:
                report["warnings"].append(f"{path}:{line_no}: warning: {w}")
        if tagged:
            continue
        for line_no, body in block:
            m = LEGACY_MARKERS.search(body)
            if m:
                report["legacy"].append(
                    (path, line_no, f"{path}:{line_no}: legacy: "
                     f"'{m.group(0)}' in a comment with no @provenance tag"))
                break


def walk(paths):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for root, dirs, files in os.walk(p):
            dirs[:] = sorted(d for d in dirs
                             if not d.startswith(".") and d != "build")
            for name in sorted(files):
                if name.endswith(SOURCE_SUFFIXES):
                    yield os.path.join(root, name)


def main():
    ap = argparse.ArgumentParser(
        description="Check @provenance tags and list untagged free-text "
                    "evidence-class comments (format: AGENTS.md, "
                    "'Provenance annotations').",
        epilog="ref= values are checked for form only. Their ledgers live "
               "outside this repo, in the sibling repos ../scdb "
               "(devices/<device>/11_validation/measurements.md for M-, "
               "12_implementation/implementation_divergences.md for D-) and "
               "../emusc-match (PROVENANCE.md P-, TAINT-REGISTER.md T-, "
               "backlog/tasks/ TASK-), which a checkout may not have. That "
               "correlation is partial and still being cross-checked: a ref "
               "with no matching entry there is unconfirmed, not wrong.")
    ap.add_argument("--strict", action="store_true",
                    help="also fail on comments that still need migrating")
    ap.add_argument("--summary", action="store_true",
                    help="print only the totals and the per-file legacy count")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    paths = args.paths or [os.path.relpath(os.path.join(repo, d))
                           for d in ("libemusc", "emuscd")]

    report = {"tags": 0, "errors": [], "warnings": [], "legacy": []}
    for path in walk(paths):
        lint_file(path, report)

    if not args.summary:
        for line in report["errors"] + report["warnings"]:
            print(line)
        for _, _, line in report["legacy"]:
            print(line)
    else:
        for line in report["errors"]:
            print(line)
        per_file = Counter(path for path, _, _ in report["legacy"])
        for path, count in per_file.most_common():
            print(f"{count:5d}  {path}")

    print(f"lint-provenance: {report['tags']} tag(s), "
          f"{len(report['errors'])} error(s), "
          f"{len(report['warnings'])} warning(s), "
          f"{len(report['legacy'])} comment(s) to migrate",
          file=sys.stderr)
    failed = report["errors"] or (args.strict and report["legacy"])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
