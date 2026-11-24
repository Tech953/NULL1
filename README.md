# Trackport

MP3 export for Windows x64. Convert DRM-free audio to MP3 (up to 320 kbps), or copy existing MP3s byte-for-byte, and export tracks individually or as a ZIP for a phone, USB drive, or music player.

## Getting started

1. Extract this ZIP and double-click `Trackport.exe`. The app opens in your default browser. Keep the Trackport console open. If a tab does not open, copy the local address printed in the console into a current Edge, Chrome or Firefox.
2. Add one or more files on the Files tab. On Download URL, paste a direct HTTPS link to a full recording. A download offered by an artist, label or store is a suitable source.
3. To use an Apple Music or Spotify reference, choose Track link and paste a shared song link. Select Look up track, then attach the matching audio file or a direct download URL. A Spotify track URI also works. For a shortened share link, open it once and copy the full song URL from the address bar.
4. Pick a bitrate and select Render queue. Choose Listen to check an export. Save MP3 downloads one track. Save ZIP packages every ready track together. Your browser saves to its normal download folder or asks where to save.
5. Extract the exported ZIP before copying its MP3s to a USB drive, player or phone. Use End session when finished.

## Which files work?

- Downloaded, DRM-free iTunes Store purchases, recordings you made, and downloadable full audio from an artist or store can be converted. For a purchase on Windows, download it in Apple's Music app, locate the M4A on disk and add it to Trackport. Apple's instructions for redownloading purchases are at https://support.apple.com/en-us/120553.
- Apple Music and Spotify subscription offline downloads are tied to their players. A song link supplies catalog information only. Trackport needs the full audio separately — attach a DRM-free file or a direct download URL. Use the specific song link when starting from an album.
- A verified MPEG Layer III input is copied byte-for-byte, including its tags and artwork. The bitrate selector applies only to new encodes.
- New encodes use 44.1 kHz mono or stereo and 128, 192, 256 or 320 kbps MP3. Choose an original lossless file when available. A higher output bitrate cannot recover detail removed by an earlier encode.
- WAV, FLAC, Ogg/Opus and DRM-free AAC-LC M4A were exercised in testing. The bundled AAC-LC decoder can read common iTunes-style files even if the browser lacks an AAC decoder. Other input codecs depend on the browser.
- For new encodes, Trackport writes available title, artist, album and track fields from supported tags. Catalog values may supply the title, artist and album. New encodes omit artwork and other original tag types. Original MP3 copies keep all bytes even when a track reference gives the copy a new name.
- Files with the same output name get numbered suffixes. If the Apple catalog supplies a different duration, the row shows the discrepancy for review.

## Operating details

Designed for current Windows 10/11 x64 and a current browser. The executable includes the interface and both audio-codec components. Local file conversion works offline. Online lookups and direct HTTPS downloads use the Windows system copy of curl.exe. No sign-in, API key, Node.js, Python or FFmpeg installation is required to use the executable.

Imports are limited to 256 MiB per file. Audio is decoded in memory and one track renders at a time. Save smaller batches for long recordings or tight memory. Completed files stay in this tab until you save, remove or close them. ZIP export uses the standard non-ZIP64 format and supports batches under 4 GiB. For a phone using MTP rather than a drive letter, save to your PC, then copy with Explorer.

The server binds to an available loopback port on this computer. The app contacts a catalog only for a lookup. A direct URL is sent to its origin and redirect destinations. Downloads are staged in a temporary file which closes after the transfer. Private subscription caches, account pages and streaming playlists are unsuitable inputs.

## Contents

| Path | Purpose |
| --- | --- |
| `Trackport.exe` | Ready-to-run Windows x64 application (distributed in the release ZIP, not stored here) |
| `source/` | Editable application and build instructions |
| `source/app/vendor/` | Included codec source, credits and licenses |

## Building from source

Trackport embeds its interface and audio workers as compressed static resources in a native launcher. The launcher serves only the loopback interface.

**Windows x64** — Python 3.8+ and a 64-bit MinGW-w64 GCC toolchain. From the `source` directory:

```sh
python build.py --windows --cc x86_64-w64-mingw32-gcc
```

Output is `build/Trackport.exe`.

**Linux** — a C11 compiler, Python 3.8+, curl, and a browser:

```sh
python3 build.py
./build/trackport
```

See [`source/BUILD.md`](source/BUILD.md) for the full build guide, including the TinyCC cross-compile path used for the packaged executable.

## Verification

The packaged app's shared code was built and run on Linux. Browser-driven checks used local HTTPS fixtures for both catalog APIs and direct audio downloads, covering redirects, failed and incomplete transfers, MP3 passthrough, WAV, FLAC, AAC-LC M4A, Ogg/Opus, Unicode tags, colliding filenames and ZIP integrity. All four bitrate presets, cancellation and retry were exercised, and exported MP3s were decoded independently to check duration, layout, tones and bitrate. **A native Windows launch and live provider lookups have not been tested.**
