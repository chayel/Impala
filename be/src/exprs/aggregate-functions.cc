// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/aggregate-functions.h"

#include <math.h>
#include <sstream>

#include "common/logging.h"
#include "runtime/string-value.h"
#include "runtime/timestamp-value.h"
#include "exprs/anyval-util.h"

using namespace std;

// TODO: this file should be cross compiled and then all of the builtin
// aggregate functions will have a codegen enabled path. Then we can remove
// the custom code in aggregation node.
namespace impala {

// Delimiter to use if the separator is NULL.
static const StringVal DEFAULT_STRING_CONCAT_DELIM((uint8_t*)", ", 2);

void AggregateFunctions::InitNull(FunctionContext*, AnyVal* dst) {
  dst->is_null = true;
}

template<typename T>
void AggregateFunctions::InitZero(FunctionContext*, T* dst) {
  dst->is_null = false;
  dst->val = 0;
}

void AggregateFunctions::CountUpdate(
    FunctionContext*, const AnyVal& src, BigIntVal* dst) {
  DCHECK(!dst->is_null);
  if (!src.is_null) ++dst->val;
}

void AggregateFunctions::CountStarUpdate(FunctionContext*, BigIntVal* dst) {
  DCHECK(!dst->is_null);
  ++dst->val;
}

template<typename SRC_VAL, typename DST_VAL>
void AggregateFunctions::Sum(FunctionContext* ctx, const SRC_VAL& src, DST_VAL* dst) {
  if (src.is_null) return;
  if (dst->is_null) InitZero<DST_VAL>(ctx, dst);
  dst->val += src.val;
}

template<typename T>
void AggregateFunctions::Min(FunctionContext*, const T& src, T* dst) {
  if (src.is_null) return;
  if (dst->is_null || src.val < dst->val) *dst = src;
}

template<typename T>
void AggregateFunctions::Max(FunctionContext*, const T& src, T* dst) {
  if (src.is_null) return;
  if (dst->is_null || src.val > dst->val) *dst = src;
}

// This is the intermediate we use when the function returns a string to avoid
// excessive allocations. This is currently wrapped in a StringValue slot which
// is not good. We want to have this be in a CHAR(N) slot.
struct StringValScratch {
  // Size of 'buffer'
  int buffer_len;

  // The length of the current string stored. buffer[0, str_len) is the current str.
  int str_len;
  uint8_t* buffer;

  StringValScratch() :
    buffer_len(0), str_len(0), buffer(NULL) {}

  // Resizes the scratch to new_len if necessary. If copy is true, the current
  // bytes are copied into the new bigger buffer. Otherwise, the buffer's values are
  // undefined.
  // If the new_len is bigger, we size the underlying buffer to 1.5 * new_len
  // TODO: what's the best constant? Any constant > 1 is O(1) amortized.
  void ResizeBufferTo(FunctionContext* ctx, int new_len, bool copy) {
    if (new_len <= buffer_len) return;
    DCHECK_GT(new_len, 0);
    new_len += new_len / 2;
    uint8_t* new_buffer = ctx->Allocate(new_len);
    if (copy) memcpy(new_buffer, buffer, str_len);
    ctx->Free(buffer);
    buffer = new_buffer;
    buffer_len = new_len;
  }

  void Set(FunctionContext* ctx, const StringVal& sv) {
    ResizeBufferTo(ctx, sv.len, false);
    memcpy(buffer, sv.ptr, sv.len);
    str_len = sv.len;
  }

