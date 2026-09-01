cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the Luna source tree")
endif()

file(READ "${LUNA_SOURCE_DIR}/VERSION" luna_version)
string(STRIP "${luna_version}" luna_version)
if(luna_version STREQUAL "")
    message(FATAL_ERROR "VERSION is empty")
endif()
set(expected_release_tag "v${luna_version}")
set(expected_toolchain_version "0.2.0")
set(expected_lunax_version "0.2.0")
set(expected_toolchain_release_tag "v${expected_toolchain_version}")
set(expected_lunax_release_tag "v${expected_lunax_version}")
set(expected_toolchain_release_url
    "https://github.com/Luna-PL/toolchains/releases/tag/${expected_toolchain_release_tag}")
set(expected_lunax_release_url
    "https://github.com/Luna-PL/Lunax/releases/tag/${expected_lunax_release_tag}")
set(expected_toolchain_artifacts
    "LUNA-SOURCE-COMMIT"
    "luna-language-v0.2.0-darwin-arm64.vsix"
    "luna-language-v0.2.0-linux-x64.vsix"
    "luna-language-v0.2.0-win32-x64.vsix"
    "luna-toolchain-0.2.0-linux-x86_64.tar.gz"
    "luna-toolchain-0.2.0-macos-arm64.tar.gz"
    "luna-toolchain-0.2.0-windows-x86_64.zip")
set(expected_lunax_artifacts
    "LUNA-SOURCE-COMMIT"
    "LUNA-SOURCE-COMMIT.sha256"
    "lunax-0.2.0-ubuntu-24.04-x86_64.tar.gz"
    "lunax-0.2.0-ubuntu-24.04-x86_64.tar.gz.sha256"
    "lunax_0.2.0_amd64.deb"
    "lunax_0.2.0_amd64.deb.sha256")
list(SORT expected_toolchain_artifacts)
list(SORT expected_lunax_artifacts)

if(DEFINED ECOSYSTEM_LOCK_PATH)
    set(lock_path "${ECOSYSTEM_LOCK_PATH}")
else()
    set(lock_path "${LUNA_SOURCE_DIR}/ecosystem.lock.json")
endif()
if(NOT EXISTS "${lock_path}")
    message(FATAL_ERROR "ecosystem lock is missing: ${lock_path}")
endif()
file(READ "${lock_path}" lock_json)

function(read_optional_json output)
    string(JSON value ERROR_VARIABLE json_error GET "${lock_json}" ${ARGN})
    if(json_error STREQUAL "NOTFOUND")
        set(${output} "${value}" PARENT_SCOPE)
    else()
        set(${output} "" PARENT_SCOPE)
    endif()
endfunction()

function(read_json_member_names output)
    string(JSON member_count ERROR_VARIABLE json_error
           LENGTH "${lock_json}" ${ARGN})
    if(NOT json_error STREQUAL "NOTFOUND")
        set(${output} "" PARENT_SCOPE)
        return()
    endif()
    set(member_names)
    if(member_count GREATER 0)
        math(EXPR final_member "${member_count} - 1")
        foreach(member_index RANGE 0 ${final_member})
            string(JSON member_name MEMBER "${lock_json}"
                   ${ARGN} ${member_index})
            list(APPEND member_names "${member_name}")
        endforeach()
    endif()
    list(SORT member_names)
    set(${output} "${member_names}" PARENT_SCOPE)
endfunction()

string(JSON schema_version GET "${lock_json}" schema_version)
if(NOT schema_version EQUAL 1)
    message(FATAL_ERROR "unsupported ecosystem lock schema: ${schema_version}")
endif()

string(JSON publish GET "${lock_json}" release publish)
string(JSON snapshot GET "${lock_json}" snapshot)
string(JSON lock_status GET "${lock_json}" status)
string(JSON locked_language_version GET
       "${lock_json}" components luna language_version)
string(JSON locked_release_tag GET
       "${lock_json}" components luna baseline_release_tag)
string(JSON toolchain_release_tag GET
       "${lock_json}" components toolchain compatible_luna_release_tag)
string(JSON lunax_release_tag GET
       "${lock_json}" components lunax compatible_luna_release_tag)
