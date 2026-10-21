# Trenmaxxrr Hacks

<img src="logo.png" width="120" alt="mod logo" />

A Geode mod for Geometry Dash where every hack is a toggle in the mod's settings.

## Get the latest build
1. Open the **Actions** tab of this repo and click the newest run with a green check.
2. Under **Artifacts**, download `trenmaxxrr-hacks-windows` and unzip it.
3. In Steam, right-click Geometry Dash, choose **Manage → Browse local files**, and open `geode/mods`.
4. Drop the `.geode` file in and restart GD.

## Toggle hacks
In GD, open the Geode mod list, find **Trenmaxxrr Hacks**, and open its settings.

## Hacks
| Hack | Setting | What it does |
|---|---|---|
| Menu button | Show menu button | Adds a button to the main menu confirming the mod loaded |
| Input buffer | Input buffer (F6) | Every gamemode except platformer and vertical sections. In a level, F6 cycles off / 2 / 4 / 8 / 16 ticks (wave capped at 8). Predicts your path with GD's own physics and shifts input timing (never moves your player): holds back inputs that would crash you; on wave and ship, switches early when you're about to crash and waits for your input to catch up. Starts off; a label shows on screen while it's on |
