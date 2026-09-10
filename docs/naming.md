# Naming Decision

Status: decided. Date: 2026-09-06. Decided by both design parties.
Foundation (ratified, not reopened): minimal stage-0 core in a conservative
C subset with cross-build orchestration via the Zig build system, per the
locked charter.

## Decision

Name: **Vow**. File extension: **`.vow`**. Binary: `vow`. Package id: `vow`.

## Candidates

- **Vow** (proposed by Party B): 3 letters, single short extension, usable
  as binary and package id.
- **Oath** (proposed by Party A): 4 letters, single short extension, usable
  as binary and package id.

Both candidates passed the agreed criteria (short; searchable; no major
collision; one short extension; usable as lowercase binary and package id).

## Rationale

Both names fit the charter's trust positioning ("the oath/vow the toolchain
keeps: one source tree, one result, on every machine"). Vow was selected on
lighter brand collision: one small legacy promise library in the JS
ecosystem plus the ordinary dictionary sense. Oath carries heavier
proximity: the Oath Keepers organization in general search, and the
oath-toolkit / OATH-OTP package family in systems package namespaces —
a real risk for a tool whose reason to exist is trust.

No merge was applied: both extensions passed, so one clean pair was kept.

## Retired

**Oath + `.oath`** — retired for brand-collision reasons stated above; not
to be reused, to avoid historical confusion.

## Follow-up

Further work proceeds under the phase 2 gates of the locked charter
(CLI-proven cross-build and distribution first, mandatory dynamic-library
ABI proof immediately after).
