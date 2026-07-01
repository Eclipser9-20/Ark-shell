# ark — Phase 1: Core Engine Design

## Vision

ark is a standalone, from-scratch shell: the speed of bash, the interactive features
of fish, loved the way zsh is loved. It is a genuine shell — its own lexer, parser,
executor, and job control — nothing delegated to another shell.

This spec covers **phase 1 only**: the core execution engine. Later phases (not
specced here) layer on top:

- **Phase 2** — fish-style interactive UX: autosuggestions from history, live
  syntax highlighting, smart completion menus.
- **Phase 3** — zsh-style extensibility: prompt/theming system, plugin/config
  framework, richer parameter expansion.

Phase 1's goal: a real, usable interactive shell with full POSIX-ish scripting
(control flow, functions, job control) and a bare-bones line editor. No fish/zsh
flair yet.

## Priorities (in order)

1. **Customizability** — internals structured so later phases (builtins, prompt,
   plugins) can extend without reworking the core. Concretely: builtins live in a
   registry (table), not a hardcoded dispatch chain.
2. **Performance** — `posix_spawn` + `file_actions` over naive `fork`+`exec` where
   possible; avoid unnecessary allocations/copies in the lexer/parser hot path;
   `unique_ptr` only, never `shared_ptr` in hot paths (refcounting overhead).
3. Everything else (feature breadth, POSIX completeness) follows from a solid
   core, not the other way around.

## Language & Ownership

