import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import render_formula

DIGEST_A = "a" * 64
DIGEST_B = "b" * 64
DIGEST_C = "c" * 64
SUMS = f"""{DIGEST_A}  llmgate-1.2.3-darwin-arm64.tar.gz
{DIGEST_B}  llmgate-1.2.3-linux-x86_64.tar.gz
{DIGEST_C}  llmgate-1.2.3-linux-arm64.tar.gz
{DIGEST_A}  SHA256SUMS.txt
"""


class RenderFormula(unittest.TestCase):
    def test_capability_each_present_platform_gets_its_url_and_digest(self):
        text = render_formula.render(SUMS, "v1.2.3")
        self.assertIn("on_macos do", text)
        self.assertIn("on_linux do", text)
        self.assertIn("releases/download/v1.2.3/llmgate-1.2.3-darwin-arm64.tar.gz", text)
        self.assertIn(f'sha256 "{DIGEST_B}"', text)
        self.assertIn('version "1.2.3"', text)

    def test_limit_a_platform_without_an_asset_is_left_out(self):
        text = render_formula.render(SUMS, "v1.2.3")
        self.assertNotIn("darwin-x86_64", text)

    def test_limit_a_tag_that_disagrees_with_the_assets_is_refused(self):
        with self.assertRaises(ValueError):
            render_formula.render(SUMS, "v9.9.9")

    def test_limit_a_malformed_digest_is_ignored(self):
        with self.assertRaises(ValueError):
            render_formula.render("zz  llmgate-1.2.3-darwin-arm64.tar.gz\n", "v1.2.3")


if __name__ == "__main__":
    unittest.main()
