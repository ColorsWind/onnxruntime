// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/providers/common.h"
#include "contrib_ops/cpu/bert/attention_common.h"

namespace onnxruntime {
namespace contrib {
namespace sparse_attention_helper {

Status CheckInputs(void* params,
                   const Tensor* query,
                   const Tensor* key,
                   const Tensor* value,
                   const Tensor* past_key,
                   const Tensor* past_value,
                   const Tensor* cos_cache,
                   const Tensor* sin_cache,
                   const Tensor* block_mask,
                   const Tensor* total_key_seq_len) {
  // No packing for q/k/v:
  //   query                (batch_size, sequence_length, num_heads * head_size)
  //   key                  (batch_size, kv_sequence_length, kv_num_heads * head_size)
  //   value                (batch_size, kv_sequence_length, kv_num_heads * head_size)
  // Packed q/k/v:
  //   query                (batch_size, sequence_length, (num_heads + 2 * kv_num_heads) * head_size)
  //   key                  nullptr
  //   value                nullptr
  // Shape for other inputs:
  //   past_key             (batch_size, kv_num_heads, max_sequence_length, head_size) or nullptr
  //   past_value           (batch_size, kv_num_heads, max_sequence_length, head_size) or nullptr
  //   block_mask           (kv_num_heads, max_blocks, max_blocks) where max_blocks = max_sequence_length / sparse_block_size
  //   total_key_seq_len    (batch_size)
  //   cos_cache            (max_sequence_length, rotary_dim / 2) when do_rotary is true.
  //   sin_cache            (max_sequence_length, rotary_dim / 2) when do_rotary is true.

  assert(params != nullptr);
  SparseAttentionParameters* parameters = reinterpret_cast<SparseAttentionParameters*>(params);

  // The following parameters shall be set by parsing node attributes before calling CheckInputs.
  const int num_heads = parameters->num_heads;
  const int kv_num_heads = parameters->kv_num_heads;
  const bool do_rotary = parameters->do_rotary;

  constexpr bool is_past_bsnh = false;
  const bool is_packed_qkv = key == nullptr;

  const auto& query_dims = query->Shape().GetDims();
  if (query_dims.size() != 3) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'query' is expected to have 3 dimensions, got ",
                           query_dims.size());
  }

  int batch_size = static_cast<int>(query_dims[0]);
  int sequence_length = static_cast<int>(query_dims[1]);
  int q_hidden_size = static_cast<int>(query_dims[2]);

