#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  //开在堆上面
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  auto iter = page_table_.find(page_id);
  //如果本来就在内存池
  if(iter != page_table_.end()) {
    //对应的内存帧
    frame_id_t frame_id = iter->second;
    Page &page = pages_[frame_id];
    page.pin_count_++;
    replacer_->Pin(frame_id);
    return &page;
  }
  frame_id_t frame_id;
  //不在内存池，先找空闲页，如果没有空闲页了，就找替换页
  if(!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else {
    if(!replacer_->Victim(&frame_id)) {
      return nullptr;
    }
    Page &page = pages_[frame_id];
    //如果是脏页，就写回磁盘
    if(page.IsDirty()) {
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
    }
    page_table_.erase(page.GetPageId());
  }
  //更新page
  Page &page = pages_[frame_id];
  page.page_id_ = page_id;
  page.pin_count_ = 1;      
  page.is_dirty_ = false;
  disk_manager_->ReadPage(page_id, page.GetData());
  //更新page_table和replacer
  page_table_[page_id] = frame_id;
  replacer_->Pin(frame_id);
  return &page;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  // 0.   Make sure you call AllocatePage!
  frame_id_t frame_id;
  if(!free_list_.empty()){
      frame_id=free_list_.front();
      free_list_.pop_front();
  }
  else{
    if(!replacer_->Victim(&frame_id)){
      return nullptr;
    }
    Page &page = pages_[frame_id];
    if(page.IsDirty()){
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
    }
    page_table_.erase(page.GetPageId());
  }
  page_id = AllocatePage();
  Page &page = pages_[frame_id];
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;
  memset(page.GetData(), 0, PAGE_SIZE);
  page_table_[page_id] = frame_id;
  replacer_->Pin(frame_id);
  return &page;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  auto iter = page_table_.find(page_id);
  //如果不在内存中，直接去磁盘上释放
  if(iter == page_table_.end()) {
    DeallocatePage(page_id);
    return true;
  }
  //如果在内存中，先检查pin_count是否为0，如果不为0，说明有人在用，不能删除
  frame_id_t frame_id = iter->second;
  Page &page = pages_[frame_id];
  if (page.pin_count_ > 0) {
    return false;
  }
  //更新bitmap
  DeallocatePage(page_id);
  //更新page_table和replacer
  page_table_.erase(page_id);
  page.page_id_ = INVALID_PAGE_ID;
  page.pin_count_ = 0;
  page.is_dirty_ = false;
  replacer_->Pin(frame_id);
  free_list_.emplace_back(frame_id);
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  auto iter = page_table_.find(page_id);
  if(iter == page_table_.end()) {
    return false;
  }
  frame_id_t frame_id = iter->second;
  Page &page = pages_[frame_id];
  if (page.pin_count_ <= 0) {
    return false;
  }
  page.pin_count_--;
  if (is_dirty) {
    page.is_dirty_ = true;
  }
  if(page.pin_count_ == 0) {
    replacer_->Unpin(frame_id);
  }
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  auto iter = page_table_.find(page_id);
  if(iter == page_table_.end()) {
    return false;
  }
  frame_id_t frame_id = iter->second;
  Page &page = pages_[frame_id];
  if(page.is_dirty_) {
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
  }
  page.is_dirty_ = false;
  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}