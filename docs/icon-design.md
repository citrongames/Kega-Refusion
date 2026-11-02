# Application icon

User-selected artwork: C, bold blue integrated KR with two white stripes and an outline. This replaces the previously selected blue/violet icon.

Original built-in ImageGen output: exec-9ca4bb33-351d-44f4-adaa-14655fe8a2c6.png. The exact earlier artwork is retained as assets/app.png. No regeneration or creative alteration was applied when changing the selection. assets/app.ico contains 16/24/32/48/64/128/256 px representations for Windows.

Windows resource compilation explicitly depends on assets/app.ico, so replacing the selected artwork updates both the Qt window icon and the executable resource during incremental builds.


## Controller illustrations (0.7.4)

Generated with the built-in ImageGen tool, not the API/CLI. Final transparent PNG assets are `assets/controllers/md3.png`, `md6.png`, `sms.png`, `gg.png`, `sg1000.png`. They are raster illustrations in an SVG-like drawn style, not actual SVG files. Rejected MD3 and SJ-200 variants are not included in the project.

Shared prompt: clean SVG-like flat vector illustration for interactive emulator controller mapping; front orthographic view, whole recognizable original Sega controller silhouette; charcoal/gray solid fills, dark crisp outlines, restrained highlights, no photographic texture/perspective, no cable/hands/environment; transparent background, landscape 1536×1024. Preserve authentic button count, placements and labels.

Final subject prompts:

- **MD3:** Follow the reference original Mega Drive three-button pad: broad oval dome top and deep central bottom semicircular notch between two short rounded lobes. Circular D-pad left; SEGA between D-pad and buttons; red angled START upper-right above A/B/C. Three black round buttons in a rising diagonal, white A/B/C on shell just above each, TRIGGER below row, MEGA DRIVE CONTROL PAD at top. No six-button shell or extra controls. [Shape reference](https://www.fullyretro.com/product/mega-drive-official-controller-3-button,41243339).
- **MD6:** Compact rounded black six-button fighting pad; circular D-pad left; smaller light-gray X/Y/Z over larger dark A/B/C on right, both rows rising slightly. Oval START near upper center; small MODE shoulder button on upper-right shell edge. Small SEGA center; no analog sticks or additional buttons.
- **SMS:** Western original black rectangular Master System control pad, softly rounded corners, restrained red face outline; square directional area with cross markings left, exactly two round black buttons 1/2 right, START text by button 1. No fictitious Pause or Start button on the controller. Console Pause is drawn separately by the UI.
- **GG:** Original black Game Gear handheld, wide rounded shell; central blank screen and bezel, left black cross D-pad, right buttons 1 lower-left and 2 upper-right; small oval START above both buttons, speaker slots below, red power dot left of screen, SEGA GAME GEAR above screen. No game screenshot.
- **SG-1000:** Follow original SG-1000 II SJ-150 reference: horizontal light gray-beige rounded rectangle, large black circular directional disk left with small central rounded nub and four cardinal raised markings; exactly two vertical oval black unlabeled buttons side by side right. Above them black badge with blue SEGA and white SJ-150 strip. No cable or generic cross substitution. [Shape reference](https://retrounit.com.au/products/official-sega-sj-150-gamepad-controller-for-sega-sg-1000-ii-console).

Only final generated artwork is embedded; downloaded hardware reference photos remain scratch and are removed after integration.


## Light-gun illustrations (0.8.0)

Generated through the built-in ImageGen tool, using hardware photos as silhouette references. Final embedded transparent 1536×1024 PNGs: `assets/controllers/phaser.png`, `assets/controllers/menacer.png`, `assets/controllers/justifier.png`. Flat SVG-like raster illustrations, not SVG files. UI hit regions follow actual visible buttons; hidden Menacer buttons and the virtual reload action are separate controls below the artwork. Only the selected resource pixmap is retained, with no per-frame reload.

References: [Light Phaser](https://www.computinghistory.org.uk/det/35159/Sega-Master-System-Light-Phaser/), [Menacer](https://www.mega-drive.net/mdguns.htm), [Justifier](https://www.ricardo.ch/de/a/the-justifier-konami-gun-fuer-sega-mega-drive-1232250942/).

Exact generation prompts:

- **phaser:** Use case: style-transfer. Reference photo is an ORIGINAL SEGA Master System Light Phaser video game accessory. Produce a clean flat SVG-like drawn UI illustration, not a photo, on transparent landscape1536x1024 canvas. Precisely preserve recognizable hardware silhouette and proportions: long angular flat black rectangular barrel pointing RIGHT, handle on left side downward, only one black trigger directly in front of grip, no trigger guard invented. Entire accessory centered with comfortable margins. Remove cable/background. Keep small LIGHT PHASER and SEGA label near rear of barrel. Charcoal flat solidfills, crisp darkoutlines, restrainedgrayhighlights, no texture/no dramaticshadow/no photorealism. No extra controls or labels. This is a toy videogame lightgun mapping diagram.

- **menacer:** Use case: style-transfer. Reference ORIGINAL grey Sega Menacer videogame lightgun main body. Draw faithful clean SVG-like illustration for controller mapping UI, not photo. Preserve reference silhouette EXACTLY: barrel pointing LEFT with orange-red muzzle rim, short hollow triangular lower frame that connects foregrip to rear pistolgrip, large triangular open center, red trigger near upper right inside opening, rear horizontal extension and two sight mounting tabs top. Small SEGA and MENACER mouldedlabels. No added stock/scope/cable. Front sideview orthographic sameasreference entirebody centered with margins, transparent landscape1536x1024 canvas. Flatgraycolors, simple crisp dark outlines, limitedflat highlights, no photographic texture/3D/externalshadows. Keep real buttons, do not invent numbered buttons on side. This is a videogame accessory illustration.

- **justifier:** Use case: style-transfer. Reference is original blue Konami The Justifier videogame lightgun. Draw faithful SVG-like flat illustration for emulator mapping UI. Preciselypreserve its revolver-shaped blue plastic shell: longbarrel LEFT, cylindrical imitationcylinder center, black curved trigger inside blue triggerguard beneathcenter, blue round STARTbutton abovegrip onright side ofcylinder, slantedbluehandle towardbottomright. Match reference buttonlocations/proportions. Wholeaccessory centered with comfortablemargins, transparent1536x1024landscape. Remove cable/photobackground. Small THE JUSTIFIER/KONAMI/START markings likehardware. Cleancrisp outlines, flatsolidblue/charcoal colors, limited simplehighlights, no photo texture/no realisticmetal/no dramatic3Dshadow/no extrabuttons. Toyvideogame accessory, not realweapon.
