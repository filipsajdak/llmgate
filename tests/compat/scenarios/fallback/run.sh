out=out.jsonl
compat_claude -p "Reply with exactly one word: pong" --output-format stream-json --verbose --model sonnet --tools "" > "$out" || { echo "claude failed through the fallback"; exit 1; }
sid=$(compat_session_id "$out")
[ -n "$sid" ] || { echo "no session id"; exit 1; }
served=$(compat_session_field "$sid" provider)
fell=$(compat_session_field "$sid" fallback)
[ "$served" = "$COMPAT_SECOND" ] || { echo "answered by '$served', wanted '$COMPAT_SECOND'"; exit 1; }
[ "$fell" = "True" ] || { echo "the reply was not marked as a fallback ($fell)"; exit 1; }
compat_result_contains "$out" pong || { echo "no answer"; exit 1; }
