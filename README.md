# PointDownMe 1.0.1 - source

When the player dies, nearby enemies point down at the corpse (Elden Ring 1.17.1). Standalone native DLL for
me3 / Mod Engine 2.

## Build

Requirements: Visual Studio 2019 or newer with "Desktop development with C++" (MSVC x64 tools). No Windows SDK,
no C runtime and no other dependencies.

```
build.bat
```

The script finds Visual Studio with vswhere, generates import libraries from `kernel32.def` / `user32.def`,
compiles `PointDownMe.cpp` and links `PointDownMe.dll` (no default libraries, entry point `DllMain`).

## Files

- `PointDownMe.cpp` - the whole mod.
- `Gesture.h` - the gesture motion: model-space bone rotations relative to the bind pose, 60 frames per second,
  recorded from the game while the player played the Point Downwards gesture.
- `kernel32.def`, `user32.def` - the Win32 functions the DLL imports.

## What the DLL does

- Finds `WorldChrMan` in the game's code by a byte pattern and reads the character list, HP and positions.
- When the player's HP drops to 0 (or F10 is pressed with the game window active), every living character within
  20 m points down:
  - characters on the player skeleton get the real gesture animation through their animation request field;
  - others get the recorded gesture written into their pose. For that it replaces one virtual function pointer of the
    game's pose importer class with a small stub that calls the original function and then the DLL's `OnPose`.
    The stub lives in a small block of memory allocated by the DLL.
- Writes no files, has no network access and touches no other process.
