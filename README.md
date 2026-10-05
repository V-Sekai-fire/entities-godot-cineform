# entities-godot-cineform

A GDExtension that gives the engine's movie recording a CineForm writer, claimed by the `.cfhd` extension.

## What it is for

It lets a Godot project record its frames straight to a wavelet intermediate codec whose licence
matches the workspace's own, so nothing LGPL enters an export. RFD 1123 owns the design, and
`demo/` is a project that loads the addon.

## Build

The two dependencies come from this repository's own `default.xml`:

```sh
repo init -u https://github.com/V-Sekai-fire/entities-godot-cineform -m default.xml
repo sync
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

The policy minimum is there because the pinned codec SDK asks for an older CMake.

## Licence

Apache-2.0 OR MIT; see `LICENSE-APACHE` and `LICENSE-MIT`.
