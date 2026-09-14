#include "Parser.h"
#include "../diagnostics/Diagnostic.h"

Parser::Parser(std::vector<Token> tokens, std::string sourceName, std::string source)
    : mTokens(std::move(tokens)), mSourceName(std::move(sourceName)), mSource(std::move(source)) {}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    if (check(TokenKind::Package)) {
        parsePackageHeader(program.get());
        program->isPackage = true;
    }
    if (check(TokenKind::Module)) parseModuleHeader(program.get());
    while (check(TokenKind::Using)) parseUsingHeader(program.get());
    while (!isAtEnd()) {
        const int declarationStart = mPos;
        if (auto decl = parseDeclaration()) {
            decl->modulePath = program->modulePath;
            program->declarations.push_back(std::move(decl));
        } else {
            // A malformed top-level declaration must not hide every later
            // error in the file. Advance to a statement terminator or the
            // next declaration introducer, then continue collecting errors.
            synchronizeDeclaration();
            if (mPos == declarationStart && !isAtEnd()) advance();
        }
    }
    return program;
}

bool Parser::parsePackageHeader(Program* program) {
    consume(TokenKind::Package, "Expected 'package'");
    if (!parsePackageId(program->packageName)) {
        addError("expected a package name, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `package com.example.my_package;`");
        return false;
    }
    consume(TokenKind::SemiColon, "Expected ';' after package name");
    return true;
}

bool Parser::parseModuleHeader(Program* program) {
    const Token start = consume(TokenKind::Module, "Expected 'module'");
    if (!parseModulePath(program->modulePath)) {
        addError("expected a module path, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `module io;` or `module io::format;`");
        return false;
    }
    consume(TokenKind::SemiColon, "Expected ';' after module path");
    (void)start;
    return true;
}

bool Parser::parseUsingHeader(Program* program) {
    const Token start = consume(TokenKind::Using, "Expected 'using'");
    Program::PackageUse use;
    use.sourcePath = mSourceName;
    use.line = start.line;
    use.col = start.col;
    if (!parsePackageId(use.packageId)) {
        addError("expected a Package ID after `using`",
                 "write `using org.example.library as library;`");
        return false;
    }
    if (!match(TokenKind::As)) {
        addError("package using declaration requires a local alias",
                 "append `as name`, for example `using org.luna.std as std;`");
        return false;
    }
    if (!match(TokenKind::Identifier)) {
        addError("expected an identifier after `as` in using declaration");
        return false;
    }
    use.alias = mTokens[mPos - 1].lexeme;
    consume(TokenKind::SemiColon, "Expected ';' after using declaration");
    program->packageUses.push_back(std::move(use));
    return true;
}

bool Parser::parsePackageId(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    while (match(TokenKind::Dot)) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a Package ID component after '.'");
            return false;
        }
        result += "." + mTokens[mPos - 1].lexeme;
    }
    return true;
}

bool Parser::parseModulePath(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    while (match(TokenKind::ColonColon)) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a module component after '::'");
            return false;
        }
        result += "::" + mTokens[mPos - 1].lexeme;
    }
    return true;
}

bool Parser::parseQualifiedName(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    parseQualifiedNameTail(result);
    return true;
}

void Parser::parseQualifiedNameTail(std::string& result) {
    while (check(TokenKind::ColonColon) &&
           peekAhead(1).kind == TokenKind::Identifier) {
        advance();
        advance();
        result += "::" + mTokens[mPos - 1].lexeme;
    }
}

