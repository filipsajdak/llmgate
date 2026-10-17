import bisect
import dataclasses
import re

IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
PP_NUMBER = re.compile(r"\.?\d(?:[eEpP][+-]|[\w.'])*")
RAW_OPEN = re.compile(r'"([^()\\\s]{0,16})\(')
RAW_PREFIXES = frozenset({"R", "u8R", "uR", "UR", "LR"})
BLANK = " "
NEWLINE = "\n"


@dataclasses.dataclass(frozen=True)
class Comment:
    line: int
    text: str
    block: bool
    whole_line: bool
    before: str


@dataclasses.dataclass(frozen=True)
class Literal:
    line: int
    raw: bool
    body: str


@dataclasses.dataclass(frozen=True)
class Source:
    text: str
    code: str
    skeleton: str
    comments: tuple
    literals: tuple
    starts: tuple

    def line(self, offset):
        return bisect.bisect_right(self.starts, offset)

    def line_text(self, number):
        lines = self.text.splitlines()
        return lines[number - 1] if 0 < number <= len(lines) else ""

    def text_at(self, offset):
        return self.line_text(self.line(offset)).strip()


def blank(buffer, begin, end):
    for index in range(begin, end):
        if buffer[index] != NEWLINE:
            buffer[index] = BLANK


def line_starts(text):
    return tuple([0] + [m.end() for m in re.finditer(NEWLINE, text)])


def scan(text):
    code = list(text)
    skeleton = list(text)
    starts = line_starts(text)
    comments = []
    literals = []
    size = len(text)

    def line_of(offset):
        return bisect.bisect_right(starts, offset)

    def before_of(offset):
        return text[starts[line_of(offset) - 1] : offset]

    def close_quoted(begin, quote):
        end = begin + 1
        while end < size and text[end] != quote and text[end] != NEWLINE:
            end += 2 if text[end] == "\\" else 1
        return min(end, size)

    def take_comment(begin, end, block):
        before = before_of(begin)
        comments.append(Comment(line_of(begin), text[begin:end], block, not before.strip(), before))
        blank(code, begin, end)
        blank(skeleton, begin, end)

    index = 0
    while index < size:
        char = text[index]
        if text.startswith("//", index):
            end = text.find(NEWLINE, index)
            end = size if end < 0 else end
            take_comment(index, end, False)
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = size if end < 0 else end + 2
            take_comment(index, end, True)
            index = end
        elif char.isdigit() or (char == "." and text[index + 1 : index + 2].isdigit()):
            index = PP_NUMBER.match(text, index).end()
        elif char.isalpha() or char == "_":
            word = IDENTIFIER.match(text, index)
            index = word.end()
            opened = RAW_OPEN.match(text, index) if word.group() in RAW_PREFIXES else None
            if opened:
                delimiter = opened.group(1)
                end = text.find(")" + delimiter + '"', opened.end())
                end = size if end < 0 else end
                literals.append(Literal(line_of(index), True, text[opened.end() : end]))
                blank(code, opened.end(), end)
                blank(skeleton, opened.end(), end)
                index = min(size, end + len(delimiter) + 2)
        elif char in "\"'":
            end = close_quoted(index, char)
            if char == '"':
                literals.append(Literal(line_of(index), False, text[index + 1 : end]))
            blank(skeleton, index + 1, end)
            index = end + 1
        else:
            index += 1
    return Source(text, "".join(code), "".join(skeleton), tuple(comments), tuple(literals), tuple(starts))
