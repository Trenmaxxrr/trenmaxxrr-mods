# Trenmaxxrr Hacks: working notes

Geode mod for Geometry Dash. The owner sends a YouTube video of a hack and wants it recreated.

## Requests
- Claude can't watch videos. Ask for the timestamp, a one-line description of what the hack does, and screenshots.
- Before writing a hack, check whether it already exists (Geode mod index, Mega Hack, Eclipse menu) and say so if it does.
- Hacked footage should be labeled as hacked and never submitted to lists.

## Layout
- One mod (`trenmaxxrr.hacks`). Each hack goes in its own file: `src/hacks/<hack-name>.cpp`.
- Each hack gets a `bool` setting in `mod.json` whose key matches the file name. It defaults to `false` unless the owner asks otherwise. Check the setting at hook time with `Mod::get()->getSettingValue<bool>("<hack-name>")`, so toggling works without a restart where possible.
- `src/main.cpp` holds only the starter main-menu button.
- Add each new hack to the table in `README.md`.

## Versions
- Targets Geode `5.11.2` / GD `2.2081` (set in `mod.json`; the build action uses that SDK version). Check the latest Geode release before bumping.
- Bump `version` in `mod.json` for every push: minor for a new hack, patch for a fix. Add a matching entry at the top of `changelog.md`.

## Build
- Claude can't compile locally. Pushing to any branch triggers `.github/workflows/build.yml`, which builds a Windows `.geode` file and uploads it as the `trenmaxxrr-hacks-windows` artifact.
- After pushing, check the run result. If it fails, read the log, fix the error, and push again before telling the owner it's ready.
- The full job log isn't reachable from this environment. Compile and link errors show up as annotations instead: `gh api repos/Trenmaxxrr/trenmaxxrr-mods/check-runs/<job id>/annotations` (job id from `.../actions/runs/<run id>/jobs`).
- The owner installs the build by dropping the `.geode` file into `geode/mods` and restarting GD, then reports back. Claude can't run GD, so the owner is the tester.
