#!/usr/bin/env python3
"""Check subjects of commits introduced between two Git revisions."""

import re
import subprocess
import sys


SUBJECT = re.compile(
    r"^\[(?:Feature|Fix|Docs|Refactor|Hardening|Test|Release|Control|"
    r"Benchmark|Build|CI|Chore)\] \S.*$"
)
ZERO_SHA = re.compile(r"^0+$")
# Last commit before the bracketed-subject policy was applied to main.
POLICY_BASE = "b8a677771053907a96569e7f7230181010b30fb9"


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True,
                          text=True).stdout.rstrip("\r\n")


def is_ancestor(base, head):
    return subprocess.run(["git", "merge-base", "--is-ancestor", base, head],
                          capture_output=True).returncode == 0


def main():
    if len(sys.argv) != 3:
        print("usage: check_commit_subjects.py BASE_SHA HEAD_SHA", file=sys.stderr)
        return 2
    base, head = sys.argv[1:]
    if ZERO_SHA.fullmatch(base):
        commits = git("rev-list", "--reverse", "--no-merges", head).splitlines()
    else:
        if not is_ancestor(base, head):
            # A force push can make the event's previous tip unavailable to
            # checkout; still check every commit subject in the rewritten era.
            if not is_ancestor(POLICY_BASE, head):
                print("Cannot find the commit-subject policy base", file=sys.stderr)
                return 2
            base = POLICY_BASE
        commits = git("rev-list", "--reverse", "--no-merges",
                      f"{base}..{head}").splitlines()
    bad = []
    for commit in commits:
        subject = git("show", "-s", "--format=%s", commit)
        if not SUBJECT.fullmatch(subject):
            bad.append((commit[:12], subject))
    for commit, subject in bad:
        print(f"{commit}: invalid subject: {subject}", file=sys.stderr)
    if bad:
        print("Expected: [Type] short description (see CONTRIBUTING.md)",
              file=sys.stderr)
        return 1
    print(f"Checked {len(commits)} new commit subject(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
