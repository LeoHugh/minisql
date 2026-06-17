#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}


uint32_t CatalogMeta::GetSerializedSize() const {
  return 12 + table_meta_pages_.size() * 8 + index_meta_pages_.size() * 8;
}

CatalogMeta::CatalogMeta() {}


CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
     Page *catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
      if (init) {
    // 创建一个新的空的 CatalogMeta 实例
    catalog_meta_ = CatalogMeta::NewInstance();
    next_table_id_ = 0;
    next_index_id_ = 0;
    // 持久化数据到磁盘
    catalog_meta_->SerializeTo(catalog_meta_page->GetData());  // 序列化元数据（也就是新建的 CatalogMeta 实例）
    buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), true);  // 标记为脏页
  } else {
  catalog_meta_ = CatalogMeta::DeserializeFrom(catalog_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), false);

    // 加载所有表和索引
    for (auto iter : catalog_meta_->table_meta_pages_) {
      LoadTable(iter.first, iter.second);
    }
    for (auto iter : catalog_meta_->index_meta_pages_) {
      LoadIndex(iter.first, iter.second);
    }
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

//注意tableinfo是输出参数
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  // 检查表名是否已经存在
  if (table_names_.find(table_name) != table_names_.end()) {
    return DB_TABLE_ALREADY_EXIST;
  }

  //深拷贝
  Schema *schema_copy = Schema::DeepCopySchema(schema);

  // 在disk上创建tableheap，获取第一个物理页id
  TableHeap *new_table = TableHeap::Create(buffer_pool_manager_, schema_copy, txn, log_manager_,lock_manager_);              
  page_id_t root_page_id = new_table->GetFirstPageId();

  //通过bufferpool申请一个新的 page 来存储表tablemetadata
  page_id_t table_page_id;
  auto table_meta_page = buffer_pool_manager_->NewPage(table_page_id);
  TableMetadata *table_meta =TableMetadata::Create(next_table_id_, table_name, root_page_id, schema_copy);  
  table_meta->SerializeTo(table_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(table_meta_page->GetPageId(), true);                   

  // 更新tables_与table_names
  table_names_.emplace(table_name, next_table_id_);   
  table_info = TableInfo::Create();
  table_info->Init(table_meta, new_table);
  tables_.emplace(next_table_id_, table_info);  
  

  // 持久化catalogmeta
  catalog_meta_->table_meta_pages_.emplace(next_table_id_, table_page_id); 
  auto catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  catalog_meta_->SerializeTo(catalog_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), true);

  //更新全局table_id
  next_table_id_++;

  return DB_SUCCESS;
}


dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto table = table_names_.find(table_name);
  if (table == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = tables_.find(table->second)->second;
  return DB_SUCCESS;
}

//输出参数，得到所有的table
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for (auto iter : tables_) {
    tables.push_back(iter.second);  
  }
  return DB_SUCCESS;
}


dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  // 检查表是否已经存在
  auto table = table_names_.find(table_name);
  if (table == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  // 检查该表上是否已经存在该索引
  if (index_names_[table_name].find(index_name) != index_names_[table_name].end()) {
    return DB_INDEX_ALREADY_EXIST;
  }

  // 获取 table id 和 info
  table_id_t table_id = table->second;
  TableInfo *table_info = tables_[table_id];
  const Schema *schema = table_info->GetSchema();

  // 验证索引列是否存在
  std::vector<uint32_t> key_map;
  for (const auto &col_name : index_keys) {
    uint32_t col_index;
    if (schema->GetColumnIndex(col_name, col_index) != DB_SUCCESS) {
      return DB_COLUMN_NAME_NOT_EXIST;
    }
    key_map.push_back(col_index);
  }

  // 构建indexmetadata，写入到disk
  IndexMetadata *index_meta = IndexMetadata::Create(next_index_id_, index_name, table_id, key_map);
  page_id_t index_page_id;
  auto index_page = buffer_pool_manager_->NewPage(index_page_id);
  index_meta->SerializeTo(index_page->GetData());
  buffer_pool_manager_->UnpinPage(index_page->GetPageId(), true);

  // 更新catalogmeta并持久化
  catalog_meta_->index_meta_pages_.emplace(next_index_id_, index_page_id);
  auto catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  catalog_meta_->SerializeTo(catalog_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), true);


  // 构建index_info
  index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);

  //更新index_names_与indexes_
  index_names_[table_name][index_name] = next_index_id_;
  indexes_.emplace(next_index_id_, index_info);
  
  //更新全局index_id
  next_index_id_++;

  return DB_SUCCESS;
}


dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table = index_names_.find(table_name);
  if (table == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  auto table_indexes = table->second;
  auto table_index = table_indexes.find(index_name);
  if (table_index == table_indexes.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  index_info = indexes_.find(table_index->second)->second;
  return DB_SUCCESS;
}

//输出参数，得到某个表的所有index
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  auto table = index_names_.find(table_name);
  if (table == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  for (auto index_id : table->second) {
    indexes.push_back(indexes_.find(index_id.second)->second);
  }
  return DB_SUCCESS;
}


dberr_t CatalogManager::DropTable(const string &table_name) {
  auto table = table_names_.find(table_name);
  if (table == table_names_.end()) {
    return DB_TABLE_ALREADY_EXIST;
  }
  table_id_t table_id = table->second;

  // 删除对应的indexes
  auto index_map_iter = index_names_.find(table_name);
  if (index_map_iter != index_names_.end()) {
    auto table_indexes = index_map_iter->second;
    for (const auto &index : table_indexes) {
      DropIndex(table_name, index.first);
    }
  }

  // 释放 TableInfo 以避免内存泄漏
  delete tables_[table_id];

  // 删除table_names与tables_
  table_names_.erase(table);
  tables_.erase(table_id);

  // 删除tablemetapage，修改catalogmeta并持久化
  auto catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  buffer_pool_manager_->DeletePage(catalog_meta_->table_meta_pages_[table_id]);
  catalog_meta_->table_meta_pages_.erase(table_id);
  catalog_meta_->SerializeTo(catalog_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), true);

  return DB_SUCCESS;
}


dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto index_map_iter = index_names_.find(table_name);
  if (index_map_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  auto index_iter = index_map_iter->second.find(index_name);
  if (index_iter == index_map_iter->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  index_id_t index_id = index_iter->second;

  // 释放 IndexInfo 内存以防泄漏
  delete indexes_[index_id];

  // 删除索引映射
  index_map_iter->second.erase(index_iter);
  indexes_.erase(index_id);

  // 持久化
  auto const catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  buffer_pool_manager_->DeletePage(catalog_meta_->index_meta_pages_[index_id]);
  catalog_meta_->index_meta_pages_.erase(index_id);
  catalog_meta_->SerializeTo(catalog_meta_page->GetData());
  buffer_pool_manager_->UnpinPage(catalog_meta_page->GetPageId(), true);

  return DB_SUCCESS;
}


dberr_t CatalogManager::FlushCatalogMetaPage() const {
  buffer_pool_manager_->FlushPage(CATALOG_META_PAGE_ID);
  return DB_SUCCESS;
}


dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  //原来应该没有
  if (tables_.find(table_id) != tables_.end()) {
    return DB_FAILED;
  }

  // 从disk根据page_id加载tablemetadata
  auto page = buffer_pool_manager_->FetchPage(page_id);
  TableMetadata *table_meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), table_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  // 根据tablemetadata构建tableheap和tableinfo，并更新tables_与table_names_
  TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_, table_meta->GetFirstPageId(), table_meta->GetSchema(),
                                            log_manager_, lock_manager_);
  TableInfo *table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);
  table_names_.emplace(table_meta->GetTableName(), table_id);
  tables_.emplace(table_id, table_info);

  return DB_SUCCESS;
}


dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  if (indexes_.find(index_id) != indexes_.end()) {
    return DB_FAILED;
  }

  // 从disk根据page_id加载indexmetadata
  auto page = buffer_pool_manager_->FetchPage(page_id);
  IndexMetadata *index_meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), index_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  // 根据indexmetadata构建indexinfo，并更新index_names_与indexes_
  IndexInfo *index_info = IndexInfo::Create();
  index_info->Init(index_meta, tables_[index_meta->GetTableId()], buffer_pool_manager_);
  std::string table_name = tables_.find(index_meta->GetTableId())->second->GetTableName();
  index_names_[table_name][index_meta->GetIndexName()] = index_id;
  indexes_.emplace(index_id, index_info);
  return DB_SUCCESS;
}

//根据表ID从内存缓存中查找并返回对应的表信息
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto table = tables_.find(table_id);
  if (table == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = table->second;
  return DB_SUCCESS;
}