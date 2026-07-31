if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

find_package(Git REQUIRED)
set(guide "${LUNA_SOURCE_DIR}/docs/file_guide.md")
file(READ "${guide}" guide_text)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" ls-files
            --cached --others --exclude-standard
    WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE repository_files
    ERROR_VARIABLE git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT git_result EQUAL 0)
    message(FATAL_ERROR "cannot enumerate repository files: ${git_error}")
endif()

string(REPLACE "\r\n" "\n" repository_files "${repository_files}")
string(REPLACE "\n" ";" repository_files "${repository_files}")
list(SORT repository_files)
set(missing)
foreach(path IN LISTS repository_files)
    string(FIND "${guide_text}" "- `${path}`" found)
    if(found EQUAL -1)
        list(APPEND missing "${path}")
    endif()
endforeach()

if(missing)
    list(JOIN missing "\n  " missing_text)
    message(FATAL_ERROR
        "Repository files missing from docs/file_guide.md inventory:\n"
        "  ${missing_text}")
endif()

string(FIND "${guide_text}" "<!-- FILE_INVENTORY_BEGIN -->" begin)
string(FIND "${guide_text}" "<!-- FILE_INVENTORY_END -->" end)
if(begin EQUAL -1 OR end EQUAL -1 OR end LESS begin)
    message(FATAL_ERROR "file guide inventory markers are missing or reversed")
endif()
math(EXPR inventory_length "${end} - ${begin}")
string(SUBSTRING "${guide_text}" "${begin}" "${inventory_length}"
       inventory_text)
string(REGEX MATCHALL "- `[^`\n]+`" documented_entries
       "${inventory_text}")
set(documented_files)
foreach(entry IN LISTS documented_entries)
    string(REGEX REPLACE "^- `|`$" "" path "${entry}")
    list(APPEND documented_files "${path}")
endforeach()

set(documented_unique "${documented_files}")
list(REMOVE_DUPLICATES documented_unique)
list(LENGTH documented_files documented_count)
list(LENGTH documented_unique unique_count)
if(NOT documented_count EQUAL unique_count)
    message(FATAL_ERROR "docs/file_guide.md inventory contains duplicates")
endif()

set(stale)
foreach(path IN LISTS documented_files)
    list(FIND repository_files "${path}" found)
    if(found EQUAL -1)
        list(APPEND stale "${path}")
    endif()
endforeach()
if(stale)
    list(JOIN stale "\n  " stale_text)
    message(FATAL_ERROR
        "Stale paths in docs/file_guide.md inventory:\n"
        "  ${stale_text}")
endif()
