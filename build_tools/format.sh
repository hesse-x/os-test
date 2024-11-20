#!/bin/bash

PROJECT=os-test

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

TARGET_DIR="$SCRIPT_DIR/../$PROJECT"

if [ ! -d "$TARGET_DIR" ]; then
  echo "Target file $TARGET_DIR not exist"
  exit 1
fi

find "$TARGET_DIR" -type f \( -name "*.cc" -o -name "*.h" -o -name "*.c" -o -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i {} +

echo "clang-format finish"
