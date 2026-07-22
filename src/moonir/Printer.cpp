#include "Printer.h"

#include <ostream>
#include <sstream>

namespace moon {

namespace {

std::string typeName(const TypePtr& type) {
    return type ? type->toString() : "<missing>";
}

void printLocation(const SourceLocation& location, std::ostream& out) {
    if (location.path.empty()) return;
    out << " loc(\"" << location.path << "\":" << location.line
        << ':' << location.column << ')';
}

} // namespace

void Printer::print(const Module& module, std::ostream& out) const {
    out << "moon.module @" << module.name << " version "
        << module.formatMajor << '.' << module.formatMinor << " {\n";
    out << "  features [";
    bool first = true;
    auto feature = [&](const char* name, bool enabled) {
        if (!enabled) return;
        if (!first) out << ", ";
        first = false;
        out << name;
    };
    feature("runtime", module.features.runtime);
    feature("dynamic_reflection", module.features.dynamicReflection);
    feature("dynamic_apply", module.features.dynamicApply);
    feature("dynamic_select", module.features.dynamicSelect);
    feature("kernel", module.features.kernel);
    feature("kernel_runtime_reserved", module.features.kernelRuntimeReserved);
    out << "]\n";
    for (const auto& sourceModule : module.sourceModules)
        out << "  moon.source_module \""
            << (sourceModule.empty() ? "<root>" : sourceModule) << "\"\n";
    for (const auto& use : module.packageUses)
        out << "  moon.using \"" << use.packageId << "\" as " << use.alias << "\n";

    for (const auto& type : module.typeTable) {
        out << "  moon.type @" << type.id.value
            << " shape @" << type.shapeId.value
            << " domain " << typeDomainName(type.domain)
            << " identity " << identityModeName(type.identityMode)
            << " spelling \"" << type.displayName << '"';
        if (!type.nominalDeclarationId.empty())
            out << " nominal @" << type.nominalDeclarationId;
        out << "\n";
    }

    for (const auto& schema : module.metadataSchemas) {
        out << "  moon.meta @" << schema.id << " " << schema.name << " {";
        for (size_t index = 0; index < schema.fields.size(); ++index) {
            if (index) out << ", ";
            out << schema.fields[index].name << ": "
                << typeName(schema.fields[index].type);
        }
        out << "}";
        printLocation(schema.location, out);
        out << "\n";
    }

    for (const auto& record : module.declarationTable) {
        out << "  moon.decl @" << record.id << " family @" << record.familyId
            << " kind " << declarationKindName(record.kind)
            << " retention " << retentionName(record.retention);
        if (!record.linkageName.empty()) out << " linkage \"" << record.linkageName << '"';
        if (record.type) out << " : " << typeName(record.type);
        printLocation(record.location, out);
        out << "\n";
        for (const auto& metadata : record.metadata) {
            out << "    moon.attach @" << metadata.schemaId << " retention "
                << retentionName(metadata.retention) << " values "
                << metadata.values.size() << "\n";
        }
    }

    for (const auto& declaration : module.declarations) {
        if (!declaration) continue;
        if (auto* function = dynamic_cast<const FunctionDecl*>(declaration.get())) {
            out << "  moon.func @" << function->declarationId << " symbol \""
                << (function->generatedSymbolName.empty()
                        ? function->name : function->generatedSymbolName)
                << "\"(";
            for (size_t index = 0; index < function->params.size(); ++index) {
                if (index) out << ", ";
                out << luna::ownership::relationName(function->params[index].relation)
                    << ' ' << luna::ownership::usageName(function->params[index].usage)
                    << ' ' << function->params[index].name << ": "
                    << typeName(function->params[index].type);
            }
            out << ") -> owned " << luna::ownership::usageName(function->returnUsage)
                << ' ' << typeName(function->returnType);
            if (function->isKernel) out << " kernel";
            if (function->isKernel && !function->isCodegenReachable)
                out << " deferred_recipe";
            if (!function->typeParams.empty() && !function->isTemplateInstance)
                out << " generic_recipe";
            if (function->isTemplateInstance) out << " instantiation";
            printLocation(function->location, out);
            out << "\n";
        } else if (auto* fragment = dynamic_cast<const FragmentDecl*>(declaration.get())) {
            out << "  moon.fragment @" << fragment->declarationId << " symbol \""
                << (fragment->generatedSymbolName.empty()
                        ? fragment->name : fragment->generatedSymbolName)
                << "\" : " << typeName(fragment->structuralType);
            printLocation(fragment->location, out);
            out << "\n";
        }
    }
    out << "}\n";
}

std::string Printer::str(const Module& module) const {
    std::ostringstream out;
    print(module, out);
    return out.str();
}

void Printer::printCostReport(const Module& module, std::ostream& out) const {
    out << "MoonIR cost report for '" << module.name << "'\n";
    if (module.costs.empty()) {
        out << "  no optional runtime or code-generation costs\n";
        return;
    }
    for (const auto& item : module.costs) {
        out << "  [" << costKindName(item.kind) << "] " << item.subject;
        if (!item.reason.empty()) out << " — " << item.reason;
        if (!item.location.path.empty())
            out << " (" << item.location.path << ':' << item.location.line << ')';
        out << "\n";
    }
}

} // namespace moon
