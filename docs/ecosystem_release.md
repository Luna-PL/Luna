# Ecosystem release snapshot

`ecosystem.lock.json` is the authoritative snapshot connecting the otherwise independent
Luna, LunaToolchain, and Lunax repositories. Its `release-ready` status must be
checked against the current Luna lineage and release evidence; the status field
alone does not authorize a tag. The Luna component is the commit containing the lock
file; child components use exact Git commits. Language, diagnostic, and analysis protocol versions are
recorded separately from component package versions.
For a released child component, `commit` tracks the current verification source while
`published_release.commit` records the immutable commit behind the public artifacts. Release
URLs, publication time, the checksum manifest digest, and every artifact digest are retained as
evidence. The current lock records passed child consumer verification and attestations; those
records refer to the Luna source candidate pinned by the child releases.

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
2. Commit and tag the toolchain and Lunax releases. Each compatibility manifest pins that exact
   Luna candidate as `source_commit`; the release workflow reads and publishes the 40-character
   commit, so a mutable branch name is never retained as evidence.
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
being enforced. Strict mode also checks the 0.3 snapshot name, Toolchain/Lunax 0.2.1 versions,
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

## Current lineage and release recovery (2026-10-08)

The `luna-0.3.0-ecosystem-release.2` lock records Toolchains and Lunax
`v0.2.1` releases, both pinned to Luna source candidate `06a438e`, and marks the
snapshot `release-ready` with `release.publish: true`. The candidate's
[Linux](https://github.com/Luna-PL/Luna/actions/runs/37569424493),
[Windows](https://github.com/Luna-PL/Luna/actions/runs/37569424497), and
[macOS](https://github.com/Luna-PL/Luna/actions/runs/37569424458) CI passed.
Local frozen-lock verification, strict `REQUIRE_READY=ON`, and the three focused
release-policy CTests passed against this lock. The independent online
[Release evidence run](https://github.com/Luna-PL/Luna/actions/runs/37575445770)
passed before the annotated root `v0.3.0` tag was created at `5641959`.
The [initial tag-triggered release](https://github.com/Luna-PL/Luna/actions/runs/37576549753)
failed: Linux and Windows package jobs lacked the Git history needed by a
release-policy CTest, and the macOS job selected unsupported Homebrew LLVM 23.
The workflow repair at `ed0ddee` fetches full history, selects LLVM 22 on macOS,
and records the checked-out tag commit in each package manifest. The
[first manual recovery run](https://github.com/Luna-PL/Luna/actions/runs/37653830186)
passed readiness, child evidence, and Linux/Windows package verification; macOS
compiled under LLVM 22 but stopped on two REPL warnings promoted to errors.
A second workflow-only repair at `33de55f` narrows this warning exception to
`v0.3.0`. The [second manual recovery run](https://github.com/Luna-PL/Luna/actions/runs/37655986456)
passed readiness, child-release evidence, all three platform build/test/package
and clean-install gates, and publication. The public
[`v0.3.0` prerelease](https://github.com/Luna-PL/Luna/releases/tag/v0.3.0)
was published on 2026-10-07 at 17:17 UTC with Linux x86_64, macOS arm64, and
Windows UCRT64 x86_64 archives plus one SHA-256 file per archive. The three
checksum files match the SHA-256 digests reported for their archives by GitHub.
The annotated tag still resolves to `5641959`. The `33de55f` push also passed
[Linux](https://github.com/Luna-PL/Luna/actions/runs/37655880401),
[Windows](https://github.com/Luna-PL/Luna/actions/runs/37655879964), and
[macOS](https://github.com/Luna-PL/Luna/actions/runs/37655880045) CI.
Strict readiness passes at the tag; current
`main` intentionally fails the candidate allowlist after the workflow-only
repair and cannot be used as a new `v0.3.0` source commit.

After this prerelease, the next implementation gate is the private source
Ref/apply control-flow and cleanup proof, including the intermittent Windows
LLVM 20 COFF relocation investigation. Then freeze the versioned host Ref
ingress/return and ownership/status contract, connect production drop glue and
Moon Container verification, and pass the two-package execution gate in that
order. Keep the feature private until those gates pass; the detailed acceptance
conditions are in the [source Ref/apply plan](slot_fragment_runtime_plan.md#next-source-refapply-gates-updated-2026-10-06).

On 2026-10-03 the local CLANG64 non-hardware suite passed 76/77 tests with
four workers; `luna.repl-smoke` timed out during process-tree cleanup under
parallel load and passed when rerun alone. Its wall-clock bound was then
adjusted for JIT setup while preserving the cleanup assertions. The complete
suite passed 77/77 with four workers on 2026-10-04.
After rebuilding the current worktree on 2026-10-06, the same local suite
passed 77/77 with four workers in 71.18 seconds, including REPL and Native
artifact gates. The read-only release-readiness check again reported that
`41ce85e` is not an ancestor of the then-current `8fae950`; it returned
success only because the fail-closed policy correctly blocked publication.
That run preceded commit `a0bf2b5` and was worktree evidence, not remote CI.
An isolated WSL Arch Linux Clang/LLVM 22.1.8 build compiled all targets and
passed the complete local Linux CTest suite 76/76 with four workers in 67.15
seconds. The first run passed 75/76: `luna.ecosystem-frozen-baseline` saw the
Windows checkout's CRLF files as modified because WSL Git did not inherit the
Windows system `core.autocrlf=true` setting. Both child worktrees were clean
under that setting; a process-local Git configuration made the failed test
pass and the complete rerun pass. This does not replace the release
candidate's required platform CI.
The implementation worktree added an experimental parallel Native v2 `i32()` entry
profile, with v1 proof/export compatibility preserved. Focused Native artifact
and canonical tests pass on Windows CLANG64 and WSL Arch Linux. In the later
complete local runs, Windows passed 76/77 and Linux 75/76; each sole failure
was the file-guide inventory missing the new helper header. After adding that
entry, the inventory test passed on both platforms. These are local worktree
results, not evidence for an immutable release candidate or remote CI.
The next worktree revision carries the validated v2 profile through Native
generation bindings and typed pinned calls, and checks profile preservation
across load-once and switching. A resealed v1-only-query variant loads through
the old path but cannot satisfy a typed requirement. After rebuilding all
targets, the full local CLANG64 suite passed 77/77 in 66.69 seconds and the
full WSL Arch Linux suite passed 76/76 in 90.60 seconds. These runs still do
not establish an immutable candidate or remote platform evidence.
An independently compiled v1-only C library was then sealed with its own
proof/trust record. The independent proof oracle, v1 call, unprofiled
generation and typed-lookup rejection pass in the focused Native artifact
CTest on both platforms; the preceding full-suite counts predate this fixture.
The v2 candidate now pins its 64-bit C record offsets and SHA-256 row
framing; independently resealed unknown-profile artifacts are rejected.
Native artifact, MoonRuntime and runtime-ABI focused tests pass on both local
platforms after these checks. Host ABI review remains open.
The release package list and CI workflows cover 64-bit Linux, Windows and the
macOS runner architecture, with no listed 32-bit package. The macOS workflow
already includes the Native artifact CTest. The freestanding 32-bit C layout
probe is now wired into Clang
CTest for GNU/Linux, Windows GNU and Darwin targets. Its three tests and the
file-guide inventory pass locally on Windows CLANG64 and WSL Arch Linux; it
does not exercise a 32-bit loader or artifact. The complete non-hardware
CTest suites now pass 80/80 and 79/79 respectively after this integration.
Commit `a0bf2b5` contains this implementation and test evidence. It is on
remote `main`, and the read-only readiness check still reports the verified child
source `41ce85e` as non-ancestral; strict readiness fails as designed.
A subsequent commit `9af653f` adds only a private three-node branching
owner cleanup proof for `?` Err, injected post-body failure and host Drop.
An independently sealed four-node fork is rejected before private JIT
materialization by the bounded shape gate.
Focused canonical tests pass in ordinary and ASAN builds on Windows CLANG64
and WSL Arch Linux. After rebuilding all targets at `9af653f`, the complete
non-hardware suites pass locally on Windows CLANG64 (80/80) and WSL Arch Linux
(79/79). Both commits are on remote `main`; this push triggered
[Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37459306824),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37459306797), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37459306874).
Linux and Windows passed. macOS failed its sole `luna.native-artifact` CTest:
the independent Python Native consumer returned nonzero, while the test harness
did not print its exit code. A [diagnostic rerun](https://github.com/Luna-PL/Luna/actions/runs/37461051981)
identified exit 24 in the test's synthetic v1-only-query artifact, made by
rewriting bytes in a linked dynamic library. That duplicate fixture is removed;
the separately compiled and sealed v1 C library retains the actual v1 loader,
generation and typed-rejection checks. On the corrected implementation commit
`8aac3a0`, [Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37462447341),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37462447181), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37462447103) all
completed successfully. Linux C++17, C++23, sanitizer, and thread-sanitizer
jobs passed; the macOS Native artifact test now passes. The next ABI gate is
host review of the parallel query/version rule, exact-size policy and pointer
lifetime. Both v1 and v2 descriptor strings now reject invalid UTF-8. The
[host-facing candidate](runtime_abi.md)
records the current behavior. A 32-bit runtime would require its own gate,
and source Ref/apply remains private.
On `80f0e47`, [Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37471375414),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37471375402), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37471375423) passed
after the host loader began rejecting malformed UTF-8 descriptor strings.
The next worktree check adds an independently compiled and sealed C v2
producer: a valid typed call succeeds and an oversized v2 row is rejected
after its proof verifies. Its focused Windows CTest passes; cross-platform
CI remains a gate for every final candidate. The lock's `release-ready` field
describes the frozen September child evidence, not current HEAD readiness:
the strict check fails because those children verified Luna `41ce85e`.
For that independent C v2 producer at `716ec17`, [Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37503367318),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37503367279), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37503367316) passed.
A subsequent focused Windows test also calls through its pinned binding after
`MoonRuntime` destruction to check the library lease's lifetime. That proof is
committed at `1db2931`, whose [Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37505248341),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37505248266), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37505248282) passed.

An exact-`1db2931` [Toolchains compatibility run](https://github.com/Luna-PL/toolchains/actions/runs/37507742264)
found that the grammar rejected `examples/fragments.luna`. The Toolchains compatibility
branch adds `fragment` to its declaration keywords; local corpus and Rust workspace
tests pass. An exact-`1db2931` [Lunax compatibility run](https://github.com/Luna-PL/Lunax/actions/runs/37507837322)
passed. Both children need new versions because their existing `v0.2.0` releases
immutably record the older Luna candidate. The root readiness gate also encoded
the old child version and asset names. Commit `129d593` updates those
expectations to `0.2.1`. Its Linux CI exposed a release-policy test fixture
still constructing `0.2.0` assets. The corrected fixture passes locally at
`06a438e`, the current Luna source candidate. Its
[Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37569424493),
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37569424497), and
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37569424458) passed.

Toolchains `main` at `48ded79` and Lunax `main` at `b71985e` both pin exact
Luna source commit `06a438ed8f8d7110d805fe1ee4fa55972b97f97f`.
Their exact-input [Toolchains CI](https://github.com/Luna-PL/toolchains/actions/runs/37569698482)
and [Lunax CI](https://github.com/Luna-PL/Lunax/actions/runs/37569824458)
passed. Toolchains CI includes Linux, Windows, macOS, and mandatory real-compiler
integration; Lunax CI includes transactional-install integration. These are
source checks. The [Toolchains `v0.2.1` release](https://github.com/Luna-PL/toolchains/releases/tag/v0.2.1)
and [Lunax `v0.2.1` prerelease](https://github.com/Luna-PL/Lunax/releases/tag/v0.2.1)
are published from those exact commits; both release workflows passed. The
[Toolchains consumer](https://github.com/Luna-PL/toolchains/actions/runs/37573408078)
verified its Linux, macOS arm64, and Windows packages, checksums, source marker,
and attestations. The [Lunax consumer](https://github.com/Luna-PL/Lunax/actions/runs/37573511968)
verified its Ubuntu archive and Debian package with their checksums, source marker,
and attestations. The lock records all attached asset digests and these
consumer runs. The root online Release evidence workflow
independently redownloads and verifies the public releases.

## Release handoff decision register (2026-09-15)

The 2026-09-15 review kept Slot/Fragment `TBD-SF007` through `TBD-SF010` deliberately open: at
that checkpoint only the static lexical slice was implemented. Slot/Fragment was excluded from
the 0.3 core freeze rather than used to block it. The rows below preserve that dated handoff
decision and candidate state; they are not a live report of the 2026-09-25 runtime implementation:

| ID | Confirmation needed | Encoded default | Recommendation | Blocks 0.3 release |
|---|---|---|---|---|
| `RLS001` | Candidate commit topology | Local candidates exist at Luna `41ce85e`, Toolchains `63c8fe1`, and Lunax `42285e1`; the three worktrees are clean before this status-only update | Preserve the Luna semantic candidate and follow it later with a separate root lock/status promotion commit | No; locally complete |
| `RLS002` | GitHub release visibility | Root `v0.3.0` and Lunax `v0.2.0` are prereleases; Toolchains `v0.2.0` is a normal release | Keep the tiers encoded by the current workflows; any unification must happen before child tags and repeat the gates | Yes; confirm before tags |
| `RLS003` | Authorization for external writes | Local commits exist; no push, tag, or publish has occurred | Authorize the remaining sequence explicitly: push/CI → child tags/releases → lock promotion → Luna tag/release | Yes |
| `RLS004` | Whether real CUDA/ROCm performance evidence is a release gate | Release workflows exclude hardware tests with `-LE hardware`; simulator/AOT gates pass | Keep hardware measurements as independent non-blocking evidence rather than making a particular GPU a 0.3 prerequisite | No |
| `RLS005` | Whether VS Code test selection, workspace status, and cache reporting enter 0.3 | Luna/Lunax expose no owner protocols for them, so the editor does not guess | Explicitly defer them until after 0.3; ship only compiler-owned check/build/run tasks | No |
| `RLS006` | Overall-design document status | It remains `Draft` because Slot/Fragment is open, while the separately documented core freeze has a local candidate | Keep the full design `Draft`; promote the core snapshot independently and change the overall status only after Slot/Fragment closes | No for the core candidate; yes before claiming the whole design stable |
| `RLS007` | Policy during a temporary attestation-service failure | Each asset retries five times and then fails closed | Wait for and rerun GitHub/Sigstore; never bypass attestations or accept checksums alone | Yes, until the network gate passes |
| `RLS008` | Slot/Fragment design closure | The static slice is implemented; runtime scope, same-slot nesting/re-entry, and descriptor promises remain open | Keep `TBD-SF007` through `TBD-SF010` open and outside the core-freeze contract; resolve them before publishing stable Slot/Fragment semantics | No for the core/alpha release; yes for stable Slot/Fragment |

With Slot/Fragment explicitly excluded from the core freeze, the release sequence is:

1. At that checkpoint, preserve `TBD-SF007` through `TBD-SF010` as open Slot/Fragment work and do
   not expand the frozen core candidate to resolve them.
2. Push the three existing candidate commits and wait for remote CI.
3. Release Toolchain and Lunax against the exact Luna candidate SHA recorded in each
   compatibility manifest, never a mutable branch.
4. Download every asset and pass consumer, checksum, source-commit, and attestation gates.
5. Atomically replace both child components' versions, commits, URLs, timestamps, artifact
   digests, and `verified_luna_source_commit` in the lock; set `status: release-ready` and
   `release.publish: true`.
6. Pass strict readiness and online evidence, commit the lock promotion, then create `v0.3.0`
   and trigger the root prerelease.

### Slot/Fragment status revision (2026-09-25)

SFR001 and the [runtime injection plan](slot_fragment_runtime_plan.md) supersede the historical
SF006 syntax and resolve the runtime-scope and retention choices in `TBD-SF007` and `TBD-SF009`.
The bounded first ABI of `TBD-SF010` is implemented: verified runtime Fragment refs, pinned
candidate snapshots, host-selected BindingSets, safe-point activation, explicit execution
contexts, and cross-package dynamic Slot dispatch. `TBD-SF008` now has host-ordered chains and
local overrides, but same-fragment re-entry is not yet a frozen stable-language promise.
Context-aware indirect calls and non-Copy exported Slot contracts remain outside the first ABI.
This revision does not enlarge the 0.3 core freeze or by itself authorize a stable
Slot/Fragment release; cross-platform CI and independent performance/stability evidence remain
separate gates.

The [SF008 bounded profile (2026-09-26)](slot_fragment_contract.md) now records
static cycle rejection, finite same-Slot continuation nesting, independent
runtime activations, nested override/None isolation, and the context-free v1
published-handler boundary with executable regression evidence. `TBD-SF008`
remains open for stable-profile acceptance and any handler context/re-entry
extension. Implemented bounded behavior is not stable-release approval and
does not reopen the core-freeze boundary.
