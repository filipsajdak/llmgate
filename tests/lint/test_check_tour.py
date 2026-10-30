import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import check_tour

GOOD = """# Tour

## What the code uses today

### Feature

- Code: [src/a.cppm](../src/a.cppm) `alpha`
- Test: [tests/a_test.cpp](../tests/a_test.cpp) `beta`
"""


class TourCheckTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        (self.root / "docs").mkdir()
        (self.root / "src").mkdir()
        (self.root / "tests").mkdir()
        (self.root / "src" / "a.cppm").write_text("int alpha;\n")
        (self.root / "tests" / "a_test.cpp").write_text("int beta;\n")
        self.document = self.root / "docs" / "cpp26-tour.md"

    def tearDown(self):
        self.directory.cleanup()

    def problems(self, text):
        self.document.write_text(text)
        return check_tour.check(self.document)

    def test_a_page_whose_links_and_symbols_exist_has_no_problems(self):
        self.assertEqual(self.problems(GOOD), [])

    def test_a_link_to_a_missing_file_is_reported(self):
        found = self.problems(GOOD.replace("src/a.cppm](../src/a.cppm)", "src/gone.cppm](../src/gone.cppm)"))
        self.assertTrue(any("gone.cppm does not exist" in each for each in found))

    def test_a_symbol_that_is_no_longer_in_the_file_is_reported(self):
        found = self.problems(GOOD.replace("`alpha`", "`renamed`"))
        self.assertTrue(any("`renamed` does not occur" in each for each in found))

    def test_a_feature_without_a_code_link_or_a_test_link_is_reported(self):
        found = self.problems("## What the code uses today\n\n### Bare\n\nNothing here.\n")
        self.assertTrue(any("no '- Code: ' link" in each for each in found) and any("no '- Test: ' link" in each for each in found))

    def test_a_page_with_no_feature_sections_is_reported(self):
        self.assertTrue(any("no feature sections" in each for each in self.problems("# Tour\n")))

    def test_external_links_and_anchors_are_not_followed(self):
        self.assertEqual(self.problems(GOOD + "\nSee [site](https://example.com/x) and [top](#tour).\n"), [])

    def test_a_link_to_a_directory_is_accepted_without_a_symbol_check(self):
        self.assertEqual(self.problems(GOOD + "\nSee [the sources](../src) `anything`.\n"), [])

    def test_sections_outside_the_features_part_need_no_links(self):
        self.assertEqual(self.problems(GOOD + "\n## Possible improvements\n\n### An idea\n\nNo links needed.\n"), [])


if __name__ == "__main__":
    unittest.main()
