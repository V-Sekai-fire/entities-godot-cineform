# godot-cineform

A GDExtension that gives Godot's Movie Maker a CineForm exporter.

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
