# GGPO source

The GGPO implementation is taken from Flycast commit
`7adbf3caef251b153e348df3f469a0f35878339d`, directory
`core/deps/ggpo`:
https://github.com/flyinghead/flycast/tree/7adbf3caef251b153e348df3f469a0f35878339d/core/deps/ggpo

The pinned Flycast source archive has SHA-256:
`84e4d734f839bd68a6ec19353ecbfb2286f93ca7969fd32dc5b9dd7b48f3519e`.

GGPO is licensed under MIT; see GGPO-LICENSE.txt. This integration extracts
only the GGPO directory. It does not copy Flycast emulator integration code.
The reproducible local transport, packet-bound, and confirmed-frame accessor changes are in
`tools/patches/ggpo-kega.patch` and applied by `tools/prepare-ggpo.py`.