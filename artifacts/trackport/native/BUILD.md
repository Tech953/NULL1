# Build Trackport

Trackport embeds its interface and audio workers as compressed static resources in a native launcher. The launcher serves only the loopback interface. Local imports stay in the browser. Direct links use a native download process and are staged until the HTTPS transfer completes.

## Windows x64

Use Python 3.8 or later and a 64-bit MinGW-w64 GCC toolchain. From the source directory, run the following command.

```sh
python build.py --windows --cc x86_64-w64-mingw32-gcc
```

A native MinGW-w64 shell may expose the same compiler as `gcc`. The output is `build/Trackport.exe`. Windows users running the finished binary only need their browser and the built-in HTTPS client. The launcher opens the default browser or prints its loopback address when run with `--no-browser`.

The packaged Windows executable was cross-compiled with TinyCC's x86_64 Windows target at revision `5cef7f3f85b0fbbd4663fd3518ba097d7fe3d738`. Build the compiler and its runtime with `sh configure` and `make cross-x86_64-win32` in the TinyCC source directory. The following command uses that build from a Linux host. Substitute the actual absolute path to TinyCC.

```sh
TCC_ROOT=/absolute/path/to/tinycc
python3 build.py --windows --cc "$TCC_ROOT/x86_64-win32-tcc" --extra "-B$TCC_ROOT/win32 -I$TCC_ROOT/include -L$TCC_ROOT"
```

## Linux

A C11 compiler, Python 3.8 or later, curl, and a browser are sufficient to build and run the same application locally.

```sh
python3 build.py
./build/trackport
```

To run a local smoke check before acceptance testing, run this single command from the `source` directory:

```sh
python3 smoke_build.py
```

It builds the Linux server in a temporary directory and checks that the executable contains the proxy and YouTube route markers. A compiler error or missing marker exits with a clear failure message; the check does not change the app or leave a build artifact.

## Project files

| Path | Purpose |
| --- | --- |
| `src/server.c` | Loopback HTTP server, embedded assets and actions |
| `src/platform.c` | OS-specific launch, HTTPS transfer and temporary-file handling |
| `app/catalog.js` | Shared song links, Apple ID lookups and Spotify oEmbed |
| `app/audio.js` | Audio identification, tag reading, ID3 writing, decoding and resampling |
| `app/decode-aac.js` | Bundled fallback for common DRM-free AAC-LC files |
| `app/encoder.js` | Background MP3 encoding |
| `app/transfer.js` | Source loading, export filenames and ZIP creation |
| `app/app.js` | Source selection, batch rendering, playback and saves |
| `app/vendor/` | Replaceable local codecs and third-party notices |

Run the build script after changing any source or vendor file. The resulting executable contains the updated resources.

## Verification

The packaged app's shared code was built and run on Linux. Browser-driven checks used local HTTPS fixtures for both catalog APIs and direct audio downloads. They included redirects, failed and incomplete transfers, MP3 passthrough, WAV, FLAC, AAC-LC M4A, Ogg/Opus, Unicode tags, colliding filenames and ZIP integrity. The browser also saved an individual MP3 and a ZIP to disk and loaded an export in its audio player. All four bitrate presets, cancellation and retry were exercised. The exported MP3s were decoded independently to check duration, mono/stereo layout, channel tones and bitrate. The Windows executable was checked as an x86-64 PE and its embedded assets were compared with the tested build. A native Windows launch and live provider lookups have not been tested in this environment.
