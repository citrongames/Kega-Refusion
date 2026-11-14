# Releases and update notes

[← Building and packaging](building.md)

## Version and files

`project(KegaRefusion VERSION X.Y.Z ...)` in `CMakeLists.txt` is the single source of the application version. Before building a release, set its final numeric version there; use the same checkout and version for Windows, Linux and corresponding sources. Do not rename an old build to claim a new version. The Git tag is `vX.Y.Z`, and the release title is `Kega Refusion X.Y.Z`.

Attach these exact filenames, replacing `X.Y.Z` with the version:

- `KegaRefusion-X.Y.Z-windows-x86_64.zip`
- `KegaRefusion-X.Y.Z-linux-x86_64.tar.gz`
- `KegaRefusion-X.Y.Z-source.tar.gz`
- `SHA256SUMS.txt`
- `update.json` (generated after the other four uploads)

The source archive includes the corresponding integrated-core and dependency sources and their build instructions. GitHub's automatically generated checkout archives do not replace it. `SHA256SUMS.txt` lists the SHA-256 and filename of each of the three attached archives in `sha256sum` format.

Build the binary archives from Release configurations using the [build guide](building.md). For Linux, use the Debian 12 build environment and pass `--max-glibc 2.36` to the packager. Verify the actual packaged application's version against the release tag on both platforms, then calculate checksums from those final archives. Preserve the user's `data/`, games and BIOS files when testing upgrades.

The updater discovers the latest stable tag through GitHub's public `/releases/latest` redirect, then reads `update.json` from that tag's release assets. Runtime checks do not need a GitHub API token or anonymous REST quota. The snapshot contains the two binary assets' `digest` fields from the authenticated GitHub API. Each must contain `sha256:` followed by 64 hexadecimal digits and match the final archive and `SHA256SUMS.txt`; a checksum file alone does not satisfy this requirement.

## Generate update metadata

Prepare a **draft**, non-prerelease GitHub release with its final tag and authored body, then upload the two binary packages, corresponding-source archive and `SHA256SUMS.txt`. With `gh` authenticated, find the draft's numeric API ID:

```sh
gh api repos/citrongames/Kega-Refusion/releases \
  --jq '.[] | select(.draft) | {id,tag_name}'
```

Use that ID to generate the fifth asset (replace the example ID and version):

```sh
python3 tools/release-update-metadata.py \
  --release-id 123456789 --output build/release-X.Y.Z/update.json
gh release upload vX.Y.Z build/release-X.Y.Z/update.json \
  --repo citrongames/Kega-Refusion
```

The generator reads the authenticated draft by ID. It validates the numeric tag, stable status, exact uploaded filenames, binary sizes, download URLs and GitHub SHA-256 digests. If a digest is missing, wait for GitHub to expose it and retry; do not invent a digest or bypass the check. It copies the authored `body` exactly, includes only the two platform archives in `assets`, and records the eventual `draft:false` and `prerelease:false` state as compact UTF-8 JSON. A draft asset may have a temporary same-repository `untagged-...` download URL; the generator accepts that draft-only placeholder and always writes the canonical URL for the validated final numeric tag.

Regenerate and replace `update.json` on the draft after any body or binary change. Publish only when all five assets and the final body have been reviewed. Keep the exact filenames and a stable, non-draft final release. After publication, check the latest-tag redirect and public `update.json`, confirm the body and package metadata match, and verify the binary downloads and archive checksums.

## Release body

Write the release body in English. Group short bullets under plain section names on their own lines. The updater displays plain text, so these labels also read cleanly in older versions. Leave a blank line before each list.

- **Added** covers new capabilities.
- **Improved** covers improvements to existing capabilities.
- **Changed** is optional. Use it when an existing default, control or workflow changes.
- **Fixed** covers corrected behavior.

Omit empty sections. Give each independently noticeable change its own bullet, even when several affect the same system. A short example or a required user action can stay with the change it explains. Keep the notes focused on what changed since the previous published tag. The version and downloads are already shown separately.

```markdown
Added

- [One new capability.]

Improved

- [One improvement.]

Changed

- [One change to an existing workflow.]

Fixed

- [One corrected problem.]
- [Another corrected problem.]
```

Keep verification evidence in the repository's [verification record](verification.md). Check the body against the outgoing tag and review the notes and assets before publishing.
