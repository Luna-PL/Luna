#include "driver/Repl.h"

#include "driver/ReplProtocol.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace luna::driver {
namespace {

using namespace repl_detail;

constexpr size_t MaxCachedTypeResults = 32;
constexpr size_t MaxCachedTypeResultBytes = 1024 * 1024;

std::string trim(std::string value) {
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    auto first = value.begin();
    while (first != value.end() && whitespace(static_cast<unsigned char>(*first)))
        ++first;
    auto last = value.end();
    while (last != first && whitespace(static_cast<unsigned char>(*(last - 1))))
        --last;
    return std::string(first, last);
}

void printReplHelp(std::ostream& output) {
    output << "Alpha REPL commands:\n"
              "  = <i32-expression>     evaluate an i32 expression\n"
              "  :type <expression>     show an expression's inferred type\n"
              "  :decl <declaration>    validate and persist one-line declarations\n"
              "  :paste decl|expr|stmt  read a multiline cell through :end\n"
              "  :load <path>           load a file as one declaration cell\n"
              "  :undo                  discard the last persisted declaration\n"
              "  :reset                 discard persisted declarations\n"
              "  :help                  show this support contract\n"
              "  :quit                  leave the REPL\n"
              "Other one-line input is executed as a statement in a temporary main.\n"
              "Declarations persist by source recompilation. Local variables, heap "
              "values, JIT globals,\n"
              "and runtime state do not persist. Multiline cells require explicit :paste.\n"
              "Every compiled cell uses a fresh time, memory, and output-bounded worker; one\n"
              "unused single-shot worker is prewarmed while the REPL waits for input.\n";
}

std::string statementProgram(const std::string& declarations, const std::string& statement) {
    std::string body = statement;
    const auto last = body.find_last_not_of(" \t\r\n");
    if (last != std::string::npos && body[last] != ';' && body[last] != '}') body += ';';
    return declarations + "\nfn main() -> i32 {\n" + body + "\nreturn 0;\n}\n";
}

} // namespace

const std::string& ReplSession::declarationsSource() const { return mDeclarationsSource; }

bool ReplSession::findCachedTypeResult(const std::string& source, std::string& type) const {
    for (auto entry = mTypeCache.rbegin(); entry != mTypeCache.rend(); ++entry) {
        if (entry->source != source) continue;
        type = entry->type;
        return true;
    }
    return false;
}

void ReplSession::rememberTypeResult(const std::string& source, const std::string& type) const {
    const size_t entryBytes = source.size() + type.size();
    if (entryBytes > MaxCachedTypeResultBytes) return;
    while (!mTypeCache.empty() && (mTypeCache.size() >= MaxCachedTypeResults ||
                                   mTypeCacheBytes > MaxCachedTypeResultBytes - entryBytes)) {
        mTypeCacheBytes -= mTypeCache.front().source.size() + mTypeCache.front().type.size();
        mTypeCache.erase(mTypeCache.begin());
    }
    mTypeCache.push_back({source, type});
    mTypeCacheBytes += entryBytes;
}

void ReplSession::clearTypeCache() {
    mTypeCache.clear();
    mTypeCacheBytes = 0;
}

std::string ReplSession::nextVirtualPath() { return "<repl:" + std::to_string(mNextCell++) + ">"; }

bool ReplSession::storeDeclaration(const std::string& declaration, const std::string& virtualPath,
                                   std::ostream& output, std::ostream& errors) {
    if (declaration.empty()) {
        errors << "error[repl]: declaration cell must not be empty\n";
        return false;
    }
    if (mDeclarationBytes + declaration.size() > MaxReplDeclarationBytes) {
        errors << "error[repl]: persisted declarations exceed the 8 MiB session limit\n";
        return false;
    }
    const std::string source =
        declarationsSource() + declaration + "\nfn main() -> i32 { return 0; }\n";
    int ignoredResult = 0;
    std::string ignoredPayload;
    if (!runInWorker(source, virtualPath, "validate", ignoredResult, ignoredPayload, output,
                     errors))
        return false;
    mDeclarationBytes += declaration.size();
    mDeclarations.push_back(declaration);
    mDeclarationsSource += declaration;
    mDeclarationsSource += '\n';
    clearTypeCache();
    output << "declaration stored\n";
    return true;
}

