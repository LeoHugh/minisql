#include "page/bitmap_page.h"

#include "glog/logging.h"

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  //检查是否已经满了
  if (page_allocated_ >= GetMaxSupportedSize()) {
    return false;
  }
  // 从 next_free_page_ 所在的字节开始查找，避免每次都从头扫描
  uint32_t start_byte = next_free_page_ / 8;
  if (start_byte >= MAX_CHARS) {
    start_byte = 0;
  }

  for (uint32_t i = start_byte; i < MAX_CHARS; i++) {
    if (bytes[i] != 0xFF) {
      for (uint32_t j = 0; j < 8; j++) {
        if (IsPageFreeLow(i, j)) { 
          page_offset = i * 8 + j;
          if (page_offset >= GetMaxSupportedSize()) return false;
          //翻转
          bytes[i] |= (1 << j);
          //更新成员变量
          page_allocated_++;
          next_free_page_ = page_offset + 1;
          return true;
        }
      }
    }
  }

  // 如果后半段没找到
  for (uint32_t i = 0; i < start_byte; i++) {
    if (bytes[i] != 0xFF) {
      for (uint32_t j = 0; j < 8; j++) {
        if (IsPageFreeLow(i, j)) { 
          page_offset = i * 8 + j;
          if (page_offset >= GetMaxSupportedSize()) return false;
          //翻转
          bytes[i] |= (1 << j);
          //更新成员变量
          page_allocated_++;
          next_free_page_ = page_offset + 1;
          return true;
        }
      }
    }
  }
  return false;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  // 越界
  if (page_offset >= GetMaxSupportedSize()) {
    return false;
  }
  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  //原来就为空
  if (IsPageFreeLow(byte_index, bit_index)) {
    return false;
  }

  bytes[byte_index] &= ~(1 << bit_index);

  //更新状态
  page_allocated_--;
  if (page_offset < next_free_page_) {
    next_free_page_ = page_offset;
  }

  return true;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  //过大
  if (page_offset >= GetMaxSupportedSize()) {
    return false;
  }
  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  return IsPageFreeLow(byte_index,bit_index);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  //构造掩码
  unsigned char mask= 1<<bit_index;
  //判断
  return !(bytes[byte_index] & mask);
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;