// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "core/graph/basic_types.h"
#include "core/providers/coreml/builders/op_builder.h"

namespace onnxruntime {

class GraphViewer;
class NodeArg;
class Node;

namespace logging {
class Logger;
}

namespace coreml {

OpBuilderInputParams MakeOpBuilderParams(const GraphViewer& graph_viewer,
                                         int32_t coreml_version,
                                         uint32_t coreml_flags);

const IOpBuilder* GetOpBuilder(const Node& node);

bool IsInputSupported(const Node& node, const NodeArg& node_arg, const OpBuilderInputParams& input_params,
                      const logging::Logger& logger,
                      bool allow_empty_input = false);

bool IsNodeSupported(const Node& node, const OpBuilderInputParams& input_params, const logging::Logger& logger);

// Gets the set of nodes that are supported by the CoreML EP.
std::unordered_set<const Node*> GetSupportedNodes(const GraphViewer& graph_viewer,
                                                  const OpBuilderInputParams& input_params,
                                                  const logging::Logger& logger);

bool CheckIsConstantInitializer(const NodeArg& node_arg, const GraphViewer& graph_viewer,
                                const logging::Logger& logger, std::string_view input_description);

// CoreML is more efficient running using Apple Neural Engine
// This is to detect if the current system has Apple Neural Engine
bool HasNeuralEngine(const logging::Logger& logger);

}  // namespace coreml
}  // namespace onnxruntime
