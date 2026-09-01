cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the Luna source tree")
endif()

set(verifier "${LUNA_SOURCE_DIR}/tools/verify_release_readiness.cmake")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DLUNA_SOURCE_DIR=${LUNA_SOURCE_DIR}
            -P "${verifier}"
    RESULT_VARIABLE frozen_result
    OUTPUT_VARIABLE frozen_output
    ERROR_VARIABLE frozen_error)
if(NOT frozen_result EQUAL 0 OR
   NOT frozen_output MATCHES "publication is blocked")
    message(FATAL_ERROR
        "frozen snapshot policy check failed:\n${frozen_output}${frozen_error}")
endif()

find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
    RESULT_VARIABLE head_result
    OUTPUT_VARIABLE head_commit
    ERROR_VARIABLE head_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT head_result EQUAL 0)
    message(FATAL_ERROR "cannot resolve Luna HEAD: ${head_error}")
endif()

file(READ "${LUNA_SOURCE_DIR}/ecosystem.lock.json" ready_lock)
file(READ "${LUNA_SOURCE_DIR}/VERSION" luna_version)
string(STRIP "${luna_version}" luna_version)
set(luna_tag "v${luna_version}")
set(fixture_digest
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
string(JSON ready_lock SET "${ready_lock}" status "\"release-ready\"")
string(JSON ready_lock SET "${ready_lock}" snapshot
       "\"luna-${luna_version}-tooling-candidate\"")
string(JSON ready_lock SET "${ready_lock}" release publish true)
string(JSON ready_lock SET "${ready_lock}"
       components luna language_version "\"${luna_version}\"")
string(JSON ready_lock SET "${ready_lock}"
       components luna baseline_release_tag "\"${luna_tag}\"")
foreach(component IN ITEMS toolchain lunax)
    string(JSON ready_lock SET "${ready_lock}"
           components ${component} compatible_luna_release_tag "\"${luna_tag}\"")
    string(JSON ready_lock SET "${ready_lock}"
           components ${component} verified_luna_source_commit "\"${head_commit}\"")
endforeach()

string(JSON ready_lock SET "${ready_lock}"
       components toolchain version "\"0.2.0\"")
string(JSON ready_lock SET "${ready_lock}"
       components toolchain published_release tag "\"v0.2.0\"")
string(JSON ready_lock SET "${ready_lock}"
       components toolchain published_release url
       "\"https://github.com/Luna-PL/toolchains/releases/tag/v0.2.0\"")
set(toolchain_artifacts "{}")
foreach(asset IN ITEMS
        "LUNA-SOURCE-COMMIT"
        "luna-language-v0.2.0-darwin-arm64.vsix"
        "luna-language-v0.2.0-linux-x64.vsix"
        "luna-language-v0.2.0-win32-x64.vsix"
        "luna-toolchain-0.2.0-linux-x86_64.tar.gz"
        "luna-toolchain-0.2.0-macos-arm64.tar.gz"
        "luna-toolchain-0.2.0-windows-x86_64.zip")
    string(JSON toolchain_artifacts SET "${toolchain_artifacts}"
           "${asset}" "\"${fixture_digest}\"")
endforeach()
string(JSON ready_lock SET "${ready_lock}"
       components toolchain published_release artifacts
       "${toolchain_artifacts}")

string(JSON ready_lock SET "${ready_lock}"
       components lunax version "\"0.2.0\"")
string(JSON ready_lock SET "${ready_lock}"
       components lunax published_release tag "\"v0.2.0\"")
string(JSON ready_lock SET "${ready_lock}"
       components lunax published_release url
       "\"https://github.com/Luna-PL/Lunax/releases/tag/v0.2.0\"")
set(lunax_artifacts "{}")
foreach(asset IN ITEMS
        "LUNA-SOURCE-COMMIT"
        "LUNA-SOURCE-COMMIT.sha256"
        "lunax-0.2.0-ubuntu-24.04-x86_64.tar.gz"
        "lunax-0.2.0-ubuntu-24.04-x86_64.tar.gz.sha256"
        "lunax_0.2.0_amd64.deb"
        "lunax_0.2.0_amd64.deb.sha256")
    string(JSON lunax_artifacts SET "${lunax_artifacts}"
           "${asset}" "\"${fixture_digest}\"")
endforeach()
string(JSON ready_lock SET "${ready_lock}"
       components lunax published_release artifacts
       "${lunax_artifacts}")

set(ready_lock_path "${CMAKE_CURRENT_BINARY_DIR}/release-ready.lock.json")
file(WRITE "${ready_lock_path}" "${ready_lock}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DLUNA_SOURCE_DIR=${LUNA_SOURCE_DIR}
            -DECOSYSTEM_LOCK_PATH=${ready_lock_path}
            -DREQUIRE_READY=ON
            -P "${verifier}"
    RESULT_VARIABLE ready_result
    OUTPUT_VARIABLE ready_output
    ERROR_VARIABLE ready_error)
if(NOT ready_result EQUAL 0 OR
   NOT ready_output MATCHES "ecosystem is release-ready")
    message(FATAL_ERROR
        "two-phase ready snapshot was rejected:\n${ready_output}${ready_error}")
endif()

set(mismatched_commit "0000000000000000000000000000000000000000")
string(JSON mismatch_lock SET "${ready_lock}"
       components lunax verified_luna_source_commit "\"${mismatched_commit}\"")
set(mismatch_lock_path "${CMAKE_CURRENT_BINARY_DIR}/release-mismatch.lock.json")
file(WRITE "${mismatch_lock_path}" "${mismatch_lock}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DLUNA_SOURCE_DIR=${LUNA_SOURCE_DIR}
            -DECOSYSTEM_LOCK_PATH=${mismatch_lock_path}
            -DREQUIRE_READY=ON
            -P "${verifier}"
    RESULT_VARIABLE mismatch_result
    OUTPUT_VARIABLE mismatch_output
    ERROR_VARIABLE mismatch_error)
if(mismatch_result EQUAL 0 OR
   NOT mismatch_error MATCHES "verified against different Luna commits")
    message(FATAL_ERROR
        "mismatched candidate commits were not rejected:\n"
        "${mismatch_output}${mismatch_error}")
endif()

string(JSON stale_evidence_lock REMOVE "${ready_lock}"
       components toolchain published_release artifacts
       "LUNA-SOURCE-COMMIT")
set(stale_evidence_lock_path
    "${CMAKE_CURRENT_BINARY_DIR}/release-stale-evidence.lock.json")
file(WRITE "${stale_evidence_lock_path}" "${stale_evidence_lock}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DLUNA_SOURCE_DIR=${LUNA_SOURCE_DIR}
            -DECOSYSTEM_LOCK_PATH=${stale_evidence_lock_path}
            -DREQUIRE_READY=ON
            -P "${verifier}"
    RESULT_VARIABLE stale_evidence_result
    OUTPUT_VARIABLE stale_evidence_output
    ERROR_VARIABLE stale_evidence_error)
if(stale_evidence_result EQUAL 0 OR
   NOT stale_evidence_error MATCHES "artifact inventory")
    message(FATAL_ERROR
        "stale child-release evidence was not rejected:\n"
        "${stale_evidence_output}${stale_evidence_error}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD^
    WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
    RESULT_VARIABLE parent_result
    OUTPUT_VARIABLE parent_commit
    ERROR_VARIABLE parent_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT parent_result EQUAL 0)
    message(FATAL_ERROR "cannot resolve Luna HEAD parent: ${parent_error}")
endif()
set(stale_lock "${ready_lock}")
foreach(component IN ITEMS toolchain lunax)
    string(JSON stale_lock SET "${stale_lock}"
           components ${component} verified_luna_source_commit "\"${parent_commit}\"")
endforeach()
set(stale_lock_path "${CMAKE_CURRENT_BINARY_DIR}/release-stale.lock.json")
file(WRITE "${stale_lock_path}" "${stale_lock}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DLUNA_SOURCE_DIR=${LUNA_SOURCE_DIR}
            -DECOSYSTEM_LOCK_PATH=${stale_lock_path}
            -DREQUIRE_READY=ON
            -P "${verifier}"
    RESULT_VARIABLE stale_result
    OUTPUT_VARIABLE stale_output
    ERROR_VARIABLE stale_error)
if(stale_result EQUAL 0 OR
   NOT stale_error MATCHES "outside the promotion allowlist")
    message(FATAL_ERROR
        "post-candidate source changes were not rejected:\n"
        "${stale_output}${stale_error}")
endif()

message(STATUS "two-phase ecosystem release policy verified")
