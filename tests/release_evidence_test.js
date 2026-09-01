const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const verifier = process.argv[2];
if (!verifier) throw new Error("verifier path is required");

const sourceCommit = "0123456789abcdef0123456789abcdef01234567";
const releaseCommit = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";

function sha256(file) {
  return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

function write(file, contents) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, contents);
}

function runVerifier(fixture, expectedSuccess, expectedMessage = "") {
  const result = spawnSync(
    process.execPath,
    [
      verifier,
      fixture.lock,
      fixture.component,
      fixture.release,
      fixture.tag,
      fixture.assets,
    ],
    { encoding: "utf8" },
  );
  if (expectedSuccess) {
    assert.equal(result.status, 0, `${result.stdout}${result.stderr}`);
  } else {
    assert.notEqual(result.status, 0, "invalid evidence must be rejected");
    assert.match(result.stderr, new RegExp(expectedMessage));
  }
}

function baseFixture(root, component) {
  const assets = path.join(root, "assets");
  const source = path.join(assets, "LUNA-SOURCE-COMMIT");
  write(source, `${sourceCommit}\n`);
  return {
    component,
    assets,
    source,
    lock: path.join(root, "lock.json"),
    release: path.join(root, "release.json"),
    tag: path.join(root, "tag.txt"),
  };
}

function writeEvidence(fixture, component) {
  const published = component.published_release;
  write(
    fixture.release,
    JSON.stringify({
      tagName: published.tag,
      url: published.url,
      publishedAt: published.published_at,
      isDraft: false,
      isPrerelease: published.prerelease ?? false,
      assets: [
        ...Object.entries(published.artifacts),
        ...(published.checksum_manifest
          ? [[published.checksum_manifest.asset, published.checksum_manifest.sha256]]
          : []),
      ].map(([name, digest]) => ({ name, digest: `sha256:${digest}` })),
    }),
  );
  write(fixture.tag, `${releaseCommit}\n`);
  write(
    fixture.lock,
    JSON.stringify({ components: { [fixture.component]: component } }),
  );
}

function toolchainFixture(root) {
  const fixture = baseFixture(root, "toolchain");
  const archive = path.join(fixture.assets, "toolchain.tar.gz");
  write(archive, "toolchain archive fixture");
  const checksum = path.join(fixture.assets, "SHA256SUMS");
  write(
    checksum,
    `${sha256(fixture.source)}  LUNA-SOURCE-COMMIT\n` +
      `${sha256(archive)}  toolchain.tar.gz\n`,
  );
  const component = {
    verified_luna_source_commit: sourceCommit,
    published_release: {
      tag: "v0.2.0",
      commit: releaseCommit,
      url: "https://example.invalid/toolchain/v0.2.0",
      published_at: "2026-08-31T00:00:00Z",
      artifacts: {
        "LUNA-SOURCE-COMMIT": sha256(fixture.source),
        "toolchain.tar.gz": sha256(archive),
      },
      checksum_manifest: {
        asset: "SHA256SUMS",
        sha256: sha256(checksum),
      },
    },
  };
  writeEvidence(fixture, component);
  return { fixture, component };
}

function lunaxFixture(root) {
  const fixture = baseFixture(root, "lunax");
  const archive = path.join(fixture.assets, "lunax.tar.gz");
  write(archive, "lunax archive fixture");
  for (const asset of [fixture.source, archive]) {
    write(
      `${asset}.sha256`,
      `${sha256(asset)}  ${path.basename(asset)}\n`,
    );
  }
  const artifacts = {};
  for (const name of [
    "LUNA-SOURCE-COMMIT",
    "LUNA-SOURCE-COMMIT.sha256",
    "lunax.tar.gz",
    "lunax.tar.gz.sha256",
  ]) {
    artifacts[name] = sha256(path.join(fixture.assets, name));
  }
  const component = {
    verified_luna_source_commit: sourceCommit,
    published_release: {
      tag: "v0.2.0",
      commit: releaseCommit,
      url: "https://example.invalid/lunax/v0.2.0",
      published_at: "2026-08-31T00:00:00Z",
      prerelease: true,
      artifacts,
    },
  };
  writeEvidence(fixture, component);
  return { fixture, component };
}

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "luna-release-evidence-"));
try {
  const toolchain = toolchainFixture(path.join(temporary, "toolchain"));
  runVerifier(toolchain.fixture, true);

  const lunax = lunaxFixture(path.join(temporary, "lunax"));
  runVerifier(lunax.fixture, true);

  lunax.component.verified_luna_source_commit =
    "fedcba9876543210fedcba9876543210fedcba98";
  writeEvidence(lunax.fixture, lunax.component);
  runVerifier(lunax.fixture, false, "verified Luna source commit");

  toolchain.component.published_release.artifacts = {
    "toolchain.tar.gz": toolchain.component.published_release.artifacts["toolchain.tar.gz"],
  };
  writeEvidence(toolchain.fixture, toolchain.component);
  runVerifier(toolchain.fixture, false, "does not record LUNA-SOURCE-COMMIT");
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}

console.log("release evidence policies verified");
