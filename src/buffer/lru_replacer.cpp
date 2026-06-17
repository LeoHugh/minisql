#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages){}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 * 最久未使用的页面被淘汰
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  if(lru_list_.empty()){
    return false;
  }
  *frame_id = lru_list_.back();
  frame_map.erase(*frame_id);
  lru_list_.pop_back();
  return true;
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  auto it = frame_map.find(frame_id);
  if(it != frame_map.end()){
    lru_list_.erase(it->second);
    frame_map.erase(it);
  }
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if(frame_map.find(frame_id) == frame_map.end()){
      lru_list_.push_front(frame_id);
      frame_map[frame_id] = lru_list_.begin();
    }
}

/**
 * TODO: Student Implement
 */
size_t LRUReplacer::Size() {
  return lru_list_.size();
}