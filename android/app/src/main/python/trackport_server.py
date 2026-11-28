"""Loopback-only Android service for Trackport's bundled WebView UI."""

from __future__ import annotations

import json
import os
import re
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urljoin, urlsplit

AUDIO_LIMIT = 256 * 1024 * 1024
CATALOG_LIMIT = 1024 * 1024
EXPORT_LIMIT = 0xFFFFFFFF
URL_LIMIT = 12_288
YOUTUBE_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")
STATIC_TYPES = {
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".txt": "text/plain; charset=utf-8",
}
SECURITY_HEADERS = (
    ("Cache-Control", "no-store"),
    ("X-Content-Type-Options", "nosniff"),
    ("Referrer-Policy", "no-referrer"),
    ("X-Frame-Options", "DENY"),
    (
        "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; worker-src 'self'; media-src 'self' blob:; "
        "img-src 'self' data:; object-src 'none'; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'none'",
    ),
)

_server: ThreadingHTTPServer | None = None
_server_lock = threading.Lock()


class RequestProblem(Exception):
    def __init__(self, status: int, reason: str, message: str):
        super().__init__(message)
        self.status = status
        self.reason = reason
        self.message = message


def _header_values(headers, name: str) -> list[str]:
    values = headers.get_all(name)
    return [] if values is None else values


def _single_header(headers, name: str) -> str | None:
    values = _header_values(headers, name)
    if len(values) > 1:
        raise RequestProblem(400, "Bad Request", "Invalid request.")
    return values[0] if values else None


def _clean_https_url(value: str) -> bool:
    if not isinstance(value, str) or not value.startswith("https://"):
        return False
    try:
        parts = urlsplit(value)
        host = parts.netloc
        if not host or "@" in host or len(host) > 300:
            return False
        if parts.username or parts.password:
            return False
    except ValueError:
        return False
    return not any(
        ord(char) < 33
        or ord(char) >= 127
        or char in '"\\<>'
        for char in value
    )


def valid_catalog_url(value: str) -> bool:
    return _clean_https_url(value) and (
        value.startswith("https://itunes.apple.com/lookup?")
        or value.startswith("https://open.spotify.com/oembed?")
    )


def valid_audio_url(value: str) -> bool:
    if not _clean_https_url(value):
        return False
    host = (urlsplit(value).hostname or "").lower()
    blocked = {
        "open.spotify.com",
        "music.apple.com",
        "itunes.apple.com",
        "spotify.link",
        "spotify.app.link",
        "apple.co",
    }
    return host not in blocked


def youtube_video_id(value: str) -> str | None:
    prefix = "https://www.youtube.com/watch?v="
    if not value.startswith(prefix):
        return None
    video_id = value[len(prefix) :]
    return video_id if YOUTUBE_ID.fullmatch(video_id) else None


def _valid_proxy_url(value: str) -> bool:
    try:
        parts = urlsplit(value)
        return (
            parts.scheme in {"http", "https", "socks5", "socks5h"}
            and bool(parts.hostname)
            and not parts.username
            and not parts.password
            and not parts.query
            and not parts.fragment
            and parts.path in {"", "/"}
            and not any(
                ord(char) < 33
                or ord(char) >= 127
                or char in '"\\?#'
                for char in value
            )
        )
    except ValueError:
        return False


def _valid_proxy_credential(value: str, is_user: bool = False) -> bool:
    return not any(
        ord(char) < 33
        or ord(char) >= 127
        or char in '"\\'
        or (is_user and char == ":")
        for char in value
    )


def parse_proxy(headers) -> dict[str, str] | None:
    enabled = _single_header(headers, "X-Proxy-Enabled")
    if enabled is None:
        return None
    url = _single_header(headers, "X-Proxy-Url")
    user = _single_header(headers, "X-Proxy-User") or ""
    password = _single_header(headers, "X-Proxy-Pass") or ""
    if (
        enabled != "1"
        or not url
        or not _valid_proxy_url(url)
        or not _valid_proxy_credential(user, is_user=True)
        or not _valid_proxy_credential(password)
    ):
        raise RequestProblem(400, "Bad Request", "Invalid proxy setting.")

    parts = urlsplit(url)
    authority = parts.netloc.rstrip("/")
    if user:
        auth = quote(user, safe="") + ":" + quote(password, safe="") + "@"
        authority = auth + authority
    return {"url": url, "request_url": f"{parts.scheme}://{authority}"}


