# Lua examples

[Русская версия](lua-examples.ru.md)

Load a game, open **File → Scripts…**, select a numbered file in the packaged `scripts` folder and choose **Run**. Use **Stop** in the same window to remove its overlay and any tool window. The examples use the emulator's English or Russian captions at startup.

The emulator's OSD switch controls its built-in indicators. Lua overlays stay visible, and their in-game text has a dark translucent background. Several examples have their own checkbox for the in-game label.

## 01_Overlay.lua

Draws an animated crosshair, a rectangle, a circle and a PNG in game coordinates. The label in HUD coordinates keeps its size when you resize the window. Keep `overlay-marker.png` beside the script if you copy this example.

## 02_Controllers.lua

Press buttons or move an analog control to see the inputs used on the latest completed frame. Each connected logical controller has its own row. During movie playback the rows show the recorded inputs.

## 03_MemoryWatch.lua

Choose a physical memory domain, an offset such as `0x10`, and a width of 1, 2 or 4 bytes. The tool reads using the domain's byte order. Choose **Add to RAM Watch** to add a temporary row to the emulator's RAM Watch. Choose **Remove watch** to remove it.

## 04_Events.lua

Watch `beforeframe` report the previous frame's clock and `frame` report a completed frame. Reset, load a state or rewind to see the corresponding events. Start the script while paused to see `draw` redraw the current frame without advancing the emulated clock.

## 05_InputSequence.lua

In local play with a directional pad, choose **Try Right for 30 frames**. The script captures a temporary checkpoint, holds Right for 30 emulated frames, then restores the checkpoint and releases the override. Resume or step if the game is paused. Use **Cancel and restore** to stop the trial early, and let the trial finish or cancel it before stopping the script itself.

## 06_Timeline.lua

Save a state while the script is running. Advance a few frames, then load the state or rewind to see the counters follow the timeline. After loading a file, advance one frame to show the counters again. The first line counts completed frames. The second shows how many of the observed frames polled a controller. To load those counters after restarting the script, run the same unedited source before loading the saved state.

## 07_FrameLog.lua

Run this example in local play.

1. Save your progress if you want to return to it. Enter a fresh movie name such as `lua-frame-log` and choose **Record from power-on**. This power-resets the game and starts a native input recording.
2. Play or step the game, then choose **Stop and save**. The script also stops at 3,600 frames. Saving produces a `.krm` file in the configured Movies folder and a separate measurement file in the script's storage. Wait for **Recording and measurements saved** before stopping the script.
3. Run the same script with the same movie name and choose **Play with samples**. The script checks the movie identity and rebuilds the graph as playback reaches the recorded positions.

While paused, choose **Check status** to process pending file results. The graph shows the percentage of frames with a controller poll in each complete 30-frame sample. The game may update its own logic at a different rate.

## 08_WindowControls.lua

Try **Pause** and **Resume**, change the HUD color, adjust the frames per sample or enter a note. The graph shows the percentage of frames that polled a controller in each sample. Use **Clear graph** to start a new series.

## Game-specific tools

The packaged `scripts/games` folder contains tools for specific cartridge revisions. Sonic 1 Lab · Contra: Hard Corps Combat Lab

Contra’s combat and solid-object layers show verified HP above the corresponding hitboxes on a translucent background.

Use the API reference to adapt an example or build your own tool. [Lua API](lua-api.md)
