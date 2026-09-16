# SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
# Copyright (c) 2023-2026 Dismo Industries LLC

"""CHANGELOG.md helpers for the release workflow.

Three subcommands, each deliberately small enough to reason about in a diff:

    extract  Print the [Unreleased] section body -- the human-written
             "what changed for you" summary that heads the release notes.
             Fails loudly when the section is empty.

    section  Print [<version>] if present, else [Unreleased]. Used by the
             release workflow after `stamp` has moved the summary under a
             version heading, and on the tag-push path where no stamp ran.
             Never fails: a missing summary is a warning, not a blocked release.

    stamp    Rewrite the [Unreleased] heading as [<version>] - <date>, open a
             fresh empty [Unreleased] above it, and repoint the compare links.
             Run once per release, alongside the version.txt bump.

Kept out of the workflow YAML so it can be tested and read like normal code.
"""

import argparse
import datetime
import re
import sys
from pathlib import Path

# [ \t] rather than \s throughout: \s also matches newlines, and under
# re.MULTILINE a trailing \s*$ silently eats the blank line that follows the
# heading, which glues the next section onto it when the file is rewritten.
UNRELEASED_HEADING = re.compile(r"^##[ \t]*\[Unreleased\][ \t]*$", re.IGNORECASE | re.MULTILINE)
ANY_HEADING = re.compile(r"^##[ \t]+", re.MULTILINE)
LINK_LINE = re.compile(r"^\[Unreleased\]:[ \t]*(\S+)[ \t]*$", re.IGNORECASE | re.MULTILINE)

# The empty skeleton opened after a release. Categories are listed even when
# blank so the next contributor has an obvious place to put their line.
EMPTY_SECTION = """## [Unreleased]

### Added

### Changed

### Fixed

### Removed
"""


def read(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def unreleased_body(text: str) -> str:
    """Return the text between the [Unreleased] heading and the next ## heading."""
    match = UNRELEASED_HEADING.search(text)
    if not match:
        raise SystemExit("release_notes: no '## [Unreleased]' heading in the changelog")
    start = match.end()
    following = ANY_HEADING.search(text, start)
    body = text[start : following.start() if following else len(text)]
    # Drop the trailing link-reference block if it fell inside the slice.
    body = LINK_LINE.sub("", body)
    return body.strip()


def is_empty(body: str) -> bool:
    """True when the section carries headings but no actual entries."""
    for line in body.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return False
    return True


def cmd_extract(args: argparse.Namespace) -> None:
    body = unreleased_body(read(args.changelog))
    if is_empty(body):
        if args.allow_empty:
            return
        raise SystemExit(
            "release_notes: the [Unreleased] section is empty.\n"
            "Add a summary to CHANGELOG.md before releasing, or pass --allow-empty."
        )
    sys.stdout.write(body + "\n")


def cmd_section(args: argparse.Namespace) -> None:
    """Print the body of [<version>], falling back to [Unreleased].

    Used by the release workflow after `stamp` has moved the summary under a
    version heading, and on the tag-push path where no stamp ran at all.
    Never fails the build: a release with no summary still publishes, with the
    generated commit list as its notes.
    """
    try:
        text = read(args.changelog)
    except OSError:
        return

    # Order matters. After `stamp` runs, the file holds a fresh EMPTY
    # [Unreleased] *above* the newly stamped [<version>]. Searching for either
    # heading at once would match the empty one first and silently drop the
    # summary, so try the version heading on its own before falling back.
    for pattern in (
        r"^##[ \t]*\[%s\][^\n]*$" % re.escape(args.version),
        r"^##[ \t]*\[Unreleased\][^\n]*$",
    ):
        match = re.search(pattern, text, re.IGNORECASE | re.MULTILINE)
        if not match:
            continue
        rest = text[match.end() :]
        following = ANY_HEADING.search(rest)
        body = rest[: following.start()] if following else rest
        body = re.sub(r"^\[[^\]]+\]:[^\n]*$", "", body, flags=re.MULTILINE).strip()
        if body and not is_empty(body):
            sys.stdout.write(body + "\n")
            return


def cmd_stamp(args: argparse.Namespace) -> None:
    path = Path(args.changelog)
    text = read(path)
    body = unreleased_body(text)

    if is_empty(body) and not args.allow_empty:
        raise SystemExit("release_notes: refusing to stamp an empty [Unreleased] section")

    date = args.date or datetime.date.today().isoformat()
    match = UNRELEASED_HEADING.search(text)
    text = (
        text[: match.start()]
        + EMPTY_SECTION
        + f"\n## [{args.version}] - {date}"
        + text[match.end() :]
    )

    # Repoint the compare links: [Unreleased] now starts at the new tag, and the
    # released version gets its own link spanning from the previous release.
    link = LINK_LINE.search(text)
    if link:
        repo_compare = link.group(1).rsplit("/compare/", 1)[0] + "/compare"
        previous = args.previous_tag
        new_links = f"[Unreleased]: {repo_compare}/{args.tag}...HEAD"
        if previous:
            new_links += f"\n[{args.version}]: {repo_compare}/{previous}...{args.tag}"
        text = LINK_LINE.sub(lambda _: new_links, text, count=1)

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"release_notes: stamped [Unreleased] as [{args.version}] - {date}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changelog", default="CHANGELOG.md")
    sub = parser.add_subparsers(dest="command", required=True)

    extract = sub.add_parser("extract", help="print the [Unreleased] section body")
    extract.add_argument("--allow-empty", action="store_true")
    extract.set_defaults(func=cmd_extract)

    section = sub.add_parser("section", help="print [<version>], else [Unreleased]")
    section.add_argument("--version", required=True, help="e.g. 1.3.3")
    section.set_defaults(func=cmd_section)

    stamp = sub.add_parser("stamp", help="close [Unreleased] as a released version")
    stamp.add_argument("--version", required=True, help="e.g. 1.3.3")
    stamp.add_argument("--tag", required=True, help="e.g. v1.3.3-rc1")
    stamp.add_argument("--previous-tag", default="", help="e.g. v1.3.2")
    stamp.add_argument("--date", default="", help="defaults to today (UTC date on CI)")
    stamp.add_argument("--allow-empty", action="store_true")
    stamp.set_defaults(func=cmd_stamp)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
