# Ecosystem release snapshot

`ecosystem.lock.json` is the authoritative candidate snapshot connecting the otherwise
independent Luna, LunaToolchain, and Lunax repositories. The Luna component is the commit
containing the lock file; child components use exact Git commits. Language, diagnostic, and
analysis protocol versions are recorded separately from component package versions.
For a released child component, `commit` tracks the current verification source while
`published_release.commit` records the immutable commit behind the public artifacts. Release
URLs, publication time, the checksum manifest digest, and every artifact digest are retained as
evidence. Consumer verification remains explicitly pending until clean runners have downloaded,
checked, extracted, and executed all supported platform packages.

The snapshot remains a candidate while `release.publish` is false. Promotion requires the
root platform gates, mandatory real-compiler toolchain integration, Lunax transactional-install
integration, and checksum verification for every published artifact. A release tag alone never
permits replacing an existing artifact with different bytes.

For an adjacent local checkout, verify the complete snapshot after building Luna:

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -DLUNA_EXECUTABLE="$PWD/build-stage-a-strict/luna" \
  -P tools/verify_ecosystem_lock.cmake
```

The verifier checks child commits and clean worktrees, component versions and compatibility,
and the compiler's self-reported source commit. It performs no network or mutation operations.

The `Release evidence` workflow is the network-backed companion gate. It fetches each locked
release with `gh`, confirms the release URL, timestamp, status, and tag commit, requires an exact
asset-name set, compares GitHub's asset digests with the lock, and validates the downloaded
checksum files against the recorded artifacts. A mismatch blocks promotion; release evidence
must never be copied into the lock without this gate passing.
