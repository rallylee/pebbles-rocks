//  Copyright (c) 2013-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run rocksdb tools\n");
  return 1;
}
#else
#include <rocksdb/db_bench_tool.h>
#include <thread>
#include <chrono>
#include <string>

int main(int argc, char** argv) {
    int toRet = rocksdb::db_bench_tool(argc, argv);
    int secToWait = 30;
    if (argc == 2) {
        secToWait = std::stoi(argv[1]);
    }
    std::this_thread::sleep_for(std::chrono::seconds(secToWait));
    return toRet;
}
#endif  // GFLAGS
