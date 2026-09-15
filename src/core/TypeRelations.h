#pragma once

#include "TypeIdentity.h"

#include <string>

namespace luna::types {

// Canonical payloads are retained separately from their compact IDs. Future
// Moon container validation must compare/recompute the payload and must not
// trust a hash alone.
std::string canonicalShape(const TypePtr& type);
std::string canonicalType(const TypePtr& type);

ShapeId shapeId(const TypePtr& type);
TypeId typeId(const TypePtr& type);
ShapeId shapeIdFromCanonical(const std::string& canonical);
TypeId typeIdFromCanonical(const std::string& canonical);

bool sameType(const TypePtr& lhs, const TypePtr& rhs);
bool sameShape(const TypePtr& lhs, const TypePtr& rhs);
bool isAssignable(const TypePtr& from, const TypePtr& to);
bool isExplicitlyConvertible(const TypePtr& from, const TypePtr& to);

// This is a conservative, target-independent precursor to LayoutEngine. It
// only accepts identical language shapes; target ABI policy will refine it.
bool isAbiCompatible(const TypePtr& lhs, const TypePtr& rhs);
bool isRecursiveShape(const TypePtr& type);

// Every Value-domain constructor may contain only Value-domain arguments.
// Compiler type parameters are permitted as unresolved placeholders, but a
// concrete Meta/Compiler argument makes the instantiated Value type invalid.
bool isWellFormedTypeDomain(const TypePtr& type,
                            std::string* reason = nullptr);

} // namespace luna::types
