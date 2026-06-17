#include "executor/execute_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>

#include "common/dberr.h"
#include "common/instance.h"
#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "parser/syntax_tree.h"
#include "planner/planner.h"
#include "utils/utils.h"

extern "C" {
int yyparse(void);
#include "parser/minisql_lex.h"
#include "parser/parser.h"
}

ExecuteEngine::ExecuteEngine(bool init) {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }

  /** When you have completed all the code for
   * the test, run it using main.cpp and uncomment
   * this part of the code.
   */
  if (!init) {
    struct dirent *stdir;
    while ((stdir = readdir(dir)) != nullptr) {
      if (strcmp(stdir->d_name, ".") == 0 || strcmp(stdir->d_name, "..") == 0 || stdir->d_name[0] == '.')
        continue;
      dbs_[stdir->d_name] = new DBStorageEngine(stdir->d_name, false);//加载已经有的数据库
    }
  }
  closedir(dir);
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                 const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    // Create a new sequential scan executor
    case PlanType::SeqScan: {
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    }
    // Create a new index scan executor
    case PlanType::IndexScan: {
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    }
    // Create a new update executor
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
    // Create a new delete executor
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values: {
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    }
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  // Construct the executor for the abstract plan node
  auto executor = CreateExecutor(exec_ctx, plan);

  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) {
        result_set->push_back(row);
      }
    }
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) {
      result_set->clear();
    }
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) {
    return DB_FAILED;
  }
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!current_db_.empty())
    context = dbs_[current_db_]->MakeExecuteContext(nullptr);
  switch (ast->type_) {
    case kNodeCreateDB:
      return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:
      return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:
      return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:
      return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:
      return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable:
      return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:
      return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes:
      return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex:
      return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:
      return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:
      return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:
      return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback:
      return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:
      return ExecuteExecfile(ast, context.get());
    case kNodeQuit:
      return ExecuteQuit(ast, context.get());
    default:
      break;
  }

  // Plan the query.
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    // Execute the query.
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }

  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time)).count());

  // Return the result set as string.
  std::stringstream ss;
  ResultWriter writer(ss);

  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      // find the max width for each column
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set) {
        for (uint32_t i = 0; i < num_of_columns; i++) {
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
        }
      }
      int k = 0;
      for (const auto &column : schema->GetColumns()) {
        data_width[k] = max(data_width[k], int(column->GetName().length()));
        k++;
      }
      // Generate header for the result set.
      writer.Divider(data_width);
      k = 0;
      writer.BeginRow();
      for (const auto &column : schema->GetColumns()) {
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      }
      writer.EndRow();
      writer.Divider(data_width);

      // Transforming result set into strings.
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        }
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  std::cout << writer.stream_.rdbuf();
  // todo:: use shared_ptr for schema
  if (ast->type_ == kNodeSelect)
    delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:
      cout << "Database already exists." << endl;
      break;
    case DB_NOT_EXIST:
      cout << "Database not exists." << endl;
      break;
    case DB_TABLE_ALREADY_EXIST:
      cout << "Table already exists." << endl;
      break;
    case DB_TABLE_NOT_EXIST:
      cout << "Table not exists." << endl;
      break;
    case DB_INDEX_ALREADY_EXIST:
      cout << "Index already exists." << endl;
      break;
    case DB_INDEX_NOT_FOUND:
      cout << "Index not exists." << endl;
      break;
    case DB_COLUMN_NAME_NOT_EXIST:
      cout << "Column not exists." << endl;
      break;
    case DB_KEY_NOT_FOUND:
      cout << "Key not exists." << endl;
      break;
    case DB_QUIT:
      cout << "Bye." << endl;
      break;
    default:
      break;
  }
}

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    return DB_ALREADY_EXIST;
  }
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) {
    return DB_NOT_EXIST;
  }
  remove(("./databases/" + db_name).c_str());
  delete dbs_[db_name];
  dbs_.erase(db_name);
  if (db_name == current_db_)
    current_db_ = "";
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowDatabases" << std::endl;
#endif
  if (dbs_.empty()) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width)
      max_width = itr.first.length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteUseDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    current_db_ = db_name;
    cout << "Database changed" << endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowTables" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  vector<TableInfo *> tables;
  if (dbs_[current_db_]->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_FAILED;
  }
  string table_in_db("Tables_in_" + current_db_);
  uint max_width = table_in_db.length();
  for (const auto &itr : tables) {
    if (itr->GetTableName().length() > max_width)
      max_width = itr->GetTableName().length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : tables) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