  // Appends sv to the end of the current string. This doubles the underlying
  // buffer if it needs to go.
  void Append(FunctionContext* ctx, const StringVal& sv) {
    if (sv.len == 0) return;
    int new_len = str_len + sv.len;
    if (new_len > buffer_len) ResizeBufferTo(ctx, new_len, true);
    memcpy(buffer + str_len, sv.ptr, sv.len);
    str_len += sv.len;
  }
};

void AggregateFunctions::InitScratch(FunctionContext* c, StringVal* dst) {
  dst->is_null = false;
  dst->len = sizeof(StringValScratch);
  // TODO: provide an object pool like interface on FunctionContext?
  dst->ptr = reinterpret_cast<uint8_t*>(new StringValScratch);
}

StringVal AggregateFunctions::SerializeScratch(FunctionContext* c, const StringVal& sv) {
  DCHECK(!sv.is_null);
  DCHECK_EQ(sv.len, sizeof(StringValScratch));
  StringValScratch* scratch = reinterpret_cast<StringValScratch*>(sv.ptr);
  StringVal result = StringVal::null();
  if (scratch->buffer != NULL) result = StringVal(scratch->buffer, scratch->str_len);
  delete scratch;
  return result;
}

template<>
void AggregateFunctions::Min(FunctionContext* ctx, const StringVal& src, StringVal* dst) {
  if (src.is_null) return;
  DCHECK(!dst->is_null);
  DCHECK_EQ(dst->len, sizeof(StringValScratch));
  StringValScratch* scratch = reinterpret_cast<StringValScratch*>(dst->ptr);
  StringValue src_sv = StringValue::FromStringVal(src);
  StringValue dst_sv = StringValue(reinterpret_cast<char*>(scratch->buffer),
      scratch->str_len);
  if (scratch->buffer == NULL || src_sv < dst_sv) scratch->Set(ctx, src);
}

template<>
void AggregateFunctions::Max(FunctionContext* ctx, const StringVal& src, StringVal* dst) {
  if (src.is_null) return;
  DCHECK(!dst->is_null);
  DCHECK_EQ(dst->len, sizeof(StringValScratch));
  StringValScratch* scratch = reinterpret_cast<StringValScratch*>(dst->ptr);
  StringValue src_sv = StringValue::FromStringVal(src);
  StringValue dst_sv = StringValue(reinterpret_cast<char*>(scratch->buffer),
      scratch->str_len);
  if (scratch->buffer == NULL || src_sv > dst_sv) scratch->Set(ctx, src);
}

template<>
void AggregateFunctions::Min(FunctionContext*,
    const TimestampVal& src, TimestampVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) {
    *dst = src;
    return;
  }
  TimestampValue src_tv = TimestampValue::FromTimestampVal(src);
  TimestampValue dst_tv = TimestampValue::FromTimestampVal(*dst);
  if (src_tv < dst_tv) *dst = src;
}

template<>
void AggregateFunctions::Max(FunctionContext*,
    const TimestampVal& src, TimestampVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) {
    *dst = src;
    return;
  }
  TimestampValue src_tv = TimestampValue::FromTimestampVal(src);
  TimestampValue dst_tv = TimestampValue::FromTimestampVal(*dst);
  if (src_tv > dst_tv) *dst = src;
}

void AggregateFunctions::StringConcat(FunctionContext* ctx, const StringVal& src,
      const StringVal& separator, StringVal* result) {
  if (src.is_null) return;
  if (src.is_null) return;
  DCHECK(!result->is_null);
  DCHECK_EQ(result->len, sizeof(StringValScratch));

  StringValScratch* scratch = reinterpret_cast<StringValScratch*>(result->ptr);
  if (scratch->buffer == NULL) {
    scratch->Set(ctx, src);
    return;
  }
  if (separator.is_null) {
    scratch->Append(ctx, DEFAULT_STRING_CONCAT_DELIM);
  } else {
    scratch->Append(ctx, separator);
  }
  scratch->Append(ctx, src);
}

// Compute distinctpc and distinctpcsa using Flajolet and Martin's algorithm
// (Probabilistic Counting Algorithms for Data Base Applications)
// We have implemented two variants here: one with stochastic averaging (with PCSA
// postfix) and one without.
// There are 4 phases to compute the aggregate:
//   1. allocate a bitmap, stored in the aggregation tuple's output string slot
//   2. update the bitmap per row (UpdateDistinctEstimateSlot)
//   3. for distribtued plan, merge the bitmaps from all the nodes
//      (UpdateMergeEstimateSlot)
//   4. compute the estimate using the bitmaps when all the rows are processed
//      (FinalizeEstimateSlot)
const static int NUM_PC_BITMAPS = 64; // number of bitmaps
const static int PC_BITMAP_LENGTH = 32; // the length of each bit map
const static float PC_THETA = 0.77351f; // the magic number to compute the final result

void AggregateFunctions::PcInit(FunctionContext* c, StringVal* dst) {
  // Initialize the distinct estimate bit map - Probabilistic Counting Algorithms for Data
  // Base Applications (Flajolet and Martin)
  //
  // The bitmap is a 64bit(1st index) x 32bit(2nd index) matrix.
  // So, the string length of 256 byte is enough.
  // The layout is:
  //   row  1: 8bit 8bit 8bit 8bit
  //   row  2: 8bit 8bit 8bit 8bit
  //   ...     ..
  //   ...     ..
  //   row 64: 8bit 8bit 8bit 8bit
  //
  // Using 32bit length, we can count up to 10^8. This will not be enough for Fact table
  // primary key, but once we approach the limit, we could interpret the result as
  // "every row is distinct".
  //
  // We use "string" type for DISTINCT_PC function so that we can use the string
  // slot to hold the bitmaps.
  dst->is_null = false;
  int str_len = NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8;
  dst->ptr = c->Allocate(str_len);
  dst->len = str_len;
  memset(dst->ptr, 0, str_len);
}

