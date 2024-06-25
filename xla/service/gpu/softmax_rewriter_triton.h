/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_SOFTMAX_REWRITER_TRITON_H_
#define XLA_SERVICE_GPU_SOFTMAX_REWRITER_TRITON_H_

#include <variant>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/service/hlo_cost_analysis.h"
#include "xla/service/hlo_pass_interface.h"
#include "xla/service/instruction_fusion.h"
#include "xla/stream_executor/device_description.h"

namespace xla {
namespace gpu {

struct DiamondChainDescriptor {
  HloInstruction* root = nullptr;
  HloInstruction* producer = nullptr;
};

using DiamondMatchingDecision = std::variant<FusionDecision, HloInstruction*>;

// Rewrite compatible Softmax into a custom fusion region to be code-generated
// with the Triton-based Softmax emitter.
class SoftmaxRewriterTriton : public HloModulePass {
 public:
  explicit SoftmaxRewriterTriton(const se::DeviceDescription& device_info,
                                 HloCostAnalysis::ShapeSizeFunction shape_size)
      : device_info_(device_info), shape_size_(shape_size) {}

  absl::string_view name() const override { return "triton-softmax-rewriter"; }

  using HloPassInterface::Run;
  absl::StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;

  // Finds and returns all the fusible diamond chains in the module. The
  // resulting vector is sorted according to a post-order matching (i.e. within
  // the same computation, producer diamonds appear before consumer diamonds).
  absl::StatusOr<std::vector<DiamondChainDescriptor>>
  FindAllFusibleDiamondChains(
      HloModule& module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) const;

  // Constructs a Softmax fusion containing all the instructions between the
  // root and the producer of a diamond chain. The producer is excluded from the
  // fusion.
  absl::Status FuseDiamondChain(const DiamondChainDescriptor& diamond_chain);

  // Return the producer of the following pattern:
  //
  // producer
  // |    \
  // |  reduce_{max,sum,...}
  // |     |
  // |  broadcast
  // |   /
  // binop (elementwise)
  //
  // where each edge is allowed to contain also trivial operations that can be
  // generated by Triton. We mean by "trivial" here those operations that do not
  // increase the amount of memory read/written by the fusion, and that are
  // compatible with any chosen tiling.
  //
  // We also assume that the reduction is done on the last axis of the producer
  // array.
  DiamondMatchingDecision MatchesTritonCompatibleClosedReductionDiamond(
      HloInstruction* instr) const;

 private:
  const se::DeviceDescription& device_info_;
  const HloCostAnalysis::ShapeSizeFunction shape_size_;
  mlir::MLIRContext mlir_context_;
};

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_SOFTMAX_REWRITER_TRITON_H_
