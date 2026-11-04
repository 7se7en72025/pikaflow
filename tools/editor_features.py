"""Read-only semantic highlighting and inferred local types from compiler bindings."""
from __future__ import annotations

import pathlib
import re

from editor import EditorError, line_bounds, offset, units

TOKEN_TYPES = ["namespace", "type", "struct", "enum", "enumMember", "typeParameter",
               "function", "parameter", "variable", "property"]
TOKEN_MODIFIERS = ["declaration"]
MAX_TOKENS = 50000
MAX_HINTS = 4096
MAX_LOCATIONS = 300000
KINDS = {"module": "namespace", "typeAlias": "type", "record": "struct", "enum": "enum",
         "constructor": "enumMember", "typeParameter": "typeParameter", "function": "function",
         "word": "function", "extern": "function", "parameter": "parameter",
         "variable": "variable", "field": "property"}


def _occurrences(index, path, source, *, definitions_only=False):
    """Convert sorted compiler byte columns with one forward scan per requested line."""
    bounds = line_bounds(source)
    locations = []
    for symbol in index["symbols"]:
        name = symbol.get("name")
        if not isinstance(name, str) or not name.isidentifier() or len(name) > 128:
            continue
        places = [(symbol.get("definition"), True)]
        if not definitions_only:
            places.extend((where, False) for where in symbol.get("uses", []))
        for where, declaration in places:
            if not isinstance(where, dict) or not isinstance(where.get("file"), str):
                continue
            if pathlib.Path(where["file"]) != path:
                continue
            line, column = where.get("line"), where.get("column")
            if type(line) is not int or type(column) is not int or not 1 <= line <= len(bounds) or column < 1:
                continue
            locations.append((line - 1, column - 1, symbol, declaration))
            if len(locations) > MAX_LOCATIONS:
                raise EditorError("semantic decoration exceeds 300,000 source locations")
    locations.sort(key=lambda item: (item[0], item[1], str(item[2].get("id", "")), not item[3]))
    previous_line, cursor, byte_column, character = -1, 0, 0, 0
    for line, column, symbol, declaration in locations:
        start, end = bounds[line]
        if line != previous_line:
            cursor = start
            byte_column = character = 0
            if line == 0 and source.startswith("\ufeff"):
                cursor += 1  # The compiler ignores the BOM; an open client buffer may retain it.
                character = 1
            previous_line = line
        while byte_column < column and cursor < end:
            code = ord(source[cursor])
            byte_column += 1 if code < 128 else 2 if code < 2048 else 3 if code < 65536 else 4
            character += 2 if code > 65535 else 1
            cursor += 1
        name = symbol["name"]
        finish = cursor + len(name)
        if byte_column != column or finish > end or source[cursor:finish] != name:
            continue
        if ((cursor > start and (source[cursor-1].isalnum() or source[cursor-1] == "_")) or
                (finish < end and (source[finish].isalnum() or source[finish] == "_"))):
            continue
        yield symbol, declaration, line, character, units(name), cursor, finish


def semantic_tokens(server, path):
    path = pathlib.Path(path).resolve()
    source = server.source(path)
    index = server.editor.index(path, strict=False)
    if server.source(path) != source:
        return {"data": []}
    tokens = {}
    for symbol, declaration, line, character, length, _, _ in _occurrences(index, path, source):
        kind = KINDS.get(symbol.get("kind"))
        if kind is None:
            continue
        key = (line, character, length)
        token = (TOKEN_TYPES.index(kind), int(declaration))
        if key in tokens:
            previous = tokens[key]
            tokens[key] = (token[0], previous[1] | token[1]) if previous and previous[0] == token[0] else None
        else:
            tokens[key] = token
            if len(tokens) > MAX_TOKENS:
                raise EditorError("semantic token limit (50,000) exceeded")
    data, previous_line, previous_character = [], 0, 0
    for (line, character, length), token in sorted(tokens.items()):
        if token is None:
            continue
        data.extend((line - previous_line, character - previous_character if line == previous_line else character,
                     length, token[0], token[1]))
        previous_line, previous_character = line, character
    return {"data": data}


def _range(source, params):
    if not isinstance(params, dict) or not isinstance(params.get("range"), dict):
        raise ValueError("inlay hints require a document range")
    value = params["range"]
    positions = []
    for key in ("start", "end"):
        position = value.get(key)
        if not isinstance(position, dict) or "line" not in position or "character" not in position:
            raise ValueError("inlay hint range requires start/end positions")
        positions.append(offset(source, position))
    if positions[1] < positions[0]:
        raise ValueError("inlay hint range ends before its start")
    return positions


def _type_label(symbol, files, parameters):
    value = symbol.get("type")
    if (not isinstance(value, str) or not value or value == "unit" or symbol.get("generic") or
            len(value) > 8192 or symbol.get("detail") == "generic declaration" or
            any(marker in value for marker in ("?", "<unknown>", "<unresolved>", "<generic>"))):
        return None
    for file in files:
        value = value.replace(file + "::", "")
    if len(value) > 160 or parameters.intersection(re.findall(r"[A-Za-z_]\w*", value)):
        return None
    return value


def inlay_hints(server, path, params):
    path = pathlib.Path(path).resolve()
    source = server.source(path)
    start, end = _range(source, params)
    if start == end:
        return []
    index = server.editor.index(path, strict=False)
    if server.source(path) != source:
        return []
    bounds, hints = line_bounds(source), {}
    files = sorted(index.get("sources", []), key=len, reverse=True)
    parameters = {item["name"] for item in index["symbols"] if item.get("kind") == "typeParameter"}
    for symbol, _, line, character, length, begin, finish in _occurrences(index, path, source, definitions_only=True):
        if symbol.get("kind") != "variable" or not start <= finish < end:
            continue
        line_start, line_end = bounds[line]
        keyword_end = begin
        while keyword_end > line_start and source[keyword_end-1] in " \t":
            keyword_end -= 1
        keyword_start = keyword_end - 3
        if keyword_end == begin or keyword_start < line_start or source[keyword_start:keyword_end] not in ("let", "var"):
            continue
        before = keyword_start
        while before > line_start and source[before-1] in " \t":
            before -= 1
        if before > line_start and source[before-1] not in (";", "\ufeff"):
            continue
        following = finish
        while following < line_end and source[following] in " \t":
            following += 1
        if following < line_end and source[following] == ":":
            continue
        label = _type_label(symbol, files, parameters)
        if label is None:
            continue
        key = (line, character + length)
        hint = {"position": {"line": line, "character": character + length},
                "label": ": " + label, "kind": 1, "paddingLeft": False, "paddingRight": False}
        hints[key] = hint if key not in hints or hints[key] == hint else None
        if len(hints) > MAX_HINTS:
            raise EditorError("inlay hint limit (4,096) exceeded")
    return [hint for _, hint in sorted(hints.items()) if hint is not None]