def _session(proxy: dict[str, str] | None) -> requests.Session:
    import requests

    session = requests.Session()
    session.trust_env = False
    session.headers.update({"User-Agent": "Trackport/Android"})
    if proxy:
        request_url = proxy["request_url"]
        session.proxies = {"http": request_url, "https": request_url}
    return session


def _fetch_to_file(
    value: str,
    proxy: dict[str, str] | None,
    cache_dir: Path,
    limit: int,
    timeout: int,
    headers: dict[str, str] | None = None,
) -> tuple[Path, int]:
    try:
        import requests
    except ImportError as error:
        raise RequestProblem(
            503,
            "Network Client Missing",
            "Trackport's on-device network client is unavailable. Reinstall the test app.",
        ) from error

    if not _clean_https_url(value):
        raise RequestProblem(
            400,
            "Bad Request",
            "Enter a valid direct HTTPS download URL, or use the Track link tab.",
        )

    session = _session(proxy)
    current = value
    response = None
    try:
        for redirects in range(7):
            response = session.get(
                current,
                headers=headers or {},
                stream=True,
                allow_redirects=False,
                timeout=(12, timeout),
            )
            if response.status_code not in {301, 302, 303, 307, 308}:
                break
            location = response.headers.get("Location")
            response.close()
            response = None
            if not location or redirects == 6:
                raise RequestProblem(
                    502, "Incomplete Transfer", "The source did not return a complete download."
                )
            current = urljoin(current, location)
            if not _clean_https_url(current):
                raise RequestProblem(
                    502, "Incomplete Transfer", "The source did not return a complete download."
                )

        assert response is not None
        status = response.status_code
        if status in {401, 403}:
            raise RequestProblem(
                502, "Access Denied", "The source denied the download. Check the link and proxy."
            )
        if status in {404, 410}:
            raise RequestProblem(502, "Source Missing", "The download link is missing or expired.")
        if status == 206:
            raise RequestProblem(
                502, "Partial Source", "The source only returned a partial recording."
            )
        if status != 200:
            raise RequestProblem(
                502,
                "Incomplete Transfer",
                "The source did not return a complete download. Check the link and connection.",
            )

        length = response.headers.get("Content-Length")
        if length and int(length) > limit:
            raise RequestProblem(
                413,
                "Too Large",
                "This file exceeds the 256 MiB import limit."
                if limit == AUDIO_LIMIT
                else "The response is too large.",
            )

        cache_dir.mkdir(parents=True, exist_ok=True)
        fd, path_string = tempfile.mkstemp(prefix="trackport-", dir=cache_dir)
        path = Path(path_string)
        total = 0
        try:
            with os.fdopen(fd, "wb") as output:
                for chunk in response.iter_content(chunk_size=64 * 1024):
                    if not chunk:
                        continue
                    total += len(chunk)
                    if total > limit:
                        raise RequestProblem(
                            413,
                            "Too Large",
                            "This file exceeds the 256 MiB import limit."
                            if limit == AUDIO_LIMIT
                            else "The response is too large.",
                        )
                    output.write(chunk)
            return path, status
        except Exception:
            path.unlink(missing_ok=True)
            raise
    except requests.RequestException:
        if proxy:
            raise RequestProblem(
                502,
                "Proxy Request Failed",
                "The request could not be completed through the configured proxy. "
                "No direct fallback was attempted.",
            )
        raise RequestProblem(
            502,
            "Incomplete Transfer",
            "The source did not return a complete download. Check the link and connection.",
        )
    finally:
        if response is not None:
            response.close()
        session.close()


def _read_small_response(
    value: str, proxy: dict[str, str] | None, cache_dir: Path, limit: int = CATALOG_LIMIT
) -> bytes:
    path, _ = _fetch_to_file(value, proxy, cache_dir, limit, 18)
    try:
        return path.read_bytes()
    finally:
        path.unlink(missing_ok=True)


