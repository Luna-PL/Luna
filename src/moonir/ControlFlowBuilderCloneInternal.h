#pragma once

#include "MoonIR.h"

#include <memory>
#include <string>
#include <vector>

namespace moon::control_flow_builder_detail {

std::unique_ptr<BlockStmt> cloneStructuredBlock(const BlockStmt* source);

std::unique_ptr<Stmt> rewriteCaptureReadsStmt(
    std::unique_ptr<Stmt> statement,
    const std::vector<std::string>& captures,
    const LocalId& environmentLocal,
    const TypeRef& closureType,
    const Module& module);

void resetStructureDepthLimit();
bool structureDepthExceeded();

} // namespace moon::control_flow_builder_detail
