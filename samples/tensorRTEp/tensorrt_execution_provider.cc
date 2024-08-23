#include <memory>
#include <fstream>
#include <functional>
#include <cuda_runtime.h>
#include "core/session/onnxruntime_cxx_api.h"   // TODO(leca): we should be able to use cxx APIs which are built upon C API
#include "tensorrt_execution_provider.h"
#include "tensorrt_execution_provider_utils.h"
#include "onnx_ctx_model_helper.h"

void CUDA_RETURN_IF_ERROR(cudaError_t res) { if (res != cudaSuccess) abort(); }

namespace onnxruntime {

template <typename T>
using IAllocatorUniquePtr = std::unique_ptr<T, std::function<void(T*)>>;
const OrtApi* TensorrtExecutionProvider::api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);

bool CalcMemSizeForArrayWithAlignment(size_t nmemb, size_t size, size_t alignment, size_t* out) noexcept {
    size_t alloc_size = size;
    if (alignment == 0) {
      *out = alloc_size * nmemb;
    } else {
      size_t alignment_mask = alignment - 1;
      *out = (alloc_size * nmemb + alignment_mask) & ~static_cast<size_t>(alignment_mask);
    }
  return true;
}

template <typename T>
IAllocatorUniquePtr<T> MakeUniquePtrFromOrtAllocator(OrtAllocator* ort_allocator, size_t count_or_bytes) {
  size_t alloc_size = count_or_bytes;
  // if T is not void, 'count_or_bytes' == number of items so allow for that
  if constexpr (!std::is_void<T>::value) {
    // sizeof(void) isn't valid, but the compiler isn't smart enough to ignore that this line isn't
    // reachable if T is void. use std::conditional to 'use' void* in the sizeof call
    constexpr auto size = sizeof(typename std::conditional<std::is_void<T>::value, void*, T>::type);
    CalcMemSizeForArrayWithAlignment(count_or_bytes, size, 0, &alloc_size);
  }

  T* p = static_cast<T*>(ort_allocator->Alloc(ort_allocator, alloc_size));

  return IAllocatorUniquePtr<T>{p,
                                [ort_allocator](T* p) {
                                  ort_allocator->Free(ort_allocator, p);
                                }};
}

TensorrtLogger& GetTensorrtLogger(bool verbose_log) {
  const auto log_level = verbose_log ? nvinfer1::ILogger::Severity::kVERBOSE : nvinfer1::ILogger::Severity::kWARNING;
  static TensorrtLogger trt_logger(log_level);
  if (log_level != trt_logger.get_level()) {
    trt_logger.set_level(verbose_log ? nvinfer1::ILogger::Severity::kVERBOSE : nvinfer1::ILogger::Severity::kWARNING);
  }
  return trt_logger;
}

template <typename T>
void GetShapeOfShapeTensor(Ort::ConstValue& input_tensor,
                             void* shape_values,
                             int shape_size,
                             cudaStream_t stream) {
  CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(shape_values,
                                       input_tensor.GetTensorData<T>(),
                                       shape_size * sizeof(T),
                                       cudaMemcpyDeviceToHost,
                                       stream));
  CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(stream));
}

bool ApplyProfileShapesFromProviderOptions(std::vector<nvinfer1::IOptimizationProfile*>& trt_profiles,
                                           nvinfer1::ITensor* input,
                                           std::unordered_map<std::string, std::vector<std::vector<int64_t>>>& profile_min_shapes,
                                           std::unordered_map<std::string, std::vector<std::vector<int64_t>>>& profile_max_shapes,
                                           std::unordered_map<std::string, std::vector<std::vector<int64_t>>>& profile_opt_shapes,
                                           ShapeRangesMap& input_explicit_shape_ranges) {
  if (trt_profiles.size() == 0) {
//    LOGS_DEFAULT(WARNING) << "[TensorRT EP] Number of optimization profiles should be greater than 0, but it's 0.";
    return false;
  }

  const std::string& input_name = input->getName();
  if (profile_min_shapes.find(input_name) == profile_min_shapes.end()) {
    return false;
  }

  if (input_explicit_shape_ranges.find(input_name) == input_explicit_shape_ranges.end()) {
    std::unordered_map<size_t, std::vector<std::vector<int64_t>>> inner_map;
    input_explicit_shape_ranges[input_name] = inner_map;
  }

//  LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Begin to apply profile shapes ...";
//  LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Input tensor name is '" << input_name << "', number of profiles found is " << trt_profiles.size();

  for (size_t i = 0; i < trt_profiles.size(); i++) {
    nvinfer1::Dims dims = input->getDimensions();
    int nb_dims = dims.nbDims;

    auto trt_profile = trt_profiles[i];

    // Shape tensor
    if (input->isShapeTensor()) {
      int shape_size = nb_dims == 0 ? 1 : static_cast<int>(profile_min_shapes[input_name][i].size());
      std::vector<int32_t> shapes_min(shape_size), shapes_opt(shape_size), shapes_max(shape_size);

//      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] shape size of this shape tensor is " << shape_size;

      for (int j = 0; j < shape_size; j++) {
        auto min_value = profile_min_shapes[input_name][i][j];
        auto max_value = profile_max_shapes[input_name][i][j];
        auto opt_value = profile_opt_shapes[input_name][i][j];
        shapes_min[j] = static_cast<int32_t>(min_value);
        shapes_max[j] = static_cast<int32_t>(max_value);
        shapes_opt[j] = static_cast<int32_t>(opt_value);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] shapes_min.d[" << j << "] is " << shapes_min[j];
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] shapes_max.d[" << j << "] is " << shapes_max[j];
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] shapes_opt.d[" << j << "] is " << shapes_opt[j];

        if (input_explicit_shape_ranges[input_name].find(j) == input_explicit_shape_ranges[input_name].end()) {
          std::vector<std::vector<int64_t>> profile_vector(trt_profiles.size());
          input_explicit_shape_ranges[input_name][j] = profile_vector;
        }
        input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(min_value);
        input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(max_value);
        input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(opt_value);
      }

      trt_profile->setShapeValues(input_name.c_str(), nvinfer1::OptProfileSelector::kMIN, &shapes_min[0], shape_size);
      trt_profile->setShapeValues(input_name.c_str(), nvinfer1::OptProfileSelector::kMAX, &shapes_max[0], shape_size);
      trt_profile->setShapeValues(input_name.c_str(), nvinfer1::OptProfileSelector::kOPT, &shapes_opt[0], shape_size);
    }
    // Execution tensor
    else {
      nvinfer1::Dims dims_min, dims_opt, dims_max;
      dims_min.nbDims = nb_dims;
      dims_max.nbDims = nb_dims;
      dims_opt.nbDims = nb_dims;

//      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] number of dimension of this execution tensor is " << nb_dims;

      for (int j = 0; j < nb_dims; j++) {
        if (dims.d[j] == -1) {
          auto min_value = profile_min_shapes[input_name][i][j];
          auto max_value = profile_max_shapes[input_name][i][j];
          auto opt_value = profile_opt_shapes[input_name][i][j];
          dims_min.d[j] = static_cast<int32_t>(min_value);
          dims_max.d[j] = static_cast<int32_t>(max_value);
          dims_opt.d[j] = static_cast<int32_t>(opt_value);
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] dims_min.d[" << j << "] is " << dims_min.d[j];
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] dims_max.d[" << j << "] is " << dims_max.d[j];
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] dims_opt.d[" << j << "] is " << dims_opt.d[j];

          if (input_explicit_shape_ranges[input_name].find(j) == input_explicit_shape_ranges[input_name].end()) {
            std::vector<std::vector<int64_t>> profile_vector(trt_profiles.size());
            input_explicit_shape_ranges[input_name][j] = profile_vector;
          }
          input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(min_value);
          input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(max_value);
          input_explicit_shape_ranges[input_name][static_cast<int64_t>(j)][i].push_back(opt_value);
        } else {
          dims_min.d[j] = dims.d[j];
          dims_max.d[j] = dims.d[j];
          dims_opt.d[j] = dims.d[j];
        }
      }

      trt_profile->setDimensions(input_name.c_str(), nvinfer1::OptProfileSelector::kMIN, dims_min);
      trt_profile->setDimensions(input_name.c_str(), nvinfer1::OptProfileSelector::kMAX, dims_max);
      trt_profile->setDimensions(input_name.c_str(), nvinfer1::OptProfileSelector::kOPT, dims_opt);
    }
  }
  return true;
}

#define CASE_GET_INPUT_TENSOR(DATA_TYPE, SrcT)                                              \
  case DATA_TYPE: {                                                                         \
    auto input_tensor_ptr = input_tensor.GetTensorData<SrcT>();                             \
    if (input_tensor_ptr != nullptr && elem_cnt > 0) {                                      \
      data = const_cast<SrcT*>(input_tensor_ptr);                                           \
    } else {                                                                                \
      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, 1)); \
      data = scratch_buffers.back().get();                                                  \
    }                                                                                       \
    break;                                                                                  \
  }

//#define CASE_GET_CAST_INPUT_TENSOR(DATA_TYPE, SrcT, DstT)                                                         \
//  case DATA_TYPE: {                                                                                               \
//    auto input_tensor_ptr = input_tensor.GetTensorData<SrcT>();                                                   \
//    if (input_tensor_ptr != nullptr && elem_cnt > 0) {                                                            \
//      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, elem_cnt * sizeof(DstT))); \
//      data = scratch_buffers.back().get();                                                                        \
//      cuda::Impl_Cast<SrcT, DstT>(stream, input_tensor_ptr, reinterpret_cast<DstT*>(data), elem_cnt);             \
//    } else {                                                                                                      \
//      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, 1));                       \
//      data = scratch_buffers.back().get();                                                                        \
//    }                                                                                                             \
//    break;                                                                                                        \
//  }

#define CASE_GET_OUTPUT_TENSOR(DATA_TYPE, SrcT)                                             \
  case DATA_TYPE: {                                                                         \
    auto output_tensor_ptr = output_tensor.GetTensorMutableData<SrcT>();                    \
    if (output_tensor_ptr != nullptr && elem_cnt > 0) {                                     \
      buffers[output_name] = output_tensor_ptr;                                             \
    } else {                                                                                \
      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, 1)); \
      buffers[output_name] = scratch_buffers.back().get();                                  \
    }                                                                                       \
    break;                                                                                  \
  }

#define CASE_GET_CAST_OUTPUT_TENSOR(DATA_TYPE, SrcT, DstT)                                                        \
  case DATA_TYPE: {                                                                                               \
    auto output_tensor_ptr = output_tensor.GetTensorMutableData<SrcT>();                                          \
    if (output_tensor_ptr != nullptr && elem_cnt > 0) {                                                           \
      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, elem_cnt * sizeof(DstT))); \
      buffers[output_name] = scratch_buffers.back().get();                                                        \
      output_dim_sizes[i] = static_cast<int>(elem_cnt);                                                           \
    } else {                                                                                                      \
      scratch_buffers.push_back(MakeUniquePtrFromOrtAllocator<void>(alloc, 1));                       \
      buffers[output_name] = scratch_buffers.back().get();                                                        \
      output_dim_sizes[i] = 1;                                                                                    \
    }                                                                                                             \
    break;                                                                                                        \
  }

#define CASE_COPY_TENSOR(DATA_TYPE, DstT)                                                                                                          \
  case DATA_TYPE: {                                                                                                                                \
    auto output_tensor_ptr = output_tensor.GetTensorMutableData<DstT>();                                                                           \
    if (output_tensor_ptr != nullptr && elem_cnt > 0) {                                                                                            \
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(output_tensor_ptr, allocator->getBuffer(), elem_cnt * sizeof(DstT), cudaMemcpyDeviceToDevice, stream)); \
    }                                                                                                                                              \
    break;                                                                                                                                         \
  }

//#define CASE_CAST_TENSOR(DATA_TYPE, SrcT, DstT)                                                                                                   \
//  case DATA_TYPE: {                                                                                                                               \
//    auto output_tensor_ptr = output_tensor.GetTensorMutableData<DstT>();                                                                          \
//    if (output_tensor_ptr != nullptr && elem_cnt > 0) {                                                                                           \
//      cuda::Impl_Cast<SrcT, DstT>(stream, reinterpret_cast<SrcT*>(allocator->getBuffer()), reinterpret_cast<DstT*>(output_tensor_ptr), elem_cnt); \
//    }                                                                                                                                             \
//    break;                                                                                                                                        \
//  }

