if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(workspace "${LUNA_BINARY_DIR}/core-surface-workspace")
file(REMOVE_RECURSE "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/stdlib/" DESTINATION "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/core_surface_app/"
     DESTINATION "${workspace}/app")
file(WRITE "${workspace}/luna.workspace"
"[workspace]\nmembers = [\"core\", \"std\", \"app\"]\n")
file(APPEND "${workspace}/luna.lock"
"\n[[package]]\nid = \"org.luna.fixture.core_surface\"\nversion = \"1.0.0\"\nsource = \"workspace:app\"\nhash = \"fixture-core-surface-v1\"\n")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${workspace}/app"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 42)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "materialized Core surface failed.\n"
        "Result: ${run_result}\n${run_output}\n${run_error}")
endif()
string(FIND "${run_output}"
       "74\n73\n71\n71\n72\n72\n181\n182\n183"
       resource_drop_at)
if(resource_drop_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Core Iterator move-only items were not cleaned exactly once on "
        "normal and returning paths.\n${run_output}\n${run_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${workspace}/app" -O0
    RESULT_VARIABLE aot_build_result
    OUTPUT_VARIABLE aot_build_output
    ERROR_VARIABLE aot_build_error
)
set(aot_executable
    "${workspace}/app/org.luna.fixture.core_surface")
if(WIN32)
    string(APPEND aot_executable ".exe")
endif()
if(NOT aot_build_result EQUAL 0 OR
   NOT EXISTS "${aot_executable}")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Core Iterator AOT build failed.\n"
        "${aot_build_output}\n${aot_build_error}")
endif()
execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
if(NOT aot_result EQUAL 42 OR
   NOT "${aot_output}" STREQUAL
       "74\n73\n71\n71\n72\n72\n181\n182\n183\n" OR
   NOT "${aot_error}" STREQUAL "")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Core Iterator JIT/AOT behavior diverged.\n"
        "Result: ${aot_result}\nstdout:\n${aot_output}\n"
        "stderr:\n${aot_error}")
endif()

# A same-shaped user trait must never be accepted as the compiler's Core
# iteration protocol.  This guards the stable trait identity boundary rather
# than only testing a method named `next`.
file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_surface;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"trait PretendIterator<Item> {\n"
"    fn next(iterator: &mut Self) -> core::option::Option<Item>;\n"
"}\n\n"
"nominal struct Pretend { value: i32; }\n\n"
"impl PretendIterator<i32> for Pretend {\n"
"    fn next(iterator: &mut Pretend) -> core::option::Option<i32> {\n"
"        return core::option::Option::<i32>::None();\n"
"    }\n"
"}\n\n"
"fn main() -> i32 {\n"
"    let value = new Pretend(1);\n"
"    for item in value { print(item); }\n"
"    return 0;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE pretend_result
    OUTPUT_VARIABLE pretend_output
    ERROR_VARIABLE pretend_error
)
set(pretend_transcript "${pretend_output}\n${pretend_error}")
string(FIND "${pretend_transcript}"
       "implements neither core::iter::Iterator nor"
       pretend_error_at)
if(NOT pretend_result EQUAL 1 OR pretend_error_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "same-shaped non-Core iterator trait crossed the protocol identity "
        "boundary.\n${pretend_transcript}")
endif()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_surface;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"nominal struct Counter { value: i32; }\n"
"impl core::iter::Iterator<i32> for Counter {\n"
"    fn next(iterator: &mut Counter) -> core::option::Option<i32> {\n"
"        return core::option::Option::<i32>::None();\n"
"    }\n"
"}\n"
"nominal struct Bag { value: i32; }\n"
"impl core::iter::IntoIterator<i32, Counter> for Bag {\n"
"    fn into_iter(value: affine Bag) -> affine Counter {\n"
"        let item: i32 = value.value;\n"
"        return new Counter(item);\n"
"    }\n"
"}\n"
"fn main() -> i32 {\n"
"    let bag = new Bag(1);\n"
"    for item in bag { print(item); }\n"
"    print(bag.value);\n"
"    return 0;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE consumed_result
    OUTPUT_VARIABLE consumed_output
    ERROR_VARIABLE consumed_error
)
set(consumed_transcript
    "${consumed_output}\n${consumed_error}")
