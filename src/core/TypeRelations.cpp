#include "TypeRelations.h"

#include "TypeSystem.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace luna::types {

namespace {

void appendPart(std::string& output, const std::string& part) {
    output += std::to_string(part.size());
    output += ':';
    output += part;
    output += ';';
}

uint64_t stableHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string compactId(const char* prefix, const std::string& canonical) {
    std::ostringstream output;
    output << prefix << std::hex << std::setw(16) << std::setfill('0')
           << stableHash(canonical);
    return output.str();
}

const char* domainName(TypeDomain domain) {
    switch (domain) {
        case TypeDomain::Value: return "value";
        case TypeDomain::Meta: return "meta";
        case TypeDomain::Compiler: return "compiler";
        case TypeDomain::Inference: return "inference";
        case TypeDomain::Error: return "error";
    }
    return "invalid";
}

const char* kindName(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8: return "i8";
        case TypeKind::I16: return "i16";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::U8: return "u8";
        case TypeKind::U16: return "u16";
        case TypeKind::U32: return "u32";
        case TypeKind::U64: return "u64";
        case TypeKind::USize: return "usize";
        case TypeKind::ISize: return "isize";
        case TypeKind::F32: return "f32";
        case TypeKind::F64: return "f64";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::CStr: return "cstr";
        case TypeKind::RawPointer: return "raw";
        case TypeKind::Unit: return "unit";
        case TypeKind::Never: return "never";
        case TypeKind::Struct: return "product";
        case TypeKind::Record: return "product";
        case TypeKind::Enum: return "sum";
        case TypeKind::Result: return "result";
        case TypeKind::Trait: return "trait";
        case TypeKind::TypeParam: return "type_param";
        case TypeKind::Reference: return "reference";
        case TypeKind::Function: return "function";
        case TypeKind::Closure: return "closure";
        case TypeKind::Slot: return "slot";
        case TypeKind::Fragment: return "fragment";
        case TypeKind::Iterator: return "iterator";
        case TypeKind::DeviceBuffer: return "device_buffer";
        case TypeKind::Event: return "event";
        case TypeKind::Array: return "array";
        case TypeKind::Slice: return "slice";
        case TypeKind::Metadata: return "metadata";
        case TypeKind::MetadataView: return "metadata_view";
        case TypeKind::SymbolSet: return "symbol_set";
        case TypeKind::DeclarationView: return "declaration_view";
        case TypeKind::DeclarationRef: return "declaration_ref";
        case TypeKind::InferenceVar: return "inference_var";
        case TypeKind::Unknown: return "unknown";
    }
    return "invalid";
}

