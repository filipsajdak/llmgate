[ "$(cat mathlib.py)" = "$(printf 'def add(a, b):\n    return a + b')" ] && transcript_has_tool ExitPlanMode
