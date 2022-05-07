#ifndef NTSOPS_HPP
#define NTSOPS_HPP
#include <stack>
#include "ntsBaseOp.hpp"
#include<type_traits>
namespace nts {

namespace ctx {

typedef uint32_t OpType;

const OpType NNOP = 0;
const OpType GRAPHOP = 1;
const OpType SEGMENTOP = 2;

class ntsOperator{
public:
    ntsOperator(){
        
    }
    ntsOperator(nts::op::ntsGraphOp* op_,OpType op_t_){
        assert(GRAPHOP==op_t_);
        op=op_;
        op_t=op_t_;
    }
    ntsOperator(OpType op_t_){
        assert(NNOP==op_t_);
        op_t=op_t_;
    }
    nts::op::ntsGraphOp* op;
    OpType op_t;
};
/**
 * @brief
 * since GNN operation is just iteration of graph operation and NN operation.
 * so we can simply use a chain to represent GNN operation, which can reduce
 * system complexity greatly.
 * you can also regard it as the NN operation splited by graph operation.
 * And as the extention of auto diff library, we will provide backward
 * computation for graph operation. And thus, the computation path of GNN is
 * constructed.
 */
class NtsContext {
public:
  NtsContext(){
  std::stack<OpType>().swap(op);
  std::stack<NtsVar>().swap(output);
  std::stack<NtsVar>().swap(input);
  std::stack<ntsOperator>().swap(ntsOp);
  output_grad.clear();
  count = 0;
}
  template <typename GOPT>
  NtsVar runGraphOp(Graph<Empty> *graph, VertexSubset *active,
                   std::vector<CSC_segment_pinned *> &subgraphs_,NtsVar &f_input){//graph op
      
    static_assert(std::is_base_of<nts::op::ntsGraphOp,GOPT>::value,
                "template must be a type of graph op!");
    nts::op::ntsGraphOp * curr=new GOPT(graph,active,subgraphs_);
    NtsVar f_output=curr->forward(f_input); 
    NtsVar ig;
    op.push(GRAPHOP);
    output.push(f_output);
    input.push(f_input);
    ntsOp.push(ntsOperator(curr,GRAPHOP));
    // pre-alloc space to save graident
    output_grad.push_back(ig);
    count++;
    return f_output;
} 
 NtsVar runVertexForward(std::function<NtsVar(NtsVar &, NtsVar &)> vertexforward,
            NtsVar &nbr_input,NtsVar &vtx_input){//NNOP
     LOG_INFO("call run vertex forward");
    NtsVar f_output=vertexforward(nbr_input,vtx_input); 
    NtsVar ig;
    if (count > 0 && op.top() == NNOP) {
        output.pop();
        output.push(f_output);
        //     LOG_INFO("TRIG");
    }else{
        op.push(NNOP);
        output.push(f_output);
        input.push(nbr_input);
        ntsOp.push(ntsOperator(NNOP));
        // pre-alloc space to save graident
        output_grad.push_back(ig);
        count++;
        return f_output;
    }
} 
 
  
  
//  void op_push(nts::op::ntsGraphOp* op_, NtsVar &input_t, NtsVar &output_t, OpType op_type){//graph op
////    if(output_t.dim()>1&&input_t.dim()>1)
////  LOG_INFO("input dim %d \t output dim %d \t OP type %d", input_t.size(1),output_t.size(1),op_type);
//  NtsVar ig;
//  NtsVar og;
//
//  assert(op_type == 1);
//
//  // we will chain the NNOP together, because torch lib will handle the backward propagation
//  // when there is no graph operation
//  if (count > 0 && NNOP == op_type && op.top() == NNOP) {
//    output.pop();
//    output.push(output_t);
//    //     LOG_INFO("TRIG");
//  } else {
//    count++;
//    op.push(op_type);
//    output.push(output_t);
//    input.push(input_t);
//    ntsOp.push(ntsOperator(op_,GRAPHOP));
//    // pre-alloc space to save graident
//    output_grad.push_back(ig);
//  }
//}
  void appendNNOp(NtsVar &input_t, NtsVar &output_t){
    NtsVar ig;
    NtsVar og;

    // we will chain the NNOP together, because torch lib will handle the backward propagation
    // when there is no graph operation
    if (count > 0 && op.top() == NNOP) {
        output.pop();
        output.push(output_t);
        //     LOG_INFO("TRIG");
    } else {
        count++;
        op.push(NNOP);
        output.push(output_t);
        input.push(input_t);
        ntsOp.push(ntsOperator(NNOP));
        // pre-alloc space to save graident
        output_grad.push_back(ig);
    }
  }
 