OrtStatusPtr BindContextInput(Ort::KernelContext& ctx,
                        nvinfer1::ICudaEngine* trt_engine,
                        nvinfer1::IExecutionContext* trt_context,
                        const char* input_name,
                        size_t input_index,
                        std::unordered_map<std::string, std::vector<int32_t>>& shape_tensor_values,
                        std::unordered_map<std::string, std::vector<int64_t>>& shape_tensor_values_int64,
                        std::vector<IAllocatorUniquePtr<void>>& scratch_buffers,
                        OrtAllocator* alloc,
                        cudaStream_t stream) {
  auto input_tensor = ctx.GetInput(input_index);
  auto tensor_info = input_tensor.GetTensorTypeAndShapeInfo();
  const auto tensor_shapes = tensor_info.GetShape();
  const auto tensor_type = tensor_info.GetElementType();
  /*
   * Return the number of elements specified by the tensor shape (all dimensions multiplied by each other).
   * For 0 dimensions, 1 is returned. If any dimension is less than 0, the result is always -1.
   *
   * Examples:<br>
   * [] = 1<br>
   * [1,3,4] = 12<br>
   * [2,0,4] = 0<br>
   * [-1,3,4] = -1<br>
   */
  const auto elem_cnt = tensor_info.GetElementCount();

  if (trt_engine->isShapeInferenceIO(input_name)) {
    // Bind "shape tensor" input buffer

    // The shape of the "shape tensor" is either zero dimension (scalar) or 1-dimension
    int shape_size = trt_engine->getTensorShape(input_name).nbDims == 0 ? 1 : static_cast<int>(tensor_shapes[0]);
    switch (tensor_type) {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: {
        // get shape tensor value if not present
        if (shape_tensor_values.find(input_name) == shape_tensor_values.end()) {
          auto input = std::make_unique<int32_t[]>(shape_size);
          GetShapeOfShapeTensor<int32_t>(input_tensor, input.get(), shape_size, stream);
          shape_tensor_values[input_name].resize(shape_size);
          for (int i = 0; i < shape_size; ++i) {
            shape_tensor_values[input_name][i] = input[i];
          }
        }

        if (!trt_context->setTensorAddress(input_name, &shape_tensor_values[input_name][0])) {
          std::string error_input_name = input_name;
          std::string error_msg =
              "TensorRT EP failed to call nvinfer1::IExecutionContext::setTensorAddress() for shape input '" +
              error_input_name + "'";
          return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, error_msg.c_str());
        }
        break;
      }
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: {
        // get shape tensor value if not present
        if (shape_tensor_values_int64.find(input_name) == shape_tensor_values_int64.end()) {
          auto input = std::make_unique<int64_t[]>(shape_size);
          GetShapeOfShapeTensor<int64_t>(input_tensor, input.get(), shape_size, stream);
          shape_tensor_values_int64[input_name].resize(shape_size);
          for (int i = 0; i < shape_size; ++i) {
            shape_tensor_values_int64[input_name][i] = input[i];
          }
        }

        if (!trt_context->setTensorAddress(input_name, &shape_tensor_values_int64[input_name][0])) {
          std::string error_input_name = input_name;
          std::string error_msg =
              "TensorRT EP failed to call nvinfer1::IExecutionContext::setTensorAddress() for shape input '" +
              error_input_name + "'";
          return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, error_msg.c_str());
        }
        break;
      }
      default: {
        std::string error_input_name = input_name;
        return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, std::string("The data type of shape tensor should be INT32 or INT64. Please check the data type of " + error_input_name).c_str());
      }
    }
  } else {
    // Set shape for input tensor which is execution tensor
    nvinfer1::Dims dims = trt_context->getTensorShape(input_name);
    int nb_dims = dims.nbDims;
    for (int j = 0, end = nb_dims; j < end; ++j) {
      dims.d[j] = static_cast<int32_t>(tensor_shapes[j]);
    }
    if (!trt_context->setInputShape(input_name, dims)) {
      std::string error_input_name = input_name;
      return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, std::string("TensorRT EP failed to call nvinfer1::IExecutionContext::setInputShape() for input '" + error_input_name + "'").c_str());
    }

    // Bind "execution tensor" input buffer
    //
    // Note: If an engine binding is an empty tensor, it still needs a non-null memory address, and different tensors should have different addresses.
    //       Therefore, in the case of empty tensor, TRT EP always allocates a dummy byte.
    //       https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#empty-tensors
    void* data = nullptr;
    switch (tensor_type) {
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, float)
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, uint16_t)
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, bool)
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8, int8_t)
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, uint8_t)
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, int32_t)
#if NV_TENSORRT_MAJOR >= 10
      CASE_GET_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int64_t)
#else
      // Cast int64 input to int32 input because TensorRT < 10 doesn't support int64
//      CASE_GET_CAST_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int64_t, int32_t)
#endif
      // Cast double input to float because TensorRT doesn't support double
//      CASE_GET_CAST_INPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE, double, float)
      default: {
        return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, std::string("TensorRT EP input onnx tensor data type: " + std::to_string(tensor_type) + " not supported.").c_str());
      }
    }
    trt_context->setTensorAddress(input_name, data);
  }

  return nullptr;
}

OrtStatusPtr BindContextOutput(Ort::KernelContext& ctx,
                         nvinfer1::IExecutionContext* trt_context,
                         const char* output_name,
                         size_t output_index,
                         size_t output_type,
                         size_t i,
                         std::unordered_map<size_t, Ort::UnownedValue>& output_tensors,
                         std::unordered_map<size_t, int>& output_dim_sizes,
                         DDSOutputAllocatorMap& dds_output_allocator_map,
                         std::vector<IAllocatorUniquePtr<void>>& scratch_buffers,
                         OrtAllocator* alloc,
                         std::unordered_map<char const*, void*>& buffers) {
  // Get output shape
  nvinfer1::Dims dims = trt_context->getTensorShape(output_name);
  int nb_dims = dims.nbDims;
  bool is_DDS = false;
  std::vector<int64_t> output_shapes(nb_dims);
  for (int j = 0, end = nb_dims; j < end; ++j) {
    // data-dependent shape
    if (dims.d[j] == -1) {
      is_DDS = true;
      break;
    }
    output_shapes[j] = dims.d[j];
  }

  auto known_DDS = dds_output_allocator_map.find(output_name) != dds_output_allocator_map.end();

  // If the output tensor has data-dependent shape, TRT EP will provide an IOutputAllocator for enqueueV3 to dynamically allocate memory buffer.
  // Once enqueueV3 returns, TRT EP will then bind the output allocation to ORT kernel context output.
  // (Please note that we take strategy A mentioned in https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#dynamic-shaped-output,
  //  which we defer allocation until the size is known and don't call IExecution::setTensorAddress)
  //
  // Otherwise, if the shape of the output tensor is known prior to the runtime, ORT will pre-allocate memory buffer for the output tensor for enqueueV3.
  if (is_DDS || known_DDS) {
    if (!known_DDS) {
      auto allocatorPtr = std::make_unique<OutputAllocator>();
      trt_context->setOutputAllocator(output_name, allocatorPtr.get());
      dds_output_allocator_map[output_name] = std::move(allocatorPtr);
    }
  } else {
    output_tensors[i] = ctx.GetOutput(output_index, output_shapes);
    auto& output_tensor = output_tensors[i];
    const auto elem_cnt = output_tensor.GetTensorTypeAndShapeInfo().GetElementCount();

    switch (output_type) {
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, float)
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, uint16_t)
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, bool)
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8, int8_t)
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, uint8_t)
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, int32_t)
#if NV_TENSORRT_MAJOR >= 10
      CASE_GET_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int64_t)
#else
      // Allocate int32 CUDA memory for int64 output type because TensorRT < 10 doesn't support int64
      CASE_GET_CAST_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int64_t, int32_t)
#endif
      // Allocate float CUDA memory for double output type because TensorRT doesn't support double
      CASE_GET_CAST_OUTPUT_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE, double, float)
      default: {
        return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, std::string("TensorRT EP output tensor data type: " + std::to_string(output_type) + " not supported.").c_str());
      }
    }
    trt_context->setTensorAddress(output_name, buffers[output_name]);
  }

  return nullptr;
}

OrtStatusPtr BindKernelOutput(Ort::KernelContext& ctx,
                        OrtMemoryInfo* /*mem_info*/,
                        DDSOutputAllocatorMap& allocator_map,
                        char const* output_name,
                        size_t output_index,
                        size_t output_type,
                        cudaStream_t stream) {
  auto allocator = allocator_map[output_name].get();
  auto& shape = allocator->getOutputShape();
  auto output_tensor = ctx.GetOutput(output_index, shape);

  /*
   * Return the number of elements specified by the tensor shape (all dimensions multiplied by each other).
   * For 0 dimensions, 1 is returned. If any dimension is less than 0, the result is always -1.
   *
   * Examples:<br>
   * [] = 1<br>
   * [1,3,4] = 12<br>
   * [2,0,4] = 0<br>
   * [-1,3,4] = -1<br>
   */
  auto elem_cnt = output_tensor.GetTensorTypeAndShapeInfo().GetElementCount();

  /*
   * Copy output data from allocation buffer to ORT kernel context output location or
   * cast (int32 or float) -> (int64 or double) to ORT kernel context output location.
   *
   * Note:
   * 1. If the output tensor is empty tensor (i.e. any of the dimension is 0) which means element count is 0,
   *    TRT EP does not perform cuda memory copy nor cuda cast to prevent overwriting other location that might belong to other tensors.
   * 2. The cudaMemcpyAsync() and cuda::Impl_Cast() (implemented as _UnaryElementWise() in cuda ep) are all async, but we
   *    don't need to explicitly call cudaStreamSynchronize() after those APIs due to CUDA EP and TRT EP uses same stream,
   *    and within the same stream, operations are guaranteed to be executed in order.
   */
  switch (output_type) {
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, float)
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, uint16_t)
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, bool)
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8, int8_t)
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, uint8_t)
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, int32_t)
#if NV_TENSORRT_MAJOR >= 10
    CASE_COPY_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int64_t)
#else
    // The allocation buffer holds the int32 output data since TRT doesn't support int64. So, we need to cast the data (int32 -> int64) for ORT kernel output.
//    CASE_CAST_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, int32_t, int64_t)
#endif
    // The allocation buffer holds the float output data since TRT doesn't support double. So, we need to cast the data (float -> double) for ORT kernel output.
//    CASE_CAST_TENSOR(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE, float, double)
    default: {
      return TensorrtExecutionProvider::api_->CreateStatus(ORT_EP_FAIL, std::string("TensorRT EP output tensor data type: " + std::to_string(output_type) + " not supported.").c_str());
    }
  }
  return nullptr;
}

TensorrtExecutionProvider::TensorrtExecutionProvider(const char* ep_type, const ProviderOptions& ep_info) : OrtExecutionProvider() {
    OrtExecutionProvider::GetCapability = [](const OrtExecutionProvider* this_, const OrtGraphViewer* graph, size_t* cnt, OrtIndexedSubGraph*** indexed_sub_graph) {
    };

    OrtExecutionProvider::Compile = [](OrtExecutionProvider* this_, const OrtGraphViewer** graph, const OrtNode** node, size_t cnt, OrtNodeComputeInfo** node_compute_info) -> OrtStatusPtr {
        const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        TensorrtExecutionProvider* p = static_cast<TensorrtExecutionProvider*>(this_);
        this_->extra_param_for_create_state_func = p;
        this_->extra_param_for_compute_func = p;
        for (size_t j = 0; j < cnt; j++) {
            std::unordered_map<std::string, size_t> input_map, output_map;
            size_t input_size = 0;
            api->OrtNode_GetInputSize(node[j], &input_size);
            for (size_t i = 0; i < input_size; i++) {
                const char* ith_input_name = nullptr;
                api->OrtNode_GetIthInputName(node[j], i, &ith_input_name);
                input_map[ith_input_name] = i;
            }

            size_t output_size = 0;
            api->OrtNode_GetOutputSize(node[j], &output_size);
            for (size_t i = 0; i < output_size; i++) {
                const char* ith_output_name = nullptr;
                api->OrtNode_GetIthOutputName(node[j], i, &ith_output_name);
                output_map[ith_output_name] = i;
            }

            OrtStatusPtr ret = nullptr;
            if (GraphHasCtxNode(graph[j])) {
                ret = p->CreateNodeComputeInfoFromPrecompiledEngine(graph[j], node[j], input_map, output_map, &node_compute_info[j]);
            } else {
                ret = p->CreateNodeComputeInfoFromGraph(graph[j], node[j], input_map, output_map, &node_compute_info[j]);
            }
            if (ret != nullptr) return api->CreateStatus(api->GetErrorCode(ret), api->GetErrorMessage(ret));
        }
        return nullptr;
    };

    OrtExecutionProvider::CanCopy = [](const OrtDevice* source, const OrtDevice* target) {
      const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
      OrtMemoryInfoDeviceType source_device_type, target_device_type;
      api->DeviceGetDeviceType(source, &source_device_type);
      api->DeviceGetDeviceType(target, &target_device_type);
      OrtMemoryType source_mem_type, target_mem_type;
      api->DeviceGetMemoryType(source, &source_mem_type);
      api->DeviceGetMemoryType(target, &target_mem_type);

      return source_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU ||
             source_mem_type == OrtMemoryType::OrtMemoryType_CUDA_PINNED ||
             target_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU ||
             target_mem_type == OrtMemoryType::OrtMemoryType_CUDA_PINNED;
    };

    OrtExecutionProvider::CopyTensor = [](const void* src, OrtMemoryInfoDeviceType source_device_type, OrtMemoryType source_mem_type, void* dst, OrtMemoryInfoDeviceType target_device_type, size_t count, void* stream) -> OrtStatusPtr {
        // TODO(leca): convert cudaError_t to OrtStatusPtr
        if (source_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU && target_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU) {
            if (src != dst) {
                stream ? cudaMemcpyAsync(dst, src, count, cudaMemcpyDeviceToDevice, static_cast<cudaStream_t>(stream)) : cudaMemcpy(dst, src, count, cudaMemcpyDeviceToDevice);
            }
            return nullptr;
        }
        if (source_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_CPU && target_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU) {
            if (stream) cudaMemcpyAsync(dst, src, count, cudaMemcpyHostToDevice, static_cast<cudaStream_t>(stream));
            else {
                cudaMemcpy(dst, src, count, cudaMemcpyHostToDevice);
                cudaStreamSynchronize(nullptr);
            }
            return nullptr;
        }
        if (source_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU && target_device_type == OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_CPU) {
            if (stream) cudaMemcpyAsync(dst, src, count, cudaMemcpyDeviceToHost, static_cast<cudaStream_t>(stream));
            else {
                cudaMemcpy(dst, src, count, cudaMemcpyDeviceToHost);
                cudaStreamSynchronize(nullptr);
            }
            return nullptr;
        }
        if (stream && source_mem_type == OrtMemoryType::OrtMemoryType_CUDA_PINNED) {
            cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
        }
        memcpy(dst, src, count);
        return nullptr;
    };

    type = ep_type;
    create_stream = new OrtCreateStream();
    create_stream->CreateStreamFunc = [](const OrtDevice* device) -> void* {
        cudaStream_t stream = nullptr;
        cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
        return stream;
    };

    api_->CreateDevice(OrtMemoryInfoDeviceType::OrtMemoryInfoDeviceType_GPU, OrtMemoryType::OrtMemoryType_Default, 0, &default_device);
}

