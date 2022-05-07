#ifndef NTSGRAPHOP_HPP
#define NTSGRAPHOP_HPP
#include <assert.h>
#include <map>
#include <math.h>
#include <stack>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include "graph.hpp"
#include "ntsBaseOp.hpp"
#include "input.h"

namespace nts {
namespace op {
void comp(ValueType *input, ValueType *output, ValueType weight,
          int feat_size) {
  for (int i = 0; i < feat_size; i++) {
    output[i] += input[i] * weight;
  }
}

/**
 * @brief
 * do output += input at feature(array) level
 * @param input input feature
 * @param output output feature
 * @param feat_size feature size
 */
void acc(ValueType *input, ValueType *output, int feat_size) {
  for (int i = 0; i < feat_size; i++) {
    // atomic add
    write_add(&output[i], input[i]);
  }
}

/**
 * @brief
 * copy feature_size elements from b_src[d_offset * feature_size]
 * to d_dst[s_offset * feature_size]
 * @param b_dst dst buffer
 * @param d_offset dst offset, should be a vertex id
 * @param b_src src buffer
 * @param s_offset src offset, should be a vertex id
 * @param feat_size feature size that every vertex have
 */
void copy(ValueType *b_dst, long d_offset, ValueType *b_src, VertexId s_offset,
          int feat_size) {
  // length is the byte level space cost for a vertex feature data
  VertexId length = sizeof(ValueType) * feat_size;
  // LOG_INFO("length %d feat_size %d d_offset %d s_offset
  // %d\n",length,feat_size,d_offset,s_offset);
  memcpy((char *)b_dst + d_offset * length, (char *)b_src + s_offset * length,
         length);
}

/**
 * @brief
 * return 1 / sqrt(out_degree[src] * in_degree[dst]).
 * normally used as edge weight
 * @param src src id
 * @param dst dst id
 * @return ValueType
 */
ValueType norm_degree(Graph<Empty> *graph_, VertexId src, VertexId dst) {
  return 1 / ((ValueType)std::sqrt(graph_->out_degree_for_backward[src]) *
              (ValueType)std::sqrt(graph_->in_degree_for_backward[dst]));
}

/**
 * @brief
 * get out degree for v
 * @param v vertex id
 * @return ValueType
 */
ValueType out_degree(Graph<Empty> *graph_, VertexId v) {
  return (ValueType)(graph_->out_degree_for_backward[v]);
}

/**
 * @brief
 * get in degree for v
 * @param v vertex id
 * @return ValueType
 */
ValueType in_degree(Graph<Empty> *graph_, VertexId v) {
  return (ValueType)(graph_->in_degree_for_backward[v]);
}

//class ntsGraphOp {
//public:
//  Graph<Empty> *graph_;
//  VertexSubset *active_;
//  ntsGraphOp() { ; }
//  ntsGraphOp(Graph<Empty> *graph, VertexSubset *active) {
//    graph_ = graph;
//    active_ = active;
//  }
//  virtual NtsVar &forward(NtsVar &input) = 0;
//  virtual NtsVar backward(NtsVar &output_grad) = 0;
//};

class ForwardCPUfuseOp : public ntsGraphOp {
public:
  std::vector<CSC_segment_pinned *> subgraphs;

