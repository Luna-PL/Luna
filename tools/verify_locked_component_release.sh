#!/usr/bin/env bash
set -euo pipefail

lock_file="${1:?missing ecosystem lock path}"
component="${2:?missing component name}"
repository="${3:?missing owner/repository}"
evidence_directory="${4:?missing evidence directory}"

case "${component}" in
    toolchain|lunax) ;;
    *) echo "unsupported ecosystem component: ${component}" >&2; exit 2 ;;
esac
for command_name in curl gh jq node; do
    if ! command -v "${command_name}" >/dev/null; then
        echo "required release-evidence command is missing: ${command_name}" >&2
        exit 2
    fi
done

tag="$(node -e '
    const lock = require(process.argv[1]);
    process.stdout.write(lock.components[process.argv[2]].published_release.tag);
' "$(realpath "${lock_file}")" "${component}")"
signer_workflow="$(node -e '
    const lock = require(process.argv[1]);
    process.stdout.write(
      lock.components[process.argv[2]].published_release
        .artifact_attestations.signer_workflow,
    );
' "$(realpath "${lock_file}")" "${component}")"
test -n "${tag}"
test -n "${signer_workflow}"

mkdir -p "${evidence_directory}/assets"
gh release view "${tag}" --repo "${repository}" \
    --json assets,isDraft,isPrerelease,publishedAt,tagName,url \
    > "${evidence_directory}/release.json"

ref_json="$(gh api "repos/${repository}/git/ref/tags/${tag}")"
object_type="$(jq -r '.object.type' <<< "${ref_json}")"
object_sha="$(jq -r '.object.sha' <<< "${ref_json}")"
for _ in 1 2 3 4 5; do
    if [[ "${object_type}" != "tag" ]]; then
        break
    fi
    tag_json="$(gh api "repos/${repository}/git/tags/${object_sha}")"
    object_type="$(jq -r '.object.type' <<< "${tag_json}")"
    object_sha="$(jq -r '.object.sha' <<< "${tag_json}")"
done
if [[ "${object_type}" != "commit" ||
      ! "${object_sha}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "release tag ${tag} does not resolve to a Git commit" >&2
    exit 1
fi
printf '%s\n' "${object_sha}" > "${evidence_directory}/tag-sha.txt"

while IFS= read -r asset_url; do
    asset_name="${asset_url##*/}"
    curl --fail --location --silent --show-error \
        --retry 5 --retry-all-errors --retry-delay 2 \
        --connect-timeout 20 --max-time 180 \
        --output "${evidence_directory}/assets/${asset_name}" \
        "${asset_url}"
done < <(jq -r '.assets[].url' "${evidence_directory}/release.json")

node "$(dirname "${BASH_SOURCE[0]}")/verify_release_evidence.js" \
    "${lock_file}" "${component}" \
    "${evidence_directory}/release.json" \
    "${evidence_directory}/tag-sha.txt" \
    "${evidence_directory}/assets"

while IFS= read -r -d '' asset; do
    attestation_verified=false
    for attempt in 1 2 3 4 5; do
        if gh attestation verify "${asset}" \
            --repo "${repository}" \
            --signer-workflow "${signer_workflow}" \
            --deny-self-hosted-runners; then
            attestation_verified=true
            break
        fi
        echo "attestation verification attempt ${attempt}/5 failed for $(basename "${asset}")" >&2
        if [[ "${attempt}" != 5 ]]; then
            sleep 2
        fi
    done
    if [[ "${attestation_verified}" != true ]]; then
        echo "cannot verify attestation for ${asset} after 5 attempts" >&2
        exit 1
    fi
done < <(find "${evidence_directory}/assets" -type f -print0)
