#include "driver/Repl.h"

#include "driver/CompilerPipeline.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace luna::driver {
namespace {

void printReplHelp(std::ostream& output) {
    output <<
        "Alpha REPL commands:\n"
        "  = <i32-expression>     evaluate an i32 expression\n"
        "  :decl <declaration>    validate and persist one-line declarations\n"
        "  :reset                 discard persisted declarations\n"
        "  :help                  show this support contract\n"
        "  :quit                  leave the REPL\n"
        "Other one-line input is executed as a statement in a temporary main.\n"
        "Declarations persist by source recompilation. Local variables, heap "
        "values, JIT globals,\n"
        "and runtime state do not persist. Multiline input is not supported.\n";
}

void printPipelineErrors(
    const CompilerPipeline& pipeline, std::ostream& errors) {
    for (const auto& error : pipeline.errors()) {
        if (!pipeline.errorStage().empty())
            errors << "error[" << pipeline.errorStage() << "]: ";
        errors << error << '\n';
    }
}

bool compileAndRun(
    const std::string& source, int& result, std::ostream& errors) {
    CompilerPipeline pipeline;
    if (!pipeline.compileSourceToMoonIR(source, "<repl>")) {
        printPipelineErrors(pipeline, errors);
        return false;
    }
    if (!pipeline.generateCode({})) {
        printPipelineErrors(pipeline, errors);
        return false;
    }
    result = pipeline.codeGenerator().jitRun();
    std::fflush(stdout);
    return true;
}

std::string statementProgram(
    const std::string& declarations, const std::string& statement) {
    std::string body = statement;
    const auto last = body.find_last_not_of(" \t\r\n");
    if (last != std::string::npos &&
        body[last] != ';' && body[last] != '}')
        body += ';';
    return declarations + "\nfn main() -> i32 {\n" +
        body + "\nreturn 0;\n}\n";
}

} // namespace

int runRepl(
    std::istream& input, std::ostream& output, std::ostream& errors) {
    output << "Luna Alpha REPL — type :help for the support contract\n";

    std::string declarations;
    std::string line;
    while (true) {
        output << "luna> " << std::flush;
        if (!std::getline(input, line)) break;
        if (line.empty()) continue;
        if (line == ":quit" || line == "exit") break;
        if (line == ":help") {
            printReplHelp(output);
            continue;
        }
        if (line == ":reset") {
            declarations.clear();
            output << "declarations reset\n";
            continue;
        }
        if (line.rfind(":decl ", 0) == 0) {
            const std::string candidate =
                declarations + "\n" + line.substr(6) + "\n";
            CompilerPipeline pipeline;
            if (!pipeline.compileSourceToMoonIR(candidate, "<repl>")) {
                printPipelineErrors(pipeline, errors);
                continue;
            }
            declarations = candidate;
            output << "declaration stored\n";
            continue;
        }
        if (line.front() == '=') {
            const std::string expression = line.substr(1);
            const std::string source =
                declarations + "\nfn main() -> i32 {\nreturn " +
                expression + ";\n}\n";
            int result = 0;
            if (compileAndRun(source, result, errors))
                output << "= " << result << '\n';
            continue;
        }

        int result = 0;
        if (compileAndRun(
                statementProgram(declarations, line), result, errors))
            output << (result == 0 ? "ok" : "exit " + std::to_string(result))
                   << '\n';
    }
    return 0;
}

} // namespace luna::driver
