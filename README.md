# Coda Bot

A Geode mod for Geometry Dash 2.2091 where Coda learns levels by trial and error.

- Turn him on from the pause menu (Coda's face, bottom right).
- His runs never count: test mode is forced, completions are blocked, verifying is blocked.
- He's good, not perfect: sometimes he fumbles parts he already knows.

## Frame Window Counter

Measures how much timing room every click and release in a level has, like NaN GD's videos.

1. Open the Coda Bot window (~), record a macro that beats the level (or load one).
2. Press **Count frame windows**. Coda replays the macro. Before each input, an invisible copy of the
   player replays it again with that one input moved earlier and later, one physics step at a time,
   until it dies. The steps that still survive are the frame window.
3. Windows are shown in 240 Hz frames (`1`, `2`, `3` ... `9-12`) with the size popping up above your icon,
   plus ms and Hz for the last input. **Save report** writes every input to `macros/<name>-windows.txt`.

Notes: set TPS to what the macro was made at and the bot's frame offset to 0. Inputs the simulation
can't reproduce (moving objects, some triggers) are marked "couldn't measure". Counting is a replay, so
Coda Safe blocks completions while it runs.
