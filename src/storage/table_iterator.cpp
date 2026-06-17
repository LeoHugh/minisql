#include "storage/table_iterator.h"

#include "common/config.h"
#include "common/macros.h"
#include "storage/table_heap.h"

// table_heap为nullptr代表end()
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn): row_(new Row(rid)), table_heap_(table_heap), txn_(txn) {
  if (table_heap_ == nullptr || rid.GetPageId() == INVALID_PAGE_ID) {
    return;
  }
  //把对应的页取出来
  auto page = reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    table_heap_ = nullptr;
    return;
  }

  //获取对应的tuple到row_中
  bool get_tuple = page->GetTuple(row_, table_heap_->schema_, txn_, table_heap_->lock_manager_);

  table_heap_->buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  if (!get_tuple) {
    table_heap_ = nullptr;
  }
}

TableIterator::TableIterator(const TableIterator &other) : table_heap_(other.table_heap_), txn_(other.txn_) {
  if (other.row_ == nullptr) {
    row_ = nullptr;
    return;
  }

  row_ = new Row(*other.row_);
}

TableIterator::~TableIterator() { delete row_; }

bool TableIterator::operator==(const TableIterator &itr) const {
  if (table_heap_ == nullptr && itr.table_heap_ == nullptr) {
    return true;
  }
  return row_->GetRowId() == itr.row_->GetRowId() && table_heap_ == itr.table_heap_;
}

bool TableIterator::operator!=(const TableIterator &itr) const { return !this->operator==(itr); }

const Row &TableIterator::operator*() { return *row_; }

Row *TableIterator::operator->() { return row_; }

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  delete row_;
  //注意，这里是深拷贝
  row_ = new Row(*itr.row_);
  table_heap_ = itr.table_heap_;
  txn_ = itr.txn_;
  return *this;
}

// ++iter
TableIterator &TableIterator::operator++() {
  if (table_heap_ == nullptr) {
    return *this;
  }

  // find in current page
  RowId next_rid;
  bool found;
  auto page = reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(row_->GetRowId().GetPageId()));
  if (page) {
    found = page->GetNextTupleRid(row_->GetRowId(), &next_rid);
    table_heap_->buffer_pool_manager_->UnpinPage(row_->GetRowId().GetPageId(), false);
  }

  // if not found, find through next page
  if (!found) {
    page_id_t page_id = page ? page->GetNextPageId() : INVALID_PAGE_ID;
    while (page_id != INVALID_PAGE_ID) {
      auto next_page = reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(page_id));
      if (!next_page) {
        break;
      }
      RowId rid;
      bool get_first_tuple_rid = next_page->GetFirstTupleRid(&rid);
      table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);
      if (get_first_tuple_rid) {
        next_rid = rid;
        found = true;
        break;
      }
      page_id = next_page->GetNextPageId();
    }
  }

  // if not found, return end()
  if (!found) {
    table_heap_ = nullptr;
    return *this;
  }

  // 更新row_，返回下一个tuple
  delete row_;
  row_ = new Row(next_rid);
  auto next_page = reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(next_rid.GetPageId()));
  if (!next_page) {
    table_heap_ = nullptr;
    return *this;
  }
  bool get_tuple = next_page->GetTuple(row_, table_heap_->schema_, txn_, table_heap_->lock_manager_);
  table_heap_->buffer_pool_manager_->UnpinPage(next_page->GetPageId(), false);
  if (!get_tuple) {
    table_heap_ = nullptr;
  }

  return *this;
}

// iter++
TableIterator TableIterator::operator++(int) {
  TableIterator temp(*this);
  ++(*this);
  return temp;
}