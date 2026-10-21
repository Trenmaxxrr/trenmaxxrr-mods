# v2.0.1
Fixes from an audit for unintended help:
- An early switch your input never confirmed now counts as a crash. Before, switching back still let you keep the dodge, so the mod could clear an obstacle with no input from you. Turning the buffer off mid-switch counts the same way.
- Predictions no longer set off the level's event triggers (the hidden player's jumps and landings were firing them).
- The mod never blocks a real death; only the hidden player's crashes are caught.
- The on-screen label is re-checked every tick, so the buffer can't run without it showing.
- Quick taps made while an input is held back are kept (they were being dropped).
- Turning the buffer off removes the hidden player from the level right away.

# v2.0.0
- Rewrite: the buffer now changes input timing instead of moving your player.
- Predictions run a hidden copy of your player through GD's own physics and collisions (orbs, pads, portals and triggers are blocked during predictions; if one is touched, your input goes through untouched).
- Holds back clicks/releases that would crash you if a slightly later one wouldn't.
- Wave and ship: switches early when you're about to crash, then waits up to the stage's tick count for your real input; if it doesn't come, it switches back.
- Moving hazards still use their predicted positions.

# v1.4.0
- Moving hazards: while the buffer is on, nearby spikes, saws and blocks are tracked every tick, so a fix won't put you where a moving hazard is about to be (or skip a spot one is about to leave).

# v1.3.0
- Early release: on wave and ship, if holding would crash you and letting go a few ticks earlier wouldn't have, the click is released for you. Ship learns its physics from your play, so it starts working after you've let go in a ship once that session (normal and mini are learned separately).

# v1.2.0
- Input buffer now works in every gamemode (not platformer or vertical sections). Stage 4 is 16 ticks for everything and 8 for the wave, same as the original.

# v1.1.0
- Input buffer for the wave: F6 cycles off / 2 / 4 / 8 / 16 ticks. If a click would make the wave hit a spike, saw or block, it's treated as if it happened a few ticks earlier or later. Shows an on-screen label while on.

# v1.0.0
- Starter build: main menu button confirming the mod loaded.