std::string canonicalShapeImpl(
    const Type* type,
    std::unordered_map<const Type*, size_t>& active,
    size_t& nextAnchor) {
    if (!type) return "missing";
    auto recursive = active.find(type);
    if (recursive != active.end()) return "ref@" + std::to_string(recursive->second);

    const size_t anchor = nextAnchor++;
    active.emplace(type, anchor);
    std::string result = "node@" + std::to_string(anchor) + "{";
    appendPart(result, domainName(type->domain));
    appendPart(result, kindName(type->kind));

    auto appendType = [&](const TypePtr& child) {
        appendPart(result, canonicalShapeImpl(child.get(), active, nextAnchor));
    };

    switch (type->kind) {
        case TypeKind::Struct:
        case TypeKind::Record:
        case TypeKind::Metadata: {
            std::vector<const TypeField*> fields;
            fields.reserve(type->fields.size());
            for (const auto& field : type->fields) fields.push_back(&field);
            std::sort(fields.begin(), fields.end(),
                      [](const TypeField* lhs, const TypeField* rhs) {
                          return lhs->name < rhs->name;
                      });
            for (const auto* field : fields) {
                appendPart(result, field->name);
                appendType(field->type);
            }
            break;
        }
        case TypeKind::Enum:
            for (const auto& variant : type->variants) {
                appendPart(result, variant.name);
                for (const auto& field : variant.fields) appendType(field);
            }
            break;
        case TypeKind::Result:
            for (const auto& argument : type->typeArgs) appendType(argument);
            break;
        case TypeKind::Reference:
            appendPart(result, type->isMutable ? "mutable" : "shared");
            appendType(type->inner);
            break;
        case TypeKind::RawPointer:
        case TypeKind::DeviceBuffer:
        case TypeKind::Slice:
        case TypeKind::MetadataView:
        case TypeKind::SymbolSet:
        case TypeKind::DeclarationView:
        case TypeKind::DeclarationRef:
        case TypeKind::Iterator:
            appendType(type->inner);
            if (type->kind == TypeKind::Iterator) {
                appendPart(result, std::to_string(
                    static_cast<unsigned>(type->iteratorMode)));
                // Iterator recipes may carry a representation-bearing source
                // type in typeArgs. Process-local Type pointer identity used
                // to distinguish these accidentally; canonical MoonIR must
                // make every backend-significant edge part of stable shape.
                for (const auto& argument : type->typeArgs)
                    appendType(argument);
            }
            break;
        case TypeKind::Array:
            appendPart(result, std::to_string(type->arrayLength));
            appendType(type->inner);
            break;
        case TypeKind::Function:
        case TypeKind::Slot:
        case TypeKind::Fragment:
        case TypeKind::Closure:
            if (type->kind == TypeKind::Slot || type->kind == TypeKind::Fragment) {
                appendPart(result, type->isMultiShot ? "many" : "once");
                appendPart(result,
                    type->continuationKind == ContinuationKind::Interceptor
                        ? "interceptor" : "context");
            }
            for (size_t index = 0; index < type->paramTypes.size(); ++index) {
                const luna::ownership::Contract contract = index < type->paramContracts.size()
                    ? type->paramContracts[index] : luna::ownership::Contract{};
                appendPart(result, std::string(luna::ownership::relationName(contract.relation)));
                appendPart(result, std::string(luna::ownership::usageName(contract.usage)));
                appendType(type->paramTypes[index]);
            }
            appendPart(result, std::string(luna::ownership::relationName(type->returnContract.relation)));
            appendPart(result, std::string(luna::ownership::usageName(type->returnContract.usage)));
            appendType(type->returnType);
            if (type->kind == TypeKind::Closure) {
                for (const auto& field : type->capturedFields) {
                    appendPart(result, field.name);
                    appendType(field.type);
                }
            }
            break;
        case TypeKind::TypeParam:
            appendPart(result, type->name);
            break;
        case TypeKind::InferenceVar:
            appendPart(result, std::to_string(type->inferenceId));
            break;
        default:
            break;
    }
    result += '}';
    active.erase(type);
    return result;
}

std::string canonicalIdentityImpl(const TypePtr& type) {
    if (!type) return "missing";
    std::string result;
    appendPart(result, domainName(type->domain));
    appendPart(result, kindName(type->kind));
    if (type->identityMode == IdentityMode::Nominal ||
        type->identityMode == IdentityMode::MetaSchema) {
        appendPart(result, type->nominalId);
        for (const auto& argument : type->typeArgs)
            appendPart(result, canonicalIdentityImpl(argument));
        return result;
    }
    if (type->identityMode == IdentityMode::Builtin ||
        type->identityMode == IdentityMode::CompilerIntrinsic ||
        type->identityMode == IdentityMode::Inference ||
        type->identityMode == IdentityMode::Error) {
        if (type->kind == TypeKind::InferenceVar)
            appendPart(result, std::to_string(type->inferenceId));
        if (type->kind == TypeKind::TypeParam)
            appendPart(result, type->name);
        if ((type->kind == TypeKind::MetadataView ||
             type->kind == TypeKind::SymbolSet ||
             type->kind == TypeKind::DeclarationView ||
             type->kind == TypeKind::DeclarationRef) && type->inner)
            appendPart(result, canonicalIdentityImpl(type->inner));
        if (type->kind == TypeKind::Iterator)
            appendPart(result, canonicalShape(type));
        return result;
    }
    // Structural composite types (Function, Closure, Slot, Fragment, Array,
    // Record, Enum, Reference, RawPointer, DeviceBuffer, Slice) fall
    // through to here. Their identity must distinguish nominal children:
    // fn(First) and fn(Second) are different types even when First and Second
    // share the same structural shape. Use canonicalIdentityImpl for the
    // composite's children so nominal TypeIds propagate into the parent.
    if (type->kind == TypeKind::Function ||
        type->kind == TypeKind::Closure) {
        for (size_t index = 0; index < type->paramTypes.size(); ++index) {
            const luna::ownership::Contract contract =
                index < type->paramContracts.size()
                    ? type->paramContracts[index] : luna::ownership::Contract{};
            appendPart(result, std::string(luna::ownership::relationName(contract.relation)));
            appendPart(result, std::string(luna::ownership::usageName(contract.usage)));
            appendPart(result, canonicalIdentityImpl(type->paramTypes[index]));
        }
        appendPart(result, std::string(luna::ownership::relationName(
            type->returnContract.relation)));
        appendPart(result, std::string(luna::ownership::usageName(
            type->returnContract.usage)));
        appendPart(result, canonicalIdentityImpl(type->returnType));
        if (type->kind == TypeKind::Closure) {
            for (const auto& field : type->capturedFields) {
                appendPart(result, field.name);
                appendPart(result, canonicalIdentityImpl(field.type));
            }
        }
        return result;
    }
    if (type->kind == TypeKind::Array && type->inner) {
        appendPart(result, std::to_string(type->arrayLength));
        appendPart(result, canonicalIdentityImpl(type->inner));
        return result;
    }
    if (type->kind == TypeKind::Reference && type->inner) {
        appendPart(result, type->isMutable ? "mutable" : "shared");
        appendPart(result, canonicalIdentityImpl(type->inner));
        return result;
    }
    if (type->kind == TypeKind::RawPointer && type->inner) {
        appendPart(result, canonicalIdentityImpl(type->inner));
        return result;
    }
    if (type->kind == TypeKind::Record) {
        for (const auto& field : type->fields) {
            appendPart(result, field.name);
            appendPart(result, canonicalIdentityImpl(field.type));
        }
        return result;
    }
    if (type->kind == TypeKind::Result) {
        for (const auto& argument : type->typeArgs)
            appendPart(result, canonicalIdentityImpl(argument));
        return result;
    }
    if (type->kind == TypeKind::Enum) {
        for (const auto& variant : type->variants) {
            appendPart(result, variant.name);
            for (const auto& field : variant.fields)
                appendPart(result, canonicalIdentityImpl(field));
        }
        return result;
    }
    if (type->kind == TypeKind::DeviceBuffer && type->inner) {
        appendPart(result, canonicalIdentityImpl(type->inner));
        return result;
    }
    appendPart(result, canonicalShape(type));
    return result;
}

