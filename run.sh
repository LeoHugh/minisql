#!/bin/bash

# 测试列表（每行一个测试）：
TEST_SUITES=(
  'DiskManagerTest.*'
  'LRUReplacerTest.*'
  'BufferPoolManagerTest.*'
  'TupleTest.*'
  'TableHeapTest.*'
  'BPlusTreeTests.*'
  'RecoveryManagerTest.*'
  'CatalogTest.*'
  'LockManagerTest.*'
  'ExecutorTest.*'
  'PageTests.*'
)

# 将数组元素用冒号连接起来
OLD_IFS="$IFS" # 保存当前的 IFS
IFS=":"
TEST_LIST="${TEST_SUITES[*]}"
IFS="$OLD_IFS" # 恢复 IFS

# 检查.test/minisql_test是否存在
if [ ! -f "./test/minisql_test" ]; then
    echo "test/minisql_test不存在，请先构建"
    echo "note: you should run 'make -j' in 'build' directory, then run this script '../run_tests.sh'"
    exit 2
fi

# 运行测试
./test/minisql_test --gtest_filter="$TEST_LIST"
