if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(expected_tbd_ids
    TBD-EV004
    TBD-Q004
    TBD-Q005
    TBD-SF006
)
list(SORT expected_tbd_ids)

function(verify_design_document relative_path language)
    set(path "${LUNA_SOURCE_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${language} Luna 0.3 design is missing: ${path}")
    endif()

    file(READ "${path}" text)
    string(REGEX MATCHALL "TBD-[A-Z]+[0-9]+" actual_tbd_ids "${text}")
    list(REMOVE_DUPLICATES actual_tbd_ids)
    list(SORT actual_tbd_ids)
    if(NOT actual_tbd_ids STREQUAL expected_tbd_ids)
        message(FATAL_ERROR
            "${language} Luna 0.3 TBD register differs from the frozen set.\n"
            "Expected: ${expected_tbd_ids}\n"
            "Actual:   ${actual_tbd_ids}")
    endif()

    string(FIND "${text}" "(Proposed" proposed_ascii)
    string(FIND "${text}" "（Proposed" proposed_full_width)
    if(NOT proposed_ascii EQUAL -1 OR NOT proposed_full_width EQUAL -1)
        message(FATAL_ERROR
            "${language} Luna 0.3 design contains an active Proposed marker; "
            "register its boundary explicitly before implementation")
    endif()
endfunction()

verify_design_document("docs/luna_0.3_design.md" "English")
verify_design_document("docs/luna_0.3_design.zh-CN.md" "Chinese")

file(READ "${LUNA_SOURCE_DIR}/docs/luna_0.3_design.md" english_design)
foreach(required_text IN ITEMS
        "No Proposed decisions are currently registered"
        "no `language = \"0.2\"`, edition, or compatibility mode is added"
        "`T003` (Confirmed): an artifact-producing `luna build` requires a `luna.package`"
        "`M005` (Confirmed, 2026-08-20): the 0.3 Moon Container uses the 8-byte magic"
        "`TY002` (Confirmed): anonymous records do not use a `record` keyword"
        "`Q005` (Confirmed): the first public compile-time function-query surface"
        "Priority item 6 has therefore passed its completion gate"
        "`US001` (Confirmed): explicit overrides are `copy let`, `affine let`, and `linear let`"
        "Priority item 7 has therefore passed its completion gate"
        "Luna does not introduce explicit effect annotations"
        "MoonIR is the single backend IR"
        "Luna has no general `unsafe {}`")
    string(FIND "${english_design}" "${required_text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "English Luna 0.3 design lost required decision text: ${required_text}")
    endif()
endforeach()

file(READ "${LUNA_SOURCE_DIR}/docs/luna_0.3_design.zh-CN.md" chinese_design)
foreach(required_text IN ITEMS
        "当前没有登记 Proposed 决定"
        "不增加 `language = \"0.2\"`、edition 或兼容模式"
        "`T003`（Confirmed）：产生正式产物的 `luna build` 必须以 `luna.package` 为输入"
        "`M005`（Confirmed，2026-08-20）：0.3 Moon Container 使用 8 字节 magic"
        "`TY002`（Confirmed）：匿名 record 不使用 `record` 关键字"
        "`Q005`（Confirmed）：首个公开 compile-time function query 表面"
        "因此优先级第 6 项已通过完成门"
        "`US001`（Confirmed）：显式覆盖语法为 `copy let`、`affine let` 和 `linear let`"
        "因此优先级第 7 项已通过完成门"
        "Luna 不引入显式 effect annotation"
        "MoonIR 是唯一且单层的后端 IR"
        "Luna 不提供能关闭内部类型、所有权或 Moon 验证的通用")
    string(FIND "${chinese_design}" "${required_text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "Chinese Luna 0.3 design lost required decision text: ${required_text}")
    endif()
endforeach()

file(READ "${LUNA_SOURCE_DIR}/docs/luna_0.3_evolution_audit.md" english_audit)
file(READ "${LUNA_SOURCE_DIR}/docs/luna_0.3_evolution_audit.zh-CN.md" chinese_audit)
string(FIND "${english_audit}" "superseded" english_superseded)
string(FIND "${chinese_audit}" "取代" chinese_superseded)
if(english_superseded EQUAL -1 OR chinese_superseded EQUAL -1)
    message(FATAL_ERROR
        "The historical evolution audits must remain explicitly subordinate/superseded")
endif()
