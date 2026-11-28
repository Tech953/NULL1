import email.message
import importlib.util
import sys
import tempfile
import types
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


SERVER_PATH = (
    Path(__file__).parents[1]
    / "app"
    / "src"
    / "main"
    / "python"
    / "trackport_server.py"
)
spec = importlib.util.spec_from_file_location("trackport_server", SERVER_PATH)
server = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = server
spec.loader.exec_module(server)


class TrackportServerTests(unittest.TestCase):
    def test_catalog_allowlist_is_exact(self):
        self.assertTrue(
            server.valid_catalog_url(
                "https://itunes.apple.com/lookup?id=123&country=us"
            )
        )
        self.assertTrue(
            server.valid_catalog_url(
                "https://open.spotify.com/oembed?url=https%3A%2F%2Fopen.spotify.com%2Ftrack%2Fabc"
            )
        )
        self.assertFalse(
            server.valid_catalog_url(
                "https://itunes.apple.com.example/lookup?id=123"
            )
        )
        self.assertFalse(server.valid_catalog_url("https://music.apple.com/song/123"))

    def test_direct_audio_urls_keep_drm_pages_blocked(self):
        self.assertTrue(server.valid_audio_url("https://artist.example/audio.mp3"))
        for url in (
            "https://open.spotify.com/track/abc",
            "https://music.apple.com/us/song/title/123",
            "https://itunes.apple.com/lookup?id=123",
            "http://artist.example/audio.mp3",
            "https://user:password@artist.example/audio.mp3",
        ):
            with self.subTest(url=url):
                self.assertFalse(server.valid_audio_url(url))

    def test_youtube_watch_url_requires_one_video_id(self):
        video_id = "AbCdEfGhI_1"
        self.assertEqual(
            server.youtube_video_id(
                f"https://www.youtube.com/watch?v={video_id}"
            ),
            video_id,
        )
        self.assertIsNone(
            server.youtube_video_id(
                f"https://www.youtube.com/watch?v={video_id}&list=playlist"
            )
        )
        self.assertIsNone(
            server.youtube_video_id("https://youtube.com/watch?v=AbCdEfGhI_1")
        )

    def test_proxy_authentication_is_encoded_for_both_clients(self):
        headers = email.message.Message()
        headers.add_header("X-Proxy-Enabled", "1")
        headers.add_header("X-Proxy-Url", "socks5h://proxy.example:1080/")
        headers.add_header("X-Proxy-User", "user@example")
        headers.add_header("X-Proxy-Pass", "p@ss:word")
        proxy = server.parse_proxy(headers)
        self.assertEqual(proxy["url"], "socks5h://proxy.example:1080/")
        self.assertEqual(
            proxy["request_url"],
            "socks5h://user%40example:p%40ss%3Aword@proxy.example:1080",
        )

    def test_bad_or_ambiguous_proxy_settings_fail_closed(self):
        missing = email.message.Message()
        self.assertIsNone(server.parse_proxy(missing))

        bad = email.message.Message()
        bad.add_header("X-Proxy-Enabled", "1")
        bad.add_header("X-Proxy-Url", "socks5://user@proxy.example:1080")
        with self.assertRaises(server.RequestProblem):
            server.parse_proxy(bad)

        duplicate = email.message.Message()
        duplicate.add_header("X-Proxy-Enabled", "1")
        duplicate.add_header("X-Proxy-Enabled", "0")
        with self.assertRaises(server.RequestProblem):
            server.parse_proxy(duplicate)

    def test_python_requests_session_does_not_use_environment_fallback(self):
        class FakeSession:
            def __init__(self):
                self.trust_env = True
                self.headers = {}
                self.proxies = {}

        fake_requests = SimpleNamespace(Session=FakeSession)
        proxy = {
            "url": "http://proxy.example:8080",
            "request_url": "http://user:pass@proxy.example:8080",
        }
        with patch.dict(sys.modules, {"requests": fake_requests}):
            session = server._session(proxy)
        self.assertFalse(session.trust_env)
        self.assertEqual(
            session.proxies,
            {
                "http": "http://user:pass@proxy.example:8080",
                "https": "http://user:pass@proxy.example:8080",
            },
        )

    def test_failed_proxy_fetch_never_retries_without_the_proxy(self):
        class ProxyFailure(Exception):
            pass

        class FailedSession:
            calls = 0

            def __init__(self):
                self.trust_env = True
                self.headers = {}
                self.proxies = {}

            def get(self, *_args, **_kwargs):
                self.calls += 1
                raise ProxyFailure("proxy unavailable")

            def close(self):
                pass

        session = FailedSession()
        fake_requests = types.ModuleType("requests")
        fake_requests.Session = lambda: session
        fake_requests.RequestException = ProxyFailure
        proxy = {
            "url": "http://proxy.example:8080",
            "request_url": "http://proxy.example:8080",
        }
        with tempfile.TemporaryDirectory() as cache:
            with patch.dict(sys.modules, {"requests": fake_requests}):
                with self.assertRaises(server.RequestProblem) as error:
                    server._fetch_to_file(
                        "https://artist.example/audio.mp3",
                        proxy,
                        Path(cache),
                        server.AUDIO_LIMIT,
                        18,
                    )
        self.assertEqual(session.calls, 1)
        self.assertIn("No direct fallback", error.exception.message)

    def test_ytdlp_receives_the_same_proxy_configuration(self):
        options = server._yt_dlp_options(
            {
                "url": "socks5h://proxy.example:1080",
                "request_url": "socks5h://user:pass@proxy.example:1080",
            }
        )
        self.assertEqual(
            options["proxy"], "socks5h://user:pass@proxy.example:1080"
        )
        self.assertEqual(
            options["extractor_args"]["youtube"]["player_client"], ["android"]
        )
        self.assertTrue(options["ignoreconfig"])
        self.assertTrue(options["noplaylist"])

    def test_youtube_search_failure_with_proxy_does_not_fall_back(self):
        seen_options = []

        class FailingYtDlp:
            def __init__(self, options):
                seen_options.append(options)

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def extract_info(self, *_args, **_kwargs):
                raise RuntimeError("network failure")

        fake_ytdlp = types.ModuleType("yt_dlp")
        fake_ytdlp.YoutubeDL = FailingYtDlp
        proxy = {
            "url": "socks5h://proxy.example:1080",
            "request_url": "socks5h://proxy.example:1080",
        }
        with patch.dict(sys.modules, {"yt_dlp": fake_ytdlp}):
            with self.assertRaises(server.RequestProblem) as error:
                server.search_youtube("track title artist", proxy)
        self.assertEqual(seen_options[0]["proxy"], proxy["request_url"])
        self.assertIn("No direct fallback", error.exception.message)

    def test_loopback_server_sets_csp_and_rejects_wrong_host(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as cache:
            web_root = Path(root)
            (web_root / "index.html").write_text("<!doctype html><title>Trackport</title>")
            port = server.start_server(str(web_root), cache)
            try:
                response = urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/", timeout=3
                )
                self.assertEqual(response.status, 200)
                self.assertIn(
                    "connect-src 'self'",
                    response.headers["Content-Security-Policy"],
                )
                self.assertIn("Trackport", response.read().decode("utf-8"))

                wrong_host = urllib.request.Request(
                    f"http://127.0.0.1:{port}/",
                    headers={"Host": f"localhost:{port}"},
                )
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(wrong_host, timeout=3)
                self.assertEqual(error.exception.code, 403)
            finally:
                server.stop_server()


if __name__ == "__main__":
    unittest.main()
