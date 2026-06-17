#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()

/**
 * TODO: Student Implement
 */
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  //对父类变量初始化
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  SetMaxSize(max_size);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value)
      return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memmove(dest, src, pair_num * (GetKeySize() + sizeof(page_id_t)));
}
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 * 用了二分查找
 * [,) 左闭右开
 */
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
  int left = 1, right = GetSize() - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    if (KM.CompareKeys(key, KeyAt(mid)) < 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  //case：key=0
  return ValueAt(right);
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 * 新根节点，不负责处理就具体的分裂逻辑，只负责物理数据的迁移
 */
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
    SetValueAt(0, old_value);
    SetKeyAt(1, new_key);
    SetValueAt(1, new_value); 
    SetSize(2);
}

/*  内部节点上溢
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  //找旧索引
  int old_index = ValueIndex(old_value);
  if(old_index == -1) {
    return GetSize();
  }
  int new_index = old_index + 1;
  //平移右边index
  if (new_index < GetSize()) {
    PairCopy(PairPtrAt(new_index + 1), PairPtrAt(new_index), GetSize() - new_index);
  }
  SetKeyAt(new_index, new_key);
  SetValueAt(new_index, new_value);
  //更新size
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 * buffer_pool_manager 是干嘛的？传给CopyNFrom()用于fetch page
 */
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
  //分裂点
  int split_index = GetMinSize();
  int move_num = GetSize() - split_index;
  recipient->CopyNFrom(PairPtrAt(split_index), move_num, buffer_pool_manager);
  SetSize(split_index);

}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger
 *
 */
void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
  //保证通用性，split&&merge
  int start_idx = GetSize();

  // 2. 物理搬运：从当前的末尾处开始写入
  void *dest = PairPtrAt(start_idx); 
  PairCopy(dest, src, size);

  // 3. 更新自己的计数器
  IncreaseSize(size);

  // 4. 领养这些新搬来的孩子
  for (int i = 0; i < size; ++i) {
    // 逻辑下标：start_idx + i
    page_id_t child_page_id = ValueAt(start_idx + i);
    
    // 通过 BPM 拿到子页面对象
    Page *page = buffer_pool_manager->FetchPage(child_page_id);
    if (page != nullptr) {
      // 强转为 BPlusTreePage 来修改父节点 ID
      auto *b_plus_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
      b_plus_page->SetParentPageId(GetPageId());
      
      // Unpin， is_dirty 设为 true，因为修改了 ParentPageId
      buffer_pool_manager->UnpinPage(child_page_id, true);
    }
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
void InternalPage::Remove(int index) {
  if (index < 0 || index >= GetSize()) {
    return;
  }
  int move_num = GetSize() - index - 1;
  if (move_num > 0) {
    PairCopy(pairs_off + index * pair_size, pairs_off + (index + 1) * pair_size, move_num);
  }
  IncreaseSize(-1);
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 * 节点退化
 */
page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  page_id_t last_child_id = ValueAt(0);
  SetSize(0);
  return last_child_id;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key, BufferPoolManager *buffer_pool_manager) {
  //  middle_key 作为分界线，从父节点下沉到接收方
  SetKeyAt(0, middle_key);
  recipient->CopyNFrom(PairPtrAt(0), GetSize(), buffer_pool_manager);
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveFirstToEndOf(InternalPage *recipient, GenericKey *middle_key,
                                    BufferPoolManager *buffer_pool_manager) {
  page_id_t first_value = ValueAt(0);
  
  // 2. 接收方追加：它会把 middle_key 放在它的末尾有效位
  recipient->CopyLastFrom(middle_key, first_value, buffer_pool_manager);

  // 3. 上提：把我的 Index 1 处的 Key 提到父节点，作为新的分界线
  memcpy(middle_key, KeyAt(1), GetKeySize());

  // 4. 瘦身与平移：删掉原来的 Index 0，让 Index 1 顶上来变成新的 Dummy Key
  Remove(0);

}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value, BufferPoolManager *buffer_pool_manager) {
   // 1. 计算末尾位置
  int target_idx = GetSize();

  // 2. 写入：这里不需要 memmove，直接 Set
  SetKeyAt(target_idx, key);
  SetValueAt(target_idx, value);

  // 3. 更新 Size
  IncreaseSize(1);

  // 4. 领养逻辑
  auto page = buffer_pool_manager->FetchPage(value);
  if (page != nullptr) {
    auto b_plus_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    b_plus_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(value, true);
  }

}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array to position the middle_key at the
 * right place.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those pages that are
 * moved to the recipient
 */
void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {

                                      // 1. 准备数据：我要把我的最后一个孩子送走
  int last_idx = GetSize() - 1;
  page_id_t last_value = ValueAt(last_idx);
  GenericKey *last_key = KeyAt(last_idx);

  // 2. 接收方准备：recipient 先腾出空间并领养孩子
  recipient->CopyFirstFrom(last_value, buffer_pool_manager);

  // 3. 关键：把父节点的 middle_key 下沉到 recipient 的第一个有效 Key 位置（Index 1）
  // 因为 recipient 刚执行了 CopyFirstFrom，它现在的 Index 1 是空的/旧的
  recipient->SetKeyAt(1, middle_key);

  // 4. 上提：把我的最后一个 Key 提到父节点，成为新的分界线
  memcpy(middle_key, last_key, GetKeySize());

  // 5. 瘦身
  IncreaseSize(-1);
 }

/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyFirstFrom(const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  // 1. 全体右移：为新成员腾出 Index 0 和 Index 1 的位置
  // 注意：虽然我们只插入一个 ValueAt(0)，但为了保持 Key/Value 对齐，我们需要平移整个数组
  memmove(PairPtrAt(1), PairPtrAt(0), GetSize() * pair_size);

  // 2. 写入新孩子：存放在新的 Index 0 
  // 注意：此时 Index 0 的 Key 依然是无效的（Dummy Key）
  SetValueAt(0, value);

  // 3. 更新 Size
  IncreaseSize(1);

  // 4. 领养逻辑：更新子页面的父指针
  auto page = buffer_pool_manager->FetchPage(value);
  if (page != nullptr) {
    auto b_plus_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    b_plus_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(value, true);
  }
}