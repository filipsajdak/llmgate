import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import cpp_source


class TokenizerTest(unittest.TestCase):
    def test_comment_markers_inside_literals_are_not_comments(self):
        source = cpp_source.scan('auto a = "x // y"; auto b = R"j(// z /* w */)j"; char c = \'"\'; auto d = "q\\" // r";\n')
        self.assertEqual(source.comments, ())

    def test_a_comment_after_a_literal_is_found(self):
        source = cpp_source.scan('auto a = "x"; // trailing\n/* block */\n')
        self.assertEqual([c.text for c in source.comments], ["// trailing", "/* block */"])
        self.assertEqual([c.whole_line for c in source.comments], [False, True])

    def test_digit_separators_do_not_open_a_char_literal(self):
        source = cpp_source.scan("auto n = 1'000'000; // c\n")
        self.assertEqual(len(source.comments), 1)

    def test_escaped_quotes_stay_inside_the_literal(self):
        source = cpp_source.scan('auto s = "a\\"b"; // c\n')
        self.assertEqual([item.body for item in source.literals], ['a\\"b'])
        self.assertEqual(len(source.comments), 1)

    def test_the_code_view_blanks_comments_and_raw_bodies_but_keeps_lines(self):
        source = cpp_source.scan('x // c\nauto r = R"(a\nb)";\n')
        self.assertEqual(source.code.count("\n"), source.text.count("\n"))
        self.assertNotIn("// c", source.code)
        self.assertNotIn("a\n", source.code.split("R")[1])

    def test_the_skeleton_view_blanks_string_bodies(self):
        source = cpp_source.scan('f("secret", \'c\');\n')
        self.assertEqual(source.skeleton, 'f("      ", \' \');\n')

    def test_line_numbers_follow_offsets(self):
        source = cpp_source.scan("a\nb\nc\n")
        self.assertEqual(source.line(source.text.index("c")), 3)


if __name__ == "__main__":
    unittest.main()
