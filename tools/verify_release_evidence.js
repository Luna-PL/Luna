#!/usr/bin/env node

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

function fail(message) {
  console.error(`release evidence verification failed: ${message}`);
  process.exit(1);
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

function sha256(file) {
  const hash = crypto.createHash("sha256");
  hash.update(fs.readFileSync(file));
  return hash.digest("hex");
}

function assertEqual(actual, expected, label) {
  if (actual !== expected) {
    fail(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function parseChecksumFile(file) {
  const lines = fs.readFileSync(file, "utf8").trimEnd().split(/\r?\n/);
  const entries = new Map();
  for (const line of lines) {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    if (!match) {
      fail(`${file} contains an invalid checksum line: ${JSON.stringify(line)}`);
    }
    if (entries.has(match[2])) {
      fail(`${file} contains duplicate entry ${match[2]}`);
    }
    entries.set(match[2], match[1]);
  }
  return entries;
}

function assertMap(actual, expected, label) {
  assertEqual([...actual.keys()].sort().join("\n"), [...expected.keys()].sort().join("\n"), `${label} names`);
  for (const [name, digest] of expected) {
    assertEqual(actual.get(name), digest, `${label} digest for ${name}`);
  }
}

const [lockFile, componentName, releaseFile, tagShaFile, checksumDirectory] = process.argv.slice(2);
if (!lockFile || !componentName || !releaseFile || !tagShaFile || !checksumDirectory) {
  fail("usage: verify_release_evidence.js LOCK COMPONENT RELEASE_JSON TAG_SHA CHECKSUM_DIRECTORY");
}

const lock = readJson(lockFile);
const component = lock.components?.[componentName];
const published = component?.published_release;
if (!published) {
  fail(`component ${componentName} has no published_release entry`);
}

const release = readJson(releaseFile);
assertEqual(release.tagName, published.tag, "release tag");
assertEqual(release.url, published.url, "release URL");
assertEqual(release.publishedAt, published.published_at, "release publication time");
assertEqual(release.isDraft, false, "release draft status");
assertEqual(release.isPrerelease, published.prerelease ?? false, "release prerelease status");
assertEqual(fs.readFileSync(tagShaFile, "utf8").trim(), published.commit, "release tag commit");

const expectedAssets = new Map(Object.entries(published.artifacts));
if (published.checksum_manifest) {
  expectedAssets.set(published.checksum_manifest.asset, published.checksum_manifest.sha256);
}
const remoteAssets = new Map();
for (const asset of release.assets) {
  if (!asset.digest?.startsWith("sha256:")) {
    fail(`release asset ${asset.name} has no SHA-256 digest`);
  }
  if (remoteAssets.has(asset.name)) {
    fail(`release contains duplicate asset ${asset.name}`);
  }
  remoteAssets.set(asset.name, asset.digest.slice("sha256:".length));
}
assertMap(remoteAssets, expectedAssets, "release asset");

if (published.checksum_manifest) {
  const manifestPath = path.join(checksumDirectory, published.checksum_manifest.asset);
  assertEqual(sha256(manifestPath), published.checksum_manifest.sha256, "checksum manifest digest");
  assertMap(parseChecksumFile(manifestPath), new Map(Object.entries(published.artifacts)), "checksum manifest entry");
} else {
  const checksumAssets = [...expectedAssets.keys()].filter((name) => name.endsWith(".sha256"));
  if (checksumAssets.length === 0) {
    fail(`${componentName} has neither a checksum manifest nor checksum assets`);
  }
  for (const checksumAsset of checksumAssets) {
    const checksumPath = path.join(checksumDirectory, checksumAsset);
    assertEqual(sha256(checksumPath), expectedAssets.get(checksumAsset), `checksum asset digest for ${checksumAsset}`);
    const entries = parseChecksumFile(checksumPath);
    const artifact = checksumAsset.slice(0, -".sha256".length);
    assertMap(entries, new Map([[artifact, expectedAssets.get(artifact)]]), `checksum entry in ${checksumAsset}`);
  }
}

console.log(`release evidence verified for ${componentName} ${published.tag}`);
