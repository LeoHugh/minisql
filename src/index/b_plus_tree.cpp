#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

#include <type_traits>

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  //最大的size数假设除了header以外全是key-value对
  if (leaf_max_size_ == UNDEFINED_SIZE) {
    leaf_max_size_ =(PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + static_cast<int>(sizeof(RowId))) - 1;
  }
  if (internal_max_size_ == UNDEFINED_SIZE) {
    internal_max_size_ =
        (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + static_cast<int>(sizeof(page_id_t))) - 1;
  }
  // 从 index roots page 里拿到 root page id
  Page *index_roots_page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  auto *index_roots = reinterpret_cast<IndexRootsPage *>(index_roots_page->GetData());
  if (!index_roots->GetRootId(index_id_, &root_page_id_)) {
    root_page_id_ = INVALID_PAGE_ID;
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  //默认是删除整棵树
  if (current_page_id == INVALID_PAGE_ID) {
    current_page_id = root_page_id_;
  }
  if (current_page_id == INVALID_PAGE_ID) {
    return;
  }

  Page *page = buffer_pool_manager_->FetchPage(current_page_id);
  auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  if (!tree_page->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(tree_page);
    std::vector<page_id_t> children;
    children.reserve(internal->GetSize());
    for (int i = 0; i < internal->GetSize(); i++) {
      children.push_back(internal->ValueAt(i));
    }
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    //递归删除子树
    for (page_id_t child_id : children) {
      Destroy(child_id);
    }
  } else {
    buffer_pool_manager_->UnpinPage(current_page_id, false);
  }
  buffer_pool_manager_->DeletePage(current_page_id);

  //如果删除了根节点，更新index roots page
  if (current_page_id == root_page_id_) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()) {
    return false;
  }
  //找到叶子节点
  Page *leaf_page = FindLeafPage(key, root_page_id_, false);
  if (leaf_page == nullptr) {
    return false;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  //在叶子节点里找key对应的value
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  if (found) {
    result.push_back(value);
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  //注意空树的case
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t root_id;
  Page *root_page = buffer_pool_manager_->NewPage(root_id);
  ASSERT(root_page != nullptr, "Out of memory when creating root page.");

  auto *root = reinterpret_cast<LeafPage *>(root_page->GetData());
  root->Init(root_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  root->Insert(key, value, processor_);
  root_page_id_ = root_id;
  UpdateRootPageId(1);
  buffer_pool_manager_->UnpinPage(root_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  //注意，这里的page是在FindLeafPage里被pin了的，最后需要unpin
  Page *leaf_page = FindLeafPage(key, root_page_id_, false);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());

  int old_size = leaf->GetSize();
  int new_size = leaf->Insert(key, value, processor_);
  //插入失败
  if (new_size == old_size) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }

  if (new_size <= leaf->GetMaxSize()) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    return true;
  }
  //需要分割上溢，new_leaf被pin了但没有被unpin，最后需要unpin
  LeafPage *new_leaf = Split(leaf, transaction);
  //维护链表关系
  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(new_leaf->GetPageId());
  //维护与父节点关系
  InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
//内部节点
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
  page_id_t new_page_id;
  Page *new_page = buffer_pool_manager_->NewPage(new_page_id);
  ASSERT(new_page != nullptr, "Out of memory when splitting internal page.");
  auto *new_internal = reinterpret_cast<InternalPage *>(new_page->GetData());
  new_internal->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
  node->MoveHalfTo(new_internal, buffer_pool_manager_);
  return new_internal;
}
//叶子节点分裂
BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  page_id_t new_page_id;
  Page *new_page = buffer_pool_manager_->NewPage(new_page_id);
  ASSERT(new_page != nullptr, "Out of memory when splitting leaf page.");
  auto *new_leaf = reinterpret_cast<LeafPage *>(new_page->GetData());
  new_leaf->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
  node->MoveHalfTo(new_leaf);
  return new_leaf;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *transaction) {
  //oldnode已经是根节点，创建根节点
  if (old_node->IsRootPage()) {
    page_id_t new_root_page_id;
    Page *new_root_page = buffer_pool_manager_->NewPage(new_root_page_id);
    ASSERT(new_root_page != nullptr, "Out of memory when creating new root.");
    auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    //初始化根节点
    new_root->Init(new_root_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    //更新父亲
    old_node->SetParentPageId(new_root_page_id);
    new_node->SetParentPageId(new_root_page_id);
    //更新根节点id
    root_page_id_ = new_root_page_id;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(new_root_page_id, true);
    return;
  }
  //更新父节点
  page_id_t parent_page_id = old_node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_page_id)->GetData());
  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  //不需要上溢
  if (parent->GetSize() <= parent->GetMaxSize()) {
    buffer_pool_manager_->UnpinPage(parent_page_id, true);
    return;
  }
  //继续上溢
  InternalPage *new_parent = Split(parent, transaction);
  InsertIntoParent(parent, new_parent->KeyAt(0), new_parent, transaction);
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  if (IsEmpty()) {
    return;
  }
  //找leafpage
  Page *leaf_page = FindLeafPage(key, root_page_id_, false);
  if (leaf_page == nullptr) {
    return;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  int old_size = leaf->GetSize();
  int new_size = leaf->RemoveAndDeleteRecord(key, processor_);
  //删除失败
  if (new_size == old_size) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return;
  }

  bool leaf_deleted = CoalesceOrRedistribute(leaf, transaction);
  //如果leaf被删除了，就不需要unpin了，因为已经被buffer pool manager删除了
  if (!leaf_deleted) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  }
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  //根节点直接走AdjustRoot
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
  //做个防御,注意这里会导致父子节点的key不一致，但不影响正确性
  if (node->GetSize() >= node->GetMinSize()) {
    return false;
  }

  auto *parent =reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
  int index = parent->ValueIndex(node->GetPageId());
  //默认找左兄弟
  int neighbor_index = (index == 0) ? 1 : index - 1;
  auto *neighbor = reinterpret_cast<N *>(buffer_pool_manager_->FetchPage(parent->ValueAt(neighbor_index))->GetData());
  //充裕，走 redistribute借数据
  if (node->GetSize() + neighbor->GetSize() > node->GetMaxSize()) {
    Redistribute(neighbor, node, index);
    buffer_pool_manager_->UnpinPage(neighbor->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return false;//不用删除node
  }
  //较小，需要合并进入邻居，先让node始终在右
  if (index == 0) {
    std::swap(node, neighbor);
    index = 1;
  }

  bool parent_deleted = Coalesce(neighbor, node, parent, index, transaction);
  buffer_pool_manager_->UnpinPage(neighbor->GetPageId(), true);
  if (!parent_deleted) {
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
  return true;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  node->MoveAllTo(neighbor_node);
  parent->Remove(index);

  page_id_t node_id = node->GetPageId();
  buffer_pool_manager_->UnpinPage(node_id, true);
  buffer_pool_manager_->DeletePage(node_id);
  //递归处理父节点
  return CoalesceOrRedistribute(parent, transaction);
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  //可能存在dummy_node
  GenericKey *middle_key = processor_.InitKey();
  memcpy(middle_key, parent->KeyAt(index), processor_.GetKeySize());
  node->MoveAllTo(neighbor_node, middle_key, buffer_pool_manager_);
  free(middle_key);
  parent->Remove(index);

  page_id_t node_id = node->GetPageId();
  buffer_pool_manager_->UnpinPage(node_id, true);
  buffer_pool_manager_->DeletePage(node_id);
  return CoalesceOrRedistribute(parent, transaction);
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  auto *parent =
      reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
  //右边借数据给左边
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } 
  //左边借数据给右边
  else {
    neighbor_node->MoveLastToFrontOf(node);
    parent->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  auto *parent =
      reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
  } else {
    neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  //根节点还是叶子节点
  if (old_root_node->IsLeafPage()) {
    if (old_root_node->GetSize() > 0) {
      return false;
    }
    //更新rootpage,删除root_id
    page_id_t root_id = old_root_node->GetPageId();
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(root_id, true);
    buffer_pool_manager_->DeletePage(root_id);
    return true;
  }

  auto *root_internal = reinterpret_cast<InternalPage *>(old_root_node);
  if (root_internal->GetSize() > 1) {
    return false;
  }
  //下沉
  page_id_t child_id = root_internal->RemoveAndReturnOnlyChild();
  //更新child的父亲
  auto *new_root = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(child_id)->GetData());
  new_root->SetParentPageId(INVALID_PAGE_ID);
  buffer_pool_manager_->UnpinPage(child_id, true);

  //更新rootpageid
  root_page_id_ = child_id;
  UpdateRootPageId(0);
  //删除旧根节点
  page_id_t old_root_id = root_internal->GetPageId();
  buffer_pool_manager_->UnpinPage(old_root_id, true);
  buffer_pool_manager_->DeletePage(old_root_id);
  return true;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/* 
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  if (IsEmpty()) {
    return End();
  }
  Page *leaf_page = FindLeafPage(nullptr, root_page_id_, true);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, 0);
}

/* >=key
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) {
    return End();
  }
  Page *leaf_page = FindLeafPage(key, root_page_id_, false);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  int index = leaf->KeyIndex(key, processor_);
  page_id_t page_id = leaf->GetPageId();
  //如果index等于size，说明输入key比叶子节点里所有key都大，应该从下一个叶子节点开始遍历
  if (index >= leaf->GetSize()) {
    page_id_t next_id = leaf->GetNextPageId();
    buffer_pool_manager_->UnpinPage(page_id, false);
    if (next_id == INVALID_PAGE_ID) {
      return End();
    }
    return IndexIterator(next_id, buffer_pool_manager_, 0);
  }
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*注意leftmost留给了Begin()方法，FindLeafPage里leftmost为false时才会真正根据key找叶子节点
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }

  page_id_t current_page_id = (page_id == INVALID_PAGE_ID) ? root_page_id_ : page_id;
  Page *page = buffer_pool_manager_->FetchPage(current_page_id);
  auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());

  while (!tree_page->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(tree_page);
    page_id_t next_page_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, processor_);
    //先拿到下一个页面的页号，再unpin当前页面，最后fetch下一个页面
    Page *next_page = buffer_pool_manager_->FetchPage(next_page_id);
    buffer_pool_manager_->UnpinPage(internal->GetPageId(), false);
    page = next_page;
    tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  //取出buffer池里index roots page的页，修改完之后unpin并标记为dirty
  Page *page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  auto *index_roots = reinterpret_cast<IndexRootsPage *>(page->GetData());
  //插入，删除，更新
  if (insert_record) {
    index_roots->Insert(index_id_, root_page_id_);
  } 
  else if (root_page_id_ == INVALID_PAGE_ID) {
    index_roots->Delete(index_id_);
  } 
  else if (!index_roots->Update(index_id_, root_page_id_)) {
    index_roots->Insert(index_id_, root_page_id_);
  }

  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        // 显示 Dummy Key 的实际内存残留值，方便调试
        // 注意：index 0 的 Key 可能是未初始化的垃圾数据，反序列化可能崩溃
        try {
          Row ans;
          processor_.DeserializeToKey(inner->KeyAt(0), ans, schema);
          if (ans.GetFieldCount() > 0 && ans.GetField(0) != nullptr) {
            out << "[D]" << ans.GetField(0)->toString();
          } else {
            out << "[D]?";
          }
        } catch (...) {
          out << "[D]?";
        }
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}