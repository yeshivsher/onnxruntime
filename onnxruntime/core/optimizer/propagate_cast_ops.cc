// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/optimizer/initializer.h"
#include "core/optimizer/propagate_cast_ops.h"
#include "core/graph/graph_utils.h"
#include "core/optimizer/utils.h"
#include <deque>

using namespace ONNX_NAMESPACE;
using namespace onnxruntime::common;
namespace onnxruntime {

std::vector<std::string> fp16_allow = {"Transpose", "Reshape", "Gather", "Split", "Relu", "Where", "Dropout"};
std::vector<std::string> fp16_safe = { "LayerNorm", "Gelu", "FastGelu", "Tanh", "MatMul", "MatAdd", "Add",
                                       "Sub", "Mul", "Div", "Neg", "Gemm", "FusedMatMul", "FusedGemm"};

// Insert a Cast node after each NodeArg
static Status InsertCastNodes(Graph& graph, const std::set<NodeArg*>& require_cast, bool is_fp16, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  (void) removed_nodes;

  //Create requirred new Cast nodes.
  for (NodeArg* node_arg : require_cast) {
    if (!node_arg->Exists()) {
      continue;
    }
    // data_type is the data type of the Cast output.
    TensorProto_DataType data_type = is_fp16 ? TensorProto_DataType_FLOAT16 : TensorProto_DataType_FLOAT;
    TypeProto type_proto;
    bool is_node_arg_cast_output = node_arg->TypeAsProto()->tensor_type().elem_type() == data_type;
    TensorProto_DataType new_node_arg_data_type = data_type;;
    if (is_node_arg_cast_output) {
      new_node_arg_data_type = (data_type == TensorProto_DataType_FLOAT) ? TensorProto_DataType_FLOAT16 : TensorProto_DataType_FLOAT;
    }
    type_proto.mutable_tensor_type()->set_elem_type(new_node_arg_data_type);
    NodeArg& new_node_arg = graph.GetOrCreateNodeArg(graph.GenerateNodeArgName(node_arg->Name()), &type_proto);
    // Expect that a NodeArg is not both a graph input as well as a graph output
    ORT_ENFORCE(!(graph.IsInputsIncludingInitializers(node_arg) && graph.IsOutput(node_arg)));
    NodeArg& cast_input = !is_node_arg_cast_output ? *node_arg : new_node_arg;
    NodeArg& cast_output = is_node_arg_cast_output ? *node_arg : new_node_arg;
    const std::vector<NodeArg*> cast_inputs = {&cast_input};
    const std::vector<NodeArg*> cast_outputs = {&cast_output};
    ONNX_NAMESPACE::AttributeProto to_attribute;
    to_attribute.set_name("to");
    to_attribute.set_type(ONNX_NAMESPACE::AttributeProto::INT);
    to_attribute.set_i(static_cast<int64_t>(data_type));
    NodeAttributes attributes({{"to", to_attribute}});

    Node& cast = graph.AddNode(graph.GenerateNodeName(node_arg->Name() + "_cast"),
                               "Cast",
                               "Created a new Cast node",
                               cast_inputs,
                               cast_outputs,
                               &attributes);
    Node* producer = graph.GetMutableProducerNode(node_arg->Name());
    std::vector<Node*> consumers = graph.GetMutableConsumerNodes(node_arg->Name());
    int output_index = (producer != nullptr) ? optimizer_utils::IndexOfNodeOutput(*producer, *node_arg) : -1;
    // Update consumers of node_arg to use the output of the cast node
    int cast_output_index = optimizer_utils::IndexOfNodeOutput(cast, cast_output);
    for (Node* consumer : graph.GetMutableConsumerNodes(node_arg->Name())) {
      if (consumer != nullptr) {
        auto& consumer_inputs = consumer->MutableInputDefs();
        int input_index = optimizer_utils::IndexOfNodeInput(*consumer, *node_arg);
        if (producer) {
          graph.RemoveEdge(producer->Index(), consumer->Index(), output_index, input_index);
        }
        std::replace(consumer_inputs.begin(), consumer_inputs.end(), &cast_input, &cast_output);
        graph.AddEdge(cast.Index(), consumer->Index(), cast_output_index, input_index);
      }
    }
    if (producer != nullptr) {
      auto& producer_outputs = producer->MutableOutputDefs();
      std::replace(producer_outputs.begin(), producer_outputs.end(), node_arg, &cast_input);
      graph.UpdateProducerNode(cast_input.Name(), producer->Index());
      int input_index = optimizer_utils::IndexOfNodeInput(cast, cast_input);
      graph.AddEdge(producer->Index(), cast.Index(), output_index, input_index);
    }
    graph.UpdateProducerNode(cast_output.Name(), cast.Index());
  }
  return Status::OK();
}

static Status RemoveCastNodes(Graph& graph, std::vector<Node*> casts, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  (void) removed_nodes;
  ORT_ENFORCE(casts.size()>0);
  Node* lead_cast = casts.front();
  Node* trail_cast = casts.back();
  NodeArg* cast_input = lead_cast->MutableInputDefs()[0];
  NodeArg* cast_output = trail_cast->MutableOutputDefs()[0];
  // Update producer node
  Node* producer = graph.GetMutableProducerNode(cast_input->Name());
  auto consumers = graph.GetMutableConsumerNodes(cast_output->Name());
  int output_index = (producer != nullptr) ? optimizer_utils::IndexOfNodeOutput(*producer, *cast_input) : -1;
  if (producer) {
    auto& outputs = producer->MutableOutputDefs();
    int input_index = optimizer_utils::IndexOfNodeInput(*lead_cast, *cast_input);
    graph.RemoveEdge(producer->Index(), lead_cast->Index(), output_index, input_index);
    std::replace(outputs.begin(), outputs.end(), cast_input, cast_output);
    graph.UpdateProducerNode(cast_output->Name(), producer->Index());
  }
  // Update consumer nodes
  if (consumers.size()>0) {
    int cast_output_index = optimizer_utils::IndexOfNodeOutput(*trail_cast, *cast_output);
    for (Node* consumer : consumers) {
      auto& consumer_inputs = consumer->MutableInputDefs();
      int input_index = optimizer_utils::IndexOfNodeInput(*consumer, *cast_output);
      graph.RemoveEdge(trail_cast->Index(), consumer->Index(), cast_output_index, input_index);
      std::replace(consumer_inputs.begin(), consumer_inputs.end(), cast_output, cast_input);
      if (producer) {
        graph.AddEdge(producer->Index(), consumer->Index(), output_index, input_index);
      }
    }
    graph.UpdateConsumerNodes(cast_input->Name(), consumers);
  }
  for (auto cast : casts) {
    graph_utils::RemoveNodeOutputEdges(graph, *cast);
    graph.RemoveNode(cast->Index());
  }
  return Status::OK();
}

static bool RemoveBackToBackCasts(Graph& graph, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  bool modified = false;
  for (Node& node : graph.Nodes()) {
    if (node.OpType() == "Cast") {
      const NodeAttributes& attributes = node.GetAttributes();
      ORT_ENFORCE(attributes.find("to") != attributes.end());
      bool is_fp = attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT);
      bool is_fp16 = attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16);
      for (NodeArg* cast_output : node.MutableOutputDefs()) {
        for (Node* child : graph.GetMutableConsumerNodes(cast_output->Name())) {
          if (child->OpType() == "Cast") {
            const NodeAttributes& child_attributes = child->GetAttributes();
            ORT_ENFORCE(child_attributes.find("to") != child_attributes.end());
            bool is_child_fp = child_attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT);
            bool is_child_fp16 = child_attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16);
            if ((is_fp && is_child_fp16) || (is_fp16 && is_child_fp)) {
              // The parent and child cancell out
              RemoveCastNodes(graph, {&node, child}, removed_nodes);
              modified = true;
            } else if ((is_fp16 && is_child_fp16) || (is_fp && is_child_fp)) {
              // Child is a duplicate of parent
              RemoveCastNodes(graph, {child}, removed_nodes);
              modified = true;
            }
          }
        }
      }
    }
  }
  return modified;
}
// SearchUpstream:
// Recursively traverse the graph upstream collecting all the NodeArgs that require a cast
// inorder to remove an FP16 Cast operation down the graph.
static void SearchUpstream(Graph& graph, NodeArg* node_arg, std::set<NodeArg*>& require_cast)
{
  Node* node = graph.GetMutableProducerNode(node_arg->Name());
  if (node == nullptr) {
    // The graph inputs don't have the producer nodes
    require_cast.insert(node_arg);
  } else {
    std::string op_type = node->OpType();
    if (std::find(fp16_allow.begin(), fp16_allow.end(), op_type) == fp16_allow.end() &&
        std::find(fp16_safe.begin(), fp16_safe.end(), op_type) == fp16_safe.end()) {
      require_cast.insert(node_arg);
    } else {
      for (NodeArg* node_input : node->MutableInputDefs()) {
        SearchUpstream(graph, node_input, require_cast);
      }
    }
  }
}

