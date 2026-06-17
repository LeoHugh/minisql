#!/bin/bash

green="\033[32m"
red="\033[31m"
yellow="\033[33m"
blue="\033[34m"
purple="\033[35m"
cyan="\033[36m"
white="\033[37m"
into_end="\033[0m"

test_dir="./test/examples"
log_dir="./logs"

if [ ! -d "logs" ]; then
    mkdir logs
fi

declare -A test_files=(
    ["create_database"]="$test_dir/create_database.sql"
    ["create_table"]="$test_dir/create_table.sql"
    ["insert_tuple"]="$test_dir/insert_tuple.sql"
    ["inquiry"]="$test_dir/inquiry.sql"
    ["multi_condition_inquiry_and_project"]="$test_dir/multi_condition_inquiry_and_project.sql"
    ["unique_constraint"]="$test_dir/unique_constraint.sql"
    ["index"]="$test_dir/index.sql"
    ["update"]="$test_dir/update.sql"
    ["delete"]="$test_dir/delete.sql"
)
declare -A log_files=(
    ["create_database"]="$log_dir/create_database.log"
    ["create_table"]="$log_dir/create_table.log"
    ["insert_tuple"]="$log_dir/insert_tuple.log"
    ["inquiry"]="$log_dir/inquiry.log"
    ["multi_condition_inquiry_and_project"]="$log_dir/multi_condition_inquiry_and_project.log"
    ["unique_constraint"]="$log_dir/unique_constraint.log"
    ["index"]="$log_dir/index.log"
    ["update"]="$log_dir/update.log"
    ["delete"]="$log_dir/delete.log"
)

# 按声明顺序的键列表
keys=("create_database" "create_table" "insert_tuple" "inquiry" "multi_condition_inquiry_and_project" "unique_constraint" "index" "update" "delete")

echo -e "${blue}Clear old dbs...${into_end}"
rm -rf ./db/*
echo -e "${green}Old dbs cleared.${into_end}"

# 遍历 keys 数组，确保顺序一致
for key in "${keys[@]}"; do
    test_file="${test_files[$key]}"
    log_file="${log_files[$key]}"
    echo -e "${blue}Running test: $key...${into_end}"
    # 执行测试并将输出重定向到对应的日志文件
    exec_command="execfile \"$test_file\";"
    {
        echo "$exec_command"
        echo "quit;"
    } | ./build/bin/main > "$log_file" 2>&1
    echo -e "${green}Test $key completed. Log saved to $log_file.${into_end}"
done

echo -e "${blue}Extracting log info...${into_end}"

# 提取日志信息
t1=$(awk -v N=2 '/sec/ {count++; if (count == N) { match($0, /[0-9]+\.[0-9]+/); print substr($0, RSTART, RLENGTH) } }' "${log_files["inquiry"]}")
t5=$(awk -v N=4 '/sec/ {count++; if (count == N) { match($0, /[0-9]+\.[0-9]+/); print substr($0, RSTART, RLENGTH) } }' "${log_files["multi_condition_inquiry_and_project"]}")
read t2 t3 t6 t4 <<<$(awk '/sec/ {
    count++;
    if (count == 1) { match($0, /[0-9]+\.[0-9]+/); t2 = substr($0, RSTART, RLENGTH) }
    if (count == 2) { match($0, /[0-9]+\.[0-9]+/); t3 = substr($0, RSTART, RLENGTH) }
    if (count == 3) { match($0, /[0-9]+\.[0-9]+/); t6 = substr($0, RSTART, RLENGTH) }
    if (count == 6) { match($0, /[0-9]+\.[0-9]+/); t4 = substr($0, RSTART, RLENGTH) }
} END { print t2, t3, t6, t4 }' "${log_files["index"]}")
echo

# 输出提取的时间
echo -e "${purple}[index effects]${into_end}"
echo "t1($t1 sec) > t2($t2 sec)"
echo "t3($t3 sec) < t4($t4 sec)"
echo "t5($t5 sec) > t6($t6 sec)"
echo

echo -e "${purple}[insert_tuple time]${into_end}"
grep --color=auto "sec" "${log_files["insert_tuple"]}"
echo

# echo -e "${purple}[insert_tuple time]${into_end}"
# grep --color=auto "Execution" "${log_files["insert_tuple"]}"
# echo

echo -e "${purple}[select result in inquiry]${into_end}"
grep --color=auto "row" "${log_files["inquiry"]}"
echo

echo -e "${purple}[select result in multi_condition_inquiry_and_project]${into_end}"
grep --color=auto "row" "${log_files["multi_condition_inquiry_and_project"]}"