std::unique_ptr<Decl> Parser::parseDeclaration() {
    const Token start = peek();
    bool isExported = false;
    bool isConstexpr = false;
    bool isExtern = false;
    bool isKernel = false;
    RetentionKind retention = RetentionKind::CompileTime;
    std::vector<Decl::MetadataAttachment> metadata;
    bool consumedModifier = true;
    while (consumedModifier) {
        consumedModifier = false;
        if (match(TokenKind::Export)) { isExported = true; consumedModifier = true; }
        else if (match(TokenKind::Constexpr)) { isConstexpr = true; consumedModifier = true; }
        else if (match(TokenKind::Extern)) { isExtern = true; consumedModifier = true; }
        else if (match(TokenKind::Kernel)) { isKernel = true; consumedModifier = true; }
        else if (match(TokenKind::Runtime)) {
            consumedModifier = true;
            if (check(TokenKind::At))
                metadata.push_back(parseMetadataAttachment(RetentionKind::Runtime));
            else retention = RetentionKind::Runtime;
        } else if (match(TokenKind::Dynamic)) {
            consumedModifier = true;
            addError("Dynamic retention was removed in Luna 0.3",
                     "use `runtime` retention and acquire typed bindings through the host evolution API");
            if (check(TokenKind::At))
                metadata.push_back(parseMetadataAttachment(RetentionKind::Runtime));
            else retention = RetentionKind::Runtime;
        } else if (check(TokenKind::At)) {
            consumedModifier = true;
            metadata.push_back(parseMetadataAttachment(RetentionKind::CompileTime));
        }
    }
    std::string abi;
    if (check(TokenKind::StringLiteral) && (isExtern || isExported))
        abi = advance().lexeme;
    if (isExtern && abi.empty()) abi = "C";
    std::unique_ptr<Decl> decl;
    if (match(TokenKind::Fn))
        decl = parseFunctionDecl(false, isExtern, abi, isConstexpr, isKernel);
    else if (match(TokenKind::Slot)) decl = parseSlotDecl();
    else if (match(TokenKind::Interceptor)) decl = parseFragmentDecl(FragmentKind::Interceptor);
    else if (match(TokenKind::Context)) decl = parseFragmentDecl(FragmentKind::Context);
    else if (match(TokenKind::Fragment)) {
        addError("`fragment` is ambiguous and is no longer accepted",
                 "use `interceptor name { ... }`, `context name { ... }`, or `context many name { ... }`");
        return nullptr;
    }
    else if (match(TokenKind::Struct)) decl = parseStructDecl();
    else if (match(TokenKind::Enum)) decl = parseEnumDecl();
    else if (match(TokenKind::Trait)) decl = parseTraitDecl();
    else if (match(TokenKind::Impl)) decl = parseImplDecl();
    else if (match(TokenKind::Meta)) decl = parseMetaDecl();
    else if (match(TokenKind::Constraint)) decl = parseConstraintDecl();
    else {
        if (isExported || isExtern || isConstexpr || isKernel) {
            addError("expected a declaration after `export`, found " +
                     diagnostic::quotedToken(peek().lexeme),
                     "only `fn`, `slot`, `interceptor`, `context`, `struct`, `enum`, "
                     "`trait`, `meta`, and `constraint` can be exported");
        } else {
            addError("expected a declaration, found " + diagnostic::quotedToken(peek().lexeme),
                     "start a declaration with `fn`, `slot`, `interceptor`, `context`, "
                     "`struct`, `enum`, `trait`, `impl`, `meta`, or `constraint`");
        }
        advance(); // skip unexpected token
        return nullptr;
    }
    if (decl) {
        // A runtime-visible attachment needs a descriptor to live on. Keep
        // that implication local to the attached declaration so one schema
        // does not make every use of it pay a runtime cost.
        for (const auto& attachment : metadata) {
            if (attachment.retention == RetentionKind::Runtime &&
                     retention == RetentionKind::CompileTime)
                retention = RetentionKind::Runtime;
        }
        decl->isExported = isExported;
        decl->retention = retention;
        decl->metadata = std::move(metadata);
        decl->sourcePath = mSourceName;
        decl->line = start.line;
        decl->col = start.col;
    }
    return decl;
}

Decl::MetadataAttachment Parser::parseMetadataAttachment(RetentionKind retention) {
    Decl::MetadataAttachment attachment;
    attachment.retention = retention;
    consume(TokenKind::At, "Expected '@' before metadata attachment");
    if (!match(TokenKind::Identifier)) {
        addError("expected a metadata schema name after `@`",
                 "declare it with `meta name { ... }` and attach it as `@name(...)`");
        return attachment;
    }
    attachment.schemaName = mTokens[mPos - 1].lexeme;
    consume(TokenKind::LParen, "Expected '(' after metadata schema name");
    if (!check(TokenKind::RParen)) attachment.arguments = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after metadata arguments");
    return attachment;
}

std::unique_ptr<MetaDecl> Parser::parseMetaDecl() {
    auto declaration = std::make_unique<MetaDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("expected a metadata type name after `meta`");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    declaration->name = nameToken.lexeme;
    declaration->nameLine = nameToken.line;
    declaration->nameCol = nameToken.col;
    consume(TokenKind::LBrace, "Expected '{' for metadata schema body");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a metadata field name");
            synchronizeStatement();
            continue;
        }
        MetaDecl::Field field;
        field.name = mTokens[mPos - 1].lexeme;
        consume(TokenKind::Colon, "Expected ':' after metadata field name");
        field.type = parseType();
        consume(TokenKind::SemiColon, "Expected ';' after metadata field type");
        declaration->fields.push_back(std::move(field));
    }
    consume(TokenKind::RBrace, "Expected '}' after metadata schema body");
    return declaration;
}

