if(NOT DEFINED LUNA_EXECUTABLE OR NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_EXECUTABLE and LUNA_SOURCE_DIR are required")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" analyze
            "${LUNA_SOURCE_DIR}/examples/generic.luna"
            --message-format=json
    RESULT_VARIABLE analysis_result
    OUTPUT_VARIABLE analysis_output
    ERROR_VARIABLE analysis_error
)
if(NOT analysis_result EQUAL 0 OR NOT analysis_error STREQUAL "")
    message(FATAL_ERROR
        "analysis protocol invocation failed\n${analysis_output}\n${analysis_error}")
endif()

string(REPLACE "\n" ";" records "${analysis_output}")
list(FILTER records EXCLUDE REGEX "^$")
list(LENGTH records record_count)
if(record_count LESS 3)
    message(FATAL_ERROR "expected hello, symbols, summary; got: ${analysis_output}")
endif()

list(GET records 0 hello)
math(EXPR summary_index "${record_count} - 1")
list(GET records ${summary_index} summary)
string(JSON hello_protocol GET "${hello}" protocol)
string(JSON hello_version GET "${hello}" version)
string(JSON hello_kind GET "${hello}" kind)
string(JSON summary_kind GET "${summary}" kind)
string(JSON summary_symbols GET "${summary}" symbols)
string(JSON summary_references GET "${summary}" references)
string(JSON summary_complete GET "${summary}" complete)
set(observed_symbols 0)
set(observed_references 0)
set(reference "")
foreach(index RANGE 1 ${summary_index})
    if(index EQUAL summary_index)
        break()
    endif()
    list(GET records ${index} record)
    string(JSON kind GET "${record}" kind)
    if(kind STREQUAL "symbol")
        math(EXPR observed_symbols "${observed_symbols} + 1")
    elseif(kind STREQUAL "reference")
        math(EXPR observed_references "${observed_references} + 1")
        set(reference "${record}")
    else()
        message(FATAL_ERROR "unexpected analysis record: ${record}")
    endif()
endforeach()
if(NOT hello_protocol STREQUAL "luna.analysis" OR
   NOT hello_version EQUAL 1 OR
   NOT hello_kind STREQUAL "hello" OR
   NOT summary_kind STREQUAL "summary" OR
   NOT summary_symbols EQUAL observed_symbols OR
   NOT summary_references EQUAL observed_references OR
   observed_references LESS 1 OR
   NOT summary_complete)
    message(FATAL_ERROR "invalid analysis protocol envelope: ${analysis_output}")
endif()

list(GET records 1 symbol)
string(JSON symbol_kind GET "${symbol}" kind)
string(JSON symbol_id GET "${symbol}" id)
string(JSON symbol_byte GET "${symbol}" selection start byte)
if(NOT symbol_kind STREQUAL "symbol" OR
   NOT symbol_id MATCHES "^luna.symbol.v1" OR
   symbol_byte LESS 0)
    message(FATAL_ERROR "invalid analysis symbol: ${symbol}")
endif()

string(JSON reference_kind GET "${reference}" kind)
string(JSON reference_target GET "${reference}" target_id)
string(JSON reference_byte GET "${reference}" source start byte)
if(NOT reference_kind STREQUAL "reference" OR
   NOT reference_target MATCHES "^luna.symbol.v1" OR
   reference_byte LESS 0)
    message(FATAL_ERROR "invalid analysis reference: ${reference}")
endif()
