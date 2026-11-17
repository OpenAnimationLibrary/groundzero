#pragma once

#include "typetooltext.h"
#include "tvectorimage.h"

#ifdef TNZTOOLS_EXPORTS
#define TYPE_TOOL_VECTOR_API DV_EXPORT_API
#else
#define TYPE_TOOL_VECTOR_API DV_IMPORT_API
#endif

namespace TypeToolText {

// Runs inspection against the same native document that produces the outlines.
TYPE_TOOL_VECTOR_API OutlineLayout inspectOutlineLayout(
    const TypeToolTextEntry& entry, const LayoutInspection& inspection);
TYPE_TOOL_VECTOR_API TVectorImageP
vectorizeOutlineLayout(const OutlineLayout& layout, double scale, int styleId);

using VectorInspection = std::function<void(const TVectorImageP&)>;
// Inspection runs after region discovery and before recursive native fill
// painting.
TYPE_TOOL_VECTOR_API TVectorImageP
vectorizeOutlineLayout(const OutlineLayout& layout, double scale, int styleId,
                       const VectorInspection& inspection);

}  // namespace TypeToolText

#undef TYPE_TOOL_VECTOR_API