TypeId parse_column_type(std::string column_type) {
  if (column_type == "int") {
    return TypeId::kTypeInt;
  } else if (column_type == "float") {
    return TypeId::kTypeFloat;
  } else if (column_type == "char") {
    return TypeId::kTypeChar;
  } else {
    return TypeId::kTypeInvalid;
  }
}

/**
digraph G{ 
SYNTAX_NODE_13[label="kNodeCreateTable(6,1)\nid(13)"];
SYNTAX_NODE_0[label="kNodeIdentifier(1,20)\nid(0),val(account)"];
SYNTAX_NODE_14[label="kNodeColumnDefinitionList(6,1)\nid(14)"];
SYNTAX_NODE_3[label="kNodeColumnDefinition(2,11)\nid(3)"];
SYNTAX_NODE_1[label="kNodeIdentifier(2,6)\nid(1),val(id)"];
SYNTAX_NODE_2[label="kNodeColumnType(2,10)\nid(2),val(int)"];
SYNTAX_NODE_1  ->  SYNTAX_NODE_2;
{rank=same; SYNTAX_NODE_1,SYNTAX_NODE_2};
SYNTAX_NODE_3  ->  SYNTAX_NODE_1;
SYNTAX_NODE_7[label="kNodeColumnDefinition(3,24)\nid(7),val(unique)"];
SYNTAX_NODE_4[label="kNodeIdentifier(3,8)\nid(4),val(name)"];
SYNTAX_NODE_6[label="kNodeColumnType(3,17)\nid(6),val(char)"];
SYNTAX_NODE_5[label="kNodeNumber(3,16)\nid(5),val(16)"];
SYNTAX_NODE_6  ->  SYNTAX_NODE_5;
SYNTAX_NODE_4  ->  SYNTAX_NODE_6;
{rank=same; SYNTAX_NODE_4,SYNTAX_NODE_6};
SYNTAX_NODE_7  ->  SYNTAX_NODE_4;
SYNTAX_NODE_10[label="kNodeColumnDefinition(4,18)\nid(10)"];
SYNTAX_NODE_8[label="kNodeIdentifier(4,11)\nid(8),val(balance)"];
SYNTAX_NODE_9[label="kNodeColumnType(4,17)\nid(9),val(float)"];
SYNTAX_NODE_8  ->  SYNTAX_NODE_9;
{rank=same; SYNTAX_NODE_8,SYNTAX_NODE_9};
SYNTAX_NODE_10  ->  SYNTAX_NODE_8;
SYNTAX_NODE_12[label="kNodeColumnList(5,19)\nid(12),val(primary keys)"];
SYNTAX_NODE_11[label="kNodeIdentifier(5,18)\nid(11),val(id)"];
SYNTAX_NODE_12  ->  SYNTAX_NODE_11;
SYNTAX_NODE_10  ->  SYNTAX_NODE_12;
{rank=same; SYNTAX_NODE_10,SYNTAX_NODE_12};
SYNTAX_NODE_7  ->  SYNTAX_NODE_10;
{rank=same; SYNTAX_NODE_7,SYNTAX_NODE_10};
SYNTAX_NODE_3  ->  SYNTAX_NODE_7;
{rank=same; SYNTAX_NODE_3,SYNTAX_NODE_7};
SYNTAX_NODE_14  ->  SYNTAX_NODE_3;
SYNTAX_NODE_0  ->  SYNTAX_NODE_14;
{rank=same; SYNTAX_NODE_0,SYNTAX_NODE_14};
SYNTAX_NODE_13  ->  SYNTAX_NODE_0;
}

*/

dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateTable" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }

  // 表名与主键
  std::string table_name = ast->child_->val_;//表名
  pSyntaxNode columns_node = ast->child_->next_;
  pSyntaxNode pk_node = nullptr;//找主键
  for (pSyntaxNode child = columns_node->child_; child != nullptr; child = child->next_) {
    if (child->type_ == kNodeColumnList && child->val_ != nullptr && std::strcmp(child->val_, "primary keys") == 0) {
      pk_node = child;
      break;
    }
  }
  //必须要有主键
  if (pk_node == nullptr) {
    cout << "Primary key not found in table definition" << endl;
    return DB_FAILED;
  }
  std::vector<std::string> pk_columns;
  for (pSyntaxNode child = pk_node->child_; child != nullptr; child = child->next_) {
    pk_columns.push_back(child->val_);
  }
  if (pk_columns.size() > 1) {
    cout << "Multiple primary keys are not supported yet" << endl;
    return DB_FAILED;
  }

  // 列表属性，数据类型，是否唯一，是否可空
  std::vector<Column *> columns;
  for (pSyntaxNode child = columns_node->child_; child != nullptr; child = child->next_) {
    //跳过主键
    if (child->type_ != kNodeColumnDefinition) {
      continue;
    }

    bool is_unique = false;
    if (child->val_ != nullptr && std::strcmp(child->val_, "unique") == 0) {
      is_unique = true;
    }

    std::string column_name = child->child_->val_;
    std::string column_type = child->child_->next_->val_;
    TypeId type = parse_column_type(column_type);
    if (type == TypeId::kTypeInvalid) {
      cout << "Invalid column type" << endl;
      return DB_FAILED;
    }


    //主键属性覆盖
    bool is_primary_key = false;
    bool is_nullable = true;
    if (std::find(pk_columns.begin(), pk_columns.end(), column_name) != pk_columns.end()) {
      is_primary_key = true;
    }
    if (is_primary_key) {
      is_nullable = false;
      is_unique = true;
    }

    if (type == TypeId::kTypeChar) {
      //对于char要标记字符数
      uint32_t length = std::atoi(child->child_->next_->val_);
      columns.push_back(new Column(column_name, type, length, columns.size(), is_nullable, is_unique));
    } else {
      columns.push_back(new Column(column_name, type, columns.size(), is_nullable, is_unique));
    }
  }

  // 3. create table
  Schema *schema = new Schema(columns);
  TableInfo *table_info;
  //调用getcatalog
  dberr_t create_table = context->GetCatalog()->CreateTable(table_name, schema, context->GetTransaction(), table_info);
  if (create_table != DB_SUCCESS) {
    ExecuteInformation(create_table);
    return create_table;
  }

  // 创建一个索引，使用的是主键，索引类型暂时固定为bptree
  auto *index_info = IndexInfo::Create();
  dberr_t create_index = context->GetCatalog()->CreateIndex(table_name, "pkey", pk_columns, context->GetTransaction(),
                                                             index_info, "bptree");
  if (create_index != DB_SUCCESS) {
    ExecuteInformation(create_index);
    return create_index;
  }

  // 5.create unique index
  for (pSyntaxNode child = columns_node->child_; child != nullptr; child = child->next_) {
    if (child->type_ != kNodeColumnDefinition) {
      continue;
    }
    if (child->val_ != nullptr && std::strcmp(child->val_, "unique") == 0) {
      std::string column_name = child->child_->val_;
      std::vector<std::string> index_keys;
      index_keys.push_back(column_name);
      auto *index_info = IndexInfo::Create();
      dberr_t create_index = context->GetCatalog()->CreateIndex(table_name, column_name + "_unq", index_keys,
                                                                 context->GetTransaction(), index_info, "bptree");
      if (create_index != DB_SUCCESS) {
        ExecuteInformation(create_index);
        return create_index;
      }
    }
  }

  cout << "Table created successfully" << endl;
  return DB_SUCCESS;
}

