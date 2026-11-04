"""Comment-preserving formatter, checked against the compiler's lexer."""
from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile

TOKEN = re.compile(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|[A-Za-z_]\w*|\.\.\.|::|->|\|>|==|!=|<=|>=|\.\.|\S''')


def split_comment(line: str) -> tuple[str, str]:
    quote = ""
    escaped = False
    for i, char in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == "#" or line[i:i + 2] == "//":
            return line[:i], line[i:].rstrip()
    return line, ""


def format_text(source: str) -> str:
    """Keep spellings/comments and logical nesting; normalize horizontal space."""
    lines = source.removeprefix("\ufeff").replace("\r\n", "\n").splitlines()
    output: list[str] = []
    indents = [0]
    brackets = 0
    continuation_indent = 0
    for line in lines:
        if "\t" in line[:len(line) - len(line.lstrip())]:
            raise ValueError("tabs are not allowed for indentation")
        code, comment = split_comment(line)
        tokens = TOKEN.findall(code.strip())
        width = len(line) - len(line.lstrip(" "))
        if tokens and brackets == 0:
            if width > indents[-1]:
                indents.append(width)
            else:
                while width < indents[-1]:
                    indents.pop()
                if width != indents[-1]:
                    raise ValueError("inconsistent indentation")
        depth = len(indents) - 1
        if brackets:
            depth = continuation_indent + (0 if tokens and tokens[0] in (")", "]", "}") else 1)
        elif not tokens and comment:
            depth = sum(indent <= width for indent in indents[1:])
            if width > indents[-1]:
                depth += 1
        formatted = ""
        for i, token in enumerate(tokens):
            previous = tokens[i - 1] if i else ""
            space = bool(i)
            if token in (",", ";", ":", ")", "]", "}", ".", "?", "::"):
                space = False
            if previous in ("(", "[", "{", ".", "::"):
                space = False
            if token == "[" and previous and (previous[-1].isalnum() or previous in (")", "]")):
                space = False
            if token == "(" and previous and (previous[-1].isalnum() or previous in (")", "]")):
                space = previous in ("if", "while", "for", "match", "return", "case")
            formatted += (" " if space else "") + token
        if comment:
            formatted += ("  " if formatted else "") + comment
        output.append(" " * (depth * 4) + formatted if formatted else "")
        if not brackets:
            continuation_indent = len(indents) - 1
        for token in tokens:
            if token in ("(", "[", "{"):
                brackets += 1
            elif token in (")", "]", "}"):
                brackets -= 1
        if brackets < 0:
            raise ValueError("unmatched closing delimiter")
    while output and not output[-1]:
        output.pop()
    return "\n".join(output) + ("\n" if output else "")


def token_signature(compiler: pathlib.Path, path: pathlib.Path) -> list[str]:
    result = subprocess.run([str(compiler), "dump-tokens", str(path)], capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    if result.returncode:
        raise ValueError(result.stderr.strip())
    return [re.sub(r"^\d+:\d+ ", "", line) for line in result.stdout.splitlines()]


def checked_format(compiler: pathlib.Path, source: str) -> str:
    formatted = format_text(source)
    with tempfile.TemporaryDirectory(prefix="pikaflow-format-") as directory:
        before, after = (pathlib.Path(directory) / name for name in ("before.pf", "after.pf"))
        before.write_text(source, encoding="utf-8")
        after.write_text(formatted, encoding="utf-8")
        if token_signature(compiler, before) != token_signature(compiler, after):
            raise ValueError("formatter refused an edit that changes the token stream")
    return formatted
