#!/bin/bash

file_to_copy="fs"

for dir in tests/test1 tests/test2 tests/test3 tests/test4; do
    cp "$file_to_copy" "$dir"
done