/**
digraph G{ 
SYNTAX_NODE_1[label="kNodeDropTable(1,18)\nid(1)"];
SYNTAX_NODE_0[label="kNodeIdentifier(1,18)\nid(0),val(account)"];
SYNTAX_NODE_1  ->  SYNTAX_NODE_0;
}

*/
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropTable" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  std::string table_name = ast->child_->val_;
  DBStorageEngine *db = dbs_[current_db_];
  dberr_t drop_table = db->catalog_mgr_->DropTable(table_name);
  // 删除表后显示当前数据库的表
  vector<TableInfo *> tables;
  dbs_[current_db_]->catalog_mgr_->GetTables(tables);
  for (auto table : tables) {
    cout << table->GetTableName() << endl;
  }
  return drop_table;
}

/*
digraph G{ 
SYNTAX_NODE_0[label="kNodeShowIndexes(1,12)\nid(0)"];
}
*/
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowIndexes" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  auto db = dbs_[current_db_];
  std::vector<TableInfo *> tables;
  //调用getcatalog获取表和索引信息
  dberr_t get_tables = db->catalog_mgr_->GetTables(tables);
  if (get_tables != DB_SUCCESS) {
    cout << "Failed to get tables" << endl;
    return DB_FAILED;
  }
  //要在终端显示一个表格
  std::vector<std::string> header;
  header.push_back("Table_name");
  header.push_back("Index_name");
  std::vector<std::vector<std::string>> result;
  size_t table_name_width = header[0].length();
  size_t index_name_width = header[1].length();

  for (auto table : tables) {
    std::string table_name = table->GetTableName();
    std::vector<IndexInfo *> indexes;
    dberr_t get_index = db->catalog_mgr_->GetTableIndexes(table_name, indexes);
    if (get_index != DB_SUCCESS) {
      return get_index;
    }
    for (auto index : indexes) {
      std::string index_name = index->GetIndexName();
      std::vector<std::string> row;
      table_name_width = std::max(table_name_width, table_name.length());
      index_name_width = std::max(index_name_width, index_name.length());
      row.push_back(table_name);
      row.push_back(index_name);
      result.push_back(row);
    }
  }
  size_t total_width = table_name_width + index_name_width + 5;
  cout << "+" << setfill('-') << setw(total_width) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(table_name_width) << header[0] << " | " << std::left
       << setfill(' ') << setw(index_name_width) << header[1] << " |" << endl;
  cout << "+" << setfill('-') << setw(total_width) << ""
       << "+" << endl;
  for (auto row : result) {
    cout << "| " << std::left << setfill(' ') << setw(table_name_width) << row[0] << " | " << std::left
         << setfill(' ') << setw(index_name_width) << row[1] << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(total_width) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

/**
digraph G{ 
SYNTAX_NODE_3[label="kNodeCreateIndex(1,39)\nid(3)"];
SYNTAX_NODE_0[label="kNodeIdentifier(1,18)\nid(0),val(idx01)"];
SYNTAX_NODE_1[label="kNodeIdentifier(1,29)\nid(1),val(account)"];
SYNTAX_NODE_4[label="kNodeColumnList(1,39)\nid(4),val(index keys)"];
SYNTAX_NODE_2[label="kNodeIdentifier(1,37)\nid(2),val(balance)"];
SYNTAX_NODE_4  ->  SYNTAX_NODE_2;
SYNTAX_NODE_1  ->  SYNTAX_NODE_4;
{rank=same; SYNTAX_NODE_1,SYNTAX_NODE_4};
SYNTAX_NODE_0  ->  SYNTAX_NODE_1;
{rank=same; SYNTAX_NODE_0,SYNTAX_NODE_1};
SYNTAX_NODE_3  ->  SYNTAX_NODE_0;
}
*/
dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateIndex" << std::endl;
#endif
  std::string index_name(ast->child_->val_);
  std::string table_name(ast->child_->next_->val_);
  TableInfo *table_info;
  dberr_t get_table = context->GetCatalog()->GetTable(table_name, table_info);
  if (get_table != DB_SUCCESS) {
    return get_table;
  }
  pSyntaxNode index_keys_node = ast->child_->next_->next_;
  //默认为bptree索引
  pSyntaxNode index_type_node = index_keys_node->next_;
  std::vector<std::string> index_keys;
  std::vector<uint32_t> index_key_idxs;
  for (auto child = index_keys_node->child_; child != nullptr; child = child->next_) {
    uint32_t index;
    index_keys.emplace_back(child->val_);
    table_info->GetSchema()->GetColumnIndex(child->val_, index);
    index_key_idxs.push_back(index);
  }
  IndexInfo *index_info = IndexInfo::Create();
  std::string index_type = "bptree";
  if (index_type_node != nullptr) {
    index_type = index_type_node->val_;
  }
  dberr_t create_index = context->GetCatalog()->CreateIndex(table_name, index_name, index_keys,
                                                             context->GetTransaction(), index_info, index_type);
  if (create_index != DB_SUCCESS) {
    ExecuteInformation(create_index);
    return create_index;
  }
  //回填索引数据
  TableHeap *table_heap = table_info->GetTableHeap();
  Index *index = index_info->GetIndex();
  for (auto it = table_heap->Begin(context->GetTransaction()); it != table_heap->End(); it++) {
    RowId row_id = it->GetRowId();
    Row row(row_id);
    bool get_tuple = table_heap->GetTuple(&row, context->GetTransaction());
    if (!get_tuple) {
      continue;
    }
    std::vector<Field> index_key_fields;
    for (auto key_idx : index_key_idxs) {
      Field *field = row.GetField(key_idx);
      index_key_fields.push_back(*field);
    }
    Row index_key(index_key_fields);
    index->InsertEntry(index_key, row_id, context->GetTransaction());
  }
  return create_index;
}

