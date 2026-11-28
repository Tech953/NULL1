import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "prepare_web.py"
spec = importlib.util.spec_from_file_location("prepare_web", SCRIPT)
prepare_web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_web)


class AndroidWebPreparationTests(unittest.TestCase):
    def test_android_adapters_are_injected_without_changing_shared_source(self):
        workspace = Path(__file__).parents[2]
        source_candidates = (
            workspace / "artifacts" / "trackport" / "native" / "app",
            workspace / "source" / "app",
        )
        source = next(path for path in source_candidates if path.is_dir())
        overlay = (
            workspace
            / "android"
            / "app"
            / "src"
            / "main"
            / "assets"
            / "overlay"
        )
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "www"
            original_index = (source / "index.html").read_text(encoding="utf-8")
            original_app = (source / "app.js").read_text(encoding="utf-8")

            prepare_web.prepare(source, output, overlay)

            index = (output / "index.html").read_text(encoding="utf-8")
            app_js = (output / "app.js").read_text(encoding="utf-8")
            self.assertLess(index.index("android-bootstrap.js"), index.index("proxy.js"))
            self.assertLess(index.index("transfer.js"), index.index("android-export.js"))
            self.assertIn("Transfer.save(j.blob, j.outName)", app_js)
            self.assertIn('Transfer.save(b, "Trackport-MP3s.zip")', app_js)
            self.assertTrue((output / "vendor" / "lame.all.js").is_file())
            self.assertTrue((output / "android-bootstrap.js").is_file())
            self.assertTrue((output / "android-export.js").is_file())
            self.assertEqual((source / "index.html").read_text(encoding="utf-8"), original_index)
            self.assertEqual((source / "app.js").read_text(encoding="utf-8"), original_app)


if __name__ == "__main__":
    unittest.main()