  ForwardCPUfuseOp(Graph<Empty> *graph, VertexSubset *active,
                   std::vector<CSC_segment_pinned *> &subgraphs_)
      : ntsGraphOp(graph, active) {
    subgraphs = subgraphs_;
  }
  NtsVar newBuffer(NtsVar &f_input){
      NtsVar val;
      val= graph_->Nts->NewKeyTensor(f_input, torch::DeviceType::CPU);
      return val;
  }
  NtsVar forward(NtsVar &f_input) {
    //f_input = input;
    NtsVar f_output = newBuffer(f_input);
    //LOG_INFO("SUCCESS %d %d",input.dim(),f_output.dim());
    ValueType *f_input_buffer =
        graph_->Nts->getWritableBuffer(f_input, torch::DeviceType::CPU);
    ValueType *f_output_buffer =
        graph_->Nts->getWritableBuffer(f_output, torch::DeviceType::CPU);
    memset(f_output_buffer, 0,
           sizeof(ValueType) * f_input.size(0) * f_input.size(1));
    int feature_size = f_input.size(1);
    graph_->process_edges_forward_decoupled_mutisockets<int, ValueType>(
        [&](VertexId src, int current_send_partition) {
          if (graph_->rtminfo->lock_free) {
            VertexId src_trans = src - graph_->gnnctx->p_v_s;
            // check whether this vertex is necessary to send to
            // current_send_partition
            if (subgraphs[current_send_partition]->get_forward_active(
                    src_trans)) {
              // get the index where we shall place the data
              // and invoke emit_buffer_lock_free to send messages
              VertexId write_index =
                  subgraphs[current_send_partition]
                      ->forward_multisocket_message_index[src_trans];
              graph_->NtsComm->emit_buffer_lock_free(
                  src,
                  f_input_buffer + (src - graph_->gnnctx->p_v_s) * feature_size,
                  write_index, feature_size);
            }
          } else {
            // send to mirror directly
            graph_->NtsComm->emit_buffer(
                src,
                f_input_buffer + (src - graph_->gnnctx->p_v_s) * feature_size,
                feature_size);
          }
        },
        // sparse slot.
        // accumulate incoming feature for dst
        // recv_id represent the partition ID corresponding to current subgraph
        [&](VertexId dst, CSC_segment_pinned *subgraph,
            MessageBuffer **recv_buffer, std::vector<VertexIndex> &src_index,
            VertexId recv_id) {
          VertexId dst_trans =
              dst - graph_->partition_offset[graph_->partition_id];
          // for every vertex, accumulate the incoming feature though iterating
          // column vertices
          for (long idx = subgraph->column_offset[dst_trans];
               idx < subgraph->column_offset[dst_trans + 1]; idx++) {
            VertexId src = subgraph->row_indices[idx];
            VertexId src_trans = src - graph_->partition_offset[recv_id];
            // fetch input from recv buffer
            // bufferIndex indicate which socket we've placed the data
            // positionIndex indicate the index of the buffer of that socket
            ValueType *local_input =
                (ValueType *)(recv_buffer[src_index[src_trans].bufferIndex]
                                  ->data +
                              graph_->sizeofM<ValueType>(feature_size) *
                                  src_index[src_trans].positionIndex +
                              sizeof(VertexId));
            ValueType *local_output =
                f_output_buffer + dst_trans * feature_size;
            comp(local_input, local_output, norm_degree(graph_, src, dst),
                 feature_size);
          }
        },
        subgraphs, feature_size, active_);
    return f_output;
  }
  NtsVar backward(NtsVar &f_output_grad) {
    NtsVar f_input_grad=graph_->Nts->NewLeafTensor(f_output_grad, torch::DeviceType::CPU);
  ValueType *output_grad_buffer =
      graph_->Nts->getWritableBuffer(f_output_grad, torch::DeviceType::CPU);
  ValueType *input_grad_buffer =
      graph_->Nts->getWritableBuffer(f_input_grad, torch::DeviceType::CPU);
  memset(input_grad_buffer, 0, sizeof(ValueType) * f_output_grad.size(0) * f_output_grad.size(1));
  // int feature_size=graph_->gnnctx->layer_size[graph_->rtminfo->curr_layer];
  int feature_size = f_output_grad.size(1);
  ValueType *output_buffer = new ValueType[feature_size * graph_->threads];
  graph_->process_edges_backward_decoupled_multisockets<
      int, ValueType>( // For EACH Vertex
                       // Processing
      [&](VertexId src, VertexAdjList<Empty> outgoing_adj, VertexId thread_id,
          VertexId recv_id, VertexId socketId) { // pull
        ValueType *local_output_buffer =
            output_buffer + feature_size * thread_id;
        memset(local_output_buffer, 0, sizeof(ValueType) * feature_size);
        VertexId src_trans = src - graph_->partition_offset[recv_id];

        // iterate the outgoing vertices and combine the gradients
        for (long d_idx = subgraphs[recv_id]->row_offset[src_trans];
             d_idx < subgraphs[recv_id]->row_offset[src_trans + 1]; d_idx++) {
          VertexId dst = subgraphs[recv_id]->column_indices[d_idx];

          // FIXME: will this work?
          if ((dst < graph_->local_partition_offset[socketId]) ||
              (dst >= graph_->local_partition_offset[socketId + 1])) {
            continue;
          }
          VertexId dst_trans = dst - graph_->gnnctx->p_v_s;
          ValueType *local_input_buffer =
             output_grad_buffer + (dst_trans)*feature_size;
          comp(local_input_buffer, local_output_buffer, norm_degree(graph_,src, dst),
               feature_size);
        }
        if (graph_->rtminfo->lock_free) {
          if (subgraphs[recv_id]->source_active->get_bit(src_trans)) {
            VertexId write_index =
                subgraphs[recv_id]
                    ->backward_multisocket_message_index[src_trans]
                    .vertexSocketPosition[socketId];
            graph_->NtsComm->emit_buffer_lock_free(src, local_output_buffer,
                                                   write_index, feature_size);
          }
        } else {
          graph_->NtsComm->emit_buffer(src, local_output_buffer, feature_size);
        }
      },
      [&](VertexId src, ValueType *msg) {
        acc(msg, input_grad_buffer + (src - graph_->gnnctx->p_v_s) * feature_size, feature_size);
        return 1;
      },
      feature_size, active_);
  delete[] output_buffer;
  return f_input_grad;
  
  }
};

} // namespace graphop
} // namespace nts

#endif