bool ReplSession::evaluateExpression(const std::string& expression, const std::string& virtualPath,
                                     std::ostream& output, std::ostream& errors) const {
    if (expression.empty()) {
        errors << "error[repl]: '=' requires an i32 expression\n";
        return false;
    }
    const std::string source =
        declarationsSource() + "\nfn main() -> i32 {\nreturn " + expression + ";\n}\n";
    int result = 0;
    if (!compileAndRun(source, virtualPath, result, output, errors)) return false;
    output << "= " << result << '\n';
    return true;
}

bool ReplSession::executeStatement(const std::string& statement, const std::string& virtualPath,
                                   std::ostream& output, std::ostream& errors) const {
    if (statement.empty()) {
        errors << "error[repl]: statement cell must not be empty\n";
        return false;
    }
    int result = 0;
    if (!compileAndRun(statementProgram(declarationsSource(), statement), virtualPath, result,
                       output, errors))
        return false;
    output << (result == 0 ? "ok" : "exit " + std::to_string(result)) << '\n';
    return true;
}

bool ReplSession::inspectExpressionType(const std::string& expression,
                                        const std::string& virtualPath, std::ostream& output,
                                        std::ostream& errors) const {
    if (expression.empty()) {
        errors << "error[repl]: :type requires an expression\n";
        return false;
    }
    const std::string probeName = "__luna_repl_type_probe";
    const std::string source = declarationsSource() + "\nfn main() -> i32 {\nlet " + probeName +
                               " = " + expression + ";\nreturn 0;\n}\n";
    int ignoredResult = 0;
    std::string inferredType;
    if (!runInWorker(source, virtualPath, "type", ignoredResult, inferredType, output, errors))
        return false;
    output << "= " << inferredType << '\n';
    return true;
}

bool ReplSession::compileAndRun(const std::string& source, const std::string& virtualPath,
                                int& result, std::ostream& output, std::ostream& errors) const {
    std::string ignoredPayload;
    return runInWorker(source, virtualPath, "run", result, ignoredPayload, output, errors);
}

