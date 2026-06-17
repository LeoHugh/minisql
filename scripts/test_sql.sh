#!/bin/bash
require_build=$1
shift # 移除第一个参数，剩下的参数作为文件列表

if [ "$require_build" == "-b" ]; then
    cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -S . -B build
    cmake --build build
fi

if [ ! -d "logs" ]; then
    mkdir logs
fi

for test_file in "$@"; do
    exec_command="execfile \"./test/examples/${test_file}.sql\";"
    {
      echo "$exec_command"
      echo "quit;"
    } | ./build/bin/main > "logs/test_$test_file.log" 2>&1
done
