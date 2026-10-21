compat_claude -p "How many different ways can you make 87 cents using pennies, nickels, dimes and quarters? Work it out carefully, then answer with the number only." --effort high --output-format stream-json --verbose --model sonnet --tools "" > first.jsonl || exit 1
sid=$(compat_session_id first.jsonl)
[ -n "$sid" ] || { echo "no session id"; exit 1; }
served_first=$(compat_served "$sid")
[ "$served_first" = "$COMPAT_FIRST" ] || { echo "the first reply came from '$served_first', not '$COMPAT_FIRST'"; exit 1; }
"$LLMGATE_BIN" --config "$LLMGATE_CONFIG" use sonnet "$COMPAT_TARGET" --session "$sid" || exit 1
compat_claude -p "Now reply with exactly one word: done" --resume "$sid" --effort high --output-format stream-json --verbose --model sonnet --tools "" > second.jsonl || { echo "the second provider did not accept the history"; exit 1; }
served_second=$(compat_served "$sid")
stripped=$(compat_stat thinking_blocks_stripped)
"$LLMGATE_BIN" --config "$LLMGATE_CONFIG" use sonnet clear --session "$sid" > /dev/null
[ "$served_second" = "$COMPAT_SECOND" ] || { echo "after the switch the reply came from '$served_second', not '$COMPAT_SECOND'"; exit 1; }
[ "$stripped" -ge 1 ] || { echo "no thinking block was stripped at the switch (the first turn may have produced none)"; exit 1; }
compat_result_contains second.jsonl done || { echo "the second turn did not answer"; exit 1; }