TensorrtExecutionProviderFactory::TensorrtExecutionProviderFactory() {
    OrtExecutionProviderFactory::CreateExecutionProvider = [](OrtExecutionProviderFactory* this_, const char* const* ep_option_keys, const char* const* ep_option_values, size_t option_size) -> OrtExecutionProvider* {
        ProviderOptions options;
        for (size_t i = 0; i < option_size; i++) options[ep_option_keys[i]] = ep_option_values[i];
        std::unique_ptr<TensorrtExecutionProvider> ret = std::make_unique<TensorrtExecutionProvider>("TensorrtExecutionProvider", std::move(options));
        return ret.release();
    };
}

nvinfer1::IBuilder* TensorrtExecutionProvider::GetBuilder(TensorrtLogger& trt_logger) const {
  if (!builder_) {
    {
      // auto lock = GetApiLock();  // TODO(leca)
      builder_ = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(trt_logger));
    }
  }
  return builder_.get();
}

OrtStatusPtr TensorrtExecutionProvider::RefitEngine(std::string onnx_model_filename,
                                                      std::string& onnx_model_folder_path,
                                                      std::string& weight_stripped_engine_cath_path,
                                                      bool path_check,
                                                      nvinfer1::ICudaEngine* trt_engine,
                                                      bool serialize_refitted_engine,
                                                      bool detailed_build_log) {
#if NV_TENSORRT_MAJOR >= 10
  std::filesystem::path onnx_model_path{onnx_model_folder_path};
  onnx_model_path.append(onnx_model_filename);
  if (path_check && IsAbsolutePath(onnx_model_path.string())) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           std::string("For security purpose, the ONNX model path should be set with "
                           "a relative path, but it is an absolute path: " +
                               onnx_model_path.string()).c_str());
  }
  if (path_check && IsRelativePathToParentPath(onnx_model_path.string())) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           "The ONNX model path has '..'. For security purpose, it's not "
                           "allowed to point outside the directory.");
  }

  if (!std::filesystem::exists(onnx_model_path)) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           std::string("The ONNX model " + onnx_model_path.string() +
                               " does not exist.").c_str());
  }

  // weight-stripped engine refit logic
  TensorrtLogger& trt_logger = GetTensorrtLogger(detailed_build_log);
  auto refitter = std::unique_ptr<nvinfer1::IRefitter>(nvinfer1::createInferRefitter(*trt_engine, trt_logger));
  auto parser_refitter = std::unique_ptr<nvonnxparser::IParserRefitter>(
      nvonnxparser::createParserRefitter(*refitter, trt_logger));
  if (!parser_refitter->refitFromFile(onnx_model_path.string().c_str())) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           std::string("TensorRT EP's IParserRefitter could not refit deserialized weight-stripped engine with weights contained in: " + onnx_model_path.string()).c_str());
  }
  if (refitter->refitCudaEngine()) {
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Successfully refitted the weight-stripped engine.";
  } else {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           std::string("TensorRT EP's IRefitter could not refit deserialized weight-stripped engine with weights contained in: " + onnx_model_path.string()).c_str());
  }

  // serialize the refitted engine to disk
  if (serialize_refitted_engine) {
    std::string refitted_engine_cache = GetWeightRefittedEnginePath(weight_stripped_engine_cath_path);
    nvinfer1::IHostMemory* serialized_engine = trt_engine->serialize();
    std::ofstream engine_file(refitted_engine_cache, std::ios::binary | std::ios::out);
    engine_file.write(reinterpret_cast<const char*>(serialized_engine->data()), serialized_engine->size());
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialize the refitted engine to " << refitted_engine_cache;
  }
  return nullptr;
#else
  return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL, "TensorRT EP's IParserRefitter can only be used on TRT 10.0 onwards.");
#endif
}

OrtStatusPtr TensorrtExecutionProvider::CreateNodeComputeInfoFromGraph(const OrtGraphViewer* graph_body_viewer,
                                                                 const OrtNode* fused_node,
                                                                 std::unordered_map<std::string, size_t>& input_map,
                                                                 std::unordered_map<std::string, size_t>& output_map,
                                                                 OrtNodeComputeInfo** node_compute_funcs) {
  TensorrtLogger& trt_logger = GetTensorrtLogger(detailed_build_log_);
  auto trt_builder = GetBuilder(trt_logger);
  auto network_flags = 0;
#if NV_TENSORRT_MAJOR > 8
  network_flags |= fp16_enable_ || int8_enable_ ? 0 : 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kSTRONGLY_TYPED);
#endif
  network_flags |= 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
  auto trt_network = std::unique_ptr<nvinfer1::INetworkDefinition>(trt_builder->createNetworkV2(network_flags));
  auto trt_config = std::unique_ptr<nvinfer1::IBuilderConfig>(trt_builder->createBuilderConfig());
  auto trt_parser = tensorrt_ptr::unique_pointer<nvonnxparser::IParser>(nvonnxparser::createParser(*trt_network, trt_logger));
  void* buf_data = nullptr;
  size_t buf_size = api_->OrtGraph_SerializeToArray(graph_body_viewer, &buf_data);
  trt_parser->parse(buf_data, buf_size, model_path_);
  trt_config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, max_workspace_size_);

  // Force Pow + Reduce ops in layer norm to run in FP32 to avoid overflow
  if (fp16_enable_ && layer_norm_fp32_fallback_) {
    for (auto idx = 1; idx < trt_network->getNbLayers() - 1; ++idx) {
      auto layer = trt_network->getLayer(idx);
      auto next_layer = trt_network->getLayer(idx + 1);
      if (layer->getType() == nvinfer1::LayerType::kELEMENTWISE && next_layer->getType() == nvinfer1::LayerType::kREDUCE && (static_cast<nvinfer1::IElementWiseLayer*>(layer))->getOperation() == nvinfer1::ElementWiseOperation::kPOW) {
        //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Force Pow + Reduce ops in layer norm to run in FP32 to avoid overflow";
        layer->setPrecision(nvinfer1::DataType::kFLOAT);
        next_layer->setPrecision(nvinfer1::DataType::kFLOAT);
        layer->setOutputType(0, nvinfer1::DataType::kFLOAT);
        next_layer->setOutputType(0, nvinfer1::DataType::kFLOAT);
      }
    }
  }

  int num_inputs = trt_network->getNbInputs();
  int num_outputs = trt_network->getNbOutputs();
  std::unordered_map<std::string, size_t> input_indexes(num_inputs);
  std::unordered_map<std::string, size_t> output_indexes(num_outputs);
  std::unordered_map<std::string, size_t> output_types(num_outputs);

  /*
   * Initialize shape range for each dynamic shape input tensor:
   *   1) If user explicitly specifies optimization profiles via provider options, TRT EP will create those profiles during EP compile time.
   *      It won't make adjustment for profile values during EP compute time.
   *
   *   2) If no explicit optimization profiles provided by user, TRT EP will firstly set min/max/opt shape to [INT_MAX, INT_MIN, INT_MIN].
   *      Later in EP compute time, the shape will be adjusted to [min_input_value, max_input_value, max_input_value] based on input tensor value.
   *
   *
   * Once the TRT profiles are created:
   *   1) If all the dynamic shape input tensors have associated profiles explicitly provided by user, those profiles will be applied to TRT builder config
   *      and the engine will be built at EP compile time.
   *
   *   2) As long as one of the dynamic shape input tensors has no explicitly associated profile, TRT EP will create default shape as described above,
   *      and all the profiles won't be applied and engine won't be built until EP compute time.
   */
  bool has_dynamic_shape = false;  // True if input tensor has dynamic shape and no explicit profile is specified, otherwise false.
  bool has_explicit_profile = false;
  bool apply_explicit_profile = false;
  int num_profiles = 0;
  std::vector<nvinfer1::IOptimizationProfile*> trt_profiles;

  // Following c++ map data structure is used to help serialize/deserialize profiles where it saves dynamic shape dimension(s) and min/max/opt values for dynamic shape input tensor.
  //
  // (1) Single profile case:
  // For example, assume tensor_a has two dynamic shape dimensions: dim_0 and dim_2, and tensor_b
  // has one dynamic shape dimension: dim_1. The data will be:
  // {
  //   tensor_a: {
  //              dim_0: [[min_shape, max_shape, opt_shape]],
  //              dim_2: [[min_shape, max_shape, opt_shape]]
  //   },
  //   tensor_b: {
  //              dim_1: [[min_shape, max_shape, opt_shape]]
  //   }
  // }
  //
  // (2) Multiple profiles case:
  // For example, assume tensor_a has one dynamic shap dimension: dim 0, and tensor_b has one dynamic shape dimension: dim_1,
  // and both of the tensors have two profiles. The data will be:
  // {
  //   tensor_a: {
  //     dim_0: [[min_shape_0, max_shape_0, opt_shape_0], [min_shape_1, max_shape_1, opt_shape_1]]
  //   },
  //   tensor_b: {
  //     dim_1: [[min_shape_2, max_shape_2, opt_shape_2], [min_shape_3, max_shape_3, opt_shape_3]]
  //   }
  // }
  ShapeRangesMap input_explicit_shape_ranges;
  ShapeRangesMap input_implicit_shape_ranges;

  if ((!profile_min_shapes_.empty()) && (!profile_max_shapes_.empty()) && (!profile_opt_shapes_.empty())) {
    has_explicit_profile = true;
    num_profiles = GetNumProfiles(profile_min_shapes_);
    for (int i = 0; i < num_profiles; i++) {
      trt_profiles.push_back(trt_builder->createOptimizationProfile());
    }
  }

  // Iterate all input tensors to check dynamic shape
  for (unsigned int i = 0, end = num_inputs; i < end; ++i) {
    auto input = trt_network->getInput(i);
    const std::string& input_name = input->getName();
    nvinfer1::Dims dims = input->getDimensions();
    int nb_dims = dims.nbDims;

    // Apply explicit optimization profiles provided by user
    if (has_explicit_profile) {
      apply_explicit_profile = ApplyProfileShapesFromProviderOptions(trt_profiles, input, profile_min_shapes_, profile_max_shapes_, profile_opt_shapes_, input_explicit_shape_ranges);
    }

    // If no explicit optimization profile is being applied, TRT EP will later set min/max/opt shape values based on input tensor values at EP compute time
    if (!apply_explicit_profile) {
      if (input->isShapeTensor()) {
        // Shape tensor
        std::vector<std::vector<int64_t>> profile_vector;
        std::vector<int64_t> shape_vector{INT_MAX, INT_MIN, INT_MIN};
        profile_vector.push_back(shape_vector);  // only one profile needed
        input_implicit_shape_ranges[input_name][0] = profile_vector;
        has_dynamic_shape = true;
      } else {
        // Execution tensor
        for (int j = 0, end = nb_dims; j < end; ++j) {
          if (dims.d[j] == -1) {
            std::vector<std::vector<int64_t>> profile_vector;
            std::vector<int64_t> shape_vector{INT_MAX, INT_MIN, INT_MIN};
            profile_vector.push_back(shape_vector);  // only one profile needed
            input_implicit_shape_ranges[input_name][j] = profile_vector;
            has_dynamic_shape = true;
          }
        }
      }
      apply_explicit_profile = false;
    }
  }

  // Set explicit profiles in TRT config if all dynamic shape inputs have associated profiles provided by user
  if (has_explicit_profile) {
    // TRT EP has a constraint here.
    // Users need to provide all the dynamic shape inputs with associated profiles if they want to explicitly specify profiles through provider options.
    if (has_dynamic_shape) {
      std::ostringstream msg;
      msg << "User needs to provide all the dynamic shape inputs with associated profiles if they want to explicitly set profiles through provider options.\n";
      msg << "Please note that main graph could be partitioned into TRT/CUDA/CPU subgraphs, in this case, user also needs to provide shape profiles for the TRT subgraph's input if it's dynamic shape input.\n";
      msg << "Following input(s) has no associated shape profiles provided: ";
      auto begin = input_implicit_shape_ranges.begin();
      auto end = input_implicit_shape_ranges.end();
      auto it = begin;
      if (it != end) {
        msg << it->first;
        ++it;
      }
      for (; it != end; ++it) {
        msg << "," << it->first;
      }
      return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL, msg.str().c_str());
    } else {
      for (auto trt_profile : trt_profiles) {
        trt_config->addOptimizationProfile(trt_profile);
      }
    }
  }
  // If no explicit profile is applied and the input has dynamic shape, TRT EP simply creates one profile by default.
  // It will later set proper min/max/opt shape values duing EP compute time.
  else if (!has_explicit_profile && has_dynamic_shape) {
    trt_profiles.push_back(trt_builder->createOptimizationProfile());
  }

  // Check platform availability for low precision
  if (fp16_enable_) {
    if (!trt_builder->platformHasFastFp16()) {
      fp16_enable_ = false;
      //LOGS_DEFAULT(WARNING) << "[TensorRT EP] ORT_TENSORRT_FP16_ENABLE is set, but platform doesn't support fast native fp16";
    }
  }

  if (int8_enable_) {
    if (!trt_builder->platformHasFastInt8()) {
      int8_enable_ = false;
      //LOGS_DEFAULT(WARNING) << "[TensorRT EP] ORT_TENSORRT_INT8_ENABLE is set, but platform doesn't support fast native int8";
    }
  }

  // Load INT8 calibration table
  std::unordered_map<std::string, float> dynamic_range_map;
  if (int8_enable_ && int8_calibration_cache_available_) {
    const std::string calibration_cache_path = GetCachePath(cache_path_, int8_calibration_cache_name_);
    if (!ReadDynamicRange(calibration_cache_path, int8_use_native_tensorrt_calibration_table_, dynamic_range_map)) {
      throw std::runtime_error("Failed to read INT8 calibration table " + calibration_cache_path);
    }
  }

  // Set precision flags
  const char* node_name = nullptr;
  api_->OrtNode_GetName(fused_node, &node_name);
  std::string trt_node_name_with_precision(node_name);
  if (fp16_enable_ && int8_enable_) {
    trt_config->setFlags(1U << static_cast<uint32_t>(nvinfer1::BuilderFlag::kFP16) | 1U << static_cast<uint32_t>(nvinfer1::BuilderFlag::kINT8));
    trt_node_name_with_precision += "_fp16_int8";
    //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] FP16 and INT8 mode is enabled";
  } else if (fp16_enable_) {
    trt_config->setFlag(nvinfer1::BuilderFlag::kFP16);
    trt_node_name_with_precision += "_fp16";
    //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] FP16 mode is enabled";
  } else if (int8_enable_) {
    trt_config->setFlag(nvinfer1::BuilderFlag::kINT8);
    trt_node_name_with_precision += "_int8";
    //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] INT8 mode is enabled";
  }

  // Set DLA
  if (fp16_enable_ || int8_enable_) {
    if (dla_enable_ && dla_core_ >= 0) {  // DLA can only run with FP16 and INT8
      int number_of_dla_core = trt_builder->getNbDLACores();
      if (number_of_dla_core == 0) {
        //LOGS_DEFAULT(WARNING) << "[TensorRT EP] Try to use DLA core, but platform doesn't have any DLA core";
        dla_enable_ = false;
      } else {
        if (dla_core_ >= number_of_dla_core) {
          //LOGS_DEFAULT(WARNING) << "[TensorRT EP] Try to use DLA core #" << dla_core_ << ", but it exceeds platform's maximum DLA core number " << number_of_dla_core << ". Use DLA core 0 instead.";
          dla_core_ = 0;
        }
        //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] use DLA core " << dla_core_;
        trt_config->setFlag(nvinfer1::BuilderFlag::kGPU_FALLBACK);
        trt_config->setDefaultDeviceType(nvinfer1::DeviceType::kDLA);
        trt_config->setDLACore(dla_core_);
        trt_node_name_with_precision += "_dlacore" + std::to_string(dla_core_);
      }
    }
  }

  // enable sparse weights
  if (sparsity_enable_) {
    trt_config->setFlag(nvinfer1::BuilderFlag::kSPARSE_WEIGHTS);
    //LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Sparse weights are allowed";
  }
