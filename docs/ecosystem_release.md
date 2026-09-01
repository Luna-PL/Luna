# Ecosystem release snapshot

`ecosystem.lock.json` is the authoritative snapshot connecting the otherwise independent
Luna, LunaToolchain, and Lunax repositories. The checked-in snapshot is intentionally the
immutable 0.2.1 baseline; it is evidence for that release line, not a declaration that the
current 0.3 compiler ecosystem is ready. The Luna component is the commit containing the lock
file; child components use exact Git commits. Language, diagnostic, and analysis protocol
versions are recorded separately from component package versions.
For a released child component, `commit` tracks the current verification source while
`published_release.commit` records the immutable commit behind the public artifacts. Release
URLs, publication time, the checksum manifest digest, and every artifact digest are retained as
evidence. Consumer verification remains explicitly pending until clean runners have downloaded,
checked, extracted, and executed all supported platform packages.

The snapshot is not publishable while `release.publish` is false. Promotion requires
`status: release-ready`, the locked language version and all Luna compatibility tags to equal
the root `VERSION`, root platform gates, mandatory real-compiler toolchain integration, Lunax
transactional-install integration, and checksum and attestation verification for every
published artifact. A release tag alone never permits replacing an existing artifact with
different bytes.

Luna 0.3 uses a two-phase promotion so the Luna tag and child releases do not wait on each
other:

1. Commit the complete Luna sources, tests, and release workflows to obtain a candidate commit,
   but do not create the `v0.3.0` tag.
2. Commit and tag the toolchain and Lunax releases, then manually dispatch each release workflow
   with that exact Luna candidate commit as `luna_ref`. The workflow resolves and publishes the
   actual 40-character commit; a mutable branch name is not retained as evidence.
3. Complete child consumer, checksum, and attestation verification. Record the same commit as
   `verified_luna_source_commit` for both components together with their immutable release
   evidence.
4. Commit the promoted lock and only then create the final Luna tag. After the candidate commit,
   changes are restricted to `ecosystem.lock.json`, `CHANGELOG.md`, and the ecosystem/0.3 design
   status documents. The release gate verifies ancestry and that allowlist; any compiler,
   runtime, standard-library, test, or workflow change requires fresh child evidence.

The final Luna tag therefore need not pre-exist, and child components never build against a tag
that would later have to move. Toolchain's `SHA256SUMS` and Lunax's per-file checksums include a
`LUNA-SOURCE-COMMIT` asset so the candidate identity in the lock can be checked against attested
public artifacts.

For adjacent local checkouts, verify the frozen child-repository snapshot without probing the
current 0.3 compiler:

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -P tools/verify_ecosystem_lock.cmake
```

The verifier checks child commits and clean worktrees, component versions and compatibility,
and performs no network or mutation operations. `LUNA_EXECUTABLE` is an optional additional
probe only when the binary belongs to the language version recorded by that snapshot; the
current 0.3 binary must not be compared with the frozen 0.2.1 baseline.

The local release-policy gate is separate because a frozen historical snapshot can be valid
without being valid for today's compiler release:

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -P tools/verify_release_readiness.cmake
```

That command reports why publication is blocked and succeeds when the fail-closed policy is
being enforced. Strict mode also checks the 0.3 snapshot name, Toolchain/Lunax 0.2.0 versions,
tags, URLs, source/published commit equality, consumer/attestation status, and the exact asset
names and SHA-256 shape produced by both workflows. Merely changing `status` while retaining
0.1.2 evidence therefore cannot be reported as ready. The prebuilt-release workflow invokes the
same script with `-DREQUIRE_READY=ON`, so a tag cannot start packaging until the snapshot has
been explicitly promoted and all three components target the current Luna version.

