# Snappy Turns

A [Dusklight](https://twilitrealm.dev/) mod that makes Link and Wolf Link turn faster and respond more directly to the control stick.

In vanilla Twilight Princess, Link eases toward the stick direction over several frames, and reversing direction triggers a turnaround, skid or slip animation. Snappy Turns shortens that delay and can replace reversals with an instant pivot. Link and Wolf Link are toggled separately.

## Settings

Open **Mods → Snappy Turns** in Dusklight.

| Setting | Default | Effect |
|---|---|---|
| Snappy turns for Link | On | Enables faster turning in human form. |
| Link turn speed | 250% | 100% is vanilla. 500% faces the stick almost immediately. |
| Snappy turns for Wolf Link | On | Enables faster turning in wolf form. |
| Wolf Link turn speed | 250% | 100% is vanilla. 500% faces the stick almost immediately. |
| Quick turnaround | On | Reversing the stick pivots instantly instead of playing the turnaround, skid or slip animation. Applies to each form that has Snappy Turns enabled. |

## How it works

The mod hooks `daAlink_c::setSpeedAndAngleNormal` (Link) and `daAlink_c::setSpeedAndAngleWolf` (Wolf Link).

- **Turn speed** measures how far the game turned Link on the current frame and multiplies that step, clamped so it never overshoots the stick direction. Because it scales the game's own step, vanilla rules (stick tilt, cutscenes, walls, frames where the game intentionally does not turn) still apply.
- **Quick turnaround** snaps facing to the stick direction on a reversal before the game chooses a turnaround animation, and halves speed on that frame so the reversal reads as a pivot.

Only ordinary free movement is changed. Z-targeting, aiming, swimming, climbing, cutscenes, ice, Iron Boots and the initial wolf dash burst keep vanilla behavior. The original game function always runs, so other mods that hook the same functions continue to work.

## Installation

1. Download `snappy_turns.dusk` from [Releases](../../releases). The release bundle contains libraries for Windows (x64/ARM64), Linux (x86_64/ARM64, including Steam Deck), macOS (Apple Silicon/Intel), Android and iOS.
2. Open Dusklight's data folder (**Settings → Interface → Open Data Folder**) and place the file in the `mods` folder.
3. Enable **Snappy Turns** in the in-game mod manager.

## Compatibility

- **Twinstick Aiming**: compatible. Twinstick Aiming only changes movement while aiming; Snappy Turns does not act during aiming.
- **Dawnlight**: compatible. Dawnlight's sprint and wolf sprint hooks run the original movement function, which Snappy Turns builds on.
- **Modern Combat Input / Enhanced Swimming**: no overlap.

## Building

Requirements: CMake 3.26+, Ninja, and a C++20 compiler (MSVC on Windows, Clang or GCC elsewhere).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first configure fetches the pinned Dusklight SDK into `./dusklight`. The package is written to `build/mods/snappy_turns.dusk` and contains the library for the platform you built on.

To build against a different Dusklight version, add `-DDUSKLIGHT_VERSION=<tag-or-commit>`. To use an existing Dusklight checkout, add `-DDUSKLIGHT_DIR=<path>`.

### All platforms

The GitHub Actions workflow in `.github/workflows/build.yml` builds every supported platform and merges them into one bundle with `tools/merge_mod.py`. Pushing a version tag (for example `v1.0.0`) attaches the combined bundle to a GitHub release.

## Credits

`cmake/FetchDusklight.cmake`, `tools/merge_mod.py` and the CI workflow are adapted from [Dawnlight](https://github.com/BeZide93/dawnlight) (CC0-1.0).

## License

MIT. See [LICENSE](LICENSE).