#if NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR == 5
  if (build_heuristics_enable_) {
    trt_config->setFlag(nvinfer1::BuilderFlag::kENABLE_TACTIC_HEURISTIC);
    //LOGS_DEFAULT(WARNING) << "[TensorRT EP] Builder heuristics are enabled."
    //                      << " For TRT > 8.5, trt_build_heuristics_enable is deprecated, please set builder optimization level as 2 to enable builder heuristics.";
  }
#elif NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR > 5 || NV_TENSORRT_MAJOR > 8
  // for TRT 8.6 onwards, heuristic-based tactic option is automatically enabled by setting builder optimization level 2
  if (build_heuristics_enable_) {
    if (builder_optimization_level_ == 2) {
      //LOGS_DEFAULT(WARNING) << "[TensorRT EP] Builder heuristics are automatically enabled by builder optimization level 2. trt_build_heuristics_enable is deprecated on TRT 8.6 onwards.";
    } else {
      //LOGS_DEFAULT(WARNING) << "[TensorRT EP] trt_build_heuristics_enable is deprecated on TRT 8.6 onwards. Please set builder optimization level as 2 to enable builder heuristics.";
    }
  }
#endif

//#if NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR > 5 || NV_TENSORRT_MAJOR > 8
//  // switch optimizaion level
//  if (builder_optimization_level_ != 3) {
//    trt_config->setBuilderOptimizationLevel(builder_optimization_level_);
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Builder optimization level is set to " << builder_optimization_level_;
//  }
//
//  // limit auxiliary streams
//  if (auxiliary_streams_ >= 0) {
//    trt_config->setMaxAuxStreams(auxiliary_streams_);
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Auxiliary streams are se to " << auxiliary_streams_;
//  }
//#else
//  if (builder_optimization_level_ != 3) {
//    LOGS_DEFAULT(WARNING) << "[TensorRT EP] Builder optimization level can only be used on TRT 8.6 onwards!";
//  }
//  if (auxiliary_streams_ >= 0) {
//    LOGS_DEFAULT(WARNING) << "[TensorRT EP] Auxiliary streams can only be set on TRT 8.6 onwards!";
//  }
//#endif
//
//  if (weight_stripped_engine_enable_) {
//#if NV_TENSORRT_MAJOR >= 10
//    trt_config->setFlag(nvinfer1::BuilderFlag::kSTRIP_PLAN);
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] STRIP_PLAN is enabled";
//    trt_config->setFlag(nvinfer1::BuilderFlag::kREFIT_IDENTICAL);
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] REFIT_IDENTICAL is enabled";
//#else
//    LOGS_DEFAULT(WARNING) << "[TensorRT EP] weight-stripped engines can only be used on TRT 10.0 onwards!";
//#endif
//  }
//
//  // limit used tactic sources
//  if (!tactic_sources_.empty()) {
//    nvinfer1::TacticSources tactics = trt_config->getTacticSources();
//    tactics |= GetTacticSourceFromString(tactic_sources_);
//    trt_config->setTacticSources(tactics);
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Tactic sources are limited using " << tactic_sources_;
//  }
//
//  // Build TRT engine (if needed) and load TRT engine if:
//  //   (1) Graph has no dynamic shape input
//  //   (2) All the dynamic shape inputs have associated explicit profiles specified by user
//  //
//  // Otherwise engine will be handled at inference time.
//  std::unique_ptr<nvinfer1::ICudaEngine> trt_engine;
//  std::unique_ptr<nvinfer1::IExecutionContext> trt_context;
//
//  std::string cache_path = "";
//  std::string cache_suffix = "";
//  // Customize cache prefix if assigned
//  if (!cache_prefix_.empty()) {
//    // Generate cache suffix in case user would like to customize cache prefix
//    cache_suffix = "_" + GetCacheSuffix(fused_node.Name(), trt_node_name_with_precision);
//    cache_path = GetCachePath(cache_path_, cache_prefix_) + cache_suffix;
//  } else {
//    cache_path = GetCachePath(cache_path_, trt_node_name_with_precision);
//  }
//
//  std::string cache_hw_compat = "_sm" + compute_capability_;
//  // Enable hardware compatility mode if assigned
//  if (engine_cache_enable_ && engine_hw_compatible_) {
//    trt_config->setHardwareCompatibilityLevel(nvinfer1::HardwareCompatibilityLevel::kAMPERE_PLUS);
//    cache_hw_compat = "_sm80+";
//    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Hardware compatibility is enabled when loading and capturing engine cache.";
//  }
//
//  // Name the engine cache based on GPU compute capacity and reduce the chance of loading an incompatible cache
//  // Note: Engine cache generated on a GPU with large memory might not be loadable on a GPU with smaller memory, even if they share the same compute capacity
//  const std::string cache_path_prefix = cache_path + cache_hw_compat;
//  std::string engine_cache_path = cache_path_prefix + ".engine";
//  const std::string encrypted_engine_cache_path = engine_cache_path + ".encrypted";
//  const std::string profile_cache_path = cache_path_prefix + ".profile";
//
//  // If weight-stripped engine is enabled and refitted engine cache is not present,
//  // TRT EP will use the engine cache with ".stripped.engine" appended to the end.
//  const std::filesystem::path engine_cache_fs_path = engine_cache_path;
//  if (weight_stripped_engine_enable_ && !std::filesystem::exists(engine_cache_fs_path)) {
//    engine_cache_path = cache_path_prefix + ".stripped.engine";
//    weight_stripped_engine_refit_ = true;
//  }
//
//  // Generate file name for dumping ep context model
//  if (dump_ep_context_model_ && ctx_model_path_.empty()) {
//    ctx_model_path_ = GetCtxModelPath(ep_context_file_path_, model_path_);
//  }
//
//  if (!has_dynamic_shape) {
//    std::string timing_cache_path = "";
//    bool engine_update = false;
//    if (timing_cache_enable_) {
//      timing_cache_path = GetTimingCachePath(global_cache_path_, compute_capability_);
//    }
//    {
//      // ifstream file check, engine serialization/deserialization and engine build are in critical section. It needs lock protection to prevent race condition when inferencing with multithreading.
//      auto lock = GetApiLock();
//
//      // If explicit profile flag is on and engine cache enable flag is on,
//      // we need to compare explicit profiles and profiles used to build the engine in order to decide whether to rebuild the engine.
//      if (has_explicit_profile && engine_cache_enable_) {
//        engine_update = CompareProfiles(profile_cache_path, profile_min_shapes_, profile_max_shapes_, profile_opt_shapes_);
//        if (engine_update) {
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Engine will be built";
//        } else {
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Engine won't be rebuilt";
//        }
//      }
//
//      std::ifstream engine_file(engine_cache_path, std::ios::binary | std::ios::in);
//      if (engine_cache_enable_ && !engine_decryption_enable_ && engine_file && !engine_update) {
//        engine_file.seekg(0, std::ios::end);
//        size_t engine_size = engine_file.tellg();
//        engine_file.seekg(0, std::ios::beg);
//        std::unique_ptr<char[]> engine_buf{new char[engine_size]};
//        engine_file.read((char*)engine_buf.get(), engine_size);
//        trt_engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime_->deserializeCudaEngine(engine_buf.get(), engine_size));
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] DeSerialized " + engine_cache_path;
//        if (trt_engine == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not deserialize engine from cache: " + engine_cache_path);
//        }
//
//      } else if (engine_decryption_enable_ && engine_cache_enable_ && std::filesystem::exists(encrypted_engine_cache_path) && !engine_update) {
//        // Decrypt engine
//        size_t engine_size = 0;
//        if (!engine_decryption_(encrypted_engine_cache_path.c_str(), nullptr, &engine_size)) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not get engine buffer size");
//        }
//        std::unique_ptr<char[]> engine_buf{new char[engine_size]};
//        if (!engine_decryption_(encrypted_engine_cache_path.c_str(), &engine_buf[0], &engine_size)) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not call engine decryption function decrypt");
//        }
//        // Deserialize engine
//        trt_engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime_->deserializeCudaEngine(engine_buf.get(), engine_size));
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Decrypted and DeSerialized " + encrypted_engine_cache_path;
//        if (trt_engine == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not deserialize engine from encrypted cache: " + encrypted_engine_cache_path);
//        }
//      } else {
//        // Set INT8 per tensor dynamic range
//        if (int8_enable_ && trt_builder->platformHasFastInt8() && int8_calibration_cache_available_) {
//#if defined(_MSC_VER)
//#pragma warning(push)
//#pragma warning(disable : 4996)
//#endif
//          trt_config->setInt8Calibrator(nullptr);
//#if defined(_MSC_VER)
//#pragma warning(pop)
//#endif
//          if (!SetDynamicRange(*trt_network, dynamic_range_map)) {
//            return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                   "TensorRT EP could not set INT8 dynamic range for fused node: " + fused_node.Name());
//          }
//        }
//
//        // Load timing cache from file. Create a fresh cache if the file doesn't exist
//        std::unique_ptr<nvinfer1::ITimingCache> timing_cache = nullptr;
//        if (timing_cache_enable_) {
//          std::vector<char> loaded_timing_cache = loadTimingCacheFile(timing_cache_path);
//          timing_cache.reset(trt_config->createTimingCache(static_cast<const void*>(loaded_timing_cache.data()), loaded_timing_cache.size()));
//          if (timing_cache == nullptr) {
//            return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                   "TensorRT EP could not create timing cache: " + timing_cache_path);
//          }
//          trt_config->setTimingCache(*timing_cache, force_timing_cache_match_);
//          if (detailed_build_log_) {
//            LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Deserialized timing cache from " + timing_cache_path;
//          }
//        }
//
//        // Build engine
//        std::chrono::steady_clock::time_point engine_build_start;
//        if (detailed_build_log_) {
//          engine_build_start = std::chrono::steady_clock::now();
//        }
//        std::unique_ptr<nvinfer1::IHostMemory> serialized_engine{trt_builder->buildSerializedNetwork(*trt_network, *trt_config)};
//        if (serialized_engine == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP failed to create engine from network for fused node: " + fused_node.Name());
//        }
//        trt_engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime_->deserializeCudaEngine(serialized_engine->data(), serialized_engine->size()));
//        if (trt_engine == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP failed to deserialize engine for fused node: " + fused_node.Name());
//        }
//        if (detailed_build_log_) {
//          auto engine_build_stop = std::chrono::steady_clock::now();
//          LOGS_DEFAULT(INFO) << "TensorRT engine build for " << trt_node_name_with_precision << " took: " << std::chrono::duration_cast<std::chrono::milliseconds>(engine_build_stop - engine_build_start).count() << "ms" << std::endl;
//        }
//        if (engine_cache_enable_) {
//          // Serialize engine profile if it has explicit profiles
//          if (has_explicit_profile) {
//            SerializeProfileV2(profile_cache_path, input_explicit_shape_ranges);
//            LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized " + profile_cache_path;
//          }
//
//          if (engine_decryption_enable_) {
//            // Encrypt engine. The library is not always deployed with the encrypt function, so check if it is available first.
//            if (engine_encryption_ != nullptr) {
//              if (!engine_encryption_(encrypted_engine_cache_path.c_str(), reinterpret_cast<char*>(serialized_engine->data()), serialized_engine->size())) {
//                return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                       "TensorRT EP call to engine encryption library failed");
//              }
//              LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized and encrypted engine " + encrypted_engine_cache_path;
//            } else {
//              LOGS_DEFAULT(WARNING) << "[TensorRT EP] Engine cache encryption function is not found. No cache is written to disk";
//            }
//          } else {
//            std::ofstream file(engine_cache_path, std::ios::binary | std::ios::out);
//            file.write(reinterpret_cast<char*>(serialized_engine->data()), serialized_engine->size());
//            LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized engine " + engine_cache_path;
//          }
//        }
//        // serialize and save timing cache
//        if (timing_cache_enable_) {
//          auto timing_cache = trt_config->getTimingCache();
//          std::unique_ptr<nvinfer1::IHostMemory> timingCacheHostData{timing_cache->serialize()};
//          if (timingCacheHostData == nullptr) {
//            return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                   "TensorRT EP could not serialize timing cache: " + timing_cache_path);
//          }
//          saveTimingCacheFile(timing_cache_path, timingCacheHostData.get());
//          if (detailed_build_log_) {
//            LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized timing cache " + timing_cache_path;
//          }
//        }
//        // dump EP context node model
//        if (dump_ep_context_model_) {
//          // "ep_cache_context" node attribute should be a relative path to context model directory
//          if (ep_cache_context_attr_.empty()) {
//            auto cache_file_name = std::filesystem::path(engine_cache_path).filename();
//            ep_cache_context_attr_ = std::filesystem::path(engine_cache_relative_path_to_context_model_dir).append(cache_file_name.string()).string();
//          }
//          std::string compute_capability_hw_compat = compute_capability_;
//          if (engine_cache_enable_ && engine_hw_compatible_) {
//            compute_capability_hw_compat = "80+";
//          }
//          std::unique_ptr<ONNX_NAMESPACE::ModelProto> model_proto{CreateCtxModel(graph_body_viewer,
//                                                                                 ep_cache_context_attr_,
//                                                                                 reinterpret_cast<char*>(serialized_engine->data()),
//                                                                                 serialized_engine->size(),
//                                                                                 ep_context_embed_mode_,
//                                                                                 compute_capability_hw_compat,
//                                                                                 model_path_,
//                                                                                 GetLogger())};
//          DumpCtxModel(model_proto.get(), ctx_model_path_);
//        }
//      }
//    }
//
//    if (weight_stripped_engine_refit_) {
//      auto status = RefitEngine(model_path_,
//                                onnx_model_folder_path_,
//                                engine_cache_path,
//                                false /* path check for security */,
//                                trt_engine.get(),
//                                true /* serialize refitted engine to disk */,
//                                detailed_build_log_);
//      if (status != Status::OK()) {
//        return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, status.ErrorMessage());
//      }
//    }
//
//    // Build context
//    // Note: Creating an execution context from an engine is thread safe per TRT doc
//    // https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
//    if (context_memory_sharing_enable_) {
//#if defined(_MSC_VER)
//#pragma warning(push)
//#pragma warning(disable : 4996)
//#endif
//      size_t mem_size = trt_engine->getDeviceMemorySize();
//#if defined(_MSC_VER)
//#pragma warning(pop)
//#endif
//      if (mem_size > max_ctx_mem_size_) {
//        max_ctx_mem_size_ = mem_size;
//      }
//#if NV_TENSORRT_MAJOR < 10
//      trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContextWithoutDeviceMemory());
//#else
//      trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
//#endif
//    } else {
//      trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContext());
//    }
//    if (!trt_context) {
//      return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                             "TensorRT EP could not build execution context for fused node: " + fused_node.Name());
//    }
//  }
//
//  // Create input to index map
//  for (int i = 0; i < num_inputs; ++i) {
//    auto input = trt_network->getInput(i);
//    const std::string& input_name = input->getName();
//    const auto& iter = input_map.find(input_name);
//    if (iter != input_map.end()) {
//      input_indexes[input_name] = iter->second;
//    }
//  }
//
//  // Create output to index and type maps
//  const auto& graph_output = model_proto->graph().output();
//  for (int i = 0; i < num_outputs; ++i) {
//    const std::string& output_name = trt_network->getOutput(i)->getName();
//    const auto& iter = output_map.find(output_name);
//    if (iter != output_map.end()) {
//      output_indexes[output_name] = iter->second;
//    }
//    const auto& tensor_type = graph_output[i].type().tensor_type();
//    output_types[output_name] = tensor_type.elem_type();
//  }
//
//  // Save TRT engine, other TRT objects and input/output info to map
//  parsers_.emplace(fused_node.Name(), std::move(trt_parser));
//  engines_.emplace(fused_node.Name(), std::move(trt_engine));
//  contexts_.emplace(fused_node.Name(), std::move(trt_context));
//  networks_.emplace(fused_node.Name(), std::move(trt_network));
//  input_info_[fused_node.Name()].push_back(input_indexes);
//  output_info_[fused_node.Name()].push_back(output_indexes);
//  output_info_[fused_node.Name()].push_back(output_types);
//  input_shape_ranges_[fused_node.Name()] = input_implicit_shape_ranges;
//  profiles_.emplace(fused_node.Name(), std::move(trt_profiles));
//
//  // For dynamic shape input model, firstly TRT EP creates a model proto which includes inputs, outputs and empty engine.
//  // TRT EP will serialize the model at inference time due to engine can be updated and the updated engine should be included in the model.
//  // However, if the embed_mode is 0 (only includes engine path), TRT EP will serialize it here.
//  if (dump_ep_context_model_ && has_dynamic_shape) {
//    // "ep_cache_context" node attribute should be a relative path to context model directory
//    if (ep_cache_context_attr_.empty()) {
//      auto cache_file_name = std::filesystem::path(engine_cache_path).filename();
//      ep_cache_context_attr_ = std::filesystem::path(engine_cache_relative_path_to_context_model_dir).append(cache_file_name.string()).string();
//    }
//    std::string compute_capability_hw_compat = compute_capability_;
//    if (engine_cache_enable_ && engine_hw_compatible_) {
//      compute_capability_hw_compat = "80+";
//    }
//    model_proto_.reset(CreateCtxModel(graph_body_viewer,
//                                      ep_cache_context_attr_,
//                                      nullptr,
//                                      0,
//                                      ep_context_embed_mode_,
//                                      compute_capability_hw_compat,
//                                      model_path_,
//                                      GetLogger()));
//    if (ep_context_embed_mode_ == 0) {
//      DumpCtxModel(model_proto_.get(), ctx_model_path_);
//    }
//  }
//
//  // Create function state
//  // TODO: remove default capture
//  NodeComputeInfo compute_info;
//  compute_info.create_state_func = [=](ComputeContext* context, FunctionState* state) {
//    std::unique_ptr<TensorrtFuncState> p = std::make_unique<TensorrtFuncState>();
//    // translate tactic sources string to nvinfer1::TacticSources
//    nvinfer1::TacticSources tactics = 0;
//    if (!tactic_sources_.empty()) {
//      tactics = GetTacticSourceFromString(tactic_sources_);
//    }
//    *p = {context->allocate_func, context->release_func, context->allocator_handle, context->node_name, builder_.get(),
//          &parsers_[context->node_name], &engines_[context->node_name], &contexts_[context->node_name],
//          &networks_[context->node_name], input_info_[context->node_name], output_info_[context->node_name],
//          input_shape_ranges_[context->node_name], &tensorrt_mu_, fp16_enable_, int8_enable_, int8_calibration_cache_available_,
//          dla_enable_, dla_core_, &max_workspace_size_, trt_node_name_with_precision,
//          engine_cache_enable_, cache_path_, runtime_.get(), profiles_[context->node_name],
//          context_memory_sharing_enable_, &max_ctx_mem_size_, dynamic_range_map, engine_decryption_enable_,
//          engine_decryption_, engine_encryption_, timing_cache_enable_, global_cache_path_, force_timing_cache_match_,
//          detailed_build_log_, build_heuristics_enable_, sparsity_enable_, builder_optimization_level_,
//          auxiliary_streams_, !tactic_sources_.empty(), tactics, cuda_graph_enable_, cache_prefix_, cache_suffix, engine_hw_compatible_};
//    *state = p.release();
//    return 0;
//  };
//
//  // Release function state
//  compute_info.release_state_func = [](FunctionState state) {
//    delete static_cast<TensorrtFuncState*>(state);
//  };
//
//  // Create compute function
//  compute_info.compute_func = [this](FunctionState state, const OrtApi* api, OrtKernelContext* context) {
//    Ort::KernelContext ctx(context);
//
//    TensorrtFuncState* trt_state = reinterpret_cast<TensorrtFuncState*>(state);
//
//    // The whole compute_function should be considered the critical section where multiple threads may update kernel function state, access one builder, create/serialize/save engine,
//    // save profile and serialize/save timing cache. Therefore, those operations should be synchronized across different threads when ORT is using multithreading.
//    // More details here, https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
//    std::lock_guard<OrtMutex> lock(*(trt_state->tensorrt_mu_ptr));
//    const std::unordered_map<std::string, size_t>& input_indexes = (trt_state->input_info)[0];
//    const std::unordered_map<std::string, size_t>& output_indexes = (trt_state->output_info)[0];
//    const std::unordered_map<std::string, size_t>& output_types = (trt_state->output_info)[1];
//    auto fused_node_name = trt_state->fused_node_name;
//    // This map "shape_ranges" contains the shape range info for setting TRT optimization profiles.
//    // The info is used for both shape tensor and execution tensor:
//    // tensor name->(dimension->[min, max, opt])
//    auto& shape_ranges = trt_state->input_shape_ranges;
//    std::unordered_map<std::string, std::vector<int32_t>> shape_tensor_values;        // This map holds "shape tensor -> shape values" for the shape tensor input across this inference run
//    std::unordered_map<std::string, std::vector<int64_t>> shape_tensor_values_int64;  // same as above but for int64 shape tensor input
//    auto& dds_output_allocator_map = this->dds_output_allocator_maps_[fused_node_name];
//    auto trt_builder = trt_state->builder;
//    auto trt_engine = trt_state->engine->get();
//    auto trt_context = trt_state->context->get();
//    auto trt_profiles = trt_state->profiles;
//    auto max_context_mem_size_ptr = trt_state->max_context_mem_size_ptr;
//    int num_inputs = static_cast<int>(input_indexes.size());
//    int num_outputs = static_cast<int>(output_indexes.size());
//    bool engine_update = false;
//    bool context_update = false;
//    std::unordered_set<std::string> input_names;
//
//    OrtDevice device(OrtDevice::GPU, OrtDevice::MemType::DEFAULT, narrow<OrtDevice::DeviceId>(device_id_));
//    OrtMemoryInfo mem_info("", OrtAllocatorType::OrtDeviceAllocator, device, device_id_);
//    if (alloc_ == nullptr) {
//      Ort::ThrowOnError(api->KernelContext_GetAllocator(context, &mem_info, &alloc_));
//    }
//    OrtAllocator* alloc = alloc_;
//
//    void* cuda_stream;
//    Ort::ThrowOnError(api->KernelContext_GetGPUComputeStream(context, &cuda_stream));
//    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);
//
//    // Name the engine cache based on GPU compute capacity and reduce the chance of loading an incompatible cache
//    // Note: Engine cache generated on a GPU with large memory might not be loadable on a GPU with smaller memory, even if they share the same compute capacity
//    // Prepare cache name
//    std::string cache_path = "";
//    // Customize cache prefix if assigned
//    if (!cache_prefix_.empty()) {
//      cache_path = GetCachePath(trt_state->engine_cache_path, trt_state->cache_prefix) + trt_state->cache_suffix;
//    } else {
//      cache_path = GetCachePath(trt_state->engine_cache_path, trt_state->trt_node_name_with_precision);
//    }
//
//    // Enable hardware compatility mode if assigned
//    std::string cache_hw_compat = "_sm" + compute_capability_;
//    if (engine_cache_enable_ && engine_hw_compatible_) {
//      cache_hw_compat = "_sm80+";
//      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Hardware compatibility is enabled when loading and capturing engine cache.";
//    }
//
//    // Name the engine cache based on GPU compute capacity and reduce the chance of loading an incompatible cache
//    // Note: Engine cache generated on a GPU with large memory might not be loadable on a GPU with smaller memory, even if they share the same compute capacity
//    const std::string cache_path_prefix = cache_path + cache_hw_compat;
//    std::string engine_cache_path = cache_path_prefix + ".engine";
//    const std::string encrypted_engine_cache_path = engine_cache_path + ".encrypted";
//    const std::string profile_cache_path = cache_path_prefix + ".profile";
//    std::string timing_cache_path = "";
//    if (timing_cache_enable_) {
//      timing_cache_path = GetTimingCachePath(global_cache_path_, compute_capability_);
//    }
//
//    // If weight-stripped engine is enabled and refitted engine cache is not present,
//    // TRT EP will use the engine cache with ".stripped.engine" appended to the end.
//    const std::filesystem::path engine_cache_fs_path = engine_cache_path;
//    if (weight_stripped_engine_enable_ && !std::filesystem::exists(engine_cache_fs_path)) {
//      engine_cache_path = cache_path_prefix + ".stripped.engine";
//      weight_stripped_engine_refit_ = true;
//    }
//
//    // Load serialized engine
//    if (trt_state->engine_cache_enable && trt_engine == nullptr) {
//      std::ifstream engine_file(engine_cache_path, std::ios::binary | std::ios::in);
//      std::ifstream profile_file(profile_cache_path, std::ios::binary | std::ios::in);
//      if (engine_file && !trt_state->engine_decryption_enable && profile_file) {
//        // Deserialize profile
//        shape_ranges = DeserializeProfileV2(profile_file);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] DeSerialized " + profile_cache_path;
//
//        // Prepare buffer
//        engine_file.seekg(0, std::ios::end);
//        size_t engine_size = engine_file.tellg();
//        engine_file.seekg(0, std::ios::beg);
//        std::unique_ptr<char[]> engine_buf{new char[engine_size]};
//        engine_file.read((char*)engine_buf.get(), engine_size);
//
//        // Deserialize engine
//        // Note: Deserializing an engine from a TensorRT runtime is thread safe per TRT doc
//        // https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
//        trt_state->engine->reset();
//        *(trt_state->engine) = std::unique_ptr<nvinfer1::ICudaEngine>(
//            trt_state->runtime->deserializeCudaEngine(engine_buf.get(), engine_size));
//        if (!(*(trt_state->engine))) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP Failed to Build Engine.");
//        }
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] DeSerialized " + engine_cache_path;
//        trt_engine = trt_state->engine->get();
//        context_update = true;
//
//      } else if (trt_state->engine_decryption_enable && std::filesystem::exists(encrypted_engine_cache_path) && profile_file) {
//        shape_ranges = DeserializeProfileV2(profile_file);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] DeSerialized " + profile_cache_path;
//        // Decrypt engine
//        size_t engine_size = 0;
//        if (!trt_state->engine_decryption(encrypted_engine_cache_path.c_str(), nullptr, &engine_size)) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not get engine buffer size");
//        }
//        std::unique_ptr<char[]> engine_buf{new char[engine_size]};
//        if (!trt_state->engine_decryption(encrypted_engine_cache_path.c_str(), &engine_buf[0], &engine_size)) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not call engine decryption function decrypt");
//        }
//        // Deserialize engine
//        // Note: Deserializing an engine from a TensorRT runtime is thread safe per TRT doc
//        // https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
//        trt_state->engine->reset();
//        *(trt_state->engine) = std::unique_ptr<nvinfer1::ICudaEngine>(trt_state->runtime->deserializeCudaEngine(engine_buf.get(), engine_size));
//        if (!(*(trt_state->engine))) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not deserialize engine from encrypted cache: " + encrypted_engine_cache_path);
//        }
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Decrypted and DeSerialized " + encrypted_engine_cache_path;
//        trt_engine = trt_state->engine->get();
//        context_update = true;
//      }
//    }
//
//    // Check and update shape ranges for dynamic shape inputs.
//    for (int i = 0, end = num_inputs; i < end; ++i) {
//      auto input = trt_state->network->get()->getInput(i);
//      const std::string& input_name = input->getName();
//      input_names.insert(input_name);
//
//      // If there is any input tensor in shape_ranges, it means this input tensor has dynamic shape and its profile shape values have not yet resolved.
//      // TRT EP will help determine the min/max/opt profile values based on current input tensor value.
//      if (shape_ranges.find(input_name) != shape_ranges.end()) {
//        auto status = ApplyProfileShapesFromInputTensorValue(trt_profiles, ctx, input, shape_ranges, input_indexes, shape_tensor_values, shape_tensor_values_int64, stream, &engine_update);
//        if (status != Status::OK()) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP failed to parse input tensor and generate optimization profiles.");
//        }
//      }
//    }
//
//    // Regenerate engine
//    if (engine_update) {
//      // Destroy the IExecutionContext objects before destroying an engine object, otherwise it will lead to undefined behavior.
//      trt_state->context->reset();
//      trt_state->engine->reset();
//      auto trt_config = std::unique_ptr<nvinfer1::IBuilderConfig>(trt_builder->createBuilderConfig());
//      trt_config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, *(trt_state->max_workspace_size_ptr));
//      for (auto trt_profile : trt_profiles) {
//        trt_config->addOptimizationProfile(trt_profile);
//      }
//
//      // Set INT8 Per Tensor Dynamic range
//      if (trt_state->int8_enable && trt_builder->platformHasFastInt8() && trt_state->int8_calibration_cache_available) {
//#if defined(_MSC_VER)
//#pragma warning(push)
//#pragma warning(disable : 4996)
//#endif
//        trt_config->setInt8Calibrator(nullptr);
//#if defined(_MSC_VER)
//#pragma warning(pop)
//#endif
//        if (!SetDynamicRange(*trt_state->network->get(), trt_state->dynamic_range_map)) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP failed to set INT8 dynamic range.");
//        }
//      }
//
//      // Set precision
//      if (trt_state->fp16_enable && trt_state->int8_enable) {
//        trt_config->setFlags(1U << static_cast<uint32_t>(nvinfer1::BuilderFlag::kFP16) | 1U << static_cast<uint32_t>(nvinfer1::BuilderFlag::kINT8));
//      } else if (trt_state->fp16_enable) {
//        trt_config->setFlag(nvinfer1::BuilderFlag::kFP16);
//      } else if (trt_state->int8_enable) {
//        trt_config->setFlag(nvinfer1::BuilderFlag::kINT8);
//      }
//
//      // Set DLA (DLA can only run with FP16 or INT8)
//      if ((trt_state->fp16_enable || trt_state->int8_enable) && trt_state->dla_enable) {
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] use DLA core " << trt_state->dla_core;
//        trt_config->setFlag(nvinfer1::BuilderFlag::kGPU_FALLBACK);
//        trt_config->setDefaultDeviceType(nvinfer1::DeviceType::kDLA);
//        trt_config->setDLACore(trt_state->dla_core);
//      }
//
//      // enable sparse weights
//      if (trt_state->sparsity_enable) {
//        trt_config->setFlag(nvinfer1::BuilderFlag::kSPARSE_WEIGHTS);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Sparse weights are allowed";
//      }
//#if NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR == 5
//      // enable builder heuristics
//      if (trt_state->build_heuristics_enable) {
//        trt_config->setFlag(nvinfer1::BuilderFlag::kENABLE_TACTIC_HEURISTIC);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Builder heuristics are enabled";
//      }
//#elif NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR > 5 || NV_TENSORRT_MAJOR > 8
//      // switch optimizaion level
//      if (trt_state->builder_optimization_level != 3) {
//        trt_config->setBuilderOptimizationLevel(trt_state->builder_optimization_level);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Builder optimization level is set to " << builder_optimization_level_;
//      }
//
//      // limit auxiliary streams
//      if (trt_state->auxiliary_streams >= 0) {
//        trt_config->setMaxAuxStreams(trt_state->auxiliary_streams);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Auxiliary streams are se to " << trt_state->auxiliary_streams;
//      }
//#else
//      if (trt_state->builder_optimization_level != 3) {
//        LOGS_DEFAULT(WARNING) << "[TensorRT EP] Builder optimization level can only be used on TRT 8.6 onwards!";
//      }
//      if (trt_state->auxiliary_streams >= 0) {
//        LOGS_DEFAULT(WARNING) << "[TensorRT EP] Auxiliary streams can only be set on TRT 8.6 onwards!";
//      }
//#endif
//      if (weight_stripped_engine_enable_) {
//#if NV_TENSORRT_MAJOR >= 10
//        trt_config->setFlag(nvinfer1::BuilderFlag::kSTRIP_PLAN);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] STRIP_PLAN is enabled";
//        trt_config->setFlag(nvinfer1::BuilderFlag::kREFIT_IDENTICAL);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] REFIT_IDENTICAL is enabled";
//#else
//        LOGS_DEFAULT(WARNING) << "[TensorRT EP] weight-stripped engines can only be used on TRT 10.0 onwards!";
//#endif
//      }
//      // limit used tactic sources
//      if (trt_state->filter_tactic_sources) {
//        nvinfer1::TacticSources tactics = trt_config->getTacticSources();
//        tactics |= trt_state->tactic_sources;
//        trt_config->setTacticSources(tactics);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Tactic sources are limited using bitmask " << tactics;
//      }
//
//      // Load timing cache from file. Create a fresh cache if the file doesn't exist
//      std::unique_ptr<nvinfer1::ITimingCache> timing_cache = nullptr;
//      if (trt_state->timing_cache_enable) {
//        std::vector<char> loaded_timing_cache = loadTimingCacheFile(timing_cache_path);
//        timing_cache.reset(trt_config->createTimingCache(static_cast<const void*>(loaded_timing_cache.data()), loaded_timing_cache.size()));
//        if (timing_cache == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not create timing cache: " + timing_cache_path);
//        }
//        trt_config->setTimingCache(*timing_cache, force_timing_cache_match_);
//        if (detailed_build_log_) {
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Deserialized timing cache from " + timing_cache_path;
//        }
//      }
//
//      // Enable hardware compatility mode if assigned
//      if (trt_state->engine_hw_compatible) {
//        trt_config->setHardwareCompatibilityLevel(nvinfer1::HardwareCompatibilityLevel::kAMPERE_PLUS);
//        LOGS_DEFAULT(INFO) << "[TensorRT EP] Re-generate engine with hardware compatibility enabled.";
//      }
//
//      // Build engine
//      std::unique_ptr<nvinfer1::IHostMemory> serialized_engine;
//      {
//        auto lock = GetApiLock();
//        std::chrono::steady_clock::time_point engine_build_start;
//        if (detailed_build_log_) {
//          engine_build_start = std::chrono::steady_clock::now();
//        }
//        serialized_engine = std::unique_ptr<nvinfer1::IHostMemory>(
//            trt_builder->buildSerializedNetwork(*trt_state->network->get(), *trt_config));
//        if (!serialized_engine) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP failed to create engine from network.");
//        }
//        *(trt_state->engine) = std::unique_ptr<nvinfer1::ICudaEngine>(
//            trt_state->runtime->deserializeCudaEngine(serialized_engine->data(), serialized_engine->size()));
//        if (!(*(trt_state->engine))) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP failed to deserialize engine.");
//        }
//        if (detailed_build_log_) {
//          auto engine_build_stop = std::chrono::steady_clock::now();
//          LOGS_DEFAULT(INFO) << "TensorRT engine build for " << trt_state->trt_node_name_with_precision << " took: " << std::chrono::duration_cast<std::chrono::milliseconds>(engine_build_stop - engine_build_start).count() << "ms" << std::endl;
//        }
//      }
//      if (!(*(trt_state->engine))) {
//        return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP Failed to Build Engine.");
//      }
//      trt_engine = trt_state->engine->get();
//      if (trt_state->engine_cache_enable) {
//        // Serialize engine profile
//        SerializeProfileV2(profile_cache_path, shape_ranges);
//        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized " + profile_cache_path;
//
//        // Serialize engine
//        if (trt_state->engine_decryption_enable) {
//          // Encrypt engine. The library is not always deployed with the encrypt function, so check if it is available first.
//          if (trt_state->engine_encryption != nullptr) {
//            if (!trt_state->engine_encryption(encrypted_engine_cache_path.c_str(), reinterpret_cast<char*>(serialized_engine->data()), serialized_engine->size())) {
//              return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                     "TensorRT EP could not call engine encryption function encrypt");
//            }
//            LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized and encrypted engine " + encrypted_engine_cache_path;
//          } else {
//            LOGS_DEFAULT(WARNING) << "[TensorRT EP] Engine cache encryption function is not found. No cache is written to disk";
//          }
//        } else {
//          std::ofstream file(engine_cache_path, std::ios::binary | std::ios::out);
//          file.write(reinterpret_cast<char*>(serialized_engine->data()), serialized_engine->size());
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized " + engine_cache_path;
//        }
//      }
//
//      // serialize and save timing cache
//      if (trt_state->timing_cache_enable) {
//        auto timing_cache = trt_config->getTimingCache();
//        std::unique_ptr<nvinfer1::IHostMemory> timingCacheHostData{timing_cache->serialize()};
//        if (timingCacheHostData == nullptr) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL,
//                                 "TensorRT EP could not serialize timing cache: " + timing_cache_path);
//        }
//        saveTimingCacheFile(timing_cache_path, timingCacheHostData.get());
//        if (detailed_build_log_) {
//          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Serialized timing cache " + timing_cache_path;
//        }
//      }
//
//      // dump ep context model
//      if (dump_ep_context_model_ && ep_context_embed_mode_) {
//        UpdateCtxNodeModelEngineContext(model_proto_.get(), reinterpret_cast<char*>(serialized_engine->data()), serialized_engine->size());
//        DumpCtxModel(model_proto_.get(), ctx_model_path_);
//      }
//      context_update = true;
//
//      if (weight_stripped_engine_refit_) {
//        auto status = RefitEngine(model_path_,
//                                  onnx_model_folder_path_,
//                                  engine_cache_path,
//                                  false /* path check for security */,
//                                  trt_engine,
//                                  true /* serialize refitted engine to disk */,
//                                  detailed_build_log_);
//        if (status != Status::OK()) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, status.ErrorMessage());
//        }
//      }
//    }
//
//    if (context_update) {
//      if (trt_state->context_memory_sharing_enable) {
//#if NV_TENSORRT_MAJOR < 10
//        *(trt_state->context) = std::unique_ptr<nvinfer1::IExecutionContext>(
//            trt_state->engine->get()->createExecutionContextWithoutDeviceMemory());
//#else
//        *(trt_state->context) = std::unique_ptr<nvinfer1::IExecutionContext>(
//            trt_state->engine->get()->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
//#endif
//      } else {
//        *(trt_state->context) = std::unique_ptr<nvinfer1::IExecutionContext>(
//            trt_state->engine->get()->createExecutionContext());
//      }
//      if (!(*(trt_state->context))) {
//        return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "TensorRT EP failed to create context.");
//      }
//      trt_context = trt_state->context->get();
//    }
//
//    // Get input and output binding names
//    int total_bindings = trt_engine->getNbIOTensors();
//    std::vector<char const*> input_binding_names, output_binding_names;
//    for (int i = 0, end = total_bindings; i < end; ++i) {
//      auto const& name = trt_engine->getIOTensorName(i);
//      auto const& mode = trt_engine->getTensorIOMode(name);
//      if (mode == nvinfer1::TensorIOMode::kINPUT) {
//        input_binding_names.push_back(name);
//      } else {
//        output_binding_names.push_back(name);
//      }
//    }
//
//    /*
//     * Set input shapes and bind input buffers
//     */
//    std::vector<IAllocatorUniquePtr<void>> scratch_buffers;
//    for (size_t i = 0, end = input_binding_names.size(); i < end; ++i) {
//      char const* input_name = input_binding_names[i];
//
//      size_t input_index = 0;
//      const auto iter = input_indexes.find(input_name);
//      if (iter != input_indexes.end()) {
//        input_index = iter->second;
//      }
//      auto input_tensor = ctx.GetInput(input_index);
//      auto tensor_info = input_tensor.GetTensorTypeAndShapeInfo();
//      const auto tensor_shapes = tensor_info.GetShape();
//
//      auto status = BindContextInput(ctx, trt_engine, trt_context, input_name, input_index, shape_tensor_values, shape_tensor_values_int64, scratch_buffers, alloc, stream);
//      if (status != Status::OK()) {
//        return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, status.ErrorMessage());
//      }
//    }
//
//    /*
//     * Set output shapes and bind output buffers
//     */
//    std::unordered_map<char const*, void*> buffers;
//    buffers.reserve(num_outputs);
//    using OutputOrtValue = Ort::UnownedValue;
//    std::unordered_map<size_t, OutputOrtValue> output_tensors;
//    output_tensors.reserve(num_outputs);
//    std::unordered_map<size_t, int> output_dim_sizes;
//    output_dim_sizes.reserve(num_outputs);
//
//    for (size_t i = 0, end = output_binding_names.size(); i < end; ++i) {
//      char const* output_name = output_binding_names[i];
//
//      size_t output_index = 0;
//      const auto& index_iter = output_indexes.find(output_name);
//      if (index_iter != output_indexes.end()) {
//        output_index = index_iter->second;
//      }
//
//      size_t output_type = 0;
//      const auto type_iter = output_types.find(output_name);
//      if (type_iter != output_types.end()) {
//        output_type = type_iter->second;
//      }
//
//      Status status = BindContextOutput(ctx, trt_context, output_name, output_index, output_type, i, output_tensors, output_dim_sizes,
//                                        dds_output_allocator_map, scratch_buffers, alloc, buffers);
//      if (status != Status::OK()) {
//        return ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, status.ErrorMessage());
//      }
//    }
//
//    // Set execution context memory
//    if (trt_state->context_memory_sharing_enable) {
//#if defined(_MSC_VER)
//#pragma warning(push)
//#pragma warning(disable : 4996)
//#endif
//      size_t mem_size = trt_engine->getDeviceMemorySize();
//#if defined(_MSC_VER)
//#pragma warning(pop)
//#endif
//      if (mem_size > *max_context_mem_size_ptr) {
//        *max_context_mem_size_ptr = mem_size;
//      }
//      trt_context->setDeviceMemory(IAllocator::MakeUniquePtrFromOrtAllocator<void>(alloc, *max_context_mem_size_ptr).get());
//    }
//
//    // Start CUDA graph capture.
//    // Note: The reason we don't put graph capture in OnRunStart() like CUDA EP does is because
//    // current ORT TRT doesn't get cuda stream until compute time and graph capture requires cuda stream.
//    if (cuda_graph_enable_ && IsGraphCaptureAllowed() && !IsGraphCaptured(0)) {
//      LOGS_DEFAULT(INFO) << "Capturing the cuda graph for this model";
//      cuda_graph_.SetStream(stream);
//      CaptureBegin(0);
//    }
//
//    // Run TRT inference
//    if (!trt_context->enqueueV3(stream)) {
//      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "TensorRT EP execution context enqueue failed.");
//    }
//
//    /*
//     * Given that InferenceSession::Run() is guaranteed to be thread-safe meaning multiple threads can call this function concurrently,
//     * TRT EP needs to carefully take care of concurrency here, if not, following concurrent issue might happen:
//     *
//     * It's suggested that to perform inference concurrently in multiple streams, use one trt execution context per stream.
//     * In the design of TRT EP (Not apply per-thread context implementation) and if multiple threads are calling InferenceSession::Run() concurrently,
//     * the trt execution context instance is shared by all the threads and each thread aquires different stream from ORT.
//     * So TRT EP will end up having one trt execution context using multiple streams which is not suggested.
//     * But, since the whole compute_func() is protected by the lock and if cudaStreamSynchronize() is enforced here, one trt execution context per stream
//     * is guaranteed.
//     *
//     * Therefore, TRT EP needs to call cudaStreamSynchronize() which means to wait until stream has completed all operations to prevent the concurrent issue mentioned above.
//     * However, if cuda graph is enabled, TRT EP won't call cudaStreamSynchronize() since it's not allowed during graph capture.
//     */
//    if (sync_stream_after_enqueue_) {
//      CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(stream));
//    }
//
//    // Assign TRT output back to ORT output
//    // (1) Bind TRT DDS output to ORT kernel context output. (It needs to wait until enqueueV3 is finished)
//    // (2) Cast TRT INT32 output to ORT INT64 output or TRT double output to float output
//    for (size_t i = 0, end = output_binding_names.size(); i < end; ++i) {
//      char const* output_name = output_binding_names[i];
//
//      size_t output_type = 0;
//      const auto& iter = output_types.find(output_name);
//      if (iter != output_types.end()) {
//        output_type = iter->second;
//      }
//
//      if (dds_output_allocator_map.find(output_name) != dds_output_allocator_map.end()) {
//        size_t output_index = 0;
//        const auto& index_iter = output_indexes.find(output_name);
//        if (index_iter != output_indexes.end()) {
//          output_index = index_iter->second;
//        }
//        auto status = BindKernelOutput(ctx, &mem_info, dds_output_allocator_map, output_name, output_index, output_type, stream);
//        if (status != Status::OK()) {
//          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, status.ErrorMessage());
//        }
//      } else {
//        auto& output_tensor = output_tensors[i];
//#if NV_TENSORRT_MAJOR < 10
//        if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
//          auto output_tensor_ptr = output_tensor.GetTensorMutableData<int64_t>();
//          if (output_tensor_ptr != nullptr) {
//            cuda::Impl_Cast<int32_t, int64_t>(stream, reinterpret_cast<int32_t*>(buffers[output_name]), output_tensor_ptr, output_dim_sizes[i]);
//          }
//        }
//#endif
//        if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE) {
//          auto output_tensor_ptr = output_tensor.GetTensorMutableData<double>();
//          if (output_tensor_ptr != nullptr) {
//            cuda::Impl_Cast<float, double>(stream, reinterpret_cast<float*>(buffers[output_name]), output_tensor_ptr, output_dim_sizes[i]);
//          }
//        }
//      }
//    }
//
//    // End CUDA graph capture.
//    // Note: One reason we don't put end of graph capture in OnRunEnd() like CUDA EP does is because of cuda stream mentioned in graph capture
//    // above, another reason is because OnRunEnd() is not synchronized with OnRunStart() and ExecuteGraph() per inference_session.cc.
//    // It's safe to start/end CUDA graph capture in compute_func() here since cuda graph object is maintained by a per thread basis.
//    if (cuda_graph_enable_ && !IsGraphCaptured(0)) {
//      if (IsGraphCaptureAllowed()) {
//        CaptureEnd(0);
//        // CUDA work issued to a capturing stream doesn’t actually run on the GPU,
//        // so run the captured graph here to actually execute the work.
//        ORT_RETURN_IF_ERROR(ReplayGraph(0));
//      } else {
//        IncrementRegularRunCountBeforeGraphCapture();
//      }
//    }
//
//    return nullptr;
//  };

  return nullptr;
}