static inline void SetDistinctEstimateBit(uint8_t* bitmap,
    uint32_t row_index, uint32_t bit_index) {
  // We need to convert Bitmap[alpha,index] into the index of the string.
  // alpha tells which of the 32bit we've to jump to.
  // index then lead us to the byte and bit.
  uint32_t *int_bitmap = reinterpret_cast<uint32_t*>(bitmap);
  int_bitmap[row_index] |= (1 << bit_index);
}

static inline bool GetDistinctEstimateBit(uint8_t* bitmap,
    uint32_t row_index, uint32_t bit_index) {
  uint32_t *int_bitmap = reinterpret_cast<uint32_t*>(bitmap);
  return ((int_bitmap[row_index] & (1 << bit_index)) > 0);
}

template<typename T>
void AggregateFunctions::PcUpdate(FunctionContext* c, const T& input, StringVal* dst) {
  if (input.is_null) return;
  // Core of the algorithm. This is a direct translation of the code in the paper.
  // Please see the paper for details. For simple averaging, we need to compute hash
  // values NUM_PC_BITMAPS times using NUM_PC_BITMAPS different hash functions (by using a
  // different seed).
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    uint32_t hash_value = AnyValUtil::Hash(input, i);
    int bit_index = __builtin_ctz(hash_value);
    if (UNLIKELY(hash_value == 0)) bit_index = PC_BITMAP_LENGTH - 1;
    // Set bitmap[i, bit_index] to 1
    SetDistinctEstimateBit(dst->ptr, i, bit_index);
  }
}

template<typename T>
void AggregateFunctions::PcsaUpdate(FunctionContext* c, const T& input, StringVal* dst) {
  if (input.is_null) return;

  // Core of the algorithm. This is a direct translation of the code in the paper.
  // Please see the paper for details. Using stochastic averaging, we only need to
  // the hash value once for each row.
  uint32_t hash_value = AnyValUtil::Hash(input, 0);
  uint32_t row_index = hash_value % NUM_PC_BITMAPS;

  // We want the zero-based position of the least significant 1-bit in binary
  // representation of hash_value. __builtin_ctz does exactly this because it returns
  // the number of trailing 0-bits in x (or undefined if x is zero).
  int bit_index = __builtin_ctz(hash_value / NUM_PC_BITMAPS);
  if (UNLIKELY(hash_value == 0)) bit_index = PC_BITMAP_LENGTH - 1;

  // Set bitmap[row_index, bit_index] to 1
  SetDistinctEstimateBit(dst->ptr, row_index, bit_index);
}

string DistinctEstimateBitMapToString(uint8_t* v) {
  stringstream debugstr;
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    for (int j = 0; j < PC_BITMAP_LENGTH; ++j) {
      // print bitmap[i][j]
      debugstr << GetDistinctEstimateBit(v, i, j);
    }
    debugstr << "\n";
  }
  debugstr << "\n";
  return debugstr.str();
}

void AggregateFunctions::PcMerge(FunctionContext* c,
    const StringVal& src, StringVal* dst) {
  DCHECK(!src.is_null);
  DCHECK(!dst->is_null);
  DCHECK_EQ(src.len, NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8);

  // Merge the bits
  // I think _mm_or_ps can do it, but perf doesn't really matter here. We call this only
  // once group per node.
  for (int i = 0; i < NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8; ++i) {
    *(dst->ptr + i) |= *(src.ptr + i);
  }

  VLOG_ROW << "UpdateMergeEstimateSlot Src Bit map:\n"
           << DistinctEstimateBitMapToString(src.ptr);
  VLOG_ROW << "UpdateMergeEstimateSlot Dst Bit map:\n"
           << DistinctEstimateBitMapToString(dst->ptr);
}