read_optional_json(toolchain_luna_commit
                   components toolchain verified_luna_source_commit)
read_optional_json(lunax_luna_commit
                   components lunax verified_luna_source_commit)

foreach(component IN ITEMS toolchain lunax)
    string(JSON component_version GET
           "${lock_json}" components ${component} version)
    string(JSON component_commit GET
           "${lock_json}" components ${component} commit)
    string(JSON published_tag GET
           "${lock_json}" components ${component} published_release tag)
    string(JSON published_commit GET
           "${lock_json}" components ${component} published_release commit)
    string(JSON published_url GET
           "${lock_json}" components ${component} published_release url)
    read_optional_json(consumer_status
        components ${component} published_release consumer_verification status)
    read_optional_json(attestation_status
        components ${component} published_release artifact_attestations status)
    read_json_member_names(artifact_names
        components ${component} published_release artifacts)
    set(${component}_version "${component_version}")
    set(${component}_commit "${component_commit}")
    set(${component}_published_tag "${published_tag}")
    set(${component}_published_commit "${published_commit}")
    set(${component}_published_url "${published_url}")
    set(${component}_consumer_status "${consumer_status}")
    set(${component}_attestation_status "${attestation_status}")
    set(${component}_artifact_names "${artifact_names}")
endforeach()

set(blockers)
if(NOT publish)
    list(APPEND blockers "release.publish is false")
endif()
if(NOT snapshot MATCHES "^luna-${luna_version}-")
    list(APPEND blockers
         "snapshot '${snapshot}' is not a Luna ${luna_version} snapshot")
endif()
if(NOT lock_status STREQUAL "release-ready")
    list(APPEND blockers
         "status is '${lock_status}', expected 'release-ready'")
endif()
if(NOT locked_language_version STREQUAL luna_version)
    list(APPEND blockers
         "locked language version is ${locked_language_version}, expected ${luna_version}")
endif()
if(NOT locked_release_tag STREQUAL expected_release_tag)
    list(APPEND blockers
         "Luna release tag is ${locked_release_tag}, expected ${expected_release_tag}")
endif()
if(NOT toolchain_release_tag STREQUAL expected_release_tag)
    list(APPEND blockers
         "toolchain targets ${toolchain_release_tag}, expected ${expected_release_tag}")
endif()
if(NOT lunax_release_tag STREQUAL expected_release_tag)
    list(APPEND blockers
         "Lunax targets ${lunax_release_tag}, expected ${expected_release_tag}")
endif()

foreach(component IN ITEMS toolchain lunax)
    set(expected_version "${expected_${component}_version}")
    set(expected_component_tag "${expected_${component}_release_tag}")
    set(expected_component_url "${expected_${component}_release_url}")
    set(expected_artifacts "${expected_${component}_artifacts}")
    if(NOT ${component}_version STREQUAL expected_version)
        list(APPEND blockers
             "${component} version is ${${component}_version}, expected ${expected_version}")
    endif()
    if(NOT ${component}_published_tag STREQUAL expected_component_tag)
        list(APPEND blockers
             "${component} published tag is ${${component}_published_tag}, expected ${expected_component_tag}")
    endif()
    if(NOT ${component}_published_url STREQUAL expected_component_url)
        list(APPEND blockers
             "${component} release URL does not match ${expected_component_url}")
    endif()
    if(NOT ${component}_commit STREQUAL ${component}_published_commit)
        list(APPEND blockers
             "${component} source commit and published commit differ")
    endif()
    foreach(commit_value IN ITEMS
            "${${component}_commit}"
            "${${component}_published_commit}")
        string(LENGTH "${commit_value}" commit_length)
        if(NOT commit_length EQUAL 40 OR
           NOT commit_value MATCHES "^[0-9a-f]+$")
            list(APPEND blockers
                 "${component} has an invalid release commit")
            break()
        endif()
    endforeach()
    if(NOT ${component}_consumer_status STREQUAL "passed")
        list(APPEND blockers
             "${component} consumer verification is not passed")
    endif()
    if(NOT ${component}_attestation_status STREQUAL "passed")
        list(APPEND blockers
             "${component} artifact attestation verification is not passed")
    endif()
    if(NOT "${${component}_artifact_names}" STREQUAL
           "${expected_artifacts}")
        list(APPEND blockers
             "${component} artifact inventory is not the expected ${expected_version} release set")
    endif()
    foreach(asset_name IN LISTS expected_artifacts)
        read_optional_json(asset_digest
            components ${component} published_release artifacts "${asset_name}")
        string(LENGTH "${asset_digest}" digest_length)
        if(NOT digest_length EQUAL 64 OR
           NOT asset_digest MATCHES "^[0-9a-f]+$")
            list(APPEND blockers
                 "${component} artifact ${asset_name} has no valid SHA-256 digest")
        endif()
    endforeach()