The `Release evidence` workflow is the network-backed companion gate. It fetches each locked
release with `gh`, confirms the release URL, timestamp, status, and tag commit, requires an exact
asset-name set, compares GitHub's asset digests with the lock, and validates the downloaded
checksum files against the recorded artifacts. It also requires every asset's GitHub/Sigstore
attestation to be signed by that component's release workflow on a GitHub-hosted runner. A
mismatch blocks promotion; release evidence must never be copied into the lock without this
gate passing.

The root prebuilt-release workflow does not trust the `status` field alone. After readiness passes
and before any platform package starts, it redownloads each child release's complete public asset
set, resolves lightweight or annotated tags to the final commit, and reverifies checksums,
`LUNA-SOURCE-COMMIT`, and GitHub/Sigstore attestations. The independent Release evidence workflow
and final tag publication use the same verification script so the two gates cannot drift.

## Release handoff decision register (2026-08-31)

The 2026-08-31 review reopened Slot/Fragment `TBD-SF007` through `TBD-SF010`: the static lexical
slice is implemented, but the full runtime model is not closed. Candidate commits are therefore
paused until the blocking items are resolved; the rows below also record artifact authorization,
release scope, and explicit deferrals:

| ID | Confirmation needed | Encoded default | Recommendation | Blocks 0.3 release |
|---|---|---|---|---|
| `RLS001` | Candidate commit topology | All three independent worktrees are uncommitted | Create one fully tested candidate commit per repository, followed later by a separate root lock/status promotion commit; do not create untested intermediate semantic commits | Yes; confirm before any push/tag |
| `RLS002` | GitHub release visibility | Root `v0.3.0` and Lunax `v0.2.0` are prereleases; Toolchain `v0.2.0` is a normal release | Keep the tiers encoded by the current workflows; any unification must happen before the candidate commit and repeat the gates | Yes |
| `RLS003` | Authorization for external writes | No commit, push, tag, or publish has occurred | Authorize the two-phase sequence once: candidate commits → push/CI → child tags/releases → lock promotion → Luna tag/release | Yes |
| `RLS004` | Whether real CUDA/ROCm performance evidence is a release gate | Release workflows exclude hardware tests with `-LE hardware`; simulator/AOT gates pass | Keep hardware measurements as independent non-blocking evidence rather than making a particular GPU a 0.3 prerequisite | No |
| `RLS005` | Whether VS Code test selection, workspace status, and cache reporting enter 0.3 | Luna/Lunax expose no owner protocols for them, so the editor does not guess | Explicitly defer them until after 0.3; ship only compiler-owned check/build/run tasks | No |
| `RLS006` | Overall-design document status | It still says `Draft`, and `TBD-SF007` through `TBD-SF010` are registered again | Change it to `Accepted release candidate` only after the blockers close and the candidate is accepted, then to a released status after the final tag | Yes |
| `RLS007` | Policy during a temporary attestation-service failure | Each asset retries five times and then fails closed | Wait for and rerun GitHub/Sigstore; never bypass attestations or accept checksums alone | Yes, until the network gate passes |
| `RLS008` | Slot/Fragment design closure | The static slice is implemented; runtime scope, same-slot nesting/re-entry, and descriptor promises are not frozen | Decide and implement `TBD-SF007` through `TBD-SF009`; if static-only is chosen, explicitly defer `TBD-SF010` | Yes |

After the Slot/Fragment decisions close, the release sequence is:

1. Close `TBD-SF007` through `TBD-SF009`, complete the implementation, negative tests, and
   specification implied by those choices, and disposition `TBD-SF010`.
2. Review the three diffs, create and push the three candidate commits, and wait for remote CI.
3. Release Toolchain and Lunax against the exact Luna candidate SHA, never a mutable branch.
4. Download every asset and pass consumer, checksum, source-commit, and attestation gates.
5. Atomically replace both child components' versions, commits, URLs, timestamps, artifact
   digests, and `verified_luna_source_commit` in the lock; set `status: release-ready` and
   `release.publish: true`.
6. Pass strict readiness and online evidence, commit the lock promotion, then create `v0.3.0`
   and trigger the root prerelease.