  void reset(){
    assert(count<=1);
    if(count==1&&ntsOp.top().op_t==GRAPHOP){
        printf("call delete");
        delete ntsOp.top().op;
    }
    count = 0;
    std::stack<OpType>().swap(op);
    std::stack<NtsVar>().swap(output);
    std::stack<NtsVar>().swap(input);
    std::stack<ntsOperator>().swap(ntsOp);
    output_grad.clear();
}
  void pop_one_op(){
    if(ntsOp.top().op_t==GRAPHOP){
        printf("call delete");
        delete ntsOp.top().op;
    }
    op.pop();
    output.pop();
    input.pop();
    ntsOp.pop();
    count--;
  }
  void self_backward(bool retain_graph = true){
    NtsVar final_output = output.top();
    NtsVar final_input = input.top();
    final_output.backward(torch::ones_like(final_output), retain_graph);
    output_grad[top_idx()-1]= final_input.grad();// grad of loss
    pop_one_op();
      while (count > 1 || (count == 1 && NNOP == op.top())) {
    // NNOP means we are using torch lib to do the forward computation
    // thus we can use auto diff framework in libtorch
    if (GRAPHOP == op.top()) { // test
      output_grad[top_idx()-1]=ntsOp.top().op->backward(output_grad[top_idx()]);
      pop_one_op();

    } else if (NNOP == op.top()) {
      // LOG_INFO("NOP %d",output_grad[count].dim());
      NtsVar inter_output = output.top();
      NtsVar inter_input = input.top();
      // backward will compute the bottom_diff for inter_output
      // the top_diff is output_grad[count]
      // and the bottom_diff for inter_output, also is top_diff for inter_input
      // will store in inter_input.grad()
      // then we retrieve it for future use
      inter_output.backward(output_grad[top_idx()], retain_graph);
      NtsVar grad_to_previous_op = inter_input.grad();
      if(count>1)
      output_grad[top_idx()-1] = grad_to_previous_op;
      pop_one_op();
//       LOG_INFO("finish nn op\n");
  //    LOG_INFO("output_grad[count] %d \t input_grad[count] %d \t OP type %d", output_grad[count-1].size(1),input_grad[count-1].size(1),op.top());
    } else {
      LOG_INFO("NOT SUPPORT OP");
      assert(true);
    }
  }
    reset();  
  }
  void debug(){
    printf("ADDEBUG input.size()%d\n", input.size());
    // for(int i=0;i<count;i++){
    while (!input.empty()) {
        LOG_INFO("input dim %d \t output dim %d \t OP type %d", input.top().dim(),output.top().dim(),op.top());
        input.pop();
        output.pop();
        op.pop();
        ntsOp.pop();
    }
  }
  
  int top_idx(){
    return count - 1;
  }

private:
  std::stack<OpType> op;
  std::stack<NtsVar> output;
  std::stack<NtsVar> input;
  std::stack<ntsOperator> ntsOp;
  std::vector<NtsVar> output_grad;
  int count;
//  GraphOperation *gt;
//  std::vector<CSC_segment_pinned *> subgraphs;
//  bool bi_direction;
};

} // namespace autodiff
} // namespace nts

#endif
