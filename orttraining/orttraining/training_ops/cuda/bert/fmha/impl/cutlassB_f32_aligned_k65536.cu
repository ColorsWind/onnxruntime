#include "orttraining/training_ops/cuda/bert/fmha/kernel_backward.h"

__global__ void __launch_bounds__(
    AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 128, 64, 65536>::kNumThreads,
    AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 128, 64, 65536>::kMinBlocksPerSm)
fmha_cutlassB_f32_aligned_128x64_k65536_sm80(typename AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 128, 64, 65536>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 800
#if __CUDA_ARCH__ < 900
  if (!p.advance_to_block()) {
    return;
  }
  AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 128, 64, 65536>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassB_f32_aligned_128x64_k65536_sm80` is for sm80-sm90, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionBackwardKernel<cutlass::arch::Sm50, float, true, false, false, 64, 64, 65536>::kNumThreads,
    AttentionBackwardKernel<cutlass::arch::Sm50, float, true, false, false, 64, 64, 65536>::kMinBlocksPerSm)
fmha_cutlassB_f32_aligned_64x64_k65536_sm50(typename AttentionBackwardKernel<cutlass::arch::Sm50, float, true, false, false, 64, 64, 65536>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 500
#if __CUDA_ARCH__ < 700
  if (!p.advance_to_block()) {
    return;
  }
  AttentionBackwardKernel<cutlass::arch::Sm50, float, true, false, false, 64, 64, 65536>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassB_f32_aligned_64x64_k65536_sm50` is for sm50-sm70, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionBackwardKernel<cutlass::arch::Sm70, float, true, false, false, 64, 64, 65536>::kNumThreads,
    AttentionBackwardKernel<cutlass::arch::Sm70, float, true, false, false, 64, 64, 65536>::kMinBlocksPerSm)
fmha_cutlassB_f32_aligned_64x64_k65536_sm70(typename AttentionBackwardKernel<cutlass::arch::Sm70, float, true, false, false, 64, 64, 65536>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 700
#if __CUDA_ARCH__ < 750
  if (!p.advance_to_block()) {
    return;
  }
  AttentionBackwardKernel<cutlass::arch::Sm70, float, true, false, false, 64, 64, 65536>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassB_f32_aligned_64x64_k65536_sm70` is for sm70-sm75, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionBackwardKernel<cutlass::arch::Sm75, float, true, false, false, 64, 64, 65536>::kNumThreads,
    AttentionBackwardKernel<cutlass::arch::Sm75, float, true, false, false, 64, 64, 65536>::kMinBlocksPerSm)
fmha_cutlassB_f32_aligned_64x64_k65536_sm75(typename AttentionBackwardKernel<cutlass::arch::Sm75, float, true, false, false, 64, 64, 65536>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 750
#if __CUDA_ARCH__ < 800
  if (!p.advance_to_block()) {
    return;
  }
  AttentionBackwardKernel<cutlass::arch::Sm75, float, true, false, false, 64, 64, 65536>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassB_f32_aligned_64x64_k65536_sm75` is for sm75-sm80, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 64, 64, 65536>::kNumThreads,
    AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 64, 64, 65536>::kMinBlocksPerSm)
fmha_cutlassB_f32_aligned_64x64_k65536_sm80(typename AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 64, 64, 65536>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 800
#if __CUDA_ARCH__ < 900
  if (!p.advance_to_block()) {
    return;
  }
  AttentionBackwardKernel<cutlass::arch::Sm80, float, true, false, false, 64, 64, 65536>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassB_f32_aligned_64x64_k65536_sm80` is for sm80-sm90, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
