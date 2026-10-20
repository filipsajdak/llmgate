turn() {
  compat_claude -p "$1" --resume "$sid" --output-format stream-json --verbose --model sonnet --tools "" > "turn-$2.jsonl" || { echo "turn $2 failed"; exit 1; }
}
compat_claude -p "Remember this code word: quartz. Reply with ok." --output-format stream-json --verbose --model sonnet --tools "" > turn-1.jsonl || exit 1
sid=$(compat_session_id turn-1.jsonl)
[ -n "$sid" ] || { echo "no session id"; exit 1; }
turn "Reply with ok." 2
turn "/compact" 3
grep -q compact_boundary turn-3.jsonl || { echo "no compact_boundary event: the session was not compacted"; exit 1; }
turn "What was the code word? Answer with that one word." 4
compat_result_contains turn-4.jsonl quartz || { echo "the code word was lost after compaction"; exit 1; }