**C++, with `unique_ptr` used narrowly**: AST node ownership, and RAII wrappers
around file descriptors / pipes / child-process handles. This matches the rest of
the from-scratch project lineage (NXRT, Pistin, pullio, Lumen) and gives
exception-safe, leak-safe cleanup at zero runtime cost when used for unique
ownership (no vtables, no atomic refcounts — that's `shared_ptr`'s cost, not
`unique_ptr`'s). Rust was considered but isn't installed and installing new
toolchains is off the table; raw C was considered but the memory-safety
discipline cost (in a program parsing constantly-changing untrusted input, meant
to run daily) isn't worth it when C++ gets the same performance ceiling.

## How it's run

Phase 1 is **not** set as the macOS login shell (`chsh -s`). It's launched
manually from within zsh, like any other program. zsh remains the real login
shell / safety net. Making ark the actual login shell is a decision for well
after phase 1 (and likely phase 2/3) have been used daily without incident —
a login-shell crash has a much higher blast radius (could mean no working shell
on a fresh terminal) than anything in this session's prompt/bar experiments.

## Architecture

```
input line/script
      |
   Lexer          -> tokens (words, operators | < > >> 2>> && || & ; ( ),
      |               keywords if/then/else/fi, while/do/done, for/in, case/esac,
      |               function; marks $VAR / ${...} / $(...) spans, unexpanded)
      |
   Parser          -> AST (recursive descent; Node subtypes: Command, Pipeline,
      |               If, While, For, Case, FunctionDef, Subshell; unique_ptr
      |               ownership of children)
      |
   Expander         -> exec-time only: parameter expansion (${var:-default},
      |               ${#var}, ...), command substitution ($(...) via subshell +
      |               captured stdout), $IFS word-splitting, glob expansion
      |
   Executor         -> tree-walks the AST
      |                - simple command -> builtin registry lookup, or
      |                  posix_spawn (external)
      |                - pipeline (|)   -> pipe() per stage, wired via
      |                  posix_spawn_file_actions dup2/close
      |                - control flow   -> If/While/For/Case evaluated directly
      |                  by the interpreter; truthiness = exit-code-zero
      |                  (standard POSIX convention)
      |                - function call  -> new scope, walks function body AST
      |
   Job Control      -> process-group per pipeline, tcsetpgrp for terminal
      |                handoff, job table (pid, pgid, cmd string,
      |                Running/Stopped/Done) updated from a SIGCHLD-signaled
      |                but main-loop-drained queue (not mutated inside the
      |                signal handler itself)
      |
   Line Editor      -> raw termios (VMIN=1/VTIME=0, ICANON/ECHO off — same
                        pattern as Pistin), arrow-key history recall, basic
                        filesystem-path tab completion. Bare-bones by design;
                        fish-style ghost-text/highlighting is phase 2.
```

## Components

| Module | File(s) | Responsibility |
|---|---|---|
| Lexer | `lexer.h/.cpp` | Raw text -> token stream. Quoting (`'`, `"`, `\`), operators, keyword recognition, marks substitution spans (unexpanded). |
| Parser | `parser.h/.cpp` | Recursive descent, tokens -> `unique_ptr<Node>` AST. |
| Expander | `expand.h/.cpp` | Exec-time parameter/command substitution, word-splitting, globbing. Separate module from parsing since it depends on runtime variable state. |
| Executor | `exec.h/.cpp` | Tree-walks AST. Builtin registry (`string -> function pointer`: `cd`, `export`, `unset`, `alias`, `.`/`source`, `exit`, `read`, `type`, `pwd`, `echo`, `jobs`, `fg`, `bg`, `wait`). `posix_spawn` for externals. |
| Job control | `jobs.h/.cpp` | Job table, async-signal-safe `SIGCHLD` handling, `tcsetpgrp` terminal handoff. |
| Line editor | `edit.h/.cpp` | Raw termios line editing, history recall, path tab-complete. |
| History | `history.h/.cpp` | Append-only log at `~/.config/ark/.history`, loaded on startup. |

## Data Flow

```
raw line/script text
  -> Lexer::tokenize()              -> vector<Token>
  -> Parser::parse()                -> unique_ptr<Node> (AST root)
  -> Executor::run(Node*)
       -> per Command node:
            Expander::expand(words)   -> argv (post substitution/split/glob)
            registry.lookup(argv[0])  -> builtin fn, OR
            posix_spawn(argv, file_actions) -> pid
       -> Pipeline node: chain of Commands, pipe() between stages,
          file_actions dup2 stdin/stdout per stage
       -> control-flow nodes (If/While/For/Case) recurse Executor::run on
          branches; truthiness = exit-code-zero
  -> Job control wraps every pipeline's process group; foreground pipelines
     block via waitpid + tcsetpgrp handoff; background (&) pipelines register
     in the job table and return immediately
  -> precmd: reap finished background jobs (non-blocking waitpid(WNOHANG)
     loop), print job-state-change notices, redraw prompt
```

Note: a pipeline's exit status is its *last* stage's (standard POSIX behavior) —
`Pipeline::run()` returns this explicitly so `if`/`while`/`&&`/`||` truthiness
threads through consistently rather than falling out of loop bookkeeping
incidentally.

## Error Handling

- **Parse errors** — line/column + caret-pointer snippet (gcc/`bash -n` style).
  Non-fatal interactively (re-prompt); fatal with clean nonzero exit for
  `ark script.ark` (non-interactive).
- **Command-not-found** — stderr message, `$?=127`, shell stays alive.
- **Signal safety** — `SIGCHLD` handler only records pid+status into a
  lock-free queue (no malloc/iostream/locks inside the handler); the main loop
  drains it and updates job state safely between commands.
- **Fatal internal errors** — the top-level REPL loop catches unexpected
  exceptions/invariant violations, logs to stderr, and re-prompts rather than
  exiting the process. Since ark may eventually be a login shell, it should
  never silently `abort()`/segfault if avoidable — a crashed login shell means
  no terminal.
- **Job-control edge cases** — orphaned process groups (child outlives ark),
  stopped-jobs-on-exit warning (bash-style "you have stopped jobs"),
  `SIGTTOU`/`SIGTTIN` handled by the shell itself so background jobs writing to
  a closed terminal don't misbehave.

## Testing

A `tests/` directory of small `.ark` scripts paired with expected-output files
(covering pipes, redirects, if/while/for, job control scenarios), run through a
small harness that diffs actual vs. expected output. Matches how Pistin/NXRT
were built and verified — hand-built and exercised end-to-end rather than
unit-tested in isolation — and directly exercises parser+executor+job-control
together rather than testing internals separately. Lexer/parser unit tests can
be added later if a specific bug demonstrates the need.

## Explicitly Out of Scope (Phase 1)

- Fish-style autosuggestions / live syntax highlighting / completion menus
  (phase 2).
- zsh-style prompt/theming system, plugin framework (phase 3).
- Acting as the macOS login shell.
- Any parser-generator tooling or external line-editing library — everything
  here is hand-written, matching the from-scratch ethos.
