#include "orttraining/training_ops/cuda/bert/fmha/kernel_forward.h"

__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm50, false, 64, 64, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm50, false, 64, 64, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_64x64_rf_sm50(typename AttentionKernel<float, cutlass::arch::Sm50, false, 64, 64, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 500
#if __CUDA_ARCH__ < 700
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm50, false, 64, 64, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_64x64_rf_sm50` is for sm50-sm70, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm70, false, 64, 64, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm70, false, 64, 64, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_64x64_rf_sm70(typename AttentionKernel<float, cutlass::arch::Sm70, false, 64, 64, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 700
#if __CUDA_ARCH__ < 750
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm70, false, 64, 64, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_64x64_rf_sm70` is for sm70-sm75, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm75, false, 64, 64, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm75, false, 64, 64, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_64x64_rf_sm75(typename AttentionKernel<float, cutlass::arch::Sm75, false, 64, 64, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 750
#if __CUDA_ARCH__ < 800
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm75, false, 64, 64, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_64x64_rf_sm75` is for sm75-sm80, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_rf_sm50(typename AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 500
#if __CUDA_ARCH__ < 700
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_rf_sm50` is for sm50-sm70, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_rf_sm70(typename AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 700
#if __CUDA_ARCH__ < 750
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_rf_sm70` is for sm70-sm75, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, true, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, true, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_rf_sm75(typename AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, true, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 750
#if __CUDA_ARCH__ < 800
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, true, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_rf_sm75` is for sm75-sm80, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, false, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, false, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_gmem_sm50(typename AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, false, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 500
#if __CUDA_ARCH__ < 700
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm50, false, 32, 128, false, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_gmem_sm50` is for sm50-sm70, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, false, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, false, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_gmem_sm70(typename AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, false, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 700
#if __CUDA_ARCH__ < 750
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm70, false, 32, 128, false, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_gmem_sm70` is for sm70-sm75, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
__global__ void __launch_bounds__(
    AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, false, true, true>::kNumThreads,
    AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, false, true, true>::kMinBlocksPerSm)
fmha_cutlassF_f32_notaligned_32x128_gmem_sm75(typename AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, false, true, true>::Params p) {
#ifdef __CUDA_ARCH__
#if __CUDA_ARCH__ >= 750
#if __CUDA_ARCH__ < 800
  if (!p.advance_to_block()) {
    return;
  }
  AttentionKernel<float, cutlass::arch::Sm75, false, 32, 128, false, true, true>::attention_kernel(p);
  return;
#endif
#endif
    printf(
        "FATAL: kernel `fmha_cutlassF_f32_notaligned_32x128_gmem_sm75` is for sm75-sm80, but was built for sm%d\n",
        int(__CUDA_ARCH__ + 0) / 10);
#endif
}
