
# DISK AND BUFFER POOL MANAGER
## 数据结构
在disk上，存储了三种数据。
第一个，是metadata，格式为| num_allocated_pages_ | num_extents_ | extent_used_page_[0]... |
第二个是每个extent的bitmap page ，格式为:| page_allocated | next_free_page | bytes[PAGESIZE-2*4] |
第三个是具体的datapage，其为基类，后续会根据存储的内容分为不同的数据页
## disk manager
其负责page级别的i/o读写，因此具有了一些职责:
找一个空闲页，分配一个新的page，删除page，判断page是否为空。
注意的是，这里只负责分配删除的逻辑，是page-level的，不会涉及具pagedata.
这样，对其上层，只需要传一个logicId即可，不需要关心物理地址。
## buffer pool
其核心，是维护了一个内存池。
任何一个page进入内存后，都需要标记page_id,pin_count,is_dirty

* pin_count代表了有多少线程在使用当前page，当pin_count>0时，不会被删除或者置换

* is_dirty是page的脏页逻辑。我们在修改page后，不会立即io写入，而是标记为dirty，当发生flushpage或者从内存池驱逐或者整个内存池发生析构时，写入disk。
* FetchPage：不在内存池要从disk取，然后找到对应的位置，或者驱逐
* deletepage，删除page只需要在bitmap中标记不可用即可。另外就是，因为不会再用，可以直接从replacer中驱逐。
* 关于lru，采用了一个双向链表和hash来加快查找速度
* bitmap查找的优化上，采用了next_free_page_。其值是上一次释放后的最小值。
* 注意logicId与physicalId间的map
* diskmanager的扩容机制:先更新metedata，再新建一个bitmap，标记第一个为1，返回对应的logic_page_id
![alt text](Lab1.png)

* 此外，如果出现缓存池满了且全部pin住的情况，bufferpool的NewPage和FetchPage返回的是nullptr。所以上层要先检验，然后只能abort.


# RECORD MANAGER
record manager是与index manager同一个层次的组件。两者通过rowId相互关联。
这里提供row-level的操作.
## table_page
我们此处对page里的data_[4096]使用的是table_page,
也就是slotted_page的形式，其结构如下
* Page Header（页头）：固定大小（24字节），存储了此页的 PageId、上/下一页的指针（构成双链表）、TupleCount（元组数量）和 FreeSpacePointer（空闲空间指针）。
* Slot Array（槽位数组）：紧跟页头。每存入一条记录，就会在这里追加 8 个字节的元信息（Tuple Offset 和 Tuple Size），告诉我们这条数据存放在页面的哪个位置、有多长。它的增长方向是从前往后。
* Tuple Data（真实的元组数据）：存放 Row 序列化后的二进制字节流。为了避免内存碎片，它从 4096 字节的尾部开始，由后往前分配。FreeSpacePointer 始终指向最新的（最靠前的）元组数据的起始位置。

## 如何把table写入disk page
Row，有数组，采用bitmap来节省空间；此外，需要传递schema来确认每个数据的type
magic魔法数检验

## 堆表的tableheap
tableheap是一个page组成的双向链表

* 优化查询，加了一个freeList,
进入条件为:new page（只插入一个tuple），applydelete；
移除:插入失败

inserttuple：先走freelist--->当前page链表---->new page

* 删除逻辑。三个delete函数是方便事务回滚。

markdelete：标记删除,只在对应slot最高位上标记一个1即可  
applydelete:实际删除 ,需要移动数据来填充空洞
rollbackdelete:回滚删除，对于标记删除是方便回滚的。

* updatetuple:先原地更新，空间不够再insert更新

* deletetable:递归删除列表，有栈溢出的风险


封装了page层的细节，sql执行器不需要关系具体到哪个page
## 堆表的iterator迭代
一个row* 一个tableheap*
![alt text](Lab2.png)

# INDEX MANAGER ！！！
## pege 
这一层是不负责处理业务逻辑的，只有物理数据迁移
基础类 b_plus_tree_page
定义了Header
 ----------------------------------------------------------------------------
 | PageType (4) | KeySize (4) | LSN (4) | CurrentSize (4) | MaxSize (4) |
 ----------------------------------------------------------------------------
| ParentPageId (4) | PageId(4) |
 ----------------------------------------------------------------------------

BPlusTreeInternalPage
注意第一个key为invalid，只是为了维护键值对的形式.
 --------------------------------------------------------------------------
| HEADER (28B) | KEY(1)+PAGE_ID(1) | KEY(2)+PAGE_ID(2) | ... | KEY(n)+PAGE_ID(n) |
 --------------------------------------------------------------------------
需要实现的功能:
增:PopulateNewRoot()新增一个根节点   InsertNodeAfter()
删：remove
查：二分查找，左闭右开，Lookup
分裂，合并:MoveHalfTo()，旧的大小是minsize  MoveAllTo()

