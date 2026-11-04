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
spellings and both comment forms, including compound operators such as `+=`
as single tokens. It does not reflow expressions or comments.

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

- Initialize/shutdown/exit, incremental/full document synchronization and save/close notifications.
- Compiler-backed diagnostics for unsaved documents and their unsaved relative
  imports, without writing over the user's sources.
- Compiler-backed hover, definitions, references and verified workspace rename.
- Document formatting, document symbols and keyword/document-declaration completion.
- UTF-8 message framing and UTF-16 editor positions, including non-BMP characters.

Completion and document symbols describe declarations in the current document;
navigation and rename use the compiler's semantic source index across bounded
project contexts. Rename verifies candidate compilation and binding identities
before returning edits. Code actions and remote documents are not advertised.
See [editor.md](editor.md) for the exact protocol, scope and position contracts.

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
Message payloads require UTF-8; nonstring document URIs produce an invalid-params
response. JSON nesting is checked before decoding.

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

Package names/aliases are ASCII identifiers. Dependencies may be local paths or
Git sources pinned to exact commits, including transitive dependencies. The
manager rejects cycles, conflicting aliases, symlinked package trees, oversized
sources and dependency graphs deeper than 64. There are no lifecycle scripts,
registry services, branch/tag tracking or implicit version selection.

`pika.lock` is a versioned tab-separated file containing alias, dependency path
relative to the project, and a digest of the manifest plus `.pf` source contents.
Line endings are canonicalized to LF before hashing, so LF and CRLF Git checkouts
produce the same lock; all other content bytes remain significant.
`build/`, `out/` and dot directories are excluded. Commit the manifest and lock.
Build/output directory exclusions and source-extension hashing compare ASCII
case-insensitively, so Windows filename lookup cannot expose an unhashed source.
Dependency edits require an explicit `deps lock`; project check/run/build commands
verify the lock offline before invoking the compiler. `--locked` makes that
existing project-command behavior explicit; it never enables a download.

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

## Exact-commit Git dependencies

Use one complete commit ID from `git rev-parse HEAD`, not a branch, tag, shortened
hash or version range. A dependency specification has exactly `git` and `rev`:

```json
{
  "dependencies": {
    "mathkit": {
      "git": "https://github.com/your-org/mathkit.git",
      "rev": "0123456789abcdef0123456789abcdef01234567"
    }
  }
}
```

Replace that example URL and commit with the actual package repository and full
commit. SHA-1 commits have 40 hexadecimal digits; SHA-256 Git repositories use
64. The repository root must contain `pika.json`; `pkg:mathkit` imports its
`lib.pf`, and module suffixes use the normal package import rules.

```powershell
# Author an intentional dependency update. This may fetch missing pinned sources.
python tools/pikaflow.py deps lock --project my_project
git -C my_project add pika.json pika.lock

# On a fresh project clone, restore only sources already recorded by that lock.
python tools/pikaflow.py deps fetch --locked --project my_project

# These commands do not contact remotes or invoke Git, and work without Git installed.
python tools/pikaflow.py deps check --locked --offline --project my_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe interpret --locked --project my_project
```

`deps fetch` always preserves the existing lock, even when `--locked` is omitted.
Changed or newly added origins/revisions are rejected before fetching them;
use `deps lock` for a reviewed manifest update. `deps lock --offline` can update
path dependencies or already-cached Git pins without network access.
`deps lock --locked` only checks the existing lock offline. Missing caches fail
with a restore command instead of triggering a build-time download.

Git must be available in an absolute `PATH` directory outside the project for fetching. Only HTTPS
URLs without embedded credentials/query strings/fragments and explicit local
repository paths are accepted. SSH, `file:` URLs, custom remote helpers, network
share paths and drive-relative paths are rejected. A local fixture declaration
can use `{"git": "../mathkit", "rev": "<actual full commit>"}`. Local origins
are resolved relative to the declaring source repository; local-path dependencies
inside a Git repository must remain within that pinned repository. Remote HTTPS
packages may declare other HTTPS Git dependencies or internal monorepo path
dependencies. Dependency aliases still form one conflict-checked project-wide
namespace.

Fetches run unattended with global/system Git configuration, credential helpers,
templates, hooks, lazy fetching and user-provided `GIT_*` settings disabled.
There are no credential prompts; for a private repository, clone it yourself
using your preferred authentication and declare that local Git repository.
Git's [upload-pack security contract](https://git-scm.com/docs/git-upload-pack#_security)
supports serving local repositories without executing their repository hooks.

Sources live under project-local `.pika/deps/<origin-and-revision-hash>/`; ignore
`.pika/` in version control. The compiler's three-column lock format stays the
same: alias, cached relative directory, SHA-256 source digest. The manager fetches
into a temporary bare repository, reads raw blobs, and materializes ordinary
files. It never performs checkout, runs filters, exports attributes, initializes
submodules or installs scripts. In particular, Git's
[archive export attributes](https://git-scm.com/docs/git-archive#_attributes)
cannot omit or rewrite dependency source files.

The reserved `.pika-source.json` cache record identifies the origin and revision,
preserves the raw commit and records file modes/object IDs. Offline verification
hashes every blob and reconstructs Git trees against the pinned commit, then
checks the normal SHA-256 dependency lock. Editing a cache and rewriting its own
metadata cannot certify different source as the original commit. Cache symlinks,
junctions, Git links, case-colliding names and unsafe portable paths are rejected.
Each Git package is limited to 10,000 files, 16 MiB per file, 128 MiB materialized
content and 64 path levels. Each Git subprocess has a 120-second timeout.

Corrupt caches fail without being overwritten. Review and remove only the named
cache directory, then run `deps fetch --locked` to restore the pinned source.
Project-tool build outputs cannot target `.pika/`. A source checkout of the
project plus its committed manifest/lock is sufficient to restore dependencies;
offline builds additionally need the corresponding verified cache directories.
`tests/test_packages.py` exercises these workflows with real local Git fixtures,
including a fresh cache, transitive sources, both object formats and tampering;
the tests never contact the internet.
