# godot-cineform

A GDExtension that gives Godot's Movie Maker a CineForm exporter.

Godot records offline video through `MovieWriter`. It ships two writers, an AVI of raw
uncompressed frames and a PNG image sequence. Raw AVI at 1024 by 1024 costs 3 GB for every
1000 frames. CineForm costs about 127 MB for the same frames, and every frame stays a
keyframe.

Set the output file extension to `.cfhd` and this writer claims it.

## Build

    repo sync                      # or clone the two thirdparty repos by hand
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build --parallel

`CMAKE_POLICY_VERSION_MINIMUM` is needed because `cineform-sdk` asks for CMake 3.5.1 and
CMake 4 refuses anything below 3.10 without it. The alternative is patching a vendored
dependency, which turns every future update into a merge.

Build `-DCMAKE_BUILD_TYPE=Debug` as well. The Godot **editor** carries the `debug` feature
tag, so it loads `template_debug`. A release only build leaves the editor resolving a library
that does not exist, and it says nothing at all when that happens.

The library lands in `demo/addons/cineform/bin` under the exact name
`cineform.gdextension` expects. Godot reports a name mismatch as "extension not found" and
says nothing about which name it wanted, so the CMake rule builds it rather than a person.

## Use

Movie Maker is driven from the command line or from the editor.

    godot --headless --write-movie out.cfhd --quit-after 300

From a script, the writer exposes three settings.

    var w := MovieWriterCineForm.new()
    w.set_quality(2)        # 0 low .. 5 filmscan3, default 2 which is HIGH
    w.set_thread_count(0)   # 0 lets the encoder pool choose
    w.set_keep_alpha(false) # true encodes RGBA_4444 instead of RGB_444
