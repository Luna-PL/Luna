if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(expected_tbd_ids TBD-SF007 TBD-SF008 TBD-SF009 TBD-SF010)
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
    if(NOT "${actual_tbd_ids}" STREQUAL "${expected_tbd_ids}")
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

set(expected_release_decisions
    RLS001 RLS002 RLS003 RLS004 RLS005 RLS006 RLS007 RLS008)
foreach(release_document IN ITEMS
        "docs/ecosystem_release.md"
        "docs/ecosystem_release.zh-CN.md")
    file(READ "${LUNA_SOURCE_DIR}/${release_document}" release_text)
    string(REGEX MATCHALL "RLS[0-9]+" actual_release_decisions
           "${release_text}")
    list(REMOVE_DUPLICATES actual_release_decisions)
    list(SORT actual_release_decisions)
    if(NOT "${actual_release_decisions}" STREQUAL
           "${expected_release_decisions}")
        message(FATAL_ERROR
            "${release_document} release handoff register drifted.\n"
            "Expected: ${expected_release_decisions}\n"
            "Actual:   ${actual_release_decisions}")
    endif()
endforeach()

file(READ "${LUNA_SOURCE_DIR}/docs/luna_0.3_design.md" english_design)
foreach(required_text IN ITEMS
        "No Proposed decisions are currently registered"
        "no `language = \"0.2\"`, edition, or compatibility mode is added"
        "`T003` (Confirmed): an artifact-producing `luna build` requires a `luna.package`"
        "`M005` (Confirmed, 2026-08-20): the 0.3 Moon Container uses the 8-byte magic"
        "`TY002` (Confirmed): anonymous records do not use a `record` keyword"
        "`Q005` (Confirmed): the first public compile-time function-query surface"
        "`Q006` (Confirmed): `.optional()` returns the compiler-domain specialization"
        "`Q004` (Confirmed, 2026-08-26): `.all()` returns a compiler-domain"
        "`Q007` (Confirmed, 2026-08-27): `symbols(Name)` infers the declaration kind"
        "`SF006` (Confirmed, 2026-08-29): a slot is declared at module scope"
        "`EV004` (Confirmed, 2026-08-30): 0.3 adds no Luna source construct"
        "Priority item 15 passed its completion gate on 2026-08-30"
        "Priority item 16 passed its completion gate on 2026-08-30"
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
        "`Q006`（Confirmed）：`.optional()` 返回 compiler-domain specialization"
        "`Q004`（Confirmed，2026-08-26）：`.all()` 返回 compiler-domain"
        "`Q007`（Confirmed，2026-08-27）：`symbols(Name)` 为具有公开 source name"
        "`SF006`（Confirmed，2026-08-29）：slot 使用模块级声明"
        "`EV004`（Confirmed，2026-08-30）：0.3 不增加 evolution 的 Luna 源码构造"
        "优先级第 15 项已于 2026-08-30 通过完成门"
        "优先级第 16 项已于 2026-08-30 通过完成门"
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
