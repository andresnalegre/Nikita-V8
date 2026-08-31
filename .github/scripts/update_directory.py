#!/usr/bin/env python3
"""Regenerate firmware/directory.json — the update feed nikita-qflipper and
Nikita-iOS read.

Both clients speak the Flipper `directory.json` format, so this fork serves one
rather than inventing a protocol. It lives in the repo (raw.githubusercontent.com
serves it) instead of on an update server: the release workflow regenerates and
commits it, so there is no host to keep alive and no second place a version can
drift from the release it describes.

Channels follow the tag:

    nkt-001        -> release
    nkt-001-rc     -> release-candidate
    nkt-001-dev    -> development

Each channel keeps its newest entries first (both clients read versions[0] as
"latest") and is capped, so the file cannot grow without bound.

Usage:
    update_directory.py --tag nkt-001 --tgz dist/flipper-z-f7-update-nkt-001.tgz \
                        --repo andresnalegre/Nikita-V8 --changelog CHANGELOG.md
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time

# Keep this many entries per channel. Old releases stay downloadable on the
# GitHub releases page; the feed only needs enough history for a client to see
# what it is currently on.
MAX_VERSIONS_PER_CHANNEL = 10

TAG_RE = re.compile(r"^nkt-(\d+)(-rc|-dev)?$")

CHANNELS = {
    "release": (
        "Release",
        "Stable Nikita firmware. Recommended.",
    ),
    "release-candidate": (
        "Release Candidate",
        "Nikita firmware under test before it becomes a release.",
    ),
    "development": (
        "Development",
        "Nikita firmware built from the tip. Expect bugs.",
    ),
}


def channel_for(tag):
    match = TAG_RE.match(tag)
    if not match:
        raise SystemExit(
            f"tag {tag!r} is not a Nikita release tag "
            "(expected nkt-<number>, optionally -rc or -dev)"
        )
    suffix = match.group(2)
    if suffix == "-rc":
        return "release-candidate"
    if suffix == "-dev":
        return "development"
    return "release"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def empty_feed():
    return {
        "channels": [
            {"id": cid, "title": title, "description": desc, "versions": []}
            for cid, (title, desc) in CHANNELS.items()
        ]
    }


def load_feed(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return empty_feed()
    with open(path, encoding="utf-8") as handle:
        try:
            feed = json.load(handle)
        except json.JSONDecodeError as error:
            # Refuse rather than start over from an empty feed: overwriting a
            # damaged file would silently drop every release already published
            # in it, and the clients would stop offering them.
            raise SystemExit(
                f"{path} is not valid JSON ({error}). Fix or delete it; "
                "this script will not overwrite a feed it cannot read."
            )
    if not isinstance(feed, dict) or not isinstance(feed.get("channels"), list):
        raise SystemExit(f"{path} is not an update feed (no 'channels' array).")
    # Make sure every channel this script knows about exists, so a feed written
    # by an older revision of it still gains new channels rather than failing.
    have = {channel["id"] for channel in feed.get("channels", [])}
    for cid, (title, desc) in CHANNELS.items():
        if cid not in have:
            feed.setdefault("channels", []).append(
                {"id": cid, "title": title, "description": desc, "versions": []}
            )
    return feed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True, help="release tag, e.g. nkt-001")
    parser.add_argument("--tgz", required=True, help="path to the f7 update bundle")
    parser.add_argument("--repo", required=True, help="owner/name on GitHub")
    parser.add_argument("--changelog", default="", help="file to read the changelog from")
    parser.add_argument(
        "--out", default="firmware/directory.json", help="feed to rewrite"
    )
    parser.add_argument(
        "--timestamp",
        type=int,
        default=int(time.time()),
        help="release time, unix seconds (defaults to now)",
    )
    args = parser.parse_args()

    channel_id = channel_for(args.tag)

    if not os.path.isfile(args.tgz):
        raise SystemExit(f"update bundle not found: {args.tgz}")

    changelog = ""
    if args.changelog and os.path.isfile(args.changelog):
        with open(args.changelog, encoding="utf-8") as handle:
            changelog = handle.read().strip()
    if not changelog:
        changelog = f"Nikita firmware {args.tag}."

    asset = os.path.basename(args.tgz)
    entry = {
        "version": args.tag,
        "changelog": changelog,
        "timestamp": args.timestamp,
        "files": [
            {
                # Both clients look for target "f7" AND type "update_tgz";
                # either one wrong and the release is invisible to them.
                "url": (
                    f"https://github.com/{args.repo}/releases/download/{args.tag}/{asset}"
                ),
                "target": "f7",
                "type": "update_tgz",
                "sha256": sha256(args.tgz),
            }
        ],
    }

    feed = load_feed(args.out)
    for channel in feed["channels"]:
        if channel["id"] != channel_id:
            continue
        # Re-tagging the same version replaces its entry rather than adding a
        # duplicate the clients would have to disambiguate.
        versions = [v for v in channel["versions"] if v.get("version") != args.tag]

        # The two clients disagree about what "latest" means: Nikita-iOS sorts
        # a channel by timestamp and takes the newest, nikita-qflipper takes
        # the highest version number. They agree only while the two orderings
        # agree -- so refuse to write an entry that would split them.
        #
        # This is reachable in ordinary use: the timestamp is the tag's commit
        # date, and tagging a commit older than the previous release's (a
        # backport, a re-tag onto an earlier point) produces exactly that.
        newest = max((v.get("timestamp", 0) for v in versions), default=None)
        if newest is not None and entry["timestamp"] <= newest:
            entry["timestamp"] = newest + 1
            print(
                f"warning: {args.tag} is dated no later than the release before "
                f"it; timestamp moved to {entry['timestamp']} so the phone and "
                "the desktop app agree on which build is newest.",
                file=sys.stderr,
            )

        channel["versions"] = [entry] + versions[: MAX_VERSIONS_PER_CHANNEL - 1]
        break

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(feed, handle, indent=2)
        handle.write("\n")

    print(f"{args.out}: {args.tag} -> {channel_id} ({asset})", file=sys.stderr)


if __name__ == "__main__":
    main()