/**
digraph G{ 
SYNTAX_NODE_1[label="kNodeDropIndex(1,16)\nid(1)"];
SYNTAX_NODE_0[label="kNodeIdentifier(1,16)\nid(0),val(idx01)"];
SYNTAX_NODE_1  ->  SYNTAX_NODE_0;
}
*/
dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropIndex" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  if (ast == nullptr || ast->child_ == nullptr) {
    return DB_FAILED;
  }

  std::string index_name = ast->child_->val_;
  std::string table_name;

  if (ast->child_->next_ != nullptr) {
    table_name = ast->child_->next_->val_;
  } else {
    std::vector<TableInfo *> tables;
    dberr_t get_tables = context->GetCatalog()->GetTables(tables);
    if (get_tables != DB_SUCCESS) {
      return get_tables;
    }

    int matched_count = 0;
    for (auto *table : tables) {
      if (table == nullptr)
        continue;

      std::vector<IndexInfo *> indexes;
      dberr_t get_indexes = context->GetCatalog()->GetTableIndexes(table->GetTableName(), indexes);
      if (get_indexes != DB_SUCCESS) {
        continue;
      }
      for (auto *index : indexes) {
        if (index == nullptr)
          continue;

        if (index->GetIndexName() == index_name) {
          table_name = table->GetTableName();
          matched_count++;
        }
      }
    }

    if (matched_count == 0) {
      return DB_INDEX_NOT_FOUND;
    }
    if (matched_count > 1) {
      cout << "Ambiguous index name, please specify table name." << endl;
      return DB_FAILED;
    }
  }

  // 执行最终的删除
  dberr_t drop_index = context->GetCatalog()->DropIndex(table_name, index_name);
  return drop_index;
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxBegin" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxCommit" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxRollback" << std::endl;
#endif
  return DB_FAILED;
}

