#include "CGHelpers.h"
#include "../core/TypeLayout.h"

#include <algorithm>

CGHelpers::CGHelpers(llvm::LLVMContext& ctx) : mCtx(ctx) {}

llvm::Type* CGHelpers::toLLVMType(const TypePtr& type) const {
    if (!type) return voidTy();

    switch (type->kind) {
        case TypeKind::I8:    return llvm::Type::getInt8Ty(mCtx);
        case TypeKind::I16:   return llvm::Type::getInt16Ty(mCtx);
        case TypeKind::I32:   return i32Ty();
        case TypeKind::I64:   return i64Ty();
        case TypeKind::U8:    return llvm::Type::getInt8Ty(mCtx);
        case TypeKind::U16:   return llvm::Type::getInt16Ty(mCtx);
        case TypeKind::U32:   return i32Ty();
        case TypeKind::U64:   return i64Ty();
        case TypeKind::USize:
        case TypeKind::ISize: return sizeTy();
        case TypeKind::F32:   return f32Ty();
        case TypeKind::F64:   return f64Ty();
        case TypeKind::Bool:  return boolTy();
        case TypeKind::String:
        case TypeKind::CStr:
        case TypeKind::RawPointer:
        case TypeKind::Rc:
        case TypeKind::Arc:
        case TypeKind::DeviceBuffer:
        case TypeKind::Metadata:
        case TypeKind::MetadataView:
        case TypeKind::DeclarationView:
        case TypeKind::DeclarationRef: return ptrTy();
        case TypeKind::Iterator: return ptrTy();
        case TypeKind::Result: {
            const uint64_t valueSize = type->typeArgs.size() > 0
                ? luna::layout::valueSize(type->typeArgs[0]) : 0;
            const uint64_t errorSize = type->typeArgs.size() > 1
                ? luna::layout::valueSize(type->typeArgs[1]) : 0;
            const uint64_t words =
                std::max<uint64_t>(1, (std::max(valueSize, errorSize) + 7) / 8);
            return llvm::StructType::get(
                mCtx, {boolTy(), llvm::ArrayType::get(i64Ty(), words)});
        }
        case TypeKind::Enum: {
            const uint64_t words =
                std::max<uint64_t>(
                    1, luna::layout::enumPayloadSize(type) / 8);
            return llvm::StructType::get(
                mCtx, {i32Ty(), llvm::ArrayType::get(i64Ty(), words)});
        }
        case TypeKind::Array:
            return llvm::ArrayType::get(toLLVMType(type->inner), type->arrayLength);
        case TypeKind::Slice:
            return llvm::StructType::get(mCtx, {ptrTy(), sizeTy()});
        case TypeKind::Event: return i32Ty();
        case TypeKind::Unit:
        case TypeKind::Never: return voidTy();
        case TypeKind::Reference:
            return ptrTy();
        case TypeKind::Struct:
        case TypeKind::Record:
            return ptrTy();
        case TypeKind::Function:
            return ptrTy(); // function pointers are opaque ptrs
        default:
            return i32Ty(); // fallback
    }
}

uint64_t typeSize(const TypePtr& type) {
    if (!type) return 0;
    switch (type->kind) {
        case TypeKind::I8:
        case TypeKind::U8: return 1;
        case TypeKind::I16:
        case TypeKind::U16: return 2;
        case TypeKind::I32:   return 4;
        case TypeKind::I64:   return 8;
        case TypeKind::U32:   return 4;
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize: return 8;
        case TypeKind::F32:   return 4;
        case TypeKind::F64:   return 8;
        case TypeKind::Bool:  return 1;
        case TypeKind::String:
        case TypeKind::CStr: return 8; // pointer size
        case TypeKind::Reference:
        case TypeKind::RawPointer:
        case TypeKind::Rc:
        case TypeKind::Arc: return 8;
        case TypeKind::DeviceBuffer:
        case TypeKind::Metadata:
        case TypeKind::MetadataView:
        case TypeKind::DeclarationView:
        case TypeKind::DeclarationRef: return 8;
        case TypeKind::Iterator: return 8;
        case TypeKind::Array: return type->arrayLength * typeSize(type->inner);
        case TypeKind::Slice: return 16;
        case TypeKind::Event: return 4;
        case TypeKind::Struct:
        case TypeKind::Record: {
            uint64_t size = 0;
            for (auto& field : type->fields) size += typeSize(field.type);
            return size == 0 ? 8 : size;
        }
        case TypeKind::Enum:
            return luna::layout::valueSize(type);
        case TypeKind::Result: {
            return luna::layout::valueSize(type);
        }
        case TypeKind::Never: return 0;
        default: return 0;
    }
}

uint64_t typeAlignment(const TypePtr& type) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::I8:
        case TypeKind::U8:
        case TypeKind::Bool: return 1;
        case TypeKind::I16:
        case TypeKind::U16: return 2;
        case TypeKind::I32:
        case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Event: return 4;
        case TypeKind::Array: return typeAlignment(type->inner);
        case TypeKind::Struct:
        case TypeKind::Record: {
            uint64_t alignment = 1;
            for (const auto& field : type->fields)
                alignment = std::max(alignment, typeAlignment(field.type));
            return alignment;
        }
        case TypeKind::Unit:
        case TypeKind::Never: return 1;
        case TypeKind::Result:
        case TypeKind::Enum: return 8;
        default: return 8;
    }
}
