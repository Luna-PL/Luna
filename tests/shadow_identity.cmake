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
if(NOT around_contract STREQUAL reject_contract)
    message(FATAL_ERROR
        "same-shaped context contracts did not share one shadow ContractId")
endif()

string(FIND "${first_moon}" "moon.sysmeta v1.3" sysmeta_at)
if(sysmeta_at EQUAL -1)
    message(FATAL_ERROR "MoonIR did not carry the identity-aware sysmeta schema")
endif()