endforeach()

read_optional_json(toolchain_checksum_asset
    components toolchain published_release checksum_manifest asset)
read_optional_json(toolchain_checksum_digest
    components toolchain published_release checksum_manifest sha256)
string(LENGTH "${toolchain_checksum_digest}" toolchain_checksum_length)
if(NOT toolchain_checksum_asset STREQUAL "SHA256SUMS" OR
   NOT toolchain_checksum_length EQUAL 64 OR
   NOT toolchain_checksum_digest MATCHES "^[0-9a-f]+$")
    list(APPEND blockers
         "toolchain checksum manifest is not a valid SHA256SUMS record")
endif()

foreach(component IN ITEMS toolchain lunax)
    set(candidate "${${component}_luna_commit}")
    string(LENGTH "${candidate}" candidate_length)
    if(NOT candidate_length EQUAL 40 OR NOT candidate MATCHES "^[0-9a-f]+$")
        list(APPEND blockers
             "${component} has no valid verified_luna_source_commit")
    endif()
endforeach()

if(toolchain_luna_commit AND lunax_luna_commit AND
   NOT toolchain_luna_commit STREQUAL lunax_luna_commit)
    list(APPEND blockers
         "toolchain and Lunax were verified against different Luna commits")
endif()

if(toolchain_luna_commit STREQUAL lunax_luna_commit AND
   toolchain_luna_commit MATCHES "^[0-9a-f]+$")
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        list(APPEND blockers "Git is required to verify the Luna candidate commit")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" merge-base --is-ancestor
                    "${toolchain_luna_commit}" HEAD
            WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
            RESULT_VARIABLE ancestor_result
            ERROR_QUIET)
        if(NOT ancestor_result EQUAL 0)
            list(APPEND blockers
                 "verified Luna candidate ${toolchain_luna_commit} is not an ancestor of HEAD")
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" diff --name-only
                        "${toolchain_luna_commit}..HEAD"
                WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
                RESULT_VARIABLE diff_result
                OUTPUT_VARIABLE changed_paths
                ERROR_VARIABLE diff_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT diff_result EQUAL 0)
                list(APPEND blockers
                     "cannot compare the verified Luna candidate: ${diff_error}")
            elseif(NOT changed_paths STREQUAL "")
                string(REPLACE "\n" ";" changed_path_list "${changed_paths}")
                foreach(changed_path IN LISTS changed_path_list)
                    if(NOT changed_path STREQUAL "ecosystem.lock.json" AND
                       NOT changed_path STREQUAL "CHANGELOG.md" AND
                       NOT changed_path MATCHES
                           "^docs/(ecosystem_release|luna_0\\.3_design)(\\.zh-CN)?\\.md$")
                        list(APPEND blockers
                             "post-candidate change is outside the promotion allowlist: ${changed_path}")
                    endif()
                endforeach()
            endif()
        endif()
    endif()
endif()

if(blockers)
    string(JOIN "\n  - " blocker_text ${blockers})
    if(REQUIRE_READY)
        message(FATAL_ERROR
            "Luna ${luna_version} ecosystem is not release-ready:\n"
            "  - ${blocker_text}\n"
            "Publish compatible toolchain and Lunax releases against one immutable "
            "Luna candidate commit, record their evidence, then explicitly promote "
            "ecosystem.lock.json without changing compiler sources.")
    endif()
    message(STATUS
        "release policy verified: Luna ${luna_version} publication is blocked:\n"
        "  - ${blocker_text}")
else()
    message(STATUS
        "release policy verified: Luna ${luna_version} ecosystem is release-ready")
endif()
