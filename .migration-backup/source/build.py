"""Embed Trackport's web assets and compile its native desktop launcher."""
import argparse
import gzip
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent
MIME = {
    ".js": "text/javascript; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
}


def write_assets(out):
    base = ROOT / "app"
    assets = []
    lines = [
        "struct Asset { const char *route, *mime; const unsigned char *data; int size; };"
    ]
    for path in sorted(base.rglob("*")):
        if not path.is_file() or path.suffix not in MIME:
            continue
        name = f"asset_{len(assets)}"
        compressed = gzip.compress(path.read_bytes(), compresslevel=9, mtime=0)
        lines.append(f"static const unsigned char {name}[]={{")
        for start in range(0, len(compressed), 40):
            group = compressed[start:start + 40]
            lines.append(",".join(str(byte) for byte in group) + ",")
        lines.append("};")
        route = "/" + path.relative_to(base).as_posix()
        fields = [f'"{route}"', f'"{MIME[path.suffix]}"', name, str(len(compressed))]
        assets.append("{" + ",".join(fields) + "}")
    lines.append("static const struct Asset assets[]={" + ",".join(assets) + "};")
    lines.append(f"static const int asset_count={len(assets)};")
    (out / "assets.inc").write_text("\n".join(lines) + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--windows", action="store_true")
    parser.add_argument("--extra", default="", help="Additional compiler and linker arguments")
    parser.add_argument("--out", type=Path, default=ROOT / "build")
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(exist_ok=True, parents=True)
    write_assets(out)
    exe = out / ("Trackport.exe" if args.windows else "trackport")
    command = [
        args.cc, "-I" + str(out), "-o", str(exe),
        str(ROOT / "src/server.c"), str(ROOT / "src/platform.c")
    ]
    if args.windows:
        if "tcc" in Path(args.cc).name:
            command.append(str(ROOT / "src/shell32.def"))
        else:
            command.append("-lshell32")
        command.append("-lws2_32")
    else:
        command.extend(["-O2", "-std=c11", "-Wall", "-Wextra", "-pthread"])
    subprocess.run(command + shlex.split(args.extra), check=True)
    print(exe)


if __name__ == "__main__":
    main()
