first=first.jsonl
second=second.jsonl
compat_claude -p "Reply with exactly one word: pong" --output-format stream-json --verbose --model sonnet --tools "" > "$first" || exit 1
sid=$(compat_session_id "$first")
[ -n "$sid" ] || { echo "no session id in the first run"; exit 1; }
served_first=$(compat_served "$sid")
[ "$served_first" = "$COMPAT_FIRST" ] || { echo "the first reply came from '$served_first', not '$COMPAT_FIRST'"; exit 1; }
"$LLMGATE_BIN" --config "$LLMGATE_CONFIG" use sonnet "$COMPAT_TARGET" --session "$sid" || exit 1
compat_claude -p "Now reply with exactly one word: ping" --resume "$sid" --output-format stream-json --verbose --model sonnet --tools "" > "$second" || exit 1
served_second=$(compat_served "$sid")
"$LLMGATE_BIN" --config "$LLMGATE_CONFIG" use sonnet clear --session "$sid" > /dev/null
[ "$served_second" = "$COMPAT_SECOND" ] || { echo "after the switch the reply came from '$served_second', not '$COMPAT_SECOND'"; exit 1; }
compat_result_contains "$second" ping || { echo "the second turn did not answer"; exit 1; }
