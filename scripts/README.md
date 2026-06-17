# 测试脚本

## generate_test

在项目根目录下运行 generate_test，生成测试文件。

```bash
chmod +x ./scripts/generate_test.sh
./scripts/generate_test.sh
```

## test_sql

`test_sql.sh` 用于运行单个或多个测试。

```bash
chmod +x ./scripts/test_sql.sh
# 直接运行测试
./scripts/test_sql.sh foo [test_names]
# 如
./scripts/test_sql.sh foo create_database create_table insert_tuple
./scripts/test_sql.sh -b create_database create_table insert_tuple
# 如果需要重新 build，将 foo 替换成 -b
```

所有的 test_name 为：

* create_database
* create_table
* insert_tuple
* inquiry
* multi_condition_inquiry_and_project
* unique_constraint
* index
* update
* delete

## test_sql_full

运行全部测试，并输出结果。

```bash
chmod +x ./scripts/test_sql_full.sh
./scripts/test_sql_full.sh
```

## 注意事项

* 请保证数据库文件存放目录为 `db/`，即在 `instance.cpp` 中的 `DBStorageEngine()` 中添加：

```cpp
db_file_name_ = "./db/" + db_file_name_;
```

* 请保证 `execute_engine.cpp` 中的 `ExecuteExecfile` 函数末尾有

```cpp
std::cout << "Execution completed in " << duration_in_seconds << " sec." << std::endl;
```

参考计时逻辑为

```cpp
  const auto start_time = std::chrono::high_resolution_clock::now();
  // 记录结束时间
  const auto end_time = std::chrono::high_resolution_clock::now();

  // 计算耗时
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
  const double duration_in_seconds = duration / 1000.0;  // 转化为秒
```

* 如果未支持语法

```sql
insert into account values() (0, "name00000", 0),
(1, "name00001", 1);
```

则请在 `test_sql_full` 中选择 grep "Execution"

```bash
echo -e "${purple}[insert_tuple time]${into_end}"
grep --color=auto "sec" "${log_files["insert_tuple"]}"
echo

# echo -e "${purple}[insert_tuple time]${into_end}"
# grep --color=auto "Execution" "${log_files["insert_tuple"]}"
# echo
```

并请在 `generate_test.sh` 中选择下面一种 insert 语句(移上去，不然因为EOF当字符了)

```bash
EOF
    local batch_size=10000
    local total_records=100000

    for ((batch = 0; batch < total_records / batch_size; batch++)); do
        {
            echo "insert into account values"
            for ((i = batch * batch_size; i < (batch + 1) * batch_size; i++)); do
                if [ "$i" -lt $(((batch + 1) * batch_size - 1)) ]; then
                    echo "($i, \"name$(printf "%05d" "$i")\", $i),"
                else
                    echo "($i, \"name$(printf "%05d" "$i")\", $i);"
                fi
            done
        } >>"$file_path"
    done
# EOF
#     local total_records=100000

#     for ((i = 0; i < total_records; i++)); do
#         echo "insert into account values ($i, \"name$(printf "%05d" "$i")\", $i);" >>"$file_path"
#     done
```

* 日志文件保存到了 `logs/`

* 测试文件保存到了 `test/examples/`
