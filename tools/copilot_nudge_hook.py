#!/usr/bin/env python3
import json
import re
import shlex
import subprocess
import sys

ENDPOINT = re.compile(r"(?:^|/)repos/(?P<owner>[\w.-]+)/(?P<repo>[\w.-]+)/pulls/(?P<pull>\d+)/comments/(?P<comment>\d+)/replies$")
LITERAL_FLAGS = ("-f", "--raw-field")
TYPED_FLAGS = ("-F", "--field")
PUNCTUATION = frozenset("();&|<>")
REDIRECTION = frozenset("<>&")
DIRECTION = frozenset("<>")
OPENS_WITH_MENTION = re.compile(r"\s*@copilot(?![\w-])", re.IGNORECASE)
COPILOT_AUTHOR = re.compile(r"^copilot", re.IGNORECASE)
FIELD_VALUE = re.compile(r"^[\w.\[\]-]+=")
WRAPPERS = frozenset({"env", "command", "time", "sudo", "exec", "nohup", "builtin"})
LOOKUP_SECONDS = 10

REASON = (
    "This reply goes into a review thread that Copilot started, so it must address Copilot and ask it to recheck: "
    "write the body inline as -f body='@copilot please recheck ...' (the mention is the first word) and name the fixing commit "
    "(skill: copilot-collaboration)."
)


def lines_of(command):
    lines, current, quote, escaped, comment = [], [], "", False, False
    for char in command:
        if comment:
            if char != "\n":
                continue
            comment = False
        if escaped and char == "\r":
            continue
        if escaped and char == "\n":
            current[-1] = " "
            escaped = False
            continue
        if escaped:
            escaped = False
        elif char == "\\" and quote != "'":
            escaped = True
        elif quote:
            quote = "" if char == quote else quote
        elif char == "#" and (not current or current[-1].isspace()):
            comment = True
        elif char in "'\"":
            quote = char
        elif char == "\n":
            lines.append("".join(current))
            current = []
            continue
        current.append(char)
    lines.append("".join(current))
    return lines


def is_separator(token):
    characters = set(token)
    redirection = characters <= REDIRECTION and bool(characters & DIRECTION)
    return bool(characters) and characters <= PUNCTUATION and not redirection


def commands_of(command):
    return [words for line in lines_of(command) for words in words_of(line)]


def words_of(command):
    lexer = shlex.shlex(command, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    current, found = [], []
    for token in lexer:
        if is_separator(token):
            found.append(current)
            current = []
        else:
            current.append(token)
    found.append(current)
    return [words for words in found if words]


def field_value(word, following):
    for flag in LITERAL_FLAGS + TYPED_FLAGS:
        if word == flag:
            return flag, following
        if word.startswith(flag + "=") and flag.startswith("--"):
            return flag, word[len(flag) + 1:]
    return None, None


def body_of(words):
    for at, word in enumerate(words):
        flag, value = field_value(word, words[at + 1] if at + 1 < len(words) else None)
        if value is None or not value.startswith("body="):
            continue
        text = value[len("body="):]
        return "" if flag in TYPED_FLAGS and text.startswith("@") else text
    return ""


def is_prefix(word, before):
    return word in WRAPPERS or "=" in word.split("/")[-1] or word.startswith("-") or before.startswith("-")


def runs_gh_api(words):
    for at in range(len(words) - 1):
        if words[at] == "gh" and words[at + 1] == "api":
            return all(is_prefix(word, words[index - 1] if index else "") for index, word in enumerate(words[:at]))
    return False


def replies(command):
    try:
        parsed = commands_of(command)
    except ValueError:
        return []
    found = []
    for words in parsed:
        if not runs_gh_api(words):
            continue
        for at, word in enumerate(words):
            if FIELD_VALUE.match(word) or (at and words[at - 1] in LITERAL_FLAGS + TYPED_FLAGS):
                continue
            target = ENDPOINT.search(word)
            if target:
                found.append({**target.groupdict(), "body": body_of(words)})
    return found


def reply_target(command):
    found = replies(command)
    return {key: found[0][key] for key in ("owner", "repo", "pull", "comment")} if found else None


def needs_nudge(command, author_of):
    for reply in replies(command):
        if OPENS_WITH_MENTION.match(reply["body"]):
            continue
        author = author_of(reply["owner"], reply["repo"], reply["comment"])
        if author and COPILOT_AUTHOR.match(author) is not None:
            return True
    return False


def github_author(owner, repo, comment):
    try:
        done = subprocess.run(["gh", "api", f"repos/{owner}/{repo}/pulls/comments/{comment}", "--jq", ".user.login"],
                              capture_output=True, text=True, timeout=LOOKUP_SECONDS, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def main():
    try:
        payload = json.load(sys.stdin)
        command = (payload.get("tool_input") or payload.get("toolInput") or {}).get("command") or ""
        if needs_nudge(command, github_author):
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny", "permissionDecisionReason": REASON}}))
    except Exception:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
