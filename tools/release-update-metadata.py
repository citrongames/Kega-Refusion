#!/usr/bin/env python3
"""Generate update.json from an authenticated GitHub draft release, without publishing."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

REPOSITORY = "citrongames/Kega-Refusion"
MAX_DOWNLOAD = 512 << 20
MAX_METADATA = 1 << 20


def release_metadata(release: dict) -> dict:
    if not isinstance(release, dict):
        raise ValueError("GitHub release metadata must be an object.")
    tag = release.get("tag_name")
    if not isinstance(tag, str) or not re.fullmatch(r"v[0-9]{1,8}\.[0-9]{1,8}\.[0-9]{1,8}", tag):
        raise ValueError("The release tag must be vX.Y.Z with numeric components.")
    if release.get("prerelease") is not False:
        raise ValueError("The release must not be a prerelease.")
    if type(release.get("draft")) is not bool:
        raise ValueError("GitHub release metadata is missing its draft flag.")
    body = release.get("body")
    if not isinstance(body, str) or not body.strip():
        raise ValueError("Write the release body before generating update.json.")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValueError("GitHub release metadata is missing its asset list.")
    version = tag[1:]
    binary_names = [f"KegaRefusion-{version}-windows-x86_64.zip",
                    f"KegaRefusion-{version}-linux-x86_64.tar.gz"]
    required_names = binary_names + [f"KegaRefusion-{version}-source.tar.gz", "SHA256SUMS.txt"]
    selected = {}
    for name in required_names:
        matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
        if len(matches) != 1 or matches[0].get("state") != "uploaded":
            raise ValueError(f"Upload exactly one ready asset named {name} first.")
        selected[name] = matches[0]
    binaries = []
    for name in binary_names:
        asset = selected[name]
        digest = asset.get("digest")
        if not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
            raise ValueError(f"GitHub has not supplied a SHA-256 digest for {name}; retry after it is available.")
        size = asset.get("size")
        if type(size) is not int or not 0 < size <= MAX_DOWNLOAD:
            raise ValueError(f"The size of {name} must be between 1 byte and 512 MiB.")
        expected = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
        download_url = asset.get("browser_download_url")
        # GitHub can return a temporary untagged URL for an uploaded draft asset.
        # It is accepted only within this repository; the snapshot always names
        # the final numeric tag, never the temporary draft route.
        draft_url = (release["draft"] and isinstance(download_url, str) and
                     re.fullmatch(rf"https://github\.com/{re.escape(REPOSITORY)}/releases/download/untagged-[0-9a-f]{{1,64}}/{re.escape(name)}", download_url))
        if download_url != expected and not draft_url:
            raise ValueError(f"Unexpected download URL for {name}.")
        binaries.append({"name": name, "state": "uploaded", "size": size,
                         "digest": digest, "browser_download_url": expected})
    # This snapshot becomes public with the release; an authenticated draft is
    # expected here, but the installed updater must see the final stable state.
    return {"tag_name": tag, "body": body, "draft": False, "prerelease": False,
            "assets": binaries}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-id", required=True, type=int,
                        help="numeric GitHub release ID (draft or published stable release)")
    parser.add_argument("--output", required=True, type=Path,
                        help="path for generated update.json, normally under build/")
    args = parser.parse_args()
    if args.release_id <= 0:
        parser.error("--release-id must be positive")
    try:
        response = subprocess.run(
            ["gh", "api", "--hostname", "github.com",
             f"repos/{REPOSITORY}/releases/{args.release_id}"],
            check=True, capture_output=True, text=True, encoding="utf-8")
        metadata = release_metadata(json.loads(response.stdout))
        content = (json.dumps(metadata, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(content) > MAX_METADATA:
            raise ValueError("Generated metadata exceeds the updater's 1 MiB limit.")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(content)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
        print(f"Cannot generate update.json: {detail}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output} ({len(content)} bytes); upload it as update.json before publishing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
