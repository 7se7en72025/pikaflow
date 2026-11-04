# Formatting, editor integration, and packages

The developer tools use Python **3.10 or newer**, with no third-party Python
dependencies. The compiler itself remains C++17. Run from the repository root:

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe fmt examples/hello.pf --check
python tools/pikaflow.py --compiler build/native/pikaflow.exe fmt examples/hello.pf
python tools/pikaflow.py --compiler build/native/pikaflow.exe lsp
```

The compiler path is optional when a repository build, installed compiler next to
the installed tools, or `pikaflow` on PATH can be found. Installed Python tools
live in `share/pikaflow/tools/` beneath the installation prefix.

## Formatter

The formatter normalizes indentation to four spaces, operator/argument spacing,
trailing whitespace, line endings and the final newline. It preserves literal
spellings and both comment forms. It does not reflow expressions or comments.

Before writing, it lexes the original and proposed text through the **actual
compiler** and compares every token, including indentation/newline structure.
An invalid source or changed token stream is refused. `--check` returns 1 when
files need formatting, 0 when already formatted. Explicit files are required;
the command does not recursively rewrite a workspace. Replacement is atomic per
file, preserves file mode, and refuses symbolic links. Tests cover example token
equivalence, idempotence, strings/comments/exponents, invalid input and check mode.

## Language server

The stdio server implements the advertised subset of
[LSP 3.17](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/):

- Initialize/shutdown/exit and full-document open/change/save/close synchronization.
- Compiler-backed diagnostics for unsaved documents and their unsaved relative
  imports, without writing over the user's sources.
- Document formatting, document symbols and keyword/document-declaration completion.
- UTF-8 message framing and UTF-16 editor positions, including non-BMP characters.

It advertises only these capabilities. Completion and document symbols describe
declarations in the current document; they are not a whole-program semantic index.
Rename, cross-module semantic navigation, incremental edits, code actions and
remote documents are not advertised. These optional protocol features are not
required to use the delivered server.

Configure an LSP-capable editor with this command array (use absolute paths):

```json
{
  "command": [
    "python",
    "C:/Users/RAIYYAN/pikaflow/tools/pikaflow.py",
    "--compiler", "C:/Users/RAIYYAN/pikaflow/build/native/pikaflow.exe",
    "lsp"
  ],
  "filetypes": ["pikaflow"],
  "rootPatterns": ["pika.json", ".git"]
}
```

Client-specific configuration keys differ; the executable/arguments are the
portable part. JSON-RPC goes exclusively to stdout; diagnostics/log notifications
use the protocol. Limits: 4 MiB message/source, 32 open documents, 128 modules per
diagnostic graph, and a 30-second compiler timeout. Compilation is synchronous;
new changes are processed in order. Stale document versions are ignored.

`pikaflow check FILE --library` validates a module without requiring a source
`main`; the editor uses this mode. It does not emit a library or run an entry.

## Project and dependency workflow

```powershell
python tools/pikaflow.py init my_project --name my_project
python tools/pikaflow.py deps lock --project my_project
python tools/pikaflow.py deps check --project my_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe interpret --project my_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe build --project my_project -O2 -o build/my_project.exe
```

`init` creates `pika.json`, `pika.lock` and `src/main.pf`, refusing replacement of
existing project files. A manifest uses this format:

```json
{
  "name": "my_project",
  "version": "0.1.0",
  "entry": "src/main.pf",
  "dependencies": {
    "mathkit": {"path": "../mathkit"}
  }
}
```

Package names/aliases are ASCII identifiers. The delivered manager supports
local-path dependencies, including transitive dependencies, and deterministic
SHA-256 locks. It rejects cycles, conflicting aliases, symlinked package trees,
oversized source files and deep dependency graphs. No downloads, lifecycle
scripts, registry resolution or version solver run implicitly.

`pika.lock` is a versioned tab-separated file containing alias, dependency path
relative to the project, and a digest of the manifest plus `.pf` source contents.
Line endings are canonicalized to LF before hashing, so LF and CRLF Git checkouts
produce the same lock; all other content bytes remain significant.
`build/`, `out/` and dot directories are excluded. Commit the manifest and lock.
Build/output directory exclusions and source-extension hashing compare ASCII
case-insensitively, so Windows filename lookup cannot expose an unhashed source.
Dependency edits require an explicit `deps lock`; project check/run/build commands
verify the lock before invoking the compiler.

```text
import "pkg:mathkit"          # mathkit/lib.pf
import "pkg:mathkit/sequence" # mathkit/sequence.pf
```

The compiler resolves aliases from the entry project's nearest `pika.json` and
`pika.lock`. Package module paths cannot escape the package through `..`, absolute
paths or symlinks. Calling the compiler directly resolves the lock but does not
recompute dependency digests; use the project tool for digest verification.
Dependency aliases occupy one project-wide namespace, so conflicts diagnose.

The same boundary applies to every relative import reached from a package,
including modules previously imported by an ordinary file path. Package imports
must name `.pf` files and cannot enter excluded `build/`, `out/`, or dot paths;
those files have no content digest. A `pkg:` import into another locked dependency
switches to that dependency's boundary, while bundled `std:` imports stay trusted.
The language server enforces these boundaries before staging unsaved buffers.

Project creation rejects existing and dangling reserved-file symlinks and a
symlinked or Windows-junction `src` directory. Lock updates reject a symlinked `pika.lock` and replace
regular locks atomically, preserving the old lock if writing fails.

The checked-in [two-package example](../examples/packages/app/pika.json) runs with:

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe interpret --project examples/packages/app
```

It prints `42`. Package sources remain readable `.pf` files using the same module
privacy/export rules as ordinary relative imports.
