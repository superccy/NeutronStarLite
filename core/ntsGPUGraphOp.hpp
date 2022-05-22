/*
Copyright (c) 2021-2022 Qiange Wang, Northeastern University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#ifndef NTSGPUGRAPHOP_HPP
#define NTSGPUGRAPHOP_HPP
#include <assert.h>
#include <map>
#include <math.h>
#include <stack>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include "core/graph.hpp"
#include "core/ntsBaseOp.hpp"
#include "core/PartitionedGraph.hpp"
#include "cuda/ntsCUDA.hpp"

namespace nts {
namespace op {

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
    
class DistGPUScatterSrc : public ntsGraphOp{//assume the data in GPU
public:
  //std::vector<CSC_segment_pinned *> subgraphs;
    Cuda_Stream* cuda_stream;
    deviceCSC* device_csc;
  DistGPUScatterSrc(PartitionedGraph *partitioned_graph,VertexSubset *active)
      : ntsGraphOp(partitioned_graph, active) {
      cuda_stream=new Cuda_Stream();
      device_csc=new deviceCSC();
      device_csc->init(partitioned_graph_->owned_vertices,
              partitioned_graph_->owned_edges,true,
                 partitioned_graph_->global_vertices);
      device_csc->load_from_host(partitioned_graph_->column_offset,
              partitioned_graph_->row_indices,partitioned_graph_->MirrorIndex);
  }
  ~DistGPUScatterSrc(){//this function will be manually called 
                       //after backward computation
   cuda_stream->destory_Stream();
   device_csc->release();
  }
  NtsVar forward(NtsVar &f_input){// input edge  output vertex
    int feature_size = f_input.size(1);
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    NtsVar f_output=graph_->Nts->NewKeyTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_buffer =
      graph_->Nts->getWritableBuffer(f_input, torch::DeviceType::CUDA);
    ValueType *f_output_buffer =
      graph_->Nts->getWritableBuffer(f_output, torch::DeviceType::CUDA);  
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    cuda_stream->Scatter_Src_Mirror_to_Msg(f_output_buffer,f_input_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->mirror_index,device_csc->v_size,feature_size); 
      return f_output;
  }
  
  NtsVar backward(NtsVar &f_output_grad){// input vtx grad; output edge grad
    int feature_size=f_output_grad.size(1);
    NtsVar f_input_grad=graph_->Nts->NewLeafTensor({partitioned_graph_->owned_mirrors, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_grad_buffer =
      graph_->Nts->getWritableBuffer(f_input_grad, torch::DeviceType::CUDA);
    ValueType *f_output_grad_buffer =
      graph_->Nts->getWritableBuffer(f_output_grad, torch::DeviceType::CUDA);
    cuda_stream->Gather_Msg_To_Src_Mirror(f_input_grad_buffer,f_output_grad_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->mirror_index,device_csc->v_size,feature_size);     
      
      return f_input_grad;
  }
};

class DistGPUScatterDst : public ntsGraphOp{//assume the data in GPU
public:
  //std::vector<CSC_segment_pinned *> subgraphs;
    Cuda_Stream* cuda_stream;
    deviceCSC* device_csc;
  DistGPUScatterDst(PartitionedGraph *partitioned_graph,VertexSubset *active)
      : ntsGraphOp(partitioned_graph, active) {
      cuda_stream=new Cuda_Stream();
      device_csc=new deviceCSC();
      device_csc->init(partitioned_graph_->owned_vertices,
              partitioned_graph_->owned_edges);
      device_csc->load_from_host(partitioned_graph_->column_offset,
              partitioned_graph_->row_indices);
  }
  ~DistGPUScatterDst(){//this function will be manually called 
                       //after backward computation
   cuda_stream->destory_Stream();
   device_csc->release();
  }
  NtsVar forward(NtsVar &f_input){// input edge  output vertex
    int feature_size = f_input.size(1);
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    NtsVar f_output=graph_->Nts->NewKeyTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_buffer =
      graph_->Nts->getWritableBuffer(f_input, torch::DeviceType::CUDA);
    ValueType *f_output_buffer =
      graph_->Nts->getWritableBuffer(f_output, torch::DeviceType::CUDA);  
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    cuda_stream->Scatter_Dst_to_Msg(f_output_buffer,f_input_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size); 
      return f_output;
  }
  
  NtsVar backward(NtsVar &f_output_grad){// input vtx grad; output edge grad
    int feature_size=f_output_grad.size(1);
    NtsVar f_input_grad=graph_->Nts->NewLeafTensor({partitioned_graph_->owned_vertices, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_grad_buffer =
      graph_->Nts->getWritableBuffer(f_input_grad, torch::DeviceType::CUDA);
    ValueType *f_output_grad_buffer =
      graph_->Nts->getWritableBuffer(f_output_grad, torch::DeviceType::CUDA);
    cuda_stream->Gather_Msg_to_Dst(f_input_grad_buffer,f_output_grad_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size);     
      
      return f_input_grad;
  }
};

class DistGPUAggregateDst : public ntsGraphOp{//assume the data in GPU
public:
  //std::vector<CSC_segment_pinned *> subgraphs;
    Cuda_Stream* cuda_stream;
    deviceCSC* device_csc;
  DistGPUAggregateDst(PartitionedGraph *partitioned_graph,VertexSubset *active)
      : ntsGraphOp(partitioned_graph, active) {
      cuda_stream=new Cuda_Stream();
      device_csc=new deviceCSC();
      device_csc->init(partitioned_graph_->owned_vertices,
              partitioned_graph_->owned_edges);
      device_csc->load_from_host(partitioned_graph_->column_offset,
              partitioned_graph_->row_indices);
  }
  ~DistGPUAggregateDst(){//this function will be manually called 
                       //after backward computation
   cuda_stream->destory_Stream();
   device_csc->release();
  }
  NtsVar forward(NtsVar &f_input){// input edge  output vertex
    int feature_size = f_input.size(1);
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    NtsVar f_output=graph_->Nts->NewKeyTensor({partitioned_graph_->owned_vertices, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_buffer =
      graph_->Nts->getWritableBuffer(f_input, torch::DeviceType::CUDA);
    ValueType *f_output_buffer =
      graph_->Nts->getWritableBuffer(f_output, torch::DeviceType::CUDA);  
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    cuda_stream->Gather_Msg_to_Dst(f_output_buffer,f_input_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size); 
      return f_output;
  }
  
  NtsVar backward(NtsVar &f_output_grad){// input vtx grad; output edge grad
    int feature_size=f_output_grad.size(1);
    NtsVar f_input_grad=graph_->Nts->NewLeafTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_grad_buffer =
      graph_->Nts->getWritableBuffer(f_input_grad, torch::DeviceType::CUDA);
    ValueType *f_output_grad_buffer =
      graph_->Nts->getWritableBuffer(f_output_grad, torch::DeviceType::CUDA);
    cuda_stream->Scatter_Dst_to_Msg(f_input_grad_buffer,f_output_grad_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size);     
      
      return f_input_grad;
  }
};

class DistGPUEdgeSoftMax : public ntsGraphOp{//assume the data in GPU
public:
  //std::vector<CSC_segment_pinned *> subgraphs;
    Cuda_Stream* cuda_stream;
    deviceCSC* device_csc;
    NtsVar IntermediateResult; 
  DistGPUEdgeSoftMax(PartitionedGraph *partitioned_graph,VertexSubset *active)
      : ntsGraphOp(partitioned_graph, active) {
      cuda_stream=new Cuda_Stream();
      device_csc=new deviceCSC();
      device_csc->init(partitioned_graph_->owned_vertices,
              partitioned_graph_->owned_edges);
      device_csc->load_from_host(partitioned_graph_->column_offset,
              partitioned_graph_->row_indices);
  }
  ~DistGPUEdgeSoftMax(){//this function will be manually called 
                       //after backward computation
   cuda_stream->destory_Stream();
   device_csc->release();
  }
  NtsVar forward(NtsVar &f_input){// input edge  output vertex
    int feature_size = f_input.size(1);
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    NtsVar f_output=graph_->Nts->NewKeyTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    IntermediateResult=graph_->Nts->NewKeyTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_buffer =
      graph_->Nts->getWritableBuffer(f_input, torch::DeviceType::CUDA);
    ValueType *f_output_buffer =
      graph_->Nts->getWritableBuffer(f_output, torch::DeviceType::CUDA);
    ValueType *f_cache_buffer =
      graph_->Nts->getWritableBuffer(IntermediateResult, torch::DeviceType::CUDA);  
    //LOG_INFO("owned_mirrors (%d)",partitioned_graph_->owned_mirrors);
    cuda_stream->Edge_Softmax_Forward_Block(f_output_buffer,f_input_buffer,
            f_cache_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size); 
      return f_output;
  }
  
  NtsVar backward(NtsVar &f_output_grad){// input vtx grad; output edge grad
    int feature_size=f_output_grad.size(1);
    NtsVar f_input_grad=graph_->Nts->NewLeafTensor({partitioned_graph_->owned_edges, 
                feature_size},torch::DeviceType::CUDA);
    ValueType *f_input_grad_buffer =
      graph_->Nts->getWritableBuffer(f_input_grad, torch::DeviceType::CUDA);
    ValueType *f_output_grad_buffer =
      graph_->Nts->getWritableBuffer(f_output_grad, torch::DeviceType::CUDA);
    ValueType *f_cache_buffer =
      graph_->Nts->getWritableBuffer(IntermediateResult, torch::DeviceType::CUDA); 
    cuda_stream->Edge_Softmax_Backward_Block(f_input_grad_buffer,f_output_grad_buffer,
            f_cache_buffer,
            device_csc->row_indices,device_csc->column_offset,
            device_csc->v_size,feature_size);     
      
      return f_input_grad;
  }
};

} // namespace graphop
} // namespace nts

#endif