bool containsType(const Type* current, const Type* target,
                  std::unordered_set<const Type*>& visited) {
    if (!current) return false;
    if (!visited.insert(current).second) return false;
    auto contains = [&](const TypePtr& child) {
        return child &&
            (child.get() == target || containsType(child.get(), target, visited));
    };
    if (contains(current->inner) || contains(current->returnType)) return true;
    for (const auto& argument : current->typeArgs)
        if (contains(argument)) return true;
    for (const auto& parameter : current->paramTypes)
        if (contains(parameter)) return true;
    for (const auto& field : current->fields)
        if (contains(field.type)) return true;
    for (const auto& variant : current->variants)
        for (const auto& field : variant.fields)
            if (contains(field)) return true;
    return false;
}

} // namespace

std::string canonicalShape(const TypePtr& type) {
    std::unordered_map<const Type*, size_t> active;
    size_t nextAnchor = 0;
    return canonicalShapeImpl(type.get(), active, nextAnchor);
}

std::string canonicalType(const TypePtr& type) {
    return canonicalIdentityImpl(type);
}

ShapeId shapeId(const TypePtr& type) {
    return shapeIdFromCanonical(canonicalShape(type));
}

TypeId typeId(const TypePtr& type) {
    return typeIdFromCanonical(canonicalType(type));
}

ShapeId shapeIdFromCanonical(const std::string& canonical) {
    return {compactId("shape_", canonical)};
}

TypeId typeIdFromCanonical(const std::string& canonical) {
    return {compactId("type_", canonical)};
}

bool sameType(const TypePtr& lhs, const TypePtr& rhs) {
    if (!lhs || !rhs) return !lhs && !rhs;
    return canonicalType(lhs) == canonicalType(rhs);
}

bool sameShape(const TypePtr& lhs, const TypePtr& rhs) {
    if (!lhs || !rhs) return !lhs && !rhs;
    return canonicalShape(lhs) == canonicalShape(rhs);
}

bool isAssignable(const TypePtr& from, const TypePtr& to) {
    return sameType(from, to);
}

bool isExplicitlyConvertible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || from->domain != TypeDomain::Value ||
        to->domain != TypeDomain::Value)
        return false;
    return sameType(from, to) || sameShape(from, to);
}

bool isAbiCompatible(const TypePtr& lhs, const TypePtr& rhs) {
    if (!lhs || !rhs || lhs->domain != TypeDomain::Value ||
        rhs->domain != TypeDomain::Value)
        return false;
    return sameShape(lhs, rhs);
}

bool isRecursiveShape(const TypePtr& type) {
    if (!type) return false;
    std::unordered_set<const Type*> visited;
    return containsType(type.get(), type.get(), visited);
}

} // namespace luna::types
