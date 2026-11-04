# Compiler-backed editor workflows

Start the stdio language server with absolute paths to Python, the developer
tool and the compiler:

```text
python tools/pikaflow.py --compiler build/native/pikaflow.exe lsp
```

Use your editor's standard LSP configuration with language ID `pikaflow` and
`*.pf` files. The server supports incremental UTF-16 document synchronization,
unsaved diagnostics, formatting, declaration/keyword completion, document
symbols, semantic hover, go-to-definition, references, prepare-rename and rename,
full semantic tokens and inferred-local type hints.
No editor extension or network service is required.

## Source bindings and workspace scope

Hover, definitions, references and rename use the compiler's `symbols` JSON
index. This identifies declarations and uses by their resolved source binding,
including locals, captures, namespace aliases, qualified members, functions,
words, constructors, enum types and type parameters. Generic bodies retain
source bindings even when they have not been instantiated. Comments and string
contents are never identifier references.

An initialized workspace uses its declared folders. Without them, the server
uses the nearest package project, Git root, or containing source directory.
Closed `.pf` files are included alongside open buffers. Projects beneath a
common workspace are indexed separately using each nearest `pika.json`, entry
and verified lock; binding IDs and source uses are merged afterward. A shared
library therefore keeps references from more than one project without treating
their dependency locks as one namespace.

Dot directories, `build`, `out`, `node_modules`, symbolic links and directory
junctions are excluded from the workspace walk. The combined index is limited
to 128 sources, including imported dependencies. References, prepare-rename and
rename fail if any project group cannot be indexed; they do not return a partial
workspace edit. Hover and definition may use a successfully checked group while
an unrelated group is broken.

Dependency navigation and diagnostics use the application's entry and lock,
including files beneath `.pika/deps`. Cached files are reached only through the
application import graph, never injected as unconfined extra sources. A cached
package does not need an independent working lock to inspect its definitions.
Editor operations verify dependencies offline and never fetch missing sources.
Use `deps fetch --locked` before opening a fresh project whose cache is absent.

## Rename behavior

Rename constructs candidate buffer text, reindexes the full workspace and checks
that the renamed declaration has exactly the expected source uses afterward.
This rejects duplicate declarations, captured names, shadowing changes and
different symbol resolution, including captures in unused generic bodies.
New names must be ASCII identifiers of at most 128 characters. The `main` entry
point cannot be renamed.

The server returns a `WorkspaceEdit`; it never writes source files. Clients that
advertise `workspace.workspaceEdit.documentChanges` receive versioned document
edits for open buffers and null versions for closed files. Other clients receive
the standard `changes` mapping. The client applies the proposed edit using its
own document/version handling. Identifier slices and edited source text are
checked again while planning, and detected changes reject the operation.

Opening an external dependency does not make it editable. Every edited file must
remain inside the configured workspace, and `.pika` caches remain read-only even
when open or explicitly selected. To refactor an ordinary local-path dependency,
include its source folder in the workspace; refresh affected package locks after
saving. The server returns a diagnostic rather than proposing a partial rename
when an affected file lies outside the editable workspace.

## Positions, overlays and diagnostics

The compiler receives unsaved source through a bounded binary overlay manifest.
Real filenames and imports remain unchanged; no temporary import rewrites shift
columns or weaken package confinement. New unsaved files can import other new
unsaved files. Compiler byte columns are converted to UTF-16 positions, including
non-BMP characters. A disk UTF-8 BOM is excluded from client text; CRLF buffers
retain their original edit coordinates. Compiler overlays normalize line endings
without changing logical line/column identities.

Incremental changes are evaluated sequentially on a temporary string and committed
only when every range succeeds. Invalid ranges, surrogate-pair splits, incorrect
UTF-16 `rangeLength` and stale document versions cannot partially update a buffer.
Diagnostics and their compiler caret snippets use unsaved text rather than stale
disk contents.

The server allows 32 open documents and a 4 MiB UTF-8 buffer/message limit. Overlay
manifests allow 256 absolute-path records, 4 MiB per source, 128 MiB total source
text and 65,536 bytes per filename. Duplicate, relative, truncated and oversized
records are rejected by the compiler. Compiler subprocesses have a 30-second
timeout. JSON is limited to 128 container levels before decoding, independently
of the Python runtime's recursion limit. Oversized/deeply nested JSON fails
through protocol errors rather than an unhandled parser exception. The 0.3.1
protocol regressions also reject UTF-16/32 JSON payloads and report invalid
parameters for nonstring document URIs; a valid request after an invalid URI
still works. These checks bound the advertised protocol flow,
not every possible client implementation.

## Semantic tokens and inferred types

`textDocument/semanticTokens/full` classifies compiler-resolved names as module
namespaces, aliases/types, records, enums/constructors, type parameters,
functions/words/externs, parameters, variables and fields. Declaration tokens
carry a declaration modifier. The response is sorted, deduplicated LSP delta
data using UTF-16 positions. Keywords, comments and string contents are not
semantic-name tokens. Unresolved names, changed source slices and ambiguous
classifications are omitted. Incremental semantic-token delta and semantic-token
range endpoints are not supported.

`textDocument/inlayHint` returns type labels after inferred simple `let`/`var`
names in the requested half-open range. Explicit annotations, parameters,
destructuring/pattern bindings, unit/unknown types and generic placeholders are
skipped. Compiler file qualifiers are removed from displayed nominal type names;
the labels are informational, not insertable annotations. Hints contain no text
edits, and there is no hint-resolution endpoint.

Both features use the same unsaved overlays and project-scoped compiler index
as navigation. They also work when inspecting reachable read-only dependencies.
They do not write source files. If the source changes while indexing, the
request returns no decorations. Limits are 50,000 tokens, 4,096 hints, 300,000
source locations and 160 displayed type characters; exceeding a result-count
limit produces an error rather than an unbounded response. Existing document,
workspace and compiler limits still apply.

## Checks

`tests/test_editor.py` runs real compiler-backed workflows and an actual stdio
LSP subprocess: cross-module and closed-file rename, captures and shadowing,
generic/enum/type-parameter bindings, namespace columns, multi-project reference
merging, UTF-16/BOM/CRLF positions, all-new unsaved imports, candidate capture
rejection, cached dependency inspection, read-only edit rejection, malformed
overlays and capability-aware workspace edits. Fixtures verify that user source
files stay unchanged throughout. Compiler symbol metadata also has dedicated
regressions in `tests/test_semantic_symbols.py`.
`tests/test_editor_features.py` separately checks real record/field/generic and
module classifications, overlays, read-only dependencies, inferred types,
UTF-16/BOM/CRLF positions, ranges, deduplication and decoration budgets.
