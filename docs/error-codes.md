# Vow Diagnostic Code Catalog

Status: LOCKED. Signed by both design parties. Every diagnostic carries a code; the code
determines the exit-code class but one code maps to exactly one class.
The format on stderr stays `file:line:col: EXXXX message` (stage-0
specification, Section 9). Conformance fixtures assert exit codes; codes
let implementations and users name the exact failure. Required before the
C reference is written.

## E1xxx — usage errors (exit 1)

| Code  | Meaning                  | Raised when                              |
| ----- | ------------------------ | ---------------------------------------- |
| E1000 | Unknown subcommand       | First argument is not `run` or `test`    |
| E1001 | Bad CLI arguments        | Wrong arity or malformed flags           |
| E1002 | Entry file not found     | `vow run` target does not exist          |
| E1003 | Target directory missing | `vow test` directory does not exist      |

## E2xxx — lex and parse errors (exit 2)

| Code  | Meaning                | Spec ref | Fixture                |
| ----- | ---------------------- | -------- | ---------------------- |
| E2000 | Invalid UTF-8 input    | §2       | LEX-02 (bad-utf8)      |
| E2001 | Illegal line ending    | §2       | LEX-02 (lone-cr)       |
| E2002 | Unknown string escape  | §3       | LEX-02 (bad-escape)    |
| E2003 | Integer out of range   | §3       | LEX-02 (big-int)       |
| E2004 | Unexpected token       | §4       | LEX-02 (stray-token)   |
| E2005 | Unterminated construct | §2–4     | LEX-02 (unterminated)  |
| E2006 | Non-literal effect scope | §4       | PAR-04 (computed-arg)  |

## E3xxx — check errors (exit 3)

| Code  | Meaning                         | Spec ref | Fixture               |
| ----- | ------------------------------- | -------- | --------------------- |
| E3000 | Effect call not in clause | §6       | EFF-01, EFF-06 (R10) |
| E3001 | Unknown effect name             | §6       | PAR-04 (bad-effect), EFF-04 |
| E3003 | Import hash mismatch            | §6       | PAR-04 (bad-hash, same-dir par-04i) |
| E3004 | Cyclic import                   | §6       | PAR-04 (cycle pair)   |
| E3005 | Duplicate map key               | §4       | PAR-04 (dup-key)      |
| E3006 | Import escapes source root      | §6       | PAR-04 (escape-root) |
| E3007 | Missing main under `vow run`    | §9       | RUN-03 (nomain)       |
| E3008 | Impure expression, pure context | §4, §6   | PAR-04 (call-in-let) |
| E3009 | Name binding failure (unknown, duplicate, or reserved root) | §6 | PAR-04 (unknown-name, dup-func, reserved-root) |

## E4xxx — test failures (exit 4)

| Code  | Meaning          | Spec ref | Fixture      |
| ----- | ---------------- | -------- | ------------ |
| E4000 | Assertion failed | §8       | RUN-02 (mini fail) |

## E5xxx — runtime failures (exit 5)

| Code  | Meaning                        | Spec ref | Fixture                  |
| ----- | ------------------------------ | -------- | ------------------------ |
| E5000 | Integer overflow               | §5       | EFF-07 (overflow)        |
| E5001 | Division or modulo by zero     | §5       | EFF-08 (divzero)         |
| E5002 | Index out of range             | §5       | EFF-09 (badindex)        |
| E5003 | Missing map key                | §5       | EFF-10 (missingkey)      |
| E5004 | Capability scope violation     | §7       | EFF-02 (read/write outside) |
| E5005 | Undeclared spawn               | §7       | EFF-02 (spawn undeclared) |
| E5006 | Resource limit breach          | §7       | EFF-05 (recursion)       |
| E5007 | Non-UTF-8 process output       | §7       | Implementation-level only (no portable fixture) |
| E5008 | Call of non-function value     | §5       | EFF-12 (nonfunc-call)    |
| E5009 | Dynamic effect violation       | §6       | Evaluator-level only (indirect calls) |
| E5010 | File not found at runtime      | §7       | EFF-13 (missing-file)    |
| E5011 | Main must return an integer    | §9       | RUN-04 (main-nonint)     |
| E5012 | Runtime type error             | §5       | EFF-15 (type-error), EFF-17 (arity) |

## Rules

- One code per failure site; the first failure aborts the program (no
  exceptions at stage-0). When several check errors coincide, detection
  order is unspecified; the exit class is still 3.
- Codes are stable once issued: renaming or reusing a code needs a joint
  decision. New codes append at the end of their class block.
- Recalled codes are never reused. E3002 (non-literal effect argument,
  exit 3) was recalled and reclassified as E2006, because the grammar
  rejects computed effect scopes at parse time.

## Sign-off

- Party A: signed.
- Party B: signed.
