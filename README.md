# godot-cineform

A GDExtension that gives Godot's Movie Maker a CineForm exporter.

Godot records offline video through `MovieWriter`. It ships two writers, an AVI of raw
uncompressed frames and a PNG image sequence. Raw AVI at 1024 by 1024 costs 3 GB for every
1000 frames. CineForm costs about 127 MB for the same frames, and every frame stays a
keyframe.

Set the output file extension to `.cfhd` and this writer claims it.

## Why not FFmpeg

`EIRTeam.FFmpeg` already does video in Godot and is MIT licensed. The plugin is not the
problem. FFmpeg is LGPL-2.1 or later, and a Godot export links everything into one binary.
LGPL then asks for relinkable object files or a swappable shared library, on every release,
forever.

GoPro publishes the CineForm SDK under `Apache-2.0 OR MIT`. It ships both licence files. That
is the same pair this repository uses, so nothing new enters the export.

RFD 0123 carries the full argument and the measurements behind it.

## Status

**It compiles and it links.** Built on Windows x86_64 with MSVC 19.44.35228, godot-cpp at
`4bc6e67d51df`, cineform-sdk at `11574d029577`. The result is a 1,191,936 byte DLL that
exports exactly one symbol, `cineform_library_init`, which is the `entry_symbol` in
`cineform.gdextension`.

**It has never been loaded by Godot.** Compiling, linking and exporting the right symbol are
three things that can all be true while the extension still fails to register, or registers
and writes a file no decoder accepts. That is the next gate and it is not done.

RETRACTED: an earlier draft of this file said the extension had not been compiled and listed
each SDK symbol against the header it was read from. Reading the headers was right and it was
not enough. The build found a real error that no amount of header reading would have caught:
`_get_audio_mix_rate` was declared `int64_t`, because `extension_api.json` calls the return
type `int`. godot-cpp generates it as `uint32_t`. The JSON names Variant types, not C++ types.

MSVC reported that as one non covariant return type, and then as six further errors about
`ClassDB::bind_method` and `memdelete` that named neither the method nor the cause. A reader
who started at the first `bind_method` error would have gone a long way in the wrong
direction.

The floor is Godot **4.2**, matching godot-whisper commit for commit. Their tree builds on
nine platforms, so a platform failure here is ours rather than a question about the binding
library. `gen_bindings.py` shows every method hash we bind is unchanged from 4.0 to 4.5, so
the floor is a choice about which tree to trust and not a limit anybody measured.

## This repository is a repo manifest root

`default.xml` here is standalone. It includes nothing and is included by nothing, so a clone
of this repository is enough to build it.

    repo init -u https://github.com/weftspun/godot-cineform -m default.xml
    repo sync

That places `godot-cpp` and `cineform-sdk` under `thirdparty/`, both forked to `weftspun` and
pinned to a commit.

They are not git submodules. `CLAUDE.md` blocklists those: a submodule pins a dependency in a
file only `git` reads, which `repo status` cannot see, and a bump appears in a diff as a bare
hash with no name and no reason. A manifest entry carries the name, the remote, the revision
and a comment saying why.

RETRACTED: an earlier arrangement put both dependencies in `weftspun/weftspun`'s manifest as
siblings at `3-interactor/godot-cpp` and `3-interactor/cineform-sdk`. This project was their
only consumer, so that made every workspace checkout fetch a C++ binding library and a video
codec for a build most desks never run. It also expressed their relationship to this project
as a comment rather than as a path.

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

## The container is AVI, and the corpus wants Matroska

This writes CineForm in AVI. GoPro's own tools emit CineForm in AVI, and the SDK ships an AVI
reader, so the two agree about the format.

Matroska is the workspace container of record. Remuxing is a stream copy and re-encodes
nothing.

    ffmpeg -i out.cfhd -c copy out.mkv

That is one pass over the bytes. It is not a second lossy step.

## What is verified, and what is not

`verify_roundtrip.py` encodes a known pattern, decodes it with FFmpeg, and reports the error
for each channel. It ships a negative control that asserts a deliberately corrupted file
fails the same check. A gate that only ever passes has proved nothing.

The SDK symbols below were read from the real headers rather than recalled, and the build
then linked against every one of them. The table stays because it records where each name came
from, which is what makes an update reviewable.

| symbol | read from |
| --- | --- |
| `CFHD_CreateEncoderPool`, `CFHD_PrepareEncoderPool`, `CFHD_StartEncoderPool` | `Common/CFHDEncoder.h` |
| `CFHD_EncodeAsyncSample`, `CFHD_WaitForSample`, `CFHD_TestForSample` | `Common/CFHDEncoder.h` |
| `CFHD_GetEncodedSample`, `CFHD_ReleaseSampleBuffer`, `CFHD_ReleaseEncoderPool` | `Common/CFHDEncoder.h` |
| `CFHD_PIXEL_FORMAT_BGRA` | `Common/CFHDTypes.h` |
| `CFHD_ENCODED_FORMAT_RGB_444`, `CFHD_ENCODED_FORMAT_RGBA_4444` | `Common/CFHDTypes.h` |
| `CFHD_ENCODING_QUALITY_LOW` through `FILMSCAN3` | `Common/CFHDTypes.h` |
| `CFHD_ENCODING_FLAGS_NONE` | `Common/CFHDTypes.h` |
| `CFHD_ERROR_OKAY` | `Common/CFHDError.h` |
| `MovieWriter` virtual signatures | godot-cpp generated `movie_writer.hpp` |

The generated header is the authority, not `extension_api.json`. The two disagree about C++
types, and that disagreement cost a build.

## Known gaps

**No audio.** `_get_audio_mix_rate` returns 0, so Godot allocates no audio blocks. A writer
that accepted audio and dropped it would look like it were recording sound.

**Even dimensions only.** CineForm works in 8 by 8 wavelet blocks. `_write_begin` rejects an
odd width or height with a message naming the size, because the SDK reports it late and
unhelpfully.

**8 bit in.** Movie Maker hands `MovieWriter` an `Image`, which is 8 bit for each channel.
The encoder carries 12 bits internally, so nothing is lost here, but this path cannot record
more than the engine renders. The 12 bit depth corpus is written by the offline renderer
through FFmpeg, not through this extension.

## Licence

`Apache-2.0 OR MIT`, matching the CineForm SDK and the rest of this workspace.
