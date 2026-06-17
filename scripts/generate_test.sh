#!/bin/bash
# 生成 .sql 测试文件，数据规模约十万行，并测试索引对查询的影响
delete_old="$1"
test_dir="./test/examples"

# 定义颜色
green="\033[32m"
red="\033[31m"
yellow="\033[33m"
blue="\033[34m"
purple="\033[35m"
cyan="\033[36m"
white="\033[37m"
into_end="\033[0m"

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

delete_old_files() {
    for file in "${test_files[@]}"; do
        if [ -f "$file" ]; then
            rm "$file"
        fi
    done
}

generate_file() {
    local file_path="$1"
    local content_writer="$2"
    if [ ! -f "$file_path" ]; then
        echo -e "${blue}Generate: $file_path${into_end}"
        $content_writer "$file_path"
        truncate -s -1 "$file_path"
        echo -e "${green}Done: $file_path${into_end}"
    fi
}

if [ ! -d "$test_dir" ]; then
    mkdir -p "$test_dir"
fi

if [ "$delete_old" = "-d" ]; then
    delete_old_files
fi

create_database_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
create database db0;
create database db1;
create database db2;
show databases;
EOF
}

create_table_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
create table account(
    id int,
    name char(16) unique,
    balance float,
    primary key(id)
);
EOF
}

insert_tuple_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
EOF
    local batch_size=10000
    local total_records=100000

    for ((batch = 0; batch < total_records / batch_size; batch++)); do
        {
            for ((i = batch * batch_size; i < (batch + 1) * batch_size; i++)); do
                echo "insert into account values ($i, \"name$(printf "%05d" "$i")\", $i);"
            done
        } >>"$file_path"
    done
# EOF
#     local total_records=100000

#     for ((i = 0; i < total_records; i++)); do
#         echo "insert into account values ($i, \"name$(printf "%05d" "$i")\", $i);" >>"$file_path"
#     done
}

# select * from account where balance = 56789; ::t1::
inquiry_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
select * from account where id = 50000;
select * from account where balance = 56789;
select * from account where name = "name56789";
select * from account where id <> 50000;
select * from account where balance <> 50000;
select * from account where name <> "name56789";
EOF
}

# select * from account where id < 12500200 and balance < 100; ::t5::
multi_condition_inquiry_and_project_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
select id, name from account where balance >= 20000 and balance < 70000;
select name, balance from account where balance > 19999 and id <= 69999;
select * from account where id < 12515000 and name > "name14500";
select * from account where id < 12500200 and balance < 100;
EOF
}

unique_constraint_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
insert into account values(1, "name00002", 3);
insert into account values(100000, "name99999", 100000);
EOF
}
# select * from account where balance = 56789; ::t2::
# select * from account where balance = 45678; ::t3::
# select * from account where id < 12515000 and balance < 100; ::t6::
# select * from account where balance = 45678; ::t4::
index_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
create index idx01 on account(balance);
show indexes;
select * from account where balance = 56789;
select * from account where balance = 45678;
select * from account where id < 12515000 and balance < 100;
delete from account where name = "name45678";
insert into account values(45678, "name45678", 45678);
drop index idx01;
show indexes;
select * from account where balance = 45678;
EOF
}

update_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
update account set balance = 100000 where name = "name56789";
select * from account where name = "name56789";
EOF
}

delete_writer() {
    local file_path="$1"
    cat >"$file_path" <<EOF
use db0;
delete from account where balance = 100000;
select * from account where balance = 100000;
delete from account;
select * from account;
drop table account;
show tables;
EOF
}

generate_file "${test_files["create_database"]}" create_database_writer

generate_file "${test_files["create_table"]}" create_table_writer

generate_file "${test_files["insert_tuple"]}" insert_tuple_writer

generate_file "${test_files["inquiry"]}" inquiry_writer

generate_file "${test_files["multi_condition_inquiry_and_project"]}" multi_condition_inquiry_and_project_writer

generate_file "${test_files["unique_constraint"]}" unique_constraint_writer

generate_file "${test_files["index"]}" index_writer

generate_file "${test_files["update"]}" update_writer

generate_file "${test_files["delete"]}" delete_writer

echo -e "${green}All test files generated successfully!${into_end}"

# t2 < t1 (0.0000 < 0.5240)
# t3 < t4 (0.0000 < 0.5420)
# t5 > t6 (0.7400 > 0.0540)


