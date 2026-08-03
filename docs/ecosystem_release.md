# Ecosystem release snapshot

`ecosystem.lock.json` is the authoritative candidate snapshot connecting the otherwise
independent Luna, LunaToolchain, and Lunax repositories. The Luna component is the commit
containing the lock file; child components use exact Git commits. Language, diagnostic, and
analysis protocol versions are recorded separately from component package versions.

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