OrtStatusPtr TensorrtExecutionProvider::CreateNodeComputeInfoFromPrecompiledEngine(const OrtGraphViewer* graph_body_viewer, const OrtNode* fused_node,
                                                                           std::unordered_map<std::string, size_t>& input_map,
                                                                           std::unordered_map<std::string, size_t>& output_map,
                                                                           OrtNodeComputeInfo** node_compute_funcs) {
  std::unique_ptr<nvinfer1::ICudaEngine> trt_engine;
  std::unique_ptr<nvinfer1::IExecutionContext> trt_context;
  std::unordered_map<std::string, size_t> input_indexes;   // TRT engine input name -> ORT kernel context input index
  std::unordered_map<std::string, size_t> output_indexes;  // TRT engine output name -> ORT kernel context output index
  std::unordered_map<std::string, size_t> output_types;    // TRT engine output name -> ORT output tensor type

  // Get engine binary data and deserialize it
  auto trt_cache_model_handler = TensorRTCacheModelHandler(&trt_engine,
                                                           runtime_.get(),
                                                           model_path_,
                                                           compute_capability_,
                                                           weight_stripped_engine_enable_,
                                                           onnx_model_folder_path_,
                                                           detailed_build_log_);
  auto status = trt_cache_model_handler.GetEpContextFromGraph(graph_body_viewer);
  if (status != nullptr) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL, api_->GetErrorMessage(status));
  }

  // Build context
  //
  // Note: Creating an execution context from an engine is thread safe per TRT doc
  // https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
  if (context_memory_sharing_enable_) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    size_t mem_size = trt_engine->getDeviceMemorySize();
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (mem_size > max_ctx_mem_size_) {
      max_ctx_mem_size_ = mem_size;
    }