int ReplSession::run(std::istream& input, std::ostream& output, std::ostream& errors) {
    startPreparingWorker();
    output << "Luna Alpha REPL — type :help for the support contract\n";

    std::string line;
    while (true) {
        if (mOptions.showPrompts) output << "luna> " << std::flush;
        if (!std::getline(input, line)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string inputLine = trim(line);
        if (inputLine.empty()) continue;
        if (inputLine.size() > MaxReplCellBytes) {
            errors << "error[repl]: input exceeds the 1 MiB cell limit\n";
            continue;
        }

        if (inputLine == "exit") break;
        if (inputLine.front() == ':') {
            const size_t separator = inputLine.find_first_of(" \t");
            const std::string command = inputLine.substr(0, separator);
            const std::string argument =
                separator == std::string::npos ? "" : trim(inputLine.substr(separator + 1));
            if (command == ":quit") {
                if (!argument.empty()) {
                    errors << "error[repl]: :quit does not accept arguments\n";
                    continue;
                }
                break;
            }
            if (command == ":help") {
                if (!argument.empty())
                    errors << "error[repl]: :help does not accept arguments\n";
                else
                    printReplHelp(output);
                continue;
            }
            if (command == ":reset") {
                if (!argument.empty()) {
                    errors << "error[repl]: :reset does not accept arguments\n";
                    continue;
                }
                mDeclarations.clear();
                mDeclarationsSource.clear();
                mDeclarationBytes = 0;
                clearTypeCache();
                output << "declarations reset\n";
                continue;
            }
            if (command == ":undo") {
                if (!argument.empty()) {
                    errors << "error[repl]: :undo does not accept arguments\n";
                    continue;
                }
                if (mDeclarations.empty()) {
                    errors << "error[repl]: there is no declaration to undo\n";
                    continue;
                }
                mDeclarationBytes -= mDeclarations.back().size();
                mDeclarationsSource.resize(mDeclarationsSource.size() -
                                           mDeclarations.back().size() - 1);
                mDeclarations.pop_back();
                clearTypeCache();
                output << "last declaration discarded\n";
                continue;
            }
            if (command == ":decl") {
                if (argument.empty()) {
                    errors << "error[repl]: :decl requires a declaration\n";
                    continue;
                }
                storeDeclaration(argument, nextVirtualPath(), output, errors);
                continue;
            }
            if (command == ":type") {
                inspectExpressionType(argument, nextVirtualPath(), output, errors);
                continue;
            }
            if (command == ":load") {
                if (argument.empty()) {
                    errors << "error[repl]: :load requires a source path\n";
                    continue;
                }
                std::ifstream sourceFile(argument, std::ios::binary);
                if (!sourceFile) {
                    errors << "error[repl]: cannot read source file '" << argument << "'\n";
                    continue;
                }
                std::ostringstream source;
                source << sourceFile.rdbuf();
                if (sourceFile.bad()) {
                    errors << "error[repl]: failed while reading source file '" << argument
                           << "'\n";
                    continue;
                }
                std::string declaration = source.str();
                if (declaration.size() > MaxReplCellBytes) {
                    errors << "error[repl]: input exceeds the 1 MiB cell limit\n";
                    continue;
                }
                declaration = trim(std::move(declaration));
                storeDeclaration(declaration, argument, output, errors);
                continue;
            }
            if (command == ":paste") {
                if (argument != "decl" && argument != "expr" && argument != "stmt") {
                    errors << "error[repl]: :paste requires decl, expr, or stmt\n";
                    continue;
                }
                std::string cell;
                bool complete = false;
                bool limitExceeded = false;
                while (true) {
                    if (mOptions.showPrompts) output << "....> " << std::flush;
                    std::string pastedLine;
                    if (!std::getline(input, pastedLine)) break;
                    if (!pastedLine.empty() && pastedLine.back() == '\r') pastedLine.pop_back();
                    if (trim(pastedLine) == ":end") {
                        complete = true;
                        break;
                    }
                    if (cell.size() + pastedLine.size() + 1 > MaxReplCellBytes) {
                        if (!limitExceeded)
                            errors << "error[repl]: input exceeds the 1 MiB cell limit\n";
                        limitExceeded = true;
                        continue;
                    }
                    if (!limitExceeded) {
                        cell += pastedLine;
                        cell += '\n';
                    }
                }
                if (!complete) {
                    errors << "error[repl]: multiline cell ended before :end\n";
                    break;
                }
                if (limitExceeded) continue;
                cell = trim(std::move(cell));
                const std::string virtualPath = nextVirtualPath();
                if (argument == "decl")
                    storeDeclaration(cell, virtualPath, output, errors);
                else if (argument == "expr")
                    evaluateExpression(cell, virtualPath, output, errors);
                else
                    executeStatement(cell, virtualPath, output, errors);
                continue;
            }
            errors << "error[repl]: unknown command '" << command
                   << "'; use :help to list commands\n";
            continue;
        }

        const std::string virtualPath = nextVirtualPath();
        if (inputLine.front() == '=') {
            const std::string expression = trim(inputLine.substr(1));
            evaluateExpression(expression, virtualPath, output, errors);
            continue;
        }

        executeStatement(inputLine, virtualPath, output, errors);
    }
    return 0;
}

void printReplUsage(std::ostream& output) {
    output << "Usage: luna repl [options]\n\n"
              "Options:\n"
              "  -O0, -O2, -O3          select the optimization level\n"
              "  --opt O0|O2|O3         long optimization-level spelling\n"
              "  --link <library>       load a shared library for JIT lookup\n"
              "  --timeout <seconds>    limit each execution (1..3600; default: 30)\n"
              "  --memory-limit <MiB>   limit each worker (256..65536; default: 1024)\n"
              "  --output-limit <MiB>   cap each worker's output (1..1024; default: 16)\n"
              "  --timings              report per-cell compiler and execution timings\n"
              "  --no-prompt            suppress prompts for scripted input\n"
              "  --help                 show this help\n\n";
    printReplHelp(output);
}

int runRepl(std::istream& input, std::ostream& output, std::ostream& errors, ReplOptions options) {
    return ReplSession(std::move(options)).run(input, output, errors);
}

} // namespace luna::driver