// SearchDownstream:
// Recursively traverse the graph downstream collecting all the NodeArgs that require a cast
// inorder to remove an FP32 Cast operation up the graph.
static void SearchDownstream(Graph& graph, NodeArg* node_arg, std::set<NodeArg*>& require_cast)
{
  for (Node* node : graph.GetMutableConsumerNodes(node_arg->Name())) {
    if (node) {
      std::string op_type = node->OpType();
      if (std::find(fp16_allow.begin(), fp16_allow.end(), op_type) == fp16_allow.end()) {
        require_cast.insert(node_arg);
      } else {
        for (NodeArg* node_output : node->MutableOutputDefs()) {
          SearchDownstream(graph, node_output, require_cast);
        }
      }
    }
  }
}

static bool PropagateForwards(Graph& graph, Node* node, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  bool modified = false;
  if (node == nullptr) {
    return false;
  }
  if (node->OpType() == "Cast") {
    const NodeAttributes& attributes = node->GetAttributes();
    ORT_ENFORCE(attributes.find("to") != attributes.end());
    if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
      std::set<NodeArg*> require_cast;
      NodeArg* cast_output = node->MutableOutputDefs()[0];
      SearchDownstream(graph, cast_output, require_cast);
      if (require_cast.size() > 0 && require_cast.find(cast_output) == require_cast.end()) {
        // Remove Cast operation
        RemoveCastNodes(graph,{node}, removed_nodes);
        InsertCastNodes(graph, require_cast, false, removed_nodes);
        modified = true;
      }
    }
  } else if (std::find(fp16_safe.begin(), fp16_safe.end(), node->OpType()) != fp16_safe.end()) {
    bool all_inputs_have_casts = true;
    for (NodeArg* input : node->MutableInputDefs()) {
      Node* producer = graph.GetMutableProducerNode(input->Name());
      if (producer && producer->OpType() == "Cast") {
        const NodeAttributes& attributes = producer->GetAttributes();
        ORT_ENFORCE(attributes.find("to") != attributes.end());
        if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
          continue;
        }
      }
      all_inputs_have_casts = false;
      break;
    }
    if (all_inputs_have_casts) {
      for (NodeArg* input : node->MutableInputDefs()) {
        Node* producer = graph.GetMutableProducerNode(input->Name());
        RemoveCastNodes(graph, {producer}, removed_nodes);
      }
      NodeArg* node_arg = node->MutableOutputDefs()[0];
      InsertCastNodes(graph, {node_arg}, false, removed_nodes);
      modified = true;
    }
  } else {
    for (NodeArg* output: node->MutableOutputDefs()) {
      for (Node* consumer : graph.GetMutableConsumerNodes(output->Name())) {
        modified |= PropagateForwards(graph, consumer, removed_nodes);
      }
    }
  }
  return modified;
}