/**
digraph G{ 
SYNTAX_NODE_1[label="kNodeExecFile(1,43)\nid(1)"];
SYNTAX_NODE_0[label="kNodeString(1,43)\nid(0),val(./test/examples/insert_tuple.sql)"];
SYNTAX_NODE_1  ->  SYNTAX_NODE_0;
}
*/
dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteExecfile" << std::endl;
#endif
  std::string file_name = ast->child_->val_;//对应文件名
  FILE *file = fopen(file_name.c_str(), "r");
  if (file == nullptr) {
    cout << "Failed to open file \"" << file_name << "\"" << endl;
    return DB_FAILED;
  }

  auto overall_start_time = std::chrono::system_clock::now();
  size_t commands_executed = 0;
  size_t commands_scceeded = 0;
  bool all_success = true;

  char input[1024];//缓冲区为1024char
  while (!feof(file)) {
    memset(input, 0, 1024);
    int i = 0;
    char ch;
    while (!feof(file)) {
      ch = getc(file);
      if (ch == EOF || ch == ';')
        break;
      input[i++] = ch;
      if (i >= 1023) {
        LOG(WARNING) << "Command too long, truncating.";
        break;
      }
    }
    if (i == 0 && ch == EOF)
      continue;
    if (ch == ';')
      input[i] = ch;

    std::string command_str(input);
    if (command_str.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
      if (feof(file))
        break;
      continue;
    }

    bool success = true;

    YY_BUFFER_STATE bp = yy_scan_string(input);
    if (bp == nullptr) {
      LOG(ERROR) << "Failed to create yy buffer state." << std::endl;
      success = false;
      continue;
    }
    yy_switch_to_buffer(bp);

    MinisqlParserInit();
    yyparse();

    if (MinisqlParserGetError()) {
      printf("SQL Parse Error: %s\n", MinisqlParserGetErrorMessage());
      success = false;
      continue;
    }

    pSyntaxNode root_node = MinisqlGetParserRootNode();
    if (root_node == nullptr) {
      continue;
    }

    unique_ptr<ExecuteContext> statement_context(nullptr);
    if (!current_db_.empty())
      statement_context = dbs_[current_db_]->MakeExecuteContext(nullptr);

    switch (root_node->type_) {
      case kNodeCreateDB:
        success = ExecuteCreateDatabase(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeDropDB:
        success = ExecuteDropDatabase(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeShowDB:
        success = ExecuteShowDatabases(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeUseDB:
        success = ExecuteUseDatabase(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeShowTables:
        success = ExecuteShowTables(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeCreateTable:
        success = ExecuteCreateTable(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeDropTable:
        success = ExecuteDropTable(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeShowIndexes:
        success = ExecuteShowIndexes(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeCreateIndex:
        success = ExecuteCreateIndex(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeDropIndex:
        success = ExecuteDropIndex(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeTrxBegin:
        success = ExecuteTrxBegin(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeTrxCommit:
        success = ExecuteTrxCommit(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeTrxRollback:
        success = ExecuteTrxRollback(root_node, statement_context.get()) == DB_SUCCESS;
        break;
      case kNodeQuit:
        success = ExecuteQuit(root_node, statement_context.get()) == DB_QUIT;
        break;
      default: {
        Planner planner(statement_context.get());
        try {
          std::vector<Row> result_set{};
          planner.PlanQuery(root_node);
          // Execute DML query plans generated by planner.
          success = ExecutePlan(planner.plan_, &result_set, nullptr, statement_context.get()) == DB_SUCCESS;
        } catch (const exception &ex) {
          std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
          success = false;
        }
        break;
      }
    }

    if (!success) {
      continue;
    }

    commands_executed++;
    all_success = all_success && success;
    if (success) {
      commands_scceeded++;
    }

    MinisqlParserFinish();
    yy_delete_buffer(bp);
    yylex_destroy();
    if (feof(file))
      break;
  }
  fclose(file);

  auto overall_stop_time = std::chrono::system_clock::now();
  double overall_duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(overall_stop_time - overall_start_time)).count());

  std::cout << "Total execution for file: " << file_name << ", " << commands_scceeded << "/" << commands_executed
            << " commands succeeded"
            << " (" << std::fixed << std::setprecision(4) << overall_duration_time / 1000 << " sec)" << endl;

  return all_success ? DB_SUCCESS : DB_FAILED;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteQuit" << std::endl;
#endif
  // write all pages to disk
  return DB_QUIT;
}