# Documentation

[← Project overview](../README.md)

Start with the overview for current features and basic use, or the build guide for development. Feature notes and audits retain implementation detail without expanding the main README. Some documents are in Russian; dated reports describe the version and scope they tested.

## Build and development

- [Building, packaging and focused checks](building.md)
- [Release versions, assets and update notes](releasing.md)
- [Architecture and resource-management rules](architecture.md)
- [Verification record](verification.md)
- [Adding an interface translation](../languages/README.md)
- [Project licensing and core exception](../LICENSING.md)
- [Third-party components and licensing](../THIRD_PARTY.md)

## Features

- [Direct netplay and GGPO](netplay.md)
- [Mega Drive Link Cable](link-cable.md)
- [Controllers, light guns, Mouse, Paddle and J-Cart](controller-devices.md)
- [Team Player, 4 Way Play and Master Tap](multitap.md)
- [Cheats and memory search](cheats.md)
- [Graphics/audio debugging controls](debug-controls.md)
- [ZIP/7z handling](archive-support.md)
- [Video aspect ratios](video-aspect-audit.md)
- [32X / CD32X integration](32x-integration.md)
- [Paprium implementation and limitations](../src/paprium/PROVENANCE.md)

## Compatibility investigations

These are evidence records, not a claim that every listed game has been completed or is fully compatible.

| Area | Main reports |
|---|---|
| Mega Drive unlicensed games | [Per-ROM review](pirate-rom-review.txt), [machine-readable ledger](pirate-rom-review.json), [mapper audit](pirate-mapper-audit.txt) |
| Master System | [Overall review](sms-rom-review.txt), [pirate games](sms-pirate-review.txt), [homebrew](sms-homebrew-review.txt), [Codemasters](sms-codemasters-audit.txt) |
| Master System PAL detection | [Region catalog](sms-region-catalog.json), [validation](sms-region-validation.json) |
| Specific sound/protection issues | [VMV audio compatibility](vmv-audio-compatibility.txt), [Link Dragon](link-dragon-audit.txt), [Super Donkey Kong 99](pirate-sdk99-followup.json) |

Additional per-feature validation files remain beside these reports. Consult Git history for earlier README descriptions and implementation milestones.
