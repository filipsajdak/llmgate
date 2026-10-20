[ "$(cat mathlib.py)" = "$(printf 'def add(a, b):\n    return a + b')" ] && [ "$(ls | wc -l)" -eq 1 ] && transcript_result_contains sub
