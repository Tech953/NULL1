"""Build the Linux server and verify its proxy and YouTube route markers."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
MARKERS = (
    b"X-Proxy-Enabled",
    b"/api/proxy-test",
    b"/api/youtube-search",
)


def main():
    try:
        with tempfile.TemporaryDirectory(prefix="trackport-smoke-") as directory:
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "build.py"),
                    "--out",
                    directory,
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            executable = Path(directory) / "trackport"
            contents = executable.read_bytes()
    except subprocess.CalledProcessError as error:
        print(
            f"Linux smoke build failed (build process exited with status {error.returncode}).",
            file=sys.stderr,
        )
        return error.returncode or 1
    except OSError as error:
        print(f"Linux smoke build failed: {error}", file=sys.stderr)
        return 1

    missing = [marker.decode("ascii") for marker in MARKERS if marker not in contents]
    if missing:
        for marker in missing:
            print(f"Linux smoke build failed: executable is missing marker {marker!r}.", file=sys.stderr)
        return 1

    print("Linux smoke build passed: proxy and YouTube route markers are present.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
