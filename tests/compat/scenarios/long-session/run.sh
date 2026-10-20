turn() {
  compat_claude -p "$1" --resume "$sid" --output-format stream-json --verbose --model sonnet --tools "" > "turn-$2.jsonl" || { echo "turn $2 failed"; exit 1; }
}
compat_claude -p "Remember this code word: quartz. Reply with ok." --output-format stream-json --verbose --model sonnet --tools "" > turn-1.jsonl || exit 1
sid=$(compat_session_id turn-1.jsonl)
[ -n "$sid" ] || { echo "no session id"; exit 1; }
n=2
while [ "$n" -le 11 ]; do
  turn "Reply with ok." "$n"
  n=$((n + 1))
done
turn "What was the code word? Answer with that one word." 12
compat_result_contains turn-12.jsonl quartz || { echo "the code word was lost after 12 turns"; exit 1; }
