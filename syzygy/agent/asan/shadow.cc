// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/agent/asan/shadow.h"

#include <algorithm>

#include "base/strings/stringprintf.h"
#include "syzygy/common/align.h"

namespace agent {
namespace asan {

uint8 Shadow::shadow_[kShadowSize] = {};
uint8 Shadow::page_bits_[kPageBitsSize] = {};
base::Lock Shadow::page_bits_lock_;

void Shadow::SetUp() {
  // Poison the shadow memory.
  Poison(shadow_, kShadowSize, kAsanMemoryMarker);
  // Poison the first 64k of the memory as they're not addressable.
  Poison(0, kAddressLowerBound, kInvalidAddressMarker);
  // Poison the protection bits array.
  Poison(page_bits_, kPageBitsSize, kAsanMemoryMarker);
}

void Shadow::TearDown() {
  // Unpoison the shadow memory.
  Unpoison(shadow_, kShadowSize);
  // Unpoison the first 64k of the memory.
  Unpoison(0, kAddressLowerBound);
  // Unpoison the protection bits array.
  Unpoison(page_bits_, kPageBitsSize);
}

void Shadow::Reset() {
  ::memset(shadow_, 0, kShadowSize);
  ::memset(page_bits_, 0, kPageBitsSize);
}

void Shadow::Poison(const void* addr, size_t size, ShadowMarker shadow_val) {
  uintptr_t index = reinterpret_cast<uintptr_t>(addr);
  uintptr_t start = index & 0x7;
  DCHECK_EQ(0U, (index + size) & 0x7);

  index >>= 3;
  if (start)
    shadow_[index++] = start;

  size >>= 3;
  DCHECK_GT(arraysize(shadow_), index + size);
  ::memset(shadow_ + index, shadow_val, size);
}

void Shadow::Unpoison(const void* addr, size_t size) {
  uintptr_t index = reinterpret_cast<uintptr_t>(addr);
  DCHECK_EQ(0U, index & 0x7);

  uint8 remainder = size & 0x7;
  index >>= 3;
  size >>= 3;
  DCHECK_GT(arraysize(shadow_), index + size);
  ::memset(shadow_ + index, kHeapAddressableMarker, size);

  if (remainder != 0)
    shadow_[index + size] = remainder;
}

namespace {

// An array of kFreedMarkers. This is used for constructing uint16, uint32 and
// uint64 byte variants of kHeapFreedMarker.
static const uint8 kFreedMarkers[] = {
    kHeapFreedMarker, kHeapFreedMarker, kHeapFreedMarker, kHeapFreedMarker,
    kHeapFreedMarker, kHeapFreedMarker, kHeapFreedMarker, kHeapFreedMarker };
COMPILE_ASSERT(sizeof(kFreedMarkers) == sizeof(uint64),
                wrong_number_of_freed_markers);
static const uint64& kFreedMarker64 =
    *reinterpret_cast<const uint64*>(kFreedMarkers);
static const uint32& kFreedMarker32 =
    *reinterpret_cast<const uint32*>(kFreedMarkers);
static const uint16& kFreedMarker16 =
    *reinterpret_cast<const uint16*>(kFreedMarkers);
static const uint8& kFreedMarker8 =
    *reinterpret_cast<const uint8*>(kFreedMarkers);

// Marks the given range of shadow bytes as freed, preserving left and right
// redzone bytes.
inline void MarkAsFreedImpl8(uint8* cursor, uint8* cursor_end) {
  for (; cursor != cursor_end; ++cursor) {
    // Preserve block beginnings/ends/redzones as they were originally.
    // This is necessary to preserve information about nested blocks.
    if (ShadowMarkerHelper::IsActiveLeftRedzone(*cursor) ||
        ShadowMarkerHelper::IsActiveRightRedzone(*cursor)) {
      continue;
    }

    // Anything else gets marked as freed.
    *cursor = kHeapFreedMarker;
  }
}

// Marks the given range of shadow bytes as freed, preserving left and right
// redzone bytes. |cursor| and |cursor_end| must be 8-byte aligned.
inline void MarkAsFreedImplAligned64(uint64* cursor, uint64* cursor_end) {
  DCHECK(::common::IsAligned(cursor, sizeof(uint64)));
  DCHECK(::common::IsAligned(cursor_end, sizeof(uint64)));

  for (; cursor != cursor_end; ++cursor) {
    // If the block of shadow memory is entirely green then mark as freed.
    // Otherwise go check its contents byte by byte.
    if (*cursor == 0) {
      *cursor = kFreedMarker64;
    } else {
      MarkAsFreedImpl8(reinterpret_cast<uint8*>(cursor),
                       reinterpret_cast<uint8*>(cursor + 1));
    }
  }
}

inline void MarkAsFreedImpl64(uint8* cursor, uint8* cursor_end) {
  if (cursor_end - cursor >= 2 * sizeof(uint64)) {
    uint8* cursor_aligned = ::common::AlignUp(cursor, sizeof(uint64));
    uint8* cursor_end_aligned = ::common::AlignDown(cursor_end,
                                                    sizeof(uint64));
    MarkAsFreedImpl8(cursor, cursor_aligned);
    MarkAsFreedImplAligned64(reinterpret_cast<uint64*>(cursor_aligned),
                             reinterpret_cast<uint64*>(cursor_end_aligned));
    MarkAsFreedImpl8(cursor_end_aligned, cursor_end);
  } else {
    MarkAsFreedImpl8(cursor, cursor_end);
  }
}

}  // namespace

void Shadow::MarkAsFreed(const void* addr, size_t size) {
  DCHECK_LE(kAddressLowerBound, reinterpret_cast<uintptr_t>(addr));
  DCHECK(::common::IsAligned(addr, kShadowRatio));
  size_t index = reinterpret_cast<uintptr_t>(addr) / kShadowRatio;
  size_t length = (size + kShadowRatio - 1) / kShadowRatio;

  DCHECK_LE(index, kShadowSize);
  DCHECK_LE(index + length, kShadowSize);

  uint8* cursor = shadow_ + index;
  uint8* cursor_end = static_cast<uint8*>(cursor) + length;

  // This isn't as simple as a memset because we need to preserve left and
  // right redzone padding bytes that may be found in the range.
  MarkAsFreedImpl64(cursor, cursor_end);
}

bool Shadow::IsAccessible(const void* addr) {
  uintptr_t index = reinterpret_cast<uintptr_t>(addr);
  uintptr_t start = index & 0x7;

  index >>= 3;

  DCHECK_GT(arraysize(shadow_), index);
  uint8 shadow = shadow_[index];
  if (shadow == 0)
    return true;

  if (ShadowMarkerHelper::IsRedzone(shadow))
    return false;

  return start < shadow;
}

bool Shadow::IsLeftRedzone(const void* address) {
  return ShadowMarkerHelper::IsActiveLeftRedzone(
      GetShadowMarkerForAddress(address));
}

bool Shadow::IsRightRedzone(const void* address) {
  uintptr_t index = reinterpret_cast<uintptr_t>(address);
  uintptr_t start = index & 0x7;

  index >>= 3;

  DCHECK_GT(arraysize(shadow_), index);
  uint8 marker = shadow_[index];

  // If the marker is for accessible memory then some addresses may be part
  // of a right redzone, assuming that the *next* marker in the shadow is for
  // a right redzone.
  if (marker == 0)
    return false;
  if (marker <= kHeapPartiallyAddressableByte7) {
    if (index == arraysize(shadow_))
      return false;
    if (!ShadowMarkerHelper::IsActiveRightRedzone(shadow_[index + 1]))
      return false;
    return start >= marker;
  }

  // Otherwise, check the marker directly.
  return ShadowMarkerHelper::IsActiveRightRedzone(marker);
}

bool Shadow::IsBlockStartByte(const void* address) {
  uintptr_t index = reinterpret_cast<uintptr_t>(address);
  uintptr_t start = index & 0x7;

  index >>= 3;

  DCHECK_GT(arraysize(shadow_), index);
  uint8 marker = shadow_[index];

  if (start != 0)
    return false;
  if (!ShadowMarkerHelper::IsActiveBlockStart(marker))
    return false;

  return true;
}

ShadowMarker Shadow::GetShadowMarkerForAddress(const void* addr) {
  uintptr_t index = reinterpret_cast<uintptr_t>(addr);
  index >>= 3;

  DCHECK_GT(arraysize(shadow_), index);
  return static_cast<ShadowMarker>(shadow_[index]);
}

void Shadow::PoisonAllocatedBlock(const BlockInfo& info) {
  COMPILE_ASSERT((sizeof(BlockHeader) % kShadowRatio) == 0, bad_header_size);
  DCHECK(info.header->state == ALLOCATED_BLOCK);

  // Translate the block address to an offset. Sanity check a whole bunch
  // of things that we require to be true for the shadow to have 100%
  // fidelity.
  uintptr_t index = reinterpret_cast<uintptr_t>(info.block);
  DCHECK(::common::IsAligned(index, kShadowRatio));
  DCHECK(::common::IsAligned(info.header_padding_size, kShadowRatio));
  DCHECK(::common::IsAligned(info.block_size, kShadowRatio));
  index /= kShadowRatio;

  // Determine the distribution of bytes in the shadow.
  size_t left_redzone_bytes = (info.body - info.block) / kShadowRatio;
  size_t body_bytes = (info.body_size + kShadowRatio - 1) / kShadowRatio;
  size_t block_bytes = info.block_size / kShadowRatio;
  size_t right_redzone_bytes = block_bytes - left_redzone_bytes - body_bytes;

  // Determine the marker byte for the header. This encodes the length of the
  // body of the allocation modulo the shadow ratio, so that the exact length
  // can be inferred from inspecting the shadow memory.
  uint8 body_size_mod = info.body_size % kShadowRatio;
  uint8 header_marker = ShadowMarkerHelper::BuildBlockStart(
      true, info.header->is_nested, body_size_mod);

  // Determine the marker byte for the trailer.
  uint8 trailer_marker = ShadowMarkerHelper::BuildBlockEnd(
      true, info.header->is_nested);

  // Poison the header and left padding.
  uint8* cursor = shadow_ + index;
  ::memset(cursor, header_marker, 1);
  ::memset(cursor + 1, kHeapLeftPaddingMarker, left_redzone_bytes - 1);
  cursor += left_redzone_bytes;
  ::memset(cursor, kHeapAddressableMarker, body_bytes);
  cursor += body_bytes;

  // Poison the right padding and the trailer.
  if (body_size_mod > 0)
    cursor[-1] = body_size_mod;
  ::memset(cursor, kHeapRightPaddingMarker, right_redzone_bytes - 1);
  ::memset(cursor + right_redzone_bytes - 1, trailer_marker, 1);
}

bool Shadow::BlockIsNested(const BlockInfo& info) {
  uint8 marker = GetShadowMarkerForAddress(info.block);
  DCHECK(ShadowMarkerHelper::IsActiveBlockStart(marker));
  return ShadowMarkerHelper::IsNestedBlockStart(marker);
}

bool Shadow::BlockInfoFromShadow(const void* addr, CompactBlockInfo* info) {
  DCHECK_NE(static_cast<void*>(NULL), addr);
  DCHECK_NE(static_cast<CompactBlockInfo*>(NULL), info);
  if (!BlockInfoFromShadowImpl(0, addr, info))
    return false;
  return true;
}

bool Shadow::BlockInfoFromShadow(const void* addr, BlockInfo* info) {
  DCHECK_NE(static_cast<void*>(NULL), addr);
  DCHECK_NE(static_cast<BlockInfo*>(NULL), info);
  CompactBlockInfo compact = {};
  if (!BlockInfoFromShadow(addr, &compact))
    return false;
  ConvertBlockInfo(compact, info);
  return true;
}

bool Shadow::ParentBlockInfoFromShadow(const BlockInfo& nested,
                                       BlockInfo* info) {
  DCHECK_NE(static_cast<BlockInfo*>(NULL), info);
  if (!BlockIsNested(nested))
    return false;
  CompactBlockInfo compact = {};
  if (!BlockInfoFromShadowImpl(1, nested.block, &compact))
    return false;
  ConvertBlockInfo(compact, info);
  return true;
}

bool Shadow::IsBeginningOfBlockBody(const void* addr) {
  DCHECK_NE(static_cast<void*>(NULL), addr);
  // If the block has a non-zero body size then the beginning of the body will
  // be accessible or tagged as freed.
  // If the block has an empty body then the beginning of the body will be a
  // right redzone.
  if (IsAccessible(addr) || IsRightRedzone(addr) ||
      GetShadowMarkerForAddress(addr) == kHeapFreedMarker) {
    return IsLeftRedzone(reinterpret_cast<const uint8*>(addr) - 1);
  }
  return false;
}

namespace {

static const size_t kPageSize = GetPageSize();

// Converts an address to a page index and bit mask.
inline void AddressToPageMask(const void* address,
                              size_t* index,
                              uint8* mask) {
  DCHECK_NE(static_cast<size_t*>(nullptr), index);
  DCHECK_NE(static_cast<uint8*>(nullptr), mask);

  size_t i = reinterpret_cast<uintptr_t>(address) / kPageSize;
  *index = i / 8;
  *mask = 1 << (i % 8);
}

}  // namespace

bool Shadow::PageIsProtected(const void* addr) {
  // Since the page bit is read very frequently this is not performed
  // under a lock. The values change quite rarely, so this will almost always
  // be correct. However, consumers of this knowledge have to be robust to
  // getting incorrect data.
  size_t index = 0;
  uint8 mask = 0;
  AddressToPageMask(addr, &index, &mask);
  return (page_bits_[index] & mask) == mask;
}

void Shadow::MarkPageProtected(const void* addr) {
  size_t index = 0;
  uint8 mask = 0;
  AddressToPageMask(addr, &index, &mask);

  base::AutoLock lock(page_bits_lock_);
  page_bits_[index] |= mask;
}

void Shadow::MarkPageUnprotected(const void* addr) {
  size_t index = 0;
  uint8 mask = 0;
  AddressToPageMask(addr, &index, &mask);
  mask = ~mask;

  base::AutoLock lock(page_bits_lock_);
  page_bits_[index] &= mask;
}

void Shadow::MarkPagesProtected(const void* addr, size_t size) {
  const uint8* page = reinterpret_cast<const uint8*>(addr);
  const uint8* page_end = page + size;
  size_t index = 0;
  uint8 mask = 0;

  base::AutoLock lock(page_bits_lock_);
  while (page < page_end) {
    AddressToPageMask(page, &index, &mask);
    page_bits_[index] |= mask;
    page += kPageSize;
  }
}

void Shadow::MarkPagesUnprotected(const void* addr, size_t size) {
  const uint8* page = reinterpret_cast<const uint8*>(addr);
  const uint8* page_end = page + size;
  size_t index = 0;
  uint8 mask = 0;

  base::AutoLock lock(page_bits_lock_);
  while (page < page_end) {
    AddressToPageMask(page, &index, &mask);
    page_bits_[index] &= ~mask;
    page += kPageSize;
  }
}

void Shadow::CloneShadowRange(const void* src_pointer,
                              void* dst_pointer,
                              size_t size) {
  DCHECK_EQ(0U, size & 0x7);

  uintptr_t src_index = reinterpret_cast<uintptr_t>(src_pointer);
  DCHECK_EQ(0U, src_index & 0x7);
  src_index >>= 3;

  uintptr_t dst_index = reinterpret_cast<uintptr_t>(dst_pointer);
  DCHECK_EQ(0U, dst_index & 0x7);
  dst_index >>= 3;

  size_t size_shadow = size >> 3;

  memcpy(shadow_ + dst_index, shadow_ + src_index, size_shadow);
}

void Shadow::AppendShadowByteText(const char *prefix,
                                  uintptr_t index,
                                  std::string* output,
                                  size_t bug_index) {
  base::StringAppendF(
      output, "%s0x%08x:", prefix, reinterpret_cast<void*>(index << 3));
  char separator = ' ';
  for (uint32 i = 0; i < 8; i++) {
    if (index + i == bug_index)
      separator = '[';
    uint8 shadow_value = shadow_[index + i];
    base::StringAppendF(
        output, "%c%x%x", separator, shadow_value >> 4, shadow_value & 15);
    if (separator == '[')
      separator = ']';
    else if (separator == ']')
      separator = ' ';
  }
  if (separator == ']')
    base::StringAppendF(output, "]");
  base::StringAppendF(output, "\n");
}

void Shadow::AppendShadowArrayText(const void* addr, std::string* output) {
  uintptr_t index = reinterpret_cast<uintptr_t>(addr);
  index >>= 3;
  size_t index_start = index;
  index_start &= ~0x7;
  for (int i = -4; i <= 4; i++) {
    const char * const prefix = (i == 0) ? "=>" : "  ";
    AppendShadowByteText(prefix, (index_start + i * 8), output, index);
  }
}

void Shadow::AppendShadowMemoryText(const void* addr, std::string* output) {
  base::StringAppendF(output, "Shadow bytes around the buggy address:\n");
  AppendShadowArrayText(addr, output);
  base::StringAppendF(output, "Shadow byte legend (one shadow byte represents "
                              "8 application bytes):\n");
  base::StringAppendF(output, "  Addressable:           00\n");
  base::StringAppendF(output, "  Partially addressable: 01 - 07\n");
  base::StringAppendF(output, "  Block start redzone:   %02x - %02x\n",
                      kHeapBlockStartMarker0, kHeapBlockStartMarker7);
  base::StringAppendF(output, "  Nested block start:    %02x - %02x\n",
                      kHeapNestedBlockStartMarker0,
                      kHeapNestedBlockStartMarker7);
  base::StringAppendF(output, "  ASan memory byte:      %02x\n",
                      kAsanMemoryMarker);
  base::StringAppendF(output, "  Invalid address:       %02x\n",
                      kInvalidAddressMarker);
  base::StringAppendF(output, "  User redzone:          %02x\n",
                      kUserRedzoneMarker);
  base::StringAppendF(output, "  Block end redzone:     %02x\n",
                      kHeapBlockEndMarker);
  base::StringAppendF(output, "  Nested block end:      %02x\n",
                      kHeapNestedBlockEndMarker);
  base::StringAppendF(output, "  Heap left redzone:     %02x\n",
                      kHeapLeftPaddingMarker);
  base::StringAppendF(output, "  Heap right redzone:    %02x\n",
                      kHeapRightPaddingMarker);
  base::StringAppendF(output, "  ASan reserved byte:    %02x\n",
                      kAsanReservedMarker);
  base::StringAppendF(output, "  Freed heap region:     %02x\n",
                      kHeapFreedMarker);
}

size_t Shadow::GetAllocSize(const uint8* mem) {
  BlockInfo block_info = {};
  if (!Shadow::BlockInfoFromShadow(mem, &block_info))
    return 0;
  return block_info.block_size;
}

bool Shadow::ScanLeftForBracketingBlockStart(
    size_t initial_nesting_depth, size_t cursor, size_t* location) {
  DCHECK_NE(static_cast<size_t*>(NULL), location);

  static const size_t kLowerBound = kAddressLowerBound / kShadowRatio;

  size_t left = cursor;
  int nesting_depth = static_cast<int>(initial_nesting_depth);
  if (ShadowMarkerHelper::IsBlockEnd(shadow_[left]))
    --nesting_depth;
  while (true) {
    if (ShadowMarkerHelper::IsBlockStart(shadow_[left])) {
      if (nesting_depth == 0) {
        *location = left;
        return true;
      }
      // If this is not a nested block then there's no hope of finding a
      // block containing the original cursor.
      if (!ShadowMarkerHelper::IsNestedBlockStart(shadow_[left]))
        return false;
      --nesting_depth;
    } else if (ShadowMarkerHelper::IsBlockEnd(shadow_[left])) {
      ++nesting_depth;

      // If we encounter the end of a non-nested block there's no way for
      // a block to bracket us.
      if (nesting_depth > 0 &&
          !ShadowMarkerHelper::IsNestedBlockEnd(shadow_[left])) {
        return false;
      }
    }
    if (left <= kLowerBound)
      return false;
    --left;
  }

  NOTREACHED();
}

namespace {

// This handles an unaligned input cursor. It can potentially read up to 7
// bytes past the end of the cursor, but only up to an 8 byte boundary. Thus
// this out of bounds access is safe.
inline const uint8* ScanRightForPotentialHeaderBytes(
    const uint8* pos, const uint8* end) {
  DCHECK(::common::IsAligned(end, 8));

  // Handle the first few bytes that aren't aligned. If pos == end then
  // it is already 8-byte aligned and we'll simply fall through to the end.
  // This is a kind of Duffy's device that takes bytes 'as large as possible'
  // until we reach an 8 byte alignment.
  switch (reinterpret_cast<uintptr_t>(pos) & 0x7) {
    case 1:
      if (*pos != 0 && *pos != kFreedMarker8)
        return pos;
      pos += 1;
    case 2:
      if (*reinterpret_cast<const uint16*>(pos) != 0 &&
          *reinterpret_cast<const uint16*>(pos) != kFreedMarker16) {
        return pos;
      }
      pos += 2;
    case 4:
      if (*reinterpret_cast<const uint32*>(pos) != 0 &&
          *reinterpret_cast<const uint32*>(pos) != kFreedMarker32) {
        return pos;
      }
      pos += 4;
      break;

    case 3:
      if (*pos != 0 && *pos != kFreedMarker8)
        return pos;
      pos += 1;
      // Now have alignemnt of 4.
      if (*reinterpret_cast<const uint32*>(pos) != 0 &&
          *reinterpret_cast<const uint32*>(pos) != kFreedMarker32) {
        return pos;
      }
      pos += 4;
      break;

    case 5:
      if (*pos != 0 && *pos != kFreedMarker8)
        return pos;
      pos += 1;
    case 6:
      if (*reinterpret_cast<const uint16*>(pos) != 0 &&
          *reinterpret_cast<const uint16*>(pos) != kFreedMarker16) {
        return pos;
      }
      pos += 2;
      break;

    case 7:
      if (*pos != 0 && *pos != kFreedMarker8)
        return pos;
      pos += 1;

    case 0:
    default:
      // Do nothing, as we're already 8 byte aligned.
      break;
  }

  // Handle the 8-byte aligned bytes as much as we can.
  while (pos < end) {
    if (*reinterpret_cast<const uint64*>(pos) != 0 &&
        *reinterpret_cast<const uint64*>(pos) != kFreedMarker64) {
      return pos;
    }
    pos += 8;
  }

  return pos;
}

}  // namespace

bool Shadow::ScanRightForBracketingBlockEnd(
    size_t initial_nesting_depth, size_t cursor, size_t* location) {
  DCHECK_NE(static_cast<size_t*>(NULL), location);
  static const uint8* kShadowEnd = Shadow::shadow_ + Shadow::kShadowSize;

  const uint8* pos = shadow_ + cursor;
  int nesting_depth = static_cast<int>(initial_nesting_depth);
  if (ShadowMarkerHelper::IsBlockStart(*pos))
    --nesting_depth;
  while (pos < kShadowEnd) {
    // Skips past as many addressable and freed bytes as possible.
    pos = ScanRightForPotentialHeaderBytes(pos, kShadowEnd);
    if (pos == kShadowEnd)
      return false;

    // When the above loop exits early then somewhere in the next 8 bytes
    // there's non-addressable data that isn't 'freed'. Look byte by byte to
    // see what's up.

    if (ShadowMarkerHelper::IsBlockEnd(*pos)) {
      if (nesting_depth == 0) {
        *location = pos - shadow_;
        return true;
      }
      if (!ShadowMarkerHelper::IsNestedBlockEnd(*pos))
        return false;
      --nesting_depth;
    } else if (ShadowMarkerHelper::IsBlockStart(*pos)) {
      ++nesting_depth;

      // If we encounter the beginning of a non-nested block then there's
      // clearly no way for any block to bracket us.
      if (nesting_depth > 0 &&
          !ShadowMarkerHelper::IsNestedBlockStart(*pos)) {
        return false;
      }
    }
    ++pos;
  }
  return false;
}

bool Shadow::BlockInfoFromShadowImpl(
    size_t initial_nesting_depth, const void* addr, CompactBlockInfo* info) {
  DCHECK_NE(static_cast<void*>(NULL), addr);
  DCHECK_NE(static_cast<CompactBlockInfo*>(NULL), info);

  // Convert the address to an offset in the shadow memory.
  size_t left = reinterpret_cast<uintptr_t>(addr) / kShadowRatio;
  size_t right = left;

  if (!ScanLeftForBracketingBlockStart(initial_nesting_depth, left, &left))
    return false;
  if (!ScanRightForBracketingBlockEnd(initial_nesting_depth, right, &right))
    return false;
  ++right;

  info->block = reinterpret_cast<uint8*>(left * kShadowRatio);
  info->block_size = (right - left) * kShadowRatio;

  // Get the length of the body modulo the shadow ratio.
  size_t body_size_mod = ShadowMarkerHelper::GetBlockStartData(shadow_[left]);
  info->is_nested = ShadowMarkerHelper::IsNestedBlockStart(shadow_[left]);

  // Find the beginning of the body (end of the left redzone).
  ++left;
  while (left < right && shadow_[left] == kHeapLeftPaddingMarker)
    ++left;

  // Find the beginning of the right redzone (end of the body).
  --right;
  while (right > left && shadow_[right - 1] == kHeapRightPaddingMarker)
    --right;

  // Calculate the body location and size.
  uint8* body = reinterpret_cast<uint8*>(left * kShadowRatio);
  size_t body_size = (right - left) * kShadowRatio;
  if (body_size_mod > 0) {
    DCHECK_LE(8u, body_size);
    body_size = body_size - kShadowRatio + body_size_mod;
  }

  // Fill out header and trailer sizes.
  info->header_size = body - info->block;
  info->trailer_size = info->block_size - body_size - info->header_size;

  return true;
}

ShadowWalker::ShadowWalker(
    bool recursive, const void* lower_bound, const void* upper_bound)
    : recursive_(recursive), lower_bound_(0), upper_bound_(0), cursor_(0),
      nesting_depth_(0) {
  DCHECK_LE(Shadow::kAddressLowerBound, reinterpret_cast<size_t>(lower_bound));
  DCHECK_GE(Shadow::kAddressUpperBound, reinterpret_cast<size_t>(upper_bound));
  DCHECK_LE(lower_bound, upper_bound);

  lower_bound_ = ::common::AlignDown(
      reinterpret_cast<const uint8*>(lower_bound), kShadowRatio);
  upper_bound_ = ::common::AlignUp(
      reinterpret_cast<const uint8*>(upper_bound), kShadowRatio);
  Reset();
}

void ShadowWalker::Reset() {
  // Walk to the beginning of the first non-nested block, or to the end
  // of the range, whichever comes first.
  nesting_depth_ = -1;
  for (cursor_ = lower_bound_; cursor_ != upper_bound_;
       cursor_ += kShadowRatio) {
    uint8 marker = Shadow::GetShadowMarkerForAddress(cursor_);
    if (ShadowMarkerHelper::IsBlockStart(marker) &&
        !ShadowMarkerHelper::IsNestedBlockStart(marker)) {
      break;
    }
  }
}

bool ShadowWalker::Next(BlockInfo* info) {
  DCHECK_NE(static_cast<BlockInfo*>(NULL), info);

  // Iterate until a reportable block is encountered, or the slab is exhausted.
  for (; cursor_ != upper_bound_; cursor_ += kShadowRatio) {
    uint8 marker = Shadow::GetShadowMarkerForAddress(cursor_);

    // Update the nesting depth when block end markers are encountered.
    if (ShadowMarkerHelper::IsBlockEnd(marker)) {
      DCHECK_LE(0, nesting_depth_);
      --nesting_depth_;
      continue;
    }

    // Look for a block start marker.
    if (ShadowMarkerHelper::IsBlockStart(marker)) {
      // Update the nesting depth when block start bytes are encountered.
      ++nesting_depth_;

      // Non-nested blocks should only be encountered at depth 0.
      bool is_nested = ShadowMarkerHelper::IsNestedBlockStart(marker);
      DCHECK(is_nested || nesting_depth_ == 0);

      // Determine if the block is to be reported.
      if (!is_nested || recursive_) {
        // This can only fail if the shadow memory is malformed.
        CHECK(Shadow::BlockInfoFromShadow(cursor_, info));

        // In a recursive descent we have to process body contents.
        if (recursive_) {
          cursor_ += kShadowRatio;
        } else {
          // Otherwise we can skip the body of the block we just reported.
          // We skip directly to the end marker (but not past it so that depth
          // bookkeeping works properly).
          cursor_ += info->block_size - kShadowRatio;
        }
        return true;
      }
      continue;
    }
  }

  return false;
}

}  // namespace asan
}  // namespace agent