#if NV_TENSORRT_MAJOR < 10
    trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContextWithoutDeviceMemory());
#else
    trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
#endif
  } else {
    trt_context = std::unique_ptr<nvinfer1::IExecutionContext>(trt_engine->createExecutionContext());
  }

  const char* fused_node_name = nullptr;
  api_->OrtNode_GetName(fused_node, &fused_node_name);
  if (!trt_context) {
    return api_->CreateStatus(OrtErrorCode::ORT_EP_FAIL,
                           std::string("TensorRT EP could not build execution context for fused node: " + std::string(fused_node_name)).c_str());
  }

  // Create input/output to index maps
  for (int32_t i = 0; i < trt_engine->getNbIOTensors(); ++i) {
    auto const& name = trt_engine->getIOTensorName(i);
    auto const& mode = trt_engine->getTensorIOMode(name);
    if (mode == nvinfer1::TensorIOMode::kINPUT) {
      const auto& iter = input_map.find(name);
      if (iter != input_map.end()) {
        input_indexes[name] = iter->second;
      }
    } else {
      const auto& iter = output_map.find(name);
      if (iter != output_map.end()) {
        output_indexes[name] = iter->second;
      }
    }
  }

  // Create output to type map
  size_t graph_output_size = api_->OrtGraph_GetOutputSize(graph_body_viewer);
  for (size_t i = 0; i < graph_output_size; i++) {
    output_types[api_->OrtGraph_GetIthOutputName(graph_body_viewer, i)] = api_->OrtGraph_GetIthOutputElemType(graph_body_viewer, i);
  }

  // Save TRT engine, TRT context and input/output info to map
  engines_.emplace(fused_node_name, std::move(trt_engine));
  contexts_.emplace(fused_node_name, std::move(trt_context));
  input_info_[fused_node_name].push_back(input_indexes);
  output_info_[fused_node_name].push_back(output_indexes);
  output_info_[fused_node_name].push_back(output_types);

  // Create function state
  (*node_compute_funcs)->CreateFunctionStateFunc = [](OrtComputeContext* context, void* extra_param, void** state) -> int {
    TensorrtExecutionProvider* this_ = reinterpret_cast<TensorrtExecutionProvider*>(extra_param);
    std::unique_ptr<TensorrtShortFuncState> p = std::make_unique<TensorrtShortFuncState>();
    *p = { context->AllocateFunc,
           context->DestroyFunc,
           context->allocator_handle,
           context->node_name,
           &(this_->engines_[context->node_name]),
           &(this_->contexts_[context->node_name]),
           this_->input_info_[context->node_name],
           this_->output_info_[context->node_name],
           this_->context_memory_sharing_enable_,
           &this_->max_ctx_mem_size_};
    *state = p.release();
    return 0;
  };

  // Release function state
  (*node_compute_funcs)->DestroyFunctionStateFunc = [](void* state) {
    delete reinterpret_cast<TensorrtShortFuncState*>(state);
  };

  // Create compute function
  (*node_compute_funcs)->ComputeFunc = [](void* state, void* extra_param, const OrtApi* api, OrtKernelContext* context) -> OrtStatusPtr {
    TensorrtExecutionProvider* this_ = reinterpret_cast<TensorrtExecutionProvider*>(extra_param);
    TensorrtShortFuncState* trt_state = reinterpret_cast<TensorrtShortFuncState*>(state);
    Ort::KernelContext ctx(context);

    // The whole compute_function should be considered the critical section.
    // More details here, https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#threading
//TODO(leca):    std::lock_guard<OrtMutex> lock(*(trt_state->tensorrt_mu_ptr));
    const std::unordered_map<std::string, size_t>& input_indexes = (trt_state->input_info)[0];
    const std::unordered_map<std::string, size_t>& output_indexes = (trt_state->output_info)[0];
    const std::unordered_map<std::string, size_t>& output_types = (trt_state->output_info)[1];
    auto fused_node_name = trt_state->fused_node_name;
    auto& dds_output_allocator_map = this_->dds_output_allocator_maps_[fused_node_name];
    auto trt_engine = trt_state->engine->get();
    auto trt_context = trt_state->context->get();
    auto max_context_mem_size_ptr = trt_state->max_context_mem_size_ptr;
    int num_outputs = static_cast<int>(output_indexes.size());
    std::unordered_map<std::string, std::vector<int32_t>> shape_tensor_values;        // This map holds "shape tensor -> shape values" for the shape tensor input across this inference run
    std::unordered_map<std::string, std::vector<int64_t>> shape_tensor_values_int64;  // same as above but for int64 shape tensor input

    OrtMemoryInfo* mem_info = nullptr;
    api->CreateMemoryInfo("Cuda", OrtAllocatorType::OrtDeviceAllocator, this_->device_id_, OrtMemType::OrtMemTypeDefault, &mem_info);
    if (this_->alloc_ == nullptr) {
      Ort::ThrowOnError(api->KernelContext_GetAllocator(context, mem_info, &(this_->alloc_)));
    }
    OrtAllocator* alloc = this_->alloc_;

    void* cuda_stream;
    Ort::ThrowOnError(api->KernelContext_GetGPUComputeStream(context, &cuda_stream));
    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);

    // Get input and output binding names
    int total_bindings = trt_engine->getNbIOTensors();
    std::vector<char const*> input_binding_names, output_binding_names;
    for (int i = 0, end = total_bindings; i < end; ++i) {
      auto const& name = trt_engine->getIOTensorName(i);
      auto const& mode = trt_engine->getTensorIOMode(name);
      if (mode == nvinfer1::TensorIOMode::kINPUT) {
        input_binding_names.push_back(name);
      } else {
        output_binding_names.push_back(name);
      }
    }

    /*
     * Set input shapes and bind input buffers
     */
    std::vector<IAllocatorUniquePtr<void>> scratch_buffers;
    for (size_t i = 0, end = input_binding_names.size(); i < end; ++i) {
      char const* input_name = input_binding_names[i];

      size_t input_index = 0;
      const auto iter = input_indexes.find(input_name);
      if (iter != input_indexes.end()) {
        input_index = iter->second;
      }

      OrtStatusPtr status = BindContextInput(ctx, trt_engine, trt_context, input_name, input_index, shape_tensor_values, shape_tensor_values_int64, scratch_buffers, alloc, stream);
      if (status != nullptr) {
        return api->CreateStatus(OrtErrorCode::ORT_EP_FAIL, api->GetErrorMessage(status));
      }
    }

    /*
     * Set output shapes and bind output buffers
     */
    std::unordered_map<char const*, void*> buffers;
    buffers.reserve(num_outputs);
    using OutputOrtValue = Ort::UnownedValue;
    std::unordered_map<size_t, OutputOrtValue> output_tensors;
    output_tensors.reserve(num_outputs);
    std::unordered_map<size_t, int> output_dim_sizes;
    output_dim_sizes.reserve(num_outputs);

    for (size_t i = 0, end = output_binding_names.size(); i < end; ++i) {
      char const* output_name = output_binding_names[i];

      size_t output_index = 0;
      const auto& index_iter = output_indexes.find(output_name);
      if (index_iter != output_indexes.end()) {
        output_index = index_iter->second;
      }

      size_t output_type = 0;
      const auto type_iter = output_types.find(output_name);
      if (type_iter != output_types.end()) {
        output_type = type_iter->second;
      }

      OrtStatusPtr status = BindContextOutput(ctx, trt_context, output_name, output_index, output_type, i, output_tensors, output_dim_sizes,
                                        dds_output_allocator_map, scratch_buffers, alloc, buffers);
      if (status != nullptr) {
        return api->CreateStatus(OrtErrorCode::ORT_EP_FAIL, api->GetErrorMessage(status));
      }
    }

    // Set execution context memory
    if (trt_state->context_memory_sharing_enable) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
      size_t mem_size = trt_engine->getDeviceMemorySize();
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      if (mem_size > *max_context_mem_size_ptr) {
        *max_context_mem_size_ptr = mem_size;
      }
      trt_context->setDeviceMemory(MakeUniquePtrFromOrtAllocator<void>(alloc, *max_context_mem_size_ptr).get());
    }

    // Start CUDA graph capture.
    // Note: The reason we don't put graph capture in OnRunStart() like CUDA EP does is because
    // current ORT TRT doesn't get cuda stream until compute time and graph capture requires cuda stream.
    if (this_->cuda_graph_enable_ && this_->IsGraphCaptureAllowed() && !this_->IsGraphCaptured(0)) {
      //LOGS_DEFAULT(INFO) << "Capturing the cuda graph for this model";
//      cuda_graph_.SetStream(stream);
//      CaptureBegin(0);
    }

    // Run TRT inference
    if (!trt_context->enqueueV3(stream)) {
      return api->CreateStatus(OrtErrorCode::ORT_FAIL, "TensorRT EP execution context enqueue failed.");
    }

    /*
     * Given that InferenceSession::Run() is guaranteed to be thread-safe meaning multiple threads can call this function concurrently,
     * TRT EP needs to carefully take care of concurrency here, if not, following concurrent issue might happen:
     *
     * It's suggested that to perform inference concurrently in multiple streams, use one trt execution context per stream.
     * In the design of TRT EP (Not apply per-thread context implementation) and if multiple threads are calling InferenceSession::Run() concurrently,
     * the trt execution context instance is shared by all the threads and each thread aquires different stream from ORT.
     * So TRT EP will end up having one trt execution context using multiple streams which is not suggested.
     * But, since the whole compute_func() is protected by the lock and if cudaStreamSynchronize() is enforced here, one trt execution context per stream
     * is guaranteed.
     *
     * Therefore, TRT EP needs to call cudaStreamSynchronize() which means to wait until stream has completed all operations to prevent the concurrent issue mentioned above.
     * However, if cuda graph is enabled, TRT EP won't call cudaStreamSynchronize() since it's not allowed during graph capture.
     */
    if (this_->sync_stream_after_enqueue_) {
      CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(stream));
    }

    // Assign TRT output back to ORT output
    // (1) Bind TRT DDS output to ORT kernel context output. (It needs to wait until enqueueV3 is finished)
    // (2) Cast TRT INT32 output to ORT INT64 output or TRT double output to float output
    for (size_t i = 0, end = output_binding_names.size(); i < end; ++i) {
      char const* output_name = output_binding_names[i];

      size_t output_type = 0;
      const auto& iter = output_types.find(output_name);
      if (iter != output_types.end()) {
        output_type = iter->second;
      }

      if (dds_output_allocator_map.find(output_name) != dds_output_allocator_map.end()) {
        size_t output_index = 0;
        const auto& index_iter = output_indexes.find(output_name);
        if (index_iter != output_indexes.end()) {
          output_index = index_iter->second;
        }
        OrtStatusPtr status = BindKernelOutput(ctx, mem_info, dds_output_allocator_map, output_name, output_index, output_type, stream);
        if (status != nullptr) {
          return api->CreateStatus(OrtErrorCode::ORT_FAIL, api->GetErrorMessage(status));
        }
      } else {
        auto& output_tensor = output_tensors[i];
#if NV_TENSORRT_MAJOR < 10
        if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
          auto output_tensor_ptr = output_tensor.GetTensorMutableData<int64_t>();
          if (output_tensor_ptr != nullptr) {
            cuda::Impl_Cast<int32_t, int64_t>(stream, reinterpret_cast<int32_t*>(buffers[output_name]), output_tensor_ptr, output_dim_sizes[i]);
          }
        }
#endif
        if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE) {
//          auto output_tensor_ptr = output_tensor.GetTensorMutableData<double>();
//          if (output_tensor_ptr != nullptr) {
//            cuda::Impl_Cast<float, double>(stream, reinterpret_cast<float*>(buffers[output_name]), output_tensor_ptr, output_dim_sizes[i]);
//          }
        }
      }
    }

    // End CUDA graph capture.
    // Note: One reason we don't put end of graph capture in OnRunEnd() like CUDA EP does is because of cuda stream mentioned in graph capture
    // above, another reason is because OnRunEnd() is not synchronized with OnRunStart() and ExecuteGraph() per inference_session.cc.
    // It's safe to start/end CUDA graph capture in compute_func() here since cuda graph object is maintained by a per thread basis.
    if (this_->cuda_graph_enable_ && !this_->IsGraphCaptured(0)) {
//      if (IsGraphCaptureAllowed()) {
//        CaptureEnd(0);
//        // CUDA work issued to a capturing stream doesn’t actually run on the GPU,
//        // so run the captured graph here to actually execute the work.
//        ORT_RETURN_IF_ERROR(ReplayGraph(0));
//      } else {
//        IncrementRegularRunCountBeforeGraphCapture();
//      }
    }

    return nullptr;
  };

  return nullptr;
}

}   // namespace onnxruntime

#ifdef __cplusplus
extern "C" {
#endif
OrtExecutionProviderFactory* RegisterCustomEp() {
    std::unique_ptr<onnxruntime::TensorrtExecutionProviderFactory> ret = std::make_unique<onnxruntime::TensorrtExecutionProviderFactory>();
    return ret.release();
}
#ifdef __cplusplus
}
#endif