  int head_size = 0;
  int kv_hidden_size = 0;
  if (!is_packed_qkv) {
    // Check key and value when not packed
    head_size = static_cast<int>(q_hidden_size) / num_heads;
    if (head_size % 8 != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "head_size must be a multiple of 8. Got head_size = ",
                             head_size);
    }
    if (value == nullptr) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'key' and 'value' shall be both present, or both absent in the case of packed qkv.");
    }
    const auto& key_dims = key->Shape().GetDims();
    if (key_dims.size() != 3) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'key' is expected to have 3 dimensions, got ",
                             key_dims.size());
    }

    if (query_dims[0] != key_dims[0]) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'query' and 'key' shall have same dim 0 (batch size)");
    }
    if (query_dims[1] != key_dims[1]) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'query' and 'key' shall have same dim 1 (sequence length)");
    }

    kv_hidden_size = static_cast<int>(key_dims[2]);

    if (key->Shape() != value->Shape()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'query' and 'value' shall have same shape");
    }
  } else {
    // packed qkv
    if (static_cast<int>(q_hidden_size) % (num_heads + 2 * kv_num_heads) != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "packed qkv hidden size= ", q_hidden_size, " does not match num_heads and kv_num_heads",
                             num_heads, kv_num_heads);
    }

    head_size = static_cast<int>(q_hidden_size) / (num_heads + 2 * kv_num_heads);
    if (head_size % 8 != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "head_size must be a multiple of 8. Got head_size = ", head_size);
    }

    if (value != nullptr) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'key' and 'value' shall be both present, or both absent in the case of packed qkv.");
    }

    q_hidden_size = head_size * num_heads;
    kv_hidden_size = head_size * kv_num_heads;
  }

  const auto& block_mask_dim = block_mask->Shape().GetDims();
  if (block_mask_dim.size() != 3 && block_mask_dim[0] != kv_num_heads && block_mask_dim[1] != block_mask_dim[2]) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "block_mask must have shape (kv_num_heads, max_blocks, max_blocks).");
  }

  int max_blocks = static_cast<int>(block_mask_dim[1]);
  int max_sequence_length = max_blocks * parameters->sparse_block_size;

  // Check past-present KV
  if (past_key != nullptr && past_value != nullptr) {
    if (past_key->Shape() != past_value->Shape()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'past_key' and 'past_value' shall have same shape");
    }

    const auto& past_key_dims = past_key->Shape().GetDims();
    if (past_key_dims.size() != 4) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'past_key' is expected to have 4 dimensions, got ",
                             past_key_dims.size());
    }

    if (past_key_dims[0] != batch_size) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'past_key' dimension 0 should be batch_size ", batch_size, ", got ",
                             past_key_dims[0]);
    }

    if (past_key_dims[is_past_bsnh ? 2 : 1] != kv_num_heads) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'past_key' shall have kv_num_heads");
    }

    int max_cache_sequence_length = static_cast<int>(past_key_dims[is_past_bsnh ? 1 : 2]);
    if (max_cache_sequence_length != max_sequence_length) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'past_key' and 'block_mask' should have the same sequence length:",
                             "max_sequence_length deduced from past_key is ", max_cache_sequence_length,
                             "; max_sequence_length deduced from block_mask is ", max_sequence_length);
    }

    if (past_key_dims[3] != head_size) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'past_key' dimension 3 should be same as head_size, got ",
                             past_key_dims[3]);
    }
  } else if (past_key != nullptr || past_value != nullptr) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Input 'past_key' and 'past_value' shall be both present or both absent.");
  }

  // Check the shape of total_key_sequence_lengths. We do not check the values here.
  const auto& k_len_dim = total_key_seq_len->Shape().GetDims();
  if (k_len_dim.size() != 1 && k_len_dim[0] != batch_size) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "total_key_sequence_lengths must have shape (batch_size).");
  }

  int rotary_dim = 0;
  if (do_rotary) {
    if (cos_cache == nullptr || sin_cache == nullptr) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "cos_cache and sin_cache must be passed to SparseAttention when do_rotary = 1");
    }

    const auto& cos_dims = cos_cache->Shape().GetDims();
    const auto& sin_dims = sin_cache->Shape().GetDims();

    if (head_size % 16 != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "head_size shall be a multiple of 16. Got head_size = ",
                             head_size);
    }
    if (cos_dims[0] < max_sequence_length) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "cos_cache dimension 0 should be of max_sequence_length.");
    }
    if (sin_dims[0] < max_sequence_length) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "sin_cache dimension 0 should be of max_sequence_length.");
    }
    if (cos_dims[1] > (head_size / 16) * 8 || cos_dims[1] % 8 != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "cos_cache dimension 1 must be <= head_size / 2 and a multiple of 8.");
    }
    if (sin_dims[1] > (head_size / 16) * 8 || sin_dims[1] % 8 != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "sin_cache dimension 1 must be <= head_size / 2 and a multiple of 8.");
    }
    if (cos_dims[1] != sin_dims[1]) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "cos_cache and sin_cache dimension 1 must be the same.");
    }
    rotary_dim = static_cast<int>(cos_dims[1] * 2);
  }

  parameters->batch_size = batch_size;
  parameters->sequence_length = sequence_length;
  parameters->max_sequence_length = max_sequence_length;
  parameters->max_blocks = max_blocks;
  parameters->hidden_size = q_hidden_size;
  parameters->head_size = head_size;
  parameters->kv_hidden_size = kv_hidden_size;
  parameters->rotary_dim = rotary_dim;
  parameters->is_packed_qkv = is_packed_qkv;
  parameters->qkv_format = Q_K_V_BSNH;
  parameters->past_kv_format = Q_K_V_BNSH;

  return Status::OK();
}

}  // namespace sparse_attention_helper
}  // namespace contrib
}  // namespace onnxruntime
