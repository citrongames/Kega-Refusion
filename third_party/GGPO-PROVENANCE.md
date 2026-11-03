# GGPO source

The GGPO implementation is taken from Flycast commit
`7adbf3caef251b153e348df3f469a0f35878339d`, directory
`core/deps/ggpo`:
https://github.com/flyinghead/flycast/tree/7adbf3caef251b153e348df3f469a0f35878339d/core/deps/ggpo

The pinned Flycast source archive has SHA-256:
`84e4d734f839bd68a6ec19353ecbfb2286f93ca7969fd32dc5b9dd7b48f3519e`.

GGPO is licensed under MIT; see GGPO-LICENSE.txt. This integration extracts
only the GGPO directory. It does not copy Flycast emulator integration code.
The reproducible local transport, packet-bound, local-only startup, and confirmed-frame accessor changes are in
`tools/patches/ggpo-kega.patch` and applied by `tools/prepare-ggpo.py`.

The local RTT initialization and freshness extension is in
tools/patches/ggpo-quality-stats.patch, applied after the transport patch.
It initializes RTT/bandwidth statistics and adds network.quality_samples
and network.quality_age_ms to the source-built GGPONetworkStats API.
The sample count changes only on an existing GGPO quality reply; age is
-1 until the first reply. Counters remain nonzero after wrap, and age/RTT
saturate at INT_MAX. Polling statistics neither sends a packet nor creates
a measurement. GGPO still sends its existing quality reports approximately
once per second; packet layouts, protocol version and report cadence are
unchanged. Consumers must rebuild against the extended local header and use
remote-player handles. Spectator statistics remain unsupported upstream.

All patches are included in the preparation source identity. Changing
any patch regenerates the dependency from the same verified archive.

The runtime local frame-delay transmission fix is in
tools/patches/ggpo-frame-delay.patch. When InputQueue increases delay, P2P
transmits every inserted confirmed padding frame using the existing UDP
input path before the new capture. Startup padding therefore retains its
canonical zero bits; later padding repeats the actual prior queued input.
Shrinking delay keeps the existing deliberate capture-drop behavior.
Ordinary sequential frames keep the original send path without a queue
lookup or extra input copy. No packet fields or prediction rules change.
