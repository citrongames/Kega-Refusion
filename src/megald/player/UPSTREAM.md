# PD6103A player

This C state machine is adapted from ares commit `0b928930af4cc59ed99e942996f0d3c815379388`, principally `ares/md/mcd/megald.cpp` and the MegaLD playback/mixing paths in `ares/md/mcd/cdd.cpp`. Its ISC notice is in `LICENSE`.

The module retains the input/output register banks, frozen write transaction ordering, output-write readback cooldown, drive transitions, latched seeks and stop points, BCD status/TOC reporting, frame skipping/stepping, analog audio routing and attenuation, digital memory/field selection, VBI picture stops and foreground/backdrop faders. It owns no media or decoded-frame allocation. `ld_player.h` documents the CDD and field synchronization contract. Host events are synchronous and may change CDD transport state.

The host must enable the CDD clock after reset. Video uses the original NTSC 30000/1001 frame rate, and Redbook transport is 75 sectors/second with ABA = LBA + 150. Video frame 1 therefore begins at LBA -150. The host must latch the target video frame on seek completion, including a seek completing paused, before reporting completion to software.

One upstream assignment typo in the single-frame-loop predicate is corrected to comparison (`currentDriveState == 0x05`). A physical lead-in region is addressed in chronological QON order, while the transport counts its frames backwards from ABA zero. Audio fade-to-mute remains immediate and mechanical state change delay remains read-count based, matching the approximations in the pinned reference. Search playback speed 6/7 has the same fallback advance as that reference; accurate hardware search-pattern timing is not claimed.

Focused state-machine validation:

```sh
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  ld_player.c tests/player_test.c -lm -o /tmp/player_test
/tmp/player_test
```

The tests cover frozen register transactions, output readback, seek/mechanical busy transitions, target-frame latching, held video, single-step/reverse advance, analog/digital routing, attenuation, graphic faders, partial reset and picture stops. Real-core Hi-Roller validation additionally exercises BIOS boot, digital program reads, analog intro, VBI title stop, mission selection and briefing seeks, cockpit gameplay, directional reticle motion, gun/missile input and state restoration.