static bool PropagateBackwards(Graph& graph, Node* node, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  bool modified = false;
  if (node == nullptr) {
    return false;
  }
  if (node->OpType() == "Cast") {
    const NodeAttributes& attributes = node->GetAttributes();
    ORT_ENFORCE(attributes.find("to") != attributes.end());
    if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16)) {
      std::set<NodeArg*> require_cast;
      NodeArg* cast_input = node->MutableInputDefs()[0];
      SearchUpstream(graph, cast_input, require_cast);
      if (require_cast.find(cast_input) == require_cast.end()) {
        // Remove Cast operation
        RemoveCastNodes(graph, {node}, removed_nodes);
        InsertCastNodes(graph, require_cast, true, removed_nodes);
        modified = true;
      }
    }
  } else {
    for (NodeArg* input: node->MutableInputDefs()) {
      Node* producer = graph.GetMutableProducerNode(input->Name());
      modified |= PropagateBackwards(graph, producer, removed_nodes);
    }
  }
  return modified;
}

// Fuse all nodes, replace with a single node.
// Assumptions:
// 1. all nodes are Cast ops and are of the same Cast type
// 2. all the nodes have the same input
static void FuseNodes(Graph& graph, NodeArg* input, std::vector<Node*> nodes)
{
  std::vector<NodeArg*> outputs;
  for (Node* node : nodes) {
    std::vector<NodeArg*> node_outputs = node->MutableOutputDefs();
    outputs.insert(outputs.end(), node_outputs.begin(), node_outputs.end());
  }
  Node* node = nodes[0];
  (void) graph.AddNode(graph.GenerateNodeName(node->Name() + "_replace"),
                       node->OpType(),
                       "Created to replace a node",
                       {input},
                       outputs,
                       &node->GetAttributes(),
                       node->Domain());
  for (Node* n : nodes) {
    graph_utils::RemoveNodeOutputEdges(graph, *n);
    graph.RemoveNode(n->Index());    
  }
}
// Traverse the graph recursively searching/collecting sibling Cast op nodes to fuse and call FuseNodes.
static bool FuseSubgraphs(Graph& graph, Node* parent)
{
  bool modified = false;
  for (NodeArg* output : parent->MutableOutputDefs()) {
    std::vector<Node*> cast_fp16_siblings;
    std::vector<Node*> cast_fp_siblings;
    for (Node* node : graph.GetMutableConsumerNodes(output->Name())) {
      if (node == nullptr) {
        continue;
      }
      if (node->OpType() == "Cast") {
        const NodeAttributes& attributes = node->GetAttributes();
        ORT_ENFORCE(attributes.find("to") != attributes.end());
        if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16)) {
          cast_fp16_siblings.push_back(node);
        } else if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
          cast_fp_siblings.push_back(node);
        }
      }
    }
    if (cast_fp16_siblings.size() > 1) {
      modified = true;
      FuseNodes(graph, output, cast_fp16_siblings);
    }
    if (cast_fp_siblings.size() > 1) {
      modified = true;
      FuseNodes(graph, output, cast_fp_siblings);
    }
  }
  return modified;
}

Status PropagateCastOps::ApplyImpl(Graph& graph, bool& modified, int graph_level, const logging::Logger& logger) const {
  std::deque<onnxruntime::NodeIndex> removed_nodes;
  (void) graph_level;
  (void) logger;
  modified = false;
  // Propagate FP32 Casts forward
  for (Node& node : graph.Nodes()) {
      modified |= PropagateForwards(graph, &node, removed_nodes);
  }

  modified |= RemoveBackToBackCasts(graph, removed_nodes);

  // Propagate FP16 Casts backward
  if (!modified) for (const NodeArg* output: graph.GetOutputs()) {
    Node* node = graph.GetMutableProducerNode(output->Name());
    modified |= PropagateBackwards(graph, node, removed_nodes);
  }

  // Fuse subgraphs, sibling Cast nodes with same input
  for (auto& node: graph.Nodes()) {
    modified |= FuseSubgraphs(graph, &node);
  }

  for (onnxruntime::NodeIndex removed_node : removed_nodes) {
    graph.RemoveNode(removed_node);
  }

  return Status::OK();
}

} // namespace onnxruntime