std::unique_ptr<ConstraintDecl> Parser::parseConstraintDecl() {
    auto declaration = std::make_unique<ConstraintDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("expected a constraint name after `constraint`");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    declaration->name = nameToken.lexeme;
    declaration->nameLine = nameToken.line;
    declaration->nameCol = nameToken.col;
    declaration->typeParams = parseTypeParamList();
    if (declaration->typeParams.empty())
        addError("constraint '" + declaration->name +
                 "' requires at least one type parameter");
    consume(TokenKind::Eq, "Expected '=' after constraint parameters");
    declaration->predicate = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after constraint predicate");
    return declaration;
}

std::unique_ptr<FragmentDecl> Parser::parseFragmentDecl(FragmentKind kind) {
    auto decl = std::make_unique<FragmentDecl>();
    decl->kind = kind;
    if (kind == FragmentKind::Context && match(TokenKind::Many)) {
        addError("`context many` is not part of Luna 0.3",
                 "use a single-shot `context`; multi-shot continuations are deferred");
        return nullptr;
    }
    else if (kind == FragmentKind::Interceptor && check(TokenKind::Many))
        addError("interceptor is always single-pass and cannot be `many`");
    if (!match(TokenKind::Identifier)) {
        addError("expected a fragment name, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `interceptor name(args) for slot_name { ... }` or `context name(args) for slot_name { ... }`");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    decl->name = nameToken.lexeme;
    decl->nameLine = nameToken.line;
    decl->nameCol = nameToken.col;
    if (match(TokenKind::LParen)) {
        decl->params = parseParams();
        consume(TokenKind::RParen, "Expected ')' after fragment parameters");
    }
    consume(TokenKind::For, "Expected `for` and a nominal slot target after fragment parameters");
    if (!parseQualifiedName(decl->targetSlotName)) {
        addError("expected a module-level slot name after `for`");
        return nullptr;
    }
    decl->body = parseBlock();
    return decl;
}

std::unique_ptr<SlotDecl> Parser::parseSlotDecl() {
    auto decl = std::make_unique<SlotDecl>();
    if (match(TokenKind::Interceptor)) {
        decl->acceptedKind = FragmentKind::Interceptor;
    } else if (match(TokenKind::Context)) {
        decl->acceptedKind = FragmentKind::Context;
        if (match(TokenKind::Many)) {
            addError("`slot context many` is not part of Luna 0.3",
                     "declare a single-shot `slot context`; multi-shot continuations are deferred");
            return nullptr;
        }
    } else {
        addError("slot must declare its single-shot control contract",
                 "write `slot interceptor name(...);` or `slot context name(...);`");
        return nullptr;
    }
    if (!match(TokenKind::Identifier)) {
        addError("expected a module-level slot name");
        return nullptr;
    }
    const auto& name = mTokens[mPos - 1];
    decl->name = name.lexeme;
    decl->nameLine = name.line;
    decl->nameCol = name.col;
    consume(TokenKind::LParen, "Expected '(' after module-level slot name");
    if (!check(TokenKind::RParen)) decl->params = parseParams();
    consume(TokenKind::RParen, "Expected ')' after slot parameters");
    if (match(TokenKind::Default)) {
        if (!parseQualifiedName(decl->defaultFragment)) {
            addError("expected a fragment name after `default`");
            return nullptr;
        }
    }
    consume(TokenKind::SemiColon, "Expected ';' after module-level slot declaration");
    return decl;
}

// ─── Function ──────────────────────────────────────────────────────

std::unique_ptr<FunctionDecl> Parser::parseFunctionDecl(bool isTraitMethod,
                                                         bool isExtern,
                                                         std::string abi,
                                                         bool isConstexpr,
                                                         bool isKernel) {
    auto decl = std::make_unique<FunctionDecl>();
    decl->isExtern = isExtern;
    decl->isConstexpr = isConstexpr;
    decl->isKernel = isKernel;
    decl->abi = std::move(abi);

    if (match(TokenKind::Identifier) || match(TokenKind::New)) {
        const auto& nameToken = mTokens[mPos - 1];
        decl->name = nameToken.lexeme;
        decl->nameLine = nameToken.line;
        decl->nameCol = nameToken.col;
        decl->sourcePath = mSourceName;
        decl->line = nameToken.line;
        decl->col = nameToken.col;
    } else {
        // Trait methods may have operator-style names; for simplicity just require IDENT
        addError("expected a function name, found " + diagnostic::quotedToken(peek().lexeme));
        return nullptr;
    }

    decl->typeParams = parseTypeParamList(&decl->whereClauses);
    consume(TokenKind::LParen, "Expected '(' after function name");
    decl->params = parseParams();
    consume(TokenKind::RParen, "Expected ')' after parameters");

    if (match(TokenKind::Arrow)) {
        decl->returnType = parseType();
        decl->returnsLinear = dynamic_cast<LinearTypeAST*>(decl->returnType.get()) != nullptr;
        decl->returnUsage = decl->returnsLinear
            ? luna::ownership::Usage::Linear
            : (dynamic_cast<AffineTypeAST*>(decl->returnType.get())
                ? luna::ownership::Usage::Affine
                : luna::ownership::Usage::Copy);
    }

    if (isExtern && match(TokenKind::As)) {
        if (match(TokenKind::StringLiteral)) {
            decl->linkName = mTokens[mPos - 1].lexeme;
        } else {
            addError("Expected string symbol name after 'as'");
        }
    }

    auto trailingWhereClauses = parseWhereClause();
    for (auto& clause : trailingWhereClauses)
        decl->whereClauses.push_back(std::move(clause));

    if (isTraitMethod) {
        consume(TokenKind::SemiColon, "Expected ';' after trait method signature");
    } else if (isExtern) {
        consume(TokenKind::SemiColon, "Expected ';' after extern function declaration");
    } else {
        decl->body = parseBlock();
    }

    return decl;
}

// ─── Struct ────────────────────────────────────────────────────────

std::unique_ptr<StructDecl> Parser::parseStructDecl() {
    auto decl = std::make_unique<StructDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected struct name");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    decl->name = nameToken.lexeme;
    decl->nameLine = nameToken.line;
    decl->nameCol = nameToken.col;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for struct body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected field name in struct");
            break;
        }
        Param field;
        const auto& fieldToken = mTokens[mPos - 1];
        field.name = fieldToken.lexeme;
        field.sourcePath = mSourceName;
        field.nameLine = fieldToken.line;
        field.nameCol = fieldToken.col;
        consume(TokenKind::Colon, "Expected ':' after field name");
        field.type = parseType();
        consume(TokenKind::SemiColon, "Expected ';' after field type");
        decl->fields.push_back(std::move(field));
    }
    consume(TokenKind::RBrace, "Expected '}' after struct body");
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    auto decl = std::make_unique<EnumDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected enum name");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    decl->name = nameToken.lexeme;
    decl->nameLine = nameToken.line;
    decl->nameCol = nameToken.col;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for enum body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected enum variant name");
            advance();
            continue;
        }
        EnumDecl::Variant variant;
        const auto& variantToken = mTokens[mPos - 1];
        variant.name = variantToken.lexeme;
        variant.sourcePath = mSourceName;
        variant.nameLine = variantToken.line;
        variant.nameCol = variantToken.col;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                do {
                    variant.fields.push_back(parseType());
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "Expected ')' after enum variant fields");
        }
        // Both semicolon and comma are accepted between variants. The
        // semicolon form matches struct declarations and is unambiguous.
        if (!match(TokenKind::SemiColon)) match(TokenKind::Comma);
        decl->variants.push_back(std::move(variant));
    }
    consume(TokenKind::RBrace, "Expected '}' after enum body");
    return decl;
}

// ─── Trait ─────────────────────────────────────────────────────────

std::unique_ptr<TraitDecl> Parser::parseTraitDecl() {
    auto decl = std::make_unique<TraitDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected trait name");
        return nullptr;
    }
    const auto& nameToken = mTokens[mPos - 1];
    decl->name = nameToken.lexeme;
    decl->nameLine = nameToken.line;
    decl->nameCol = nameToken.col;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for trait body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (match(TokenKind::Fn)) {
            auto method = parseFunctionDecl(true);
            if (method) {
                TraitDecl::MethodSig sig;
                sig.name = method->name;
                sig.returnType = std::move(method->returnType);
                for (auto& p : method->params)
                    sig.params.push_back(std::move(p));
                decl->methods.push_back(std::move(sig));
            }
        } else {
            addError("Expected 'fn' in trait body");
            advance();
        }
    }
    consume(TokenKind::RBrace, "Expected '}' after trait body");
    return decl;
}

// ─── Impl ──────────────────────────────────────────────────────────

std::unique_ptr<ImplDecl> Parser::parseImplDecl() {
    auto decl = std::make_unique<ImplDecl>();
    decl->typeParams = parseTypeParamList();

    if (!match(TokenKind::Identifier)) {
        addError("Expected trait name in impl");
        return nullptr;
    }
    decl->trait = parseTraitRef(mTokens[mPos - 1]);

    if (!match(TokenKind::For)) {
        addError("Expected 'for' after trait name in impl");
        return nullptr;
    }

    decl->targetType = parseType();
    consume(TokenKind::LBrace, "Expected '{' in impl");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (match(TokenKind::Fn)) {
            auto method = parseFunctionDecl(false);
            if (method) decl->methods.push_back(std::move(method));
        } else {
            addError("Expected 'fn' in impl body");
            advance();
        }
    }
    consume(TokenKind::RBrace, "Expected '}' after impl body");
    return decl;
}