注意到，这里a采用了双层管理机制
情况 A（当前页被修改）： InternalPage 只管改内存。由上一级的调用者（BPlusTree）负责在用完当前页后，统一调用 UnpinPage(current_page, true) 把它标记为脏页。
情况 B（连带子页面被修改）： InternalPage 在执行高级转移操作时，连带影响了不在上层掌控中的“子页面”。此时，InternalPage 必须自己充当临时调度员，借用 buffer_pool_manager 把子页面 Fetch 上来，改完后，立刻自己把子页面 Unpin 并标记为脏页。


BPlusTreeLeafPage
 -----------------------------------------------------------------------
| HEADER (28B) | next_page_id (4B) | KEY(1)+RID(1) | KEY(2)+RID(2) | ...
 -----------------------------------------------------------------------
相比于内部节点，其维护的value是rowId.
此外，键是没有冗余的
也不需要bufferpage参与来管理子节点的继承问题
同时，节点间维护了一个单向链表

初始化时，为什么使用init而不采用默认构造函数呢



## b_Plus_tree 增删改查操作
有一颗INDEX_ROOTS_PAGE_ID，专门负责存储所有b+tree
初始化:从indexrootpage里面找
find:findpage+page里面找row
add:
```mermaid
flowchart TD
    A["Insert(key, value)"] --> B{树为空?}
    B -->|是| C["StartNewTree()：创建叶子根节点"]
    B -->|否| D["InsertIntoLeaf()"]
    D --> E["FindLeafPage() 定位叶子"]
    E --> F["leaf->Insert()"]
    F --> G{叶子溢出?}
    G -->|否| H[返回成功]
    G -->|是| I["Split(leaf)：分裂叶子"]
    I --> J["维护 next_page_id 链"]
    J --> K["InsertIntoParent()"]
    K --> L{old_node 是根?}
    L -->|是| M["创建新根节点"]
    L -->|否| N["parent->InsertNodeAfter()"]
    N --> O{父节点溢出?}
    O -->|否| P[结束]
    O -->|是| Q["Split(parent) → 递归 InsertIntoParent()"]
```

delete： 
```mermaid
flowchart TD
    A["Remove(key)"] --> B["FindLeafPage() 定位叶子"]
    B --> C["leaf->RemoveAndDeleteRecord()"]
    C --> D{下溢?}
    D -->|否| E[结束]
    D -->|是| F["CoalesceOrRedistribute()"]
    
    F --> G{是根节点?}
    
    G -->|是| H["AdjustRoot()"]
    H --> H1{根节点类型?}
    H1 -->|叶子根| H2["size == 0 → 树变空<br/>size > 0 → 正常"]
    H1 -->|内部根| H3["size == 1 → 孩子上升为新根<br/>size > 1 → 正常"]
    H2 --> J1[返回 true/false]
    H3 --> J1
    
    G -->|否| K{邻居 + 自身 > max_size?}
    K -->|是| L["Redistribute()<br/>从邻居借一个"]
    L --> J1
    
    K -->|否| M["Coalesce()<br/>与邻居合并"]
    M --> N["从父节点删除键"]
    N --> O["删除当前节点页"]
    O --> P["递归处理父节点<br/>CoalesceOrRedistribute(parent)"]
    P --> J1
```

## 迭代器




# CATALOG MANAGER 目录管理器
## catalog.h
catalogmanager是对外暴露的统一接口，其职责是在内存中维护所有表和索引的映射，并处理相关的ddl请求
可以冷启动也可以热启动

* 内存层 维护了多张hash maps
table_names-->table_id-->table_info
indexid-->indexInfo
持有了bufferpoolmanager,可以与disk交互

* 也对上层executor提供了对应的createtable,droptable,createindex
catalogManager：注意区分第一次初始化和从disk加载meta数据的两种case
createtable，这里的tableinfo是输出参数

* 元数据catalogmetadata:记录每个tablemeta||indexmetaId到哪个page id

## table.h
* 元数据：tablemetadata.需要序列化和反序列化
* 运行时内存中数据:TableInfo=&metadata+&tableheap.
## index.h
* 元数据:indexmetadata.同理需要序列化和反序列化
* 运行时数据：indexinfo=meta+schema+indexbptree.注意，这里的列shema是浅拷贝，只读

# PLANNER AND EXECUTOR
parser and planner已经被执行好了
换言之，这里已经有一个好的planner tree了
走dml的路由也已经实现好了
所以只需要实现一些ddl(data definition langguage)的路由
此处就介绍下/executor/execute_engine.cpp文件
可在https://dreampuf.github.io/实现可视化

ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context)
首先是从语法树中拿到tablename,以及找到主键(只允许1),以及列的数据类型以及属性，然后根据属性构建table
再构建主键以及unique index的bptree

删除表则直接得到即可
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) 

注意可视化
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) 

dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) 

dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) 
dropindex支持无表名与有表名两种case

dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) 
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) 



目前没有针对ddl指令的test，然后感觉这部分代码写的有点烂，考虑后续重写，代码量不大



# recover module
在目前的版本代码中，主要做了两件事。
## 写入日志
第一件事，是在内存层次上写入日志。
对于每个记录，我们有以下操作
``` c++
enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};
```
这是我们记录的结构体。需要注意的是，prev_lsn_map_与next_lsn_是类级别静态变量。我们需要维护每个事务的最新lsn.
``` c++
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};

    KeyType old_key_{};
    ValType old_val_{0};
    KeyType new_key_{};
    ValType new_val_{0};

    inline static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_ = {};//维护每个事务i最新的lsn
    inline static lsn_t next_lsn_ = 0;
};

```
在具体实现每个操作时，首先要注意的是，我们需要维护每个事务的最新lsn,但我们在commit时没有引入删除事务机制，也就是说不需要为active transaction负责。

此外，我们其实没有在业务层面保证操作的正确性。

这也体现在test中。
<T1 Delete B 2000>
<T1 abort>
即使在最初没有B,最后也会冒出一个B=2000

## 简陋的恢复算法
第二件事，是在测试代码给定checkpoint的基础上，完成analysis,redo,undo的恢复算法。

这里内存中的核心数据是checkpoint和recovermanager
``` c++
struct CheckPoint {
    // 最新checkpoitlsn
    lsn_t checkpoint_lsn_{INVALID_LSN};
    //当前活跃事务
    ATT active_txns_{};
    //全量数据
    KvDatabase persist_data_{};
}
// recovermanager
    //存储所有日志
    std::map<lsn_t, LogRecPtr> log_recs_{};
    //记录持久化的起点
    lsn_t persist_lsn_{INVALID_LSN};
    //维护当前活跃事务表
    ATT active_txns_{};
    //模拟数据库
    KvDatabase data_{};  // all data in database
```
这里，我们没有复用前面的prev_lsn_map_，而是重新在manager中维护了一个active transaction列表，个人认为设计有些冗余。
然后，我们这里维护的是一个所有日志，然后假设checkpoint前的数据都已写入。所以在做redo阶段时，选择从checkpoint开始往后redo.
在做undo时，则先通过active_transaction得到活跃事务的最新lsn,然后按照顺序逐条处理。此处没有写补偿日志。
## 思考题:如何完善恢复算法
为了实现持久化的目标，我们需要把当前的log以及buffer pool的page都实际写入disk中。
为了方便维护，我们在log的内存中记录flashpagelsn,记录最新已经进入kdis持久化的log.同理记录每个dirty page最新一次持久化的lsn为pagelsn
首先，为了实现write-ahead logging的机制，我们需要保证flushpagelsn>=pagelsn恒成立，flushpagelsn>=commitlsn恒成立
### 写入日志阶段
1,因为需要完成record的序列化与反序列化，要重新设计其struct,应该根据不同recordtype来。

2,对于写入的io.可以开两个线程，一个线程的缓冲区专门负责io写disk,另一个线程的缓冲区来存新的record.那么，什么时候触发flush呢?第一，是做transaction commit时，要检查前面的record入盘了，否则强制刷盘;二，是可以定期写入，或者buffer快满了写入；三是endcheckpoint也要flush一下。

3,fuzzy checkpoint。开一个checkpoint线程。可以选择checkpoint策略，比方说1h写一次。先写startcheckpoint日志，记录对应lsn.
然后分别对recovermanager的active_transaction和bufferpool中的dirty_page加锁并快照。然后是endcheckpoint日志，包含快照内容。然后刷盘。确保成功后，将对应的beginlsn写入master record文件。
### 恢复算法阶段
1,analysis阶段.先从masterrecord文件拿到最新的beginlsn,然后去wal中a找到对应的lastlsn,加载活跃事务与脏页表。然后往下扫描，对应的事务添加或者删除，将涉及的新page加入脏页表
2,redo阶段。先从dpt中找到最小的reclsm,然后看 pagelsn<lsn && disklsn<lsn ;然后更新buffer pool的pagelsn和内容
3,undo阶段。类似于原先的算法，然后写入补偿日志。




# 数据库的不足之处
更好的容灾容错，高可用性
单机的，分布式存储，
节点间的通信，
主从复制
分库分表

优化器过于简单，lru策略过于简单





# 关于使用ai的说明感悟
看代码，在写代码前理解每个函数的职责。因为其实注释不是很清晰。
debug时也有帮助，但不能解决所有问题。

不足:
代码冗余，防御性编程问题。
一些bug确实还是不行。打断点，慢慢调。
整体上去做一个架构，bufferpool内存的管理问题，你放这个函数里也行，你放那个函数也行。那究竟在哪里管理呢?调用者负责管理吗?

碰到过的最大的问题，
第一部分脏页的逻辑写错了，然后过了测试
结果后面出了问题