double DistinceEstimateFinalize(const StringVal& src) {
  DCHECK(!src.is_null);
  DCHECK_EQ(src.len, NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8);
  VLOG_ROW << "FinalizeEstimateSlot Bit map:\n"
           << DistinctEstimateBitMapToString(src.ptr);

  // We haven't processed any rows if none of the bits are set. Therefore, we have zero
  // distinct rows. We're overwriting the result in the same string buffer we've
  // allocated.
  bool is_empty = true;
  for (int i = 0; i < NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8; ++i) {
    if (src.ptr[i] != 0) {
      is_empty = false;
      break;
    }
  }
  if (is_empty) return 0;

  // Convert the bitmap to a number, please see the paper for details
  // In short, we count the average number of leading 1s (per row) in the bit map.
  // The number is proportional to the log2(1/NUM_PC_BITMAPS of  the actual number of
  // distinct).
  // To get the actual number of distinct, we'll do 2^avg / PC_THETA.
  // PC_THETA is a magic number.
  int sum = 0;
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    int row_bit_count = 0;
    // Count the number of leading ones for each row in the bitmap
    // We could have used the build in __builtin_clz to count of number of leading zeros
    // but we first need to invert the 1 and 0.
    while (GetDistinctEstimateBit(src.ptr, i, row_bit_count) &&
        row_bit_count < PC_BITMAP_LENGTH) {
      ++row_bit_count;
    }
    sum += row_bit_count;
  }
  double avg = static_cast<double>(sum) / static_cast<double>(NUM_PC_BITMAPS);
  double result = pow(static_cast<double>(2), avg) / PC_THETA;
  return result;
}

StringVal AggregateFunctions::PcFinalize(FunctionContext* c, const StringVal& src) {
  double estimate = DistinceEstimateFinalize(src);
  int64_t result = estimate;
  // TODO: this should return bigint. this is a hack
  stringstream ss;
  ss << result;
  string str = ss.str();
  StringVal dst = src;
  memcpy(dst.ptr, str.c_str(), str.length());
  dst.len = str.length();
  return dst;
}

StringVal AggregateFunctions::PcsaFinalize(FunctionContext* c, const StringVal& src) {
  // When using stochastic averaging, the result has to be multiplied by NUM_PC_BITMAPS.
  double estimate = DistinceEstimateFinalize(src) * NUM_PC_BITMAPS;
  int64_t result = estimate;
  // TODO: this should return bigint. this is a hack
  stringstream ss;
  ss << result;
  string str = ss.str();
  StringVal dst = src;
  memcpy(dst.ptr, str.c_str(), str.length());
  dst.len = str.length();
  return dst;
}

// Stamp out the templates for the types we need.
template void AggregateFunctions::InitZero<BigIntVal>(FunctionContext*, BigIntVal* dst);

template void AggregateFunctions::Sum<BooleanVal, BigIntVal>(
    FunctionContext*, const BooleanVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<TinyIntVal, BigIntVal>(
    FunctionContext*, const TinyIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<SmallIntVal, BigIntVal>(
    FunctionContext*, const SmallIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<IntVal, BigIntVal>(
    FunctionContext*, const IntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<BigIntVal, BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<FloatVal, DoubleVal>(
    FunctionContext*, const FloatVal& src, DoubleVal* dst);
template void AggregateFunctions::Sum<DoubleVal, DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);

template void AggregateFunctions::Min<BooleanVal>(
    FunctionContext*, const BooleanVal& src, BooleanVal* dst);
template void AggregateFunctions::Min<TinyIntVal>(
    FunctionContext*, const TinyIntVal& src, TinyIntVal* dst);
template void AggregateFunctions::Min<SmallIntVal>(
    FunctionContext*, const SmallIntVal& src, SmallIntVal* dst);
template void AggregateFunctions::Min<IntVal>(
    FunctionContext*, const IntVal& src, IntVal* dst);
template void AggregateFunctions::Min<BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Min<FloatVal>(
    FunctionContext*, const FloatVal& src, FloatVal* dst);
template void AggregateFunctions::Min<DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);
template void AggregateFunctions::Min<StringVal>(
    FunctionContext*, const StringVal& src, StringVal* dst);

template void AggregateFunctions::Max<BooleanVal>(
    FunctionContext*, const BooleanVal& src, BooleanVal* dst);
template void AggregateFunctions::Max<TinyIntVal>(
    FunctionContext*, const TinyIntVal& src, TinyIntVal* dst);
template void AggregateFunctions::Max<SmallIntVal>(
    FunctionContext*, const SmallIntVal& src, SmallIntVal* dst);
template void AggregateFunctions::Max<IntVal>(
    FunctionContext*, const IntVal& src, IntVal* dst);
template void AggregateFunctions::Max<BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Max<FloatVal>(
    FunctionContext*, const FloatVal& src, FloatVal* dst);
template void AggregateFunctions::Max<DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);
template void AggregateFunctions::Max<StringVal>(
    FunctionContext*, const StringVal& src, StringVal* dst);

template void AggregateFunctions::PcUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);

template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);
}