def _yt_dlp_options(proxy: dict[str, str] | None) -> dict:
    options = {
        "quiet": True,
        "no_warnings": True,
        "ignoreconfig": True,
        "noplaylist": True,
        "socket_timeout": 15,
        "retries": 1,
        "extractor_retries": 1,
        "extractor_args": {"youtube": {"player_client": ["android"]}},
    }
    if proxy:
        options["proxy"] = proxy["request_url"]
    return options


def search_youtube(query: str, proxy: dict[str, str] | None) -> bytes:
    if not query or len(query) > 700 or any(ord(char) < 32 or ord(char) == 127 for char in query):
        raise RequestProblem(
            400, "Bad Request", "Enter a track title to search YouTube."
        )
    try:
        from yt_dlp import YoutubeDL

        options = _yt_dlp_options(proxy)
        options.update({"extract_flat": True, "playlistend": 5})
        with YoutubeDL(options) as ytdlp:
            result = ytdlp.extract_info("ytsearch5:" + query, download=False)
        rows = []
        for entry in (result or {}).get("entries") or []:
            video_id = entry.get("id")
            title = entry.get("title")
            if not isinstance(video_id, str) or not YOUTUBE_ID.fullmatch(video_id):
                continue
            if not isinstance(title, str) or not title.strip():
                continue
            channel = entry.get("channel") or entry.get("uploader") or "Unknown channel"
            duration = entry.get("duration")
            rows.append(
                {
                    "id": video_id,
                    "title": title,
                    "channel": channel if isinstance(channel, str) else "Unknown channel",
                    "duration": duration if isinstance(duration, (int, float)) else None,
                }
            )
            if len(rows) == 5:
                break
        return "\n".join(json.dumps(row, ensure_ascii=False) for row in rows).encode("utf-8")
    except RequestProblem:
        raise
    except ImportError as error:
        raise RequestProblem(
            503,
            "yt-dlp Required",
            "yt-dlp is not available in this app. Rebuild or reinstall Trackport.",
        ) from error
    except Exception:
        message = (
            "YouTube search failed through the configured proxy. No direct fallback was attempted."
            if proxy
            else "YouTube search failed. Check yt-dlp and the network connection."
        )
        raise RequestProblem(502, "YouTube Search Failed", message)


def resolve_youtube(value: str, proxy: dict[str, str] | None) -> tuple[str, dict[str, str]]:
    if not youtube_video_id(value):
        raise RequestProblem(400, "Bad Request", "Choose a valid YouTube match.")
    try:
        from yt_dlp import YoutubeDL

        with YoutubeDL(_yt_dlp_options(proxy)) as ytdlp:
            info = ytdlp.extract_info(value, download=False)
        stream_url = info.get("url") if info else None
        if not isinstance(stream_url, str) or not _clean_https_url(stream_url):
            raise RequestProblem(
                502, "Invalid Stream URL", "yt-dlp did not return a valid HTTPS audio stream."
            )
        stream_headers = info.get("http_headers") or {}
        return stream_url, {
            str(key): str(val)
            for key, val in stream_headers.items()
            if isinstance(key, str) and isinstance(val, str)
        }
    except RequestProblem:
        raise
    except ImportError as error:
        raise RequestProblem(
            503,
            "yt-dlp Required",
            "yt-dlp is not available in this app. Rebuild or reinstall Trackport.",
        ) from error
    except Exception:
        message = (
            "YouTube stream resolution failed through the configured proxy. "
            "No direct fallback was attempted."
            if proxy
            else "yt-dlp could not resolve this YouTube match. Choose another result or retry."
        )
        raise RequestProblem(502, "YouTube Resolution Failed", message)


