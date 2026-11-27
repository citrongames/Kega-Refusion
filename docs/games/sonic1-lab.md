# Sonic 1 Lab

[English](#run-the-tool)

## Запуск

Загрузите оригинальный **Sonic the Hedgehog (USA, Europe), REV00** и запустите `games/sonic1-lab.lua` через **Файл → Скрипты…**. Панель сверяет весь ROM за несколько секунд эмуляции. Поддерживается образ 512 КиБ с серийным номером `GM 00001009-00` и CRC32 `F9394E97`. Скрипт читает память и управляет своей панелью и оверлеем.

- **Движение** показывает координаты и шестнадцатеричные доли пикселя, скорости по осям и инерцию `I` в пикселях за кадр, угол поверхности и состояние Соника.
- **Зонды поверхности** показывают пару опорных замеров на земле, стене или потолке в текущей позе. A жёлтый, B голубой. Положительное число означает зазор, отрицательное — проникновение, `--` означает отсутствие поверхности в пределах проверки. **Профили тайлов** показывают локальный рельеф в пределах 3×3 блоков около ног. Цвет профиля отмечает верхнюю коллизию жёлтым, верхнюю с боковой зелёным, боковую и нижнюю без верхней фиолетовым.
- **Хитбоксы** показывают прямоугольник касаний Соника и видимые игровые прямоугольники обычной обработки касаний. Для просмотра адреса, позиции и флагов выберите слот. Выбранный прямоугольник белый. Число рядом с переключателями учитывает предел в 12 прямоугольников плюс выбранный и предел в 24 сегмента рельефа. Платформы и прессы могут использовать собственные проверки.
- **Босс** показывает здоровье активного контроллера, таймер после попадания и состояние столкновения или фазу боя в Final Zone. **След** и **график скорости** используют до 128 замеров через кадр. Загрузка состояния начинает новые наблюдения, перемотка отбрасывает будущие замеры.

Для сравнения поставьте эмуляцию на паузу и сохраните состояние. Нажмите **Начать попытку**, выполните участок и нажмите **Сохранить как призрак**. Загрузите исходное состояние и снова нажмите **Начать попытку**. Фиолетовый прямоугольник и ΔX/ΔY показывают позицию призрака и разницу на том же кадре замера. Сравнение требует тот же акт и начальные координаты с долями пикселя. Попытка хранит до 240 замеров с шагом четыре кадра. Сохранённый призрак доступен до закрытия панели или остановки скрипта.

## Run the tool

Load the original **Sonic the Hedgehog (USA, Europe), REV00** and run `games/sonic1-lab.lua` through **File → Scripts…**. Let emulation run for a few seconds while the panel checks the entire ROM. The supported 512 KiB image has serial `GM 00001009-00` and CRC32 `F9394E97`. All controls affect the Lua tool only.

- **Motion** shows coordinates with hexadecimal subpixels, axis velocities and inertia `I` in pixels per frame, surface angle and Sonic's state.
- **Ground probes** evaluate the two support probes at the displayed pose on the floor, a wall or the ceiling. A is yellow and B is cyan. Positive distances mean separation, negative distances mean penetration and `--` means no surface within the probe's search. **Tile profiles** show collision edges in a 3×3 block area around Sonic's feet. Yellow means top solidity, green means top and side solidity, violet means side and underside solidity.
- **Touch boxes** draw the player touch rectangle and visible objects handled by the common touch routine. Pick a slot to see its address, coordinates and flags. The selected rectangle is white. The panel reports clipping beyond 12 object rectangles plus the selected one, or 24 tile profile segments. Platforms and crushers can use their own collision checks.
- **Boss** shows the active controller's hit count, post-hit timer and contact state or Final Zone phase. **Trail** and **Speed graph** share up to 128 samples taken every other frame. Loading a state starts fresh observations. Rewinding discards future samples.

Pause emulation and save a state before comparing attempts. Choose **Start attempt**, play the segment and choose **Keep as ghost**. Load the starting state and choose **Start attempt** again. The violet marker and ΔX/ΔY compare positions at the same sampled frame. Starts must match the act and both coordinates including subpixels. An attempt holds up to 240 samples at four-frame intervals. The saved ghost lasts until the panel closes or the script stops.

## Sources and verification

The overlay reads the game’s RAM and its original collision tables. The probe and touch calculations follow the REV00 paths in the disassembly. The boss timer layout was cross-checked with the TAS HUD. [Sonic 1 disassembly](https://github.com/sonicretro/s1disasm/tree/AS) · [Sonic collision and touch routine](https://github.com/sonicretro/s1disasm/blob/AS/_incObj/Sonic%20ReactToItem.asm) · [Terrain support routine](https://github.com/sonicretro/s1disasm/blob/AS/_incObj/Sonic%20AnglePos.asm) · [Sonic Gens HUD](https://github.com/flamewing/sonic-gens-hud)

The original cartridge was exercised in Green Hill Act 1 at 59.92274 Hz. Both probes read zero just above the flat terrain at frame 1090. At frame 1200 the touch rectangles aligned with the rings, Buzz Bomber and Chopper. The native core state matched runs with and without the script over frames 1200–1560, with all sections enabled and with presentation every 32nd frame. The boss, wall and ceiling paths use source-derived data.
