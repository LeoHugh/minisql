#include "storage/table_heap.h"

bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  // 太大的tuple无法插入
  if (row.GetSerializedSize(schema_) >= PAGE_SIZE - 64) { 
    return false;
  }

  TablePage *page = nullptr;

  // 从freepagelist中尝试插入，避免扫描整个page链
  for (auto it = free_page_ids_.begin(); it != free_page_ids_.end(); ) {
    page_id_t current_free_page_id = *it;
    page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(current_free_page_id));
    //插入成功
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      buffer_pool_manager_->UnpinPage(current_free_page_id, true /* dirty */);
      return true;
    }
    //插入失败，说明这个page不再有足够的free space了，从free list中移除
    buffer_pool_manager_->UnpinPage(current_free_page_id, false);
    it = free_page_ids_.erase(it);
  }

  // 从page链中扫描插入
  page_id_t current_page_id_scan = first_page_id_;
  page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(current_page_id_scan));

  if (page == nullptr) {
    return false;
  }


  while (true) {
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      buffer_pool_manager_->UnpinPage(page->GetPageId(), true /* dirty */);
      return true;
    }
    // 太满了，继续往下一个page扫描
    page_id_t next_page_id = page->GetNextPageId();
    page_id_t current_page_id_for_unpin = page->GetPageId();
    buffer_pool_manager_->UnpinPage(current_page_id_for_unpin, false);

    //到达链尾了，插入失败，需要新建一个page
    if (next_page_id == INVALID_PAGE_ID) {
      page_id_t new_page_id;
      auto new_page_raw = buffer_pool_manager_->NewPage(new_page_id);
      //空间不足，分配失败
      if (new_page_raw == nullptr) 
      {
        return false; 
      }
      TablePage *prev_last_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(current_page_id_for_unpin));
      // 将新page链接到链表末尾
      prev_last_page->SetNextPageId(new_page_id); 
      buffer_pool_manager_->UnpinPage(current_page_id_for_unpin, true );
      // 初始化新page并插入tuple
      TablePage *new_page = reinterpret_cast<TablePage *>(new_page_raw);
      new_page->Init(new_page_id, current_page_id_for_unpin, log_manager_, txn); 
      bool insert_succeeded_in_new = new_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
      buffer_pool_manager_->UnpinPage(new_page_id, true );

      if (insert_succeeded_in_new) {
        AddPageToFreeList(new_page_id); 
      }
      return insert_succeeded_in_new;
    } else {
      // 继续往下一个page扫描
      page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(next_page_id));
      if (page == nullptr) {
        return false; 
      }
    }
  }
  return false;
}


//标记删除tuple，实际删除在ApplyDelete中进行
bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

/*bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto old_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (old_page == nullptr) {
    return false;
  }
  Row old_row(rid);
  bool ans = old_page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_);
  buffer_pool_manager_->UnpinPage(old_page->GetTablePageId(), true);
  return ans;
}
*/
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto old_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (old_page == nullptr) {
    return false;
  }
  
  Row old_row(rid);
  bool is_updated = old_page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_);
  
  // 就地更新
  if (is_updated) {
    buffer_pool_manager_->UnpinPage(old_page->GetTablePageId(), true);
    return true;
  }
  
  // 释放老页面
  buffer_pool_manager_->UnpinPage(old_page->GetTablePageId(), false);

  // 3. 然后执行 "Delete + Insert" 的 fallback 策略
  // 先尝试插入新 Tuple。注意，InsertTuple 成功后会更新 row 里面的 RowId
  if (!InsertTuple(row, txn)) {
    return false;
  }
  
  // 最后标记删除旧的 Tuple 
  ApplyDelete(rid, txn);
  
  return true;
}


// 实际删除tuple，释放空间
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  // Step1: Find the page which contains the tuple.
  // Step2: Delete the tuple from the page.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  page->ApplyDelete(rid, txn, log_manager_);
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  //加入free List
  AddPageToFreeList(rid.GetPageId());
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->RollbackDelete(rid, txn, log_manager_);
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

bool TableHeap::GetTuple(Row *row, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) {
    return false;
  }
  bool ans = page->GetTuple(row, schema_, txn, lock_manager_);
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
  return ans;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  //递归删除table_heap
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID) 
    DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

//封装begin
TableIterator TableHeap::Begin(Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(first_page_id_));
  if (page == nullptr) {
    return End();
  }
  RowId row_id;
  page->GetFirstTupleRid(&row_id);
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  if (row_id.GetPageId() == INVALID_PAGE_ID) {
    return End();
  }
  return TableIterator(this, row_id, txn);
}

TableIterator TableHeap::End() 
{ return TableIterator(nullptr, INVALID_ROWID, nullptr); }