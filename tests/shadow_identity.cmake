cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()
if(NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_BINARY_DIR must point at the build tree")
endif()

set(work_dir "${LUNA_BINARY_DIR}/shadow_identity")
file(MAKE_DIRECTORY "${work_dir}")
set(source "${LUNA_SOURCE_DIR}/tests/fixtures/fragment_contracts.luna")
set(first "${work_dir}/first.moon")
set(second "${work_dir}/second.moon")

foreach(output IN ITEMS "${first}" "${second}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" check "${source}"
                --emit-moonir "${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "cannot emit shadow identity evidence: ${stderr}${stdout}")
    endif()
endforeach()

file(READ "${first}" first_moon)
file(READ "${second}" second_moon)
if(NOT first_moon STREQUAL second_moon)
    message(FATAL_ERROR "shadow identity generation is not deterministic")
endif()

string(REGEX MATCH "moon.type @[^\n]+ abi_layout @abi_[0-9a-f]+"
       type_identity "${first_moon}")
if(type_identity STREQUAL "")
    message(FATAL_ERROR "MoonIR type table has no shadow AbiLayoutId")
endif()

string(REGEX MATCH
       "moon.decl @[^\n]*fragment::around[^\n]* symbol @(symbol_[0-9a-f]+) contract @(contract_[0-9a-f]+)"
       around_record "${first_moon}")
set(around_symbol "${CMAKE_MATCH_1}")
set(around_contract "${CMAKE_MATCH_2}")
string(REGEX MATCH
       "moon.decl @[^\n]*fragment::reject[^\n]* symbol @(symbol_[0-9a-f]+) contract @(contract_[0-9a-f]+)"
       reject_record "${first_moon}")
set(reject_symbol "${CMAKE_MATCH_1}")
set(reject_contract "${CMAKE_MATCH_2}")

if(around_record STREQUAL "" OR reject_record STREQUAL "")
    message(FATAL_ERROR
        "MoonIR declaration table lost shadow SymbolId/ContractId evidence")
endif()
if(around_symbol STREQUAL reject_symbol)
    message(FATAL_ERROR "distinct fragments share one shadow SymbolId")
endif()
if(around_contract STREQUAL reject_contract)
    message(FATAL_ERROR
        "fragments targeting distinct nominal slots share one ContractId")
endif()

# A declaration's strong identity must not depend on whether another overload
# later causes its executable linkage to become isolated. Compare two complete
# frontend/lowering snapshots instead of only exercising the identity helper.
set(overload_base_source "${work_dir}/overload-base.luna")
set(overload_extended_source "${work_dir}/overload-extended.luna")
set(overload_base_moon "${work_dir}/overload-base.moon")
set(overload_extended_moon "${work_dir}/overload-extended.moon")
file(WRITE "${overload_base_source}" [=[
meta revision { major: i32; }

@revision(3)
fn route(value: i32) -> i32 {
    return value * 3;
}

fn main() -> i32 {
    return route(14);
}
]=])
file(WRITE "${overload_extended_source}" [=[
meta revision { major: i32; }

@revision(3)
fn route(value: i32) -> i32 {
    return value * 3;
}

@revision(3)
fn route() -> i32 {
    return 5;
}

fn main() -> i32 {
    let selected = symbols::<(i32) -> i32>(route)
        .matching(revision(3)).one();
    return selected(14);
}
]=])

foreach(snapshot IN ITEMS base extended)
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" check
                "${overload_${snapshot}_source}"
                --emit-moonir "${overload_${snapshot}_moon}"
        RESULT_VARIABLE overload_result
        OUTPUT_VARIABLE overload_stdout
        ERROR_VARIABLE overload_stderr
    )
    if(NOT overload_result EQUAL 0)
        message(FATAL_ERROR
            "cannot emit ${snapshot} overload identity evidence: "
            "${overload_stderr}${overload_stdout}")
    endif()
    file(READ "${overload_${snapshot}_moon}" overload_text)
    string(REPLACE "\n" ";" overload_lines "${overload_text}")
    set(overload_unary_row "")
    foreach(overload_line IN LISTS overload_lines)
        string(FIND "${overload_line}" "moon.decl @main::fn::route" route_at)
        string(FIND "${overload_line}" "1:013:default-usage" unary_at)
        if(NOT route_at EQUAL -1 AND NOT unary_at EQUAL -1)
            set(overload_unary_row "${overload_line}")
        endif()
    endforeach()
    if(overload_unary_row STREQUAL "")
        message(FATAL_ERROR
            "${snapshot} snapshot has no unary route declaration row")
    endif()
    string(REGEX MATCH "moon.decl @([^ ]+) family"
           overload_declaration_match "${overload_unary_row}")
    set(overload_${snapshot}_declaration "${CMAKE_MATCH_1}")
    string(REGEX MATCH "symbol @(symbol_[0-9a-f]+)"
           overload_symbol_match "${overload_unary_row}")
    set(overload_${snapshot}_symbol "${CMAKE_MATCH_1}")
    string(REGEX MATCH "contract @(contract_[0-9a-f]+)"
           overload_contract_match "${overload_unary_row}")
    set(overload_${snapshot}_contract "${CMAKE_MATCH_1}")
endforeach()

foreach(identity_kind IN ITEMS declaration symbol contract)
    if(NOT overload_base_${identity_kind} STREQUAL
           overload_extended_${identity_kind})
        message(FATAL_ERROR
            "unary route ${identity_kind} identity changed after adding an "
            "overload: base=${overload_base_${identity_kind}}, "
            "extended=${overload_extended_${identity_kind}}")
    endif()
endforeach()

string(FIND "${first_moon}" "moon.sysmeta v1.3" sysmeta_at)
if(sysmeta_at EQUAL -1)
    message(FATAL_ERROR "MoonIR did not carry the identity-aware sysmeta schema")
endif()
