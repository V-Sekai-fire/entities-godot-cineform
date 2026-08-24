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

## Use

From a script, the writer exposes three settings.

    var w := MovieWriterCineForm.new()
    w.set_quality(2)        # 0 low .. 5 filmscan3, default 2 which is HIGH
    w.set_thread_count(0)   # 0 lets the encoder pool choose
    w.set_keep_alpha(false) # true encodes RGBA_4444 instead of RGB_444
