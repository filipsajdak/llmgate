import json
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import copilot_nudge_hook as hook

REPLY = "gh api repos/filipsajdak/llmgate/pulls/24/comments/4508660777/replies -f body='Fixed in abc1234.'"


class Nudge(unittest.TestCase):
    def test_capability_a_plain_reply_to_a_copilot_thread_is_denied(self):
        self.assertTrue(hook.needs_nudge(REPLY, lambda owner, repo, comment: "Copilot"))
        self.assertTrue(hook.needs_nudge(REPLY, lambda owner, repo, comment: "copilot-pull-request-reviewer"))

    def test_capability_a_reply_whose_body_opens_with_the_mention_is_allowed(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        for command in (REPLY.replace("Fixed", "@copilot please recheck: fixed"), REPLY.replace("Fixed", "@Copilot please recheck: fixed"),
                        f"{base} -f 'body=@copilot please recheck'", f'{base} --raw-field body="@copilot please recheck"',
                        f"{base} -F body=\"  @COPILOT recheck\""):
            self.assertFalse(hook.needs_nudge(command, ask), command)

    def test_limit_a_mention_that_is_not_the_first_word_of_the_body_does_not_count(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        for command in (REPLY.replace("Fixed", "Fixed, @copilot please recheck"), f"{base} -f title=@copilot -f body=fixed",
                        f"{base} -f body='@copilot123 recheck'", f"{base} -f body='@copilot-bot recheck'", f"{base} -F body=@reply.md",
                        f"{base} --input reply.json", f"echo @copilot; {base} -f body=fixed"):
            self.assertTrue(hook.needs_nudge(command, ask), command)

    def test_limit_a_reply_to_a_person_or_an_unknown_author_is_allowed(self):
        self.assertFalse(hook.needs_nudge(REPLY, lambda owner, repo, comment: "octocat"))
        self.assertFalse(hook.needs_nudge(REPLY, lambda owner, repo, comment: ""))

    def test_limit_every_reply_in_a_compound_command_is_checked_not_only_the_first(self):
        ask = lambda owner, repo, comment: "Copilot" if comment == "2" else "octocat"
        base = "gh api repos/o/r/pulls/1/comments"
        good = f"{base}/2/replies -f body='@copilot please recheck'"
        bad = f"{base}/2/replies -f body='Fixed.'"

        self.assertTrue(hook.needs_nudge(f"{good} && {bad}", ask))
        self.assertTrue(hook.needs_nudge(f"{good}\n{base}/2/replies -f body='Fixed.'", ask))
        self.assertFalse(hook.needs_nudge(f"{good} && {good}", ask))
        self.assertFalse(hook.needs_nudge(f"{good} && {base}/3/replies -f body='Fixed.'", ask))

    def test_limit_another_commands_body_or_a_line_continuation_cannot_hide_a_reply(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"
        issue = "gh api repos/o/r/issues/1/comments -f body='@copilot investigate'"

        self.assertTrue(hook.needs_nudge(f"{issue} && {base} -f body='Fixed.'", ask))
        self.assertTrue(hook.needs_nudge(f"{base} -f body='Fixed.' && {issue}", ask))
        self.assertTrue(hook.needs_nudge(f"{base} && {issue}", ask))
        self.assertTrue(hook.needs_nudge("gh api \\\n  repos/o/r/pulls/1/comments/2/replies \\\n  -f body='Fixed.'", ask))
        self.assertFalse(hook.needs_nudge("gh api \\\n  repos/o/r/pulls/1/comments/2/replies \\\n  -f body='@copilot please recheck'", ask))
        self.assertFalse(hook.needs_nudge(f"{issue}; {base} -f body='@copilot please recheck'", ask))

    def test_limit_grouped_shell_operators_still_separate_the_commands(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"
        good = f"{base} -f body='@copilot please recheck'"
        bad = f"{base} -f body='Fixed.'"

        self.assertTrue(hook.needs_nudge(f"({good})&&{bad}", ask))
        self.assertTrue(hook.needs_nudge(f"({good});{bad}", ask))
        self.assertTrue(hook.needs_nudge(f"{good}||{bad}", ask))
        self.assertFalse(hook.needs_nudge(f"({good})&&({good})", ask))
        self.assertFalse(hook.needs_nudge(f"{good} 2>&1", ask))
        self.assertFalse(hook.needs_nudge(f"{good} > out.txt", ask))

    def test_limit_a_descriptor_redirection_does_not_split_the_command_it_belongs_to(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        self.assertFalse(hook.needs_nudge(f"{base} 2>&1 -f body='@copilot please recheck'", ask))
        self.assertFalse(hook.needs_nudge(f"{base} -f body='@copilot please recheck' >&2", ask))
        self.assertFalse(hook.needs_nudge(f"{base} -f body='@copilot please recheck' &> out.txt", ask))
        self.assertTrue(hook.needs_nudge(f"{base} 2>&1 -f body='Fixed.'", ask))
        self.assertTrue(hook.needs_nudge(f"{base} -f body='@copilot please recheck' & {base} -f body='Fixed.'", ask))

    def test_limit_a_prefix_before_gh_does_not_hide_the_reply(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        for prefix in ("GH_PROMPT_DISABLED=1 ", "GH_HOST=github.com GH_TOKEN=x ", "env GH_PAGER= ", "command ", "time ", "sudo -u me "):
            self.assertTrue(hook.needs_nudge(f"{prefix}{base} -f body='Fixed.'", ask), prefix)
            self.assertFalse(hook.needs_nudge(f"{prefix}{base} -f body='@copilot please recheck'", ask), prefix)

    def test_limit_a_typed_field_that_reads_a_file_does_not_count_as_the_literal_body(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        for command in (f"{base} -F body=@copilot.md", f"{base} --field body=@copilot.md", f"{base} --field=body=@copilot.md", f"{base} -F body=@-"):
            self.assertTrue(hook.needs_nudge(command, ask), command)
        for command in (f"{base} -f body=@copilot.md", f"{base} --raw-field body=@copilot.md", f"{base} --raw-field=body=@copilot.md"):
            self.assertFalse(hook.needs_nudge(command, ask), command)

    def test_limit_a_comment_with_an_apostrophe_does_not_hide_the_next_line(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        self.assertTrue(hook.needs_nudge(f"{base} -f body='@copilot please recheck' # don't omit\n{base} -f body='Fixed.'", ask))
        self.assertFalse(hook.needs_nudge(f"{base} -f body='@copilot please recheck' # don't omit\n{base} -f body='@copilot again'", ask))
        self.assertFalse(hook.needs_nudge(f"{base} -f body='@copilot please recheck #1'", ask))

    def test_limit_a_command_that_only_prints_the_text_of_a_reply_is_not_a_reply(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies -f body='Fixed.'"

        for command in (f"echo {base}", f"printf '%s' {base}", f"cat <<< {base}", f"grep -r {base} ."):
            self.assertFalse(hook.needs_nudge(command, ask), command)
        self.assertTrue(hook.needs_nudge(f"FOO=1 env -i {base}", ask))

    def test_limit_a_hash_inside_a_word_is_not_a_comment_and_a_field_value_is_not_an_endpoint(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"
        url = "https://api.github.com/repos/o/r/pulls/1/comments/2/replies"

        self.assertTrue(hook.needs_nudge(f"printf '%s' issue#1; {base} -f body='Fixed.'", ask))
        self.assertFalse(hook.needs_nudge(f"gh api repos/o/r/issues/1/comments -f body='{url}'", ask))
        self.assertFalse(hook.needs_nudge(f"gh api repos/o/r/issues/1/comments -f url={url}", ask))
        self.assertTrue(hook.needs_nudge(f"gh api {url} -f body='Fixed.'", ask))

    def test_limit_a_comment_ending_in_a_backslash_does_not_continue_onto_the_next_line(self):
        ask = lambda owner, repo, comment: "Copilot"
        base = "gh api repos/o/r/pulls/1/comments/2/replies"

        self.assertTrue(hook.needs_nudge(f"printf ready # note \\\n{base} -f body='Fixed.'", ask))
        self.assertFalse(hook.needs_nudge(f"printf ready # note \\\n{base} -f body='@copilot please recheck'", ask))
        self.assertTrue(hook.needs_nudge(f"{base} \\\r\n -f body='Fixed.'", ask))

    def test_limit_an_unparseable_command_is_allowed_rather_than_guessed_at(self):
        self.assertFalse(hook.needs_nudge("gh api repos/o/r/pulls/1/comments/2/replies -f body='unterminated", lambda owner, repo, comment: "Copilot"))

    def test_limit_a_command_that_is_not_a_thread_reply_is_never_looked_up(self):
        def forbidden(owner, repo, comment):
            raise AssertionError("looked up an author for a command that is not a reply")

        self.assertFalse(hook.needs_nudge("gh pr view 24", forbidden))
        self.assertFalse(hook.needs_nudge("gh api repos/o/r/pulls/24/comments", forbidden))
        self.assertFalse(hook.needs_nudge("gh api repos/o/r/issues/24/comments -f body=x", forbidden))

    def test_capability_the_target_is_read_from_the_command(self):
        self.assertEqual(hook.reply_target(REPLY), {"owner": "filipsajdak", "repo": "llmgate", "pull": "24", "comment": "4508660777"})
        self.assertEqual([r["comment"] for r in hook.replies(f"{REPLY} && {REPLY.replace('4508660777', '7')}")], ["4508660777", "7"])
        self.assertIsNone(hook.reply_target("ls"))

    def test_limit_the_hook_process_allows_when_the_command_is_not_a_reply_and_survives_bad_input(self):
        for payload in ('{"tool_input":{"command":"ls -l"}}', "not json", "{}"):
            done = subprocess.run([sys.executable, str(TOOLS / "copilot_nudge_hook.py")], input=payload, capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 0)
            self.assertEqual(done.stdout, "")

    def test_capability_the_denial_names_the_fix_and_the_skill(self):
        self.assertIn("@copilot please recheck", hook.REASON)
        self.assertIn("first word", hook.REASON)
        self.assertIn("copilot-collaboration", hook.REASON)
        json.dumps({"permissionDecisionReason": hook.REASON})


if __name__ == "__main__":
    unittest.main()