def _make_handler(web_root: Path, cache_dir: Path):
    class TrackportHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format: str, *_args) -> None:
            return

        @property
        def expected_authority(self) -> str:
            return f"127.0.0.1:{self.server.server_port}"

        def _respond(
            self,
            status: int,
            reason: str,
            content_type: str,
            length: int,
            body=None,
            head: bool = False,
        ) -> None:
            self.send_response(status, reason)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(length))
            self.send_header("Connection", "close")
            for name, value in SECURITY_HEADERS:
                self.send_header(name, value)
            self.end_headers()
            if head or body is None:
                return
            if isinstance(body, (bytes, bytearray)):
                self.wfile.write(body)
                return
            try:
                with body.open("rb") as source:
                    while chunk := source.read(64 * 1024):
                        self.wfile.write(chunk)
            finally:
                body.unlink(missing_ok=True)

        def _message(self, status: int, reason: str, text: str) -> None:
            body = text.encode("utf-8")
            self._respond(
                status, reason, "text/plain; charset=utf-8", len(body), body=body
            )

        def _check_host(self) -> None:
            host = _single_header(self.headers, "Host")
            if host != self.expected_authority:
                raise RequestProblem(
                    403, "Forbidden", "Use the address printed by Trackport."
                )

        def _static(self, head: bool = False) -> None:
            self._check_host()
            if "?" in self.path or "#" in self.path:
                raise RequestProblem(404, "Not Found", "This page could not be found.")
            path_part = self.path
            if path_part == "/":
                path_part = "/index.html"
            candidate = (web_root / path_part.lstrip("/")).resolve()
            if candidate != web_root and web_root not in candidate.parents:
                raise RequestProblem(403, "Forbidden", "Forbidden")
            if not candidate.is_file():
                raise RequestProblem(
                    404, "Not Found", "This page could not be found."
                )
            self._respond(
                200,
                "OK",
                STATIC_TYPES.get(candidate.suffix.lower(), "application/octet-stream"),
                candidate.stat().st_size,
                body=candidate,
                head=head,
            )

        def do_GET(self) -> None:
            try:
                self._static()
            except RequestProblem as problem:
                self._message(problem.status, problem.reason, problem.message)

        def do_HEAD(self) -> None:
            try:
                self._static(head=True)
            except RequestProblem as problem:
                self._message(problem.status, problem.reason, problem.message)

        def _read_body(self, maximum: int) -> bytes:
            if _single_header(self.headers, "Transfer-Encoding") is not None:
                raise RequestProblem(
                    400, "Bad Request", "A fixed request length is required."
                )
            length_text = _single_header(self.headers, "Content-Length")
            if not length_text or not length_text.isdigit():
                raise RequestProblem(
                    400, "Bad Request", "A fixed request length is required."
                )
            if len(length_text) > 10:
                raise RequestProblem(413, "Too Large", "The request is too large.")
            length = int(length_text)
            if length > maximum:
                raise RequestProblem(
                    413, "Too Large", "The request exceeds its size limit."
                )
            body = self.rfile.read(length)
            if len(body) != length:
                raise RequestProblem(
                    400, "Bad Request", "The request body was incomplete."
                )
            return body

        def _check_action(self) -> None:
            origin = _single_header(self.headers, "Origin")
            action = _single_header(self.headers, "X-Trackport")
            if (
                origin != f"http://{self.expected_authority}"
                or action != "1"
            ):
                raise RequestProblem(
                    403,
                    "Forbidden",
                    "Open the app in its original local tab.",
                )

        def do_POST(self) -> None:
            try:
                self._check_host()
                self._check_action()
                if self.path == "/api/export":
                    self._export()
                    return
                body = self._read_body(URL_LIMIT)
                if b"\0" in body:
                    raise RequestProblem(400, "Bad Request", "Invalid URL.")
                value = body.decode("utf-8", errors="strict")
                if self.path == "/api/quit":
                    self._message(200, "OK", "Session ended.")
                    threading.Thread(target=self.server.shutdown, daemon=True).start()
                    return
                if self.path not in {
                    "/api/catalog",
                    "/api/proxy-test",
                    "/api/youtube-search",
                    "/api/audio",
                }:
                    raise RequestProblem(404, "Not Found", "Unknown action.")
                proxy = parse_proxy(self.headers)

                if self.path == "/api/youtube-search":
                    result = search_youtube(value, proxy)
                    self._respond(
                        200,
                        "OK",
                        "application/x-ndjson; charset=utf-8",
                        len(result),
                        body=result,
                    )
                    return

                if self.path == "/api/proxy-test":
                    if not proxy:
                        raise RequestProblem(
                            400, "Bad Request", "Enable the proxy first."
                        )
                    if not valid_audio_url(value):
                        raise RequestProblem(
                            400, "Bad Request", "Enter a valid HTTPS test URL."
                        )
                    started = time.monotonic()
                    path, _ = _fetch_to_file(
                        value, proxy, cache_dir, CATALOG_LIMIT, 18
                    )
                    path.unlink(missing_ok=True)
                    self._message(
                        200,
                        "OK",
                        f"Proxy OK · {round((time.monotonic() - started) * 1000)} ms",
                    )
                    return

                if self.path == "/api/catalog":
                    if not valid_catalog_url(value):
                        raise RequestProblem(
                            400,
                            "Bad Request",
                            "Enter a valid catalog URL.",
                        )
                    result = _read_small_response(value, proxy, cache_dir)
                    self._respond(
                        200,
                        "OK",
                        "application/json; charset=utf-8",
                        len(result),
                        body=result,
                    )
                    return

                if youtube_video_id(value):
                    value, stream_headers = resolve_youtube(value, proxy)
                else:
                    stream_headers = {}
                    if not valid_audio_url(value):
                        raise RequestProblem(
                            400,
                            "Bad Request",
                            "Enter a valid direct HTTPS download URL, or use the Track link tab.",
                        )
                path, _ = _fetch_to_file(
                    value, proxy, cache_dir, AUDIO_LIMIT, 300, stream_headers
                )
                self._respond(
                    200,
                    "OK",
                    "application/octet-stream",
                    path.stat().st_size,
                    body=path,
                )
            except RequestProblem as problem:
                self._message(problem.status, problem.reason, problem.message)
            except UnicodeDecodeError:
                self._message(400, "Bad Request", "Invalid URL.")
            except Exception:
                self._message(
                    502,
                    "Request Failed",
                    "The request could not be completed. Check the link and connection.",
                )

        def _export(self) -> None:
            if _single_header(self.headers, "Transfer-Encoding") is not None:
                raise RequestProblem(
                    400, "Bad Request", "A fixed request length is required."
                )
            length_text = _single_header(self.headers, "Content-Length")
            if not length_text or not length_text.isdigit() or len(length_text) > 10:
                raise RequestProblem(
                    400, "Bad Request", "A fixed request length is required."
                )
            length = int(length_text)
            if length > EXPORT_LIMIT:
                raise RequestProblem(
                    413, "Too Large", "This export exceeds the ZIP size limit."
                )
            export_dir = cache_dir / "exports"
            export_dir.mkdir(parents=True, exist_ok=True)
            fd, path_string = tempfile.mkstemp(prefix="export-", dir=export_dir)
            path = Path(path_string)
            remaining = length
            try:
                with os.fdopen(fd, "wb") as output:
                    while remaining:
                        chunk = self.rfile.read(min(64 * 1024, remaining))
                        if not chunk:
                            raise RequestProblem(
                                400, "Bad Request", "The export upload was incomplete."
                            )
                        output.write(chunk)
                        remaining -= len(chunk)
                export_id = path.name.removeprefix("export-")
                payload = export_id.encode("ascii")
                self._respond(
                    200,
                    "OK",
                    "text/plain; charset=utf-8",
                    len(payload),
                    body=payload,
                )
            except Exception:
                path.unlink(missing_ok=True)
                raise

        def handle_expect_100(self) -> bool:
            return True

    return TrackportHandler


def start_server(web_root: str, cache_dir: str) -> int:
    global _server
    with _server_lock:
        if _server is not None:
            return _server.server_port
        root = Path(web_root).resolve()
        cache = Path(cache_dir).resolve()
        if not (root / "index.html").is_file():
            raise RuntimeError("Trackport's packaged web files are missing.")
        cache.mkdir(parents=True, exist_ok=True)
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0), _make_handler(root, cache)
        )
        server.daemon_threads = True
        server.request_queue_size = 16
        thread = threading.Thread(
            target=server.serve_forever, name="TrackportLoopback", daemon=True
        )
        thread.start()
        _server = server
        return server.server_port


def stop_server() -> None:
    global _server
    with _server_lock:
        server = _server
        _server = None
    if server is not None:
        server.shutdown()
        server.server_close()