string(FIND "${consumed_transcript}"
       "use after move"
       consumed_error_at)
if(NOT consumed_result EQUAL 1 OR
   consumed_error_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "implicit IntoIterator did not consume its source.\n"
        "${consumed_transcript}")
endif()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_surface;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"nominal struct MissingCollection { value: i32; }\n\n"
"fn main() -> i32 {\n"
"    let values = [1, 2];\n"
"    let missing = values.into_iter().collect::<MissingCollection>();\n"
"    return missing.value;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE missing_collect_result
    OUTPUT_VARIABLE missing_collect_output
    ERROR_VARIABLE missing_collect_error
)
set(missing_collect_transcript
    "${missing_collect_output}\n${missing_collect_error}")
string(FIND "${missing_collect_transcript}"
       "no coherent Core `FromIterator` implementation exists"
       missing_collect_error_at)
if(NOT missing_collect_result EQUAL 1 OR
   missing_collect_error_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "collect accepted a target without Core FromIterator.\n"
        "${missing_collect_transcript}")
endif()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_surface;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"nominal struct BoolBuilder { seen: bool; }\n"
"nominal struct BoolCollection { seen: bool; }\n\n"
"impl core::iter::FromIterator<bool, BoolBuilder> for BoolCollection {\n"
"    fn begin() -> affine BoolBuilder {\n"
"        return new BoolBuilder(false);\n"
"    }\n"
"    fn push(builder: &mut BoolBuilder, affine item: bool) -> unit {\n"
"        builder.seen = item;\n"
"    }\n"
"    fn finish(affine builder: BoolBuilder) -> affine BoolCollection {\n"
"        let seen = builder.seen;\n"
"        return new BoolCollection(seen);\n"
"    }\n"
"}\n\n"
"fn main() -> i32 {\n"
"    let values = [1, 2];\n"
"    let wrong = values.into_iter().collect::<BoolCollection>();\n"
"    if wrong.seen { return 1; }\n"
"    return 0;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE mismatch_collect_result
    OUTPUT_VARIABLE mismatch_collect_output
    ERROR_VARIABLE mismatch_collect_error
)
set(mismatch_collect_transcript
    "${mismatch_collect_output}\n${mismatch_collect_error}")
string(FIND "${mismatch_collect_transcript}"
       "collects 'bool', but this iterator yields 'i32'"
       mismatch_collect_error_at)
if(NOT mismatch_collect_result EQUAL 1 OR
   mismatch_collect_error_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "collect accepted a mismatched FromIterator item type.\n"
        "${mismatch_collect_transcript}")
endif()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_surface;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"nominal struct SumBuilder { total: i32; }\n"
"nominal struct CollectedSum { total: i32; }\n\n"
"impl core::iter::FromIterator<i32, SumBuilder> for CollectedSum {\n"
"    fn begin() -> affine SumBuilder { return new SumBuilder(0); }\n"
"    fn push(builder: &mut SumBuilder, affine item: i32) -> unit {\n"
"        builder.total += item;\n"
"    }\n"
"    fn finish(affine builder: SumBuilder) -> affine CollectedSum {\n"
"        let total = builder.total;\n"
"        return new CollectedSum(total);\n"
"    }\n"
"}\n\n"
"fn main() -> i32 {\n"
"    let values = [1, 2];\n"
"    values.into_iter().collect::<CollectedSum>();\n"
"    return 0;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE ignored_collect_result
    OUTPUT_VARIABLE ignored_collect_output
    ERROR_VARIABLE ignored_collect_error
)
set(ignored_collect_transcript
    "${ignored_collect_output}\n${ignored_collect_error}")
string(FIND "${ignored_collect_transcript}"
       "all move-only results require an explicit owner"
       ignored_collect_error_at)
if(NOT ignored_collect_result EQUAL 1 OR
   ignored_collect_error_at EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "collect allowed its affine result to be ignored.\n"
        "${ignored_collect_transcript}")
endif()
file(REMOVE_RECURSE "${workspace}")
