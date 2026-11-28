"""Create an Android-only copy of Trackport's shared web UI."""

import argparse
import shutil
from pathlib import Path


ANDROID_BOOTSTRAP = """<script src="android-bootstrap.js" defer></script>"""
ANDROID_EXPORT = """<script src="android-export.js" defer></script>"""
BASE_AUDIO_SCRIPT = """<script src="audio.js" defer></script>"""
BASE_TRANSFER_SCRIPT = """<script src="transfer.js" defer></script>"""
MP3_SAVE_CALL = "Transfer.save(j.objectURL, j.outName)"
ZIP_SAVE_CALL = """Transfer.save(url, "Trackport-MP3s.zip")"""


def prepare(source: Path, output: Path, overlay: Path) -> None:
    source = source.resolve()
    output = output.resolve()
    overlay = overlay.resolve()
    if not (source / "index.html").is_file():
        raise FileNotFoundError(f"Trackport web source missing: {source / 'index.html'}")
    if not (overlay / "android-bootstrap.js").is_file():
        raise FileNotFoundError(f"Android bootstrap missing: {overlay / 'android-bootstrap.js'}")
    if not (overlay / "android-export.js").is_file():
        raise FileNotFoundError(f"Android export bridge missing: {overlay / 'android-export.js'}")

    shutil.rmtree(output, ignore_errors=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, output)

    index_path = output / "index.html"
    html = index_path.read_text(encoding="utf-8")
    if html.count(BASE_AUDIO_SCRIPT) != 1 or html.count(BASE_TRANSFER_SCRIPT) != 1:
        raise ValueError(
            "Trackport's HTML script order changed; update the Android asset preparation step."
        )
    html = html.replace(
        BASE_AUDIO_SCRIPT,
        ANDROID_BOOTSTRAP + "\n  " + BASE_AUDIO_SCRIPT,
        1,
    )
    html = html.replace(
        BASE_TRANSFER_SCRIPT,
        BASE_TRANSFER_SCRIPT + "\n  " + ANDROID_EXPORT,
        1,
    )
    index_path.write_text(html, encoding="utf-8")

    app_path = output / "app.js"
    app_js = app_path.read_text(encoding="utf-8")
    if app_js.count(MP3_SAVE_CALL) != 1 or app_js.count(ZIP_SAVE_CALL) != 1:
        raise ValueError(
            "Trackport's MP3/ZIP export calls changed; update the Android export adapter."
        )
    app_js = app_js.replace(MP3_SAVE_CALL, "Transfer.save(j.blob, j.outName)", 1)
    app_js = app_js.replace(ZIP_SAVE_CALL, """Transfer.save(b, "Trackport-MP3s.zip")""", 1)
    app_path.write_text(app_js, encoding="utf-8")

    for overlay_file in overlay.iterdir():
        if not overlay_file.is_file():
            raise ValueError(f"Unexpected directory in Android web asset overlay: {overlay_file}")
        shutil.copy2(overlay_file, output / overlay_file.name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--overlay", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output, args.overlay)


if __name__ == "__main__":
    main()
