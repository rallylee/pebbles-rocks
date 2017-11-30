//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_builder.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/internal_stats.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "port/port.h"
#include "table/table_reader.h"

namespace rocksdb {

bool NewestFirstBySeqNo(FileMetaData* a, FileMetaData* b) {
  if (a->largest_seqno != b->largest_seqno) {
    return a->largest_seqno > b->largest_seqno;
  }
  if (a->smallest_seqno != b->smallest_seqno) {
    return a->smallest_seqno > b->smallest_seqno;
  }
  // Break ties by file number
  return a->fd.GetNumber() > b->fd.GetNumber();
}

namespace {
bool BySmallestKey(FileMetaData* a, FileMetaData* b,
                   const InternalKeyComparator* cmp) {
  int r = cmp->Compare(a->smallest, b->smallest);
  if (r != 0) {
    return (r < 0);
  }
  // Break ties by file number
  return (a->fd.GetNumber() < b->fd.GetNumber());
}
}  // namespace

class VersionBuilder::Rep {
 private:
  // Helper to sort files_ in v
  // kLevel0 -- NewestFirstBySeqNo
  // kLevelNon0 -- BySmallestKey
  struct FileComparator {
    enum SortMethod { kLevel0 = 0, kLevelNon0 = 1, } sort_method;
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      switch (sort_method) {
        case kLevel0:
          return NewestFirstBySeqNo(f1, f2);
        case kLevelNon0:
          return BySmallestKey(f1, f2, internal_comparator);
      }
      assert(false);
      return false;
    }
  };

  struct LevelState {
    std::unordered_set<uint64_t> deleted_files;
    // Map from file number to file meta data.
    std::unordered_map<uint64_t, FileMetaData*> added_files;
  };

  const EnvOptions& env_options_;
  Logger* info_log_;
  TableCache* table_cache_;
  VersionStorageInfo* base_vstorage_;
  int num_levels_;
  LevelState* levels_;
  // Store states of levels larger than num_levels_. We do this instead of
  // storing them in levels_ to avoid regression in case there are no files
  // on invalid levels. The version is not consistent if in the end the files
  // on invalid levels don't cancel out.
  std::map<int, std::unordered_set<uint64_t>> invalid_levels_;
  // Whether there are invalid new files or invalid deletion on levels larger
  // than num_levels_.
  bool has_invalid_levels_;
  FileComparator level_zero_cmp_;
  FileComparator level_nonzero_cmp_;
  std::vector<GuardMetaData> new_guards_;
  std::vector<GuardMetaData> complete_guards_;

 public:
  Rep(const EnvOptions& env_options, Logger* info_log, TableCache* table_cache,
      VersionStorageInfo* base_vstorage)
      : env_options_(env_options),
        info_log_(info_log),
        table_cache_(table_cache),
        base_vstorage_(base_vstorage),
        num_levels_(base_vstorage->num_levels()),
        has_invalid_levels_(false) {
    levels_ = new LevelState[num_levels_];
    level_zero_cmp_.sort_method = FileComparator::kLevel0;
    level_nonzero_cmp_.sort_method = FileComparator::kLevelNon0;
    level_nonzero_cmp_.internal_comparator =
        base_vstorage_->InternalComparator();
  }

  ~Rep() {
    for (int level = 0; level < num_levels_; level++) {
      const auto& added = levels_[level].added_files;
      for (auto& pair : added) {
        UnrefFile(pair.second);
      }
    }

    delete[] levels_;
  }

  void UnrefFile(FileMetaData* f) {
    f->refs--;
    if (f->refs <= 0) {
      if (f->table_reader_handle) {
        assert(table_cache_ != nullptr);
        table_cache_->ReleaseHandle(f->table_reader_handle);
        f->table_reader_handle = nullptr;
      }
      delete f;
    }
  }

  void CheckConsistency(VersionStorageInfo* vstorage) {
#ifdef NDEBUG
    if (!vstorage->force_consistency_checks()) {
      // Dont run consistency checks in release mode except if
      // explicitly asked to
      return;
    }
#endif
    // make sure the files are sorted correctly
    for (int level = 0; level < num_levels_; level++) {
      auto& level_files = vstorage->LevelFiles(level);
      for (size_t i = 1; i < level_files.size(); i++) {
        auto f1 = level_files[i - 1];
        auto f2 = level_files[i];
        if (level == 0) {
          if (!level_zero_cmp_(f1, f2)) {
            fprintf(stderr, "L0 files are not sorted properly");
            abort();
          }

          if (f2->smallest_seqno == f2->largest_seqno) {
            // This is an external file that we ingested
            SequenceNumber external_file_seqno = f2->smallest_seqno;
            if (!(external_file_seqno < f1->largest_seqno ||
                  external_file_seqno == 0)) {
              fprintf(stderr, "L0 file with seqno %" PRIu64 " %" PRIu64
                              " vs. file with global_seqno %" PRIu64 "\n",
                      f1->smallest_seqno, f1->largest_seqno,
                      external_file_seqno);
              abort();
            }
          } else if (f1->smallest_seqno <= f2->smallest_seqno) {
            fprintf(stderr, "L0 files seqno %" PRIu64 " %" PRIu64
                            " vs. %" PRIu64 " %" PRIu64 "\n",
                    f1->smallest_seqno, f1->largest_seqno, f2->smallest_seqno,
                    f2->largest_seqno);
            abort();
          }
        } else {
          if (!level_nonzero_cmp_(f1, f2)) {
            fprintf(stderr, "L%d files are not sorted properly", level);
            abort();
          }

          // Make sure there is no overlap in levels > 0
          if (vstorage->InternalComparator()->Compare(f1->largest,
                                                      f2->smallest) >= 0) {
            fprintf(stderr, "L%d have overlapping ranges %s vs. %s\n", level,
                    (f1->largest).DebugString(true).c_str(),
                    (f2->smallest).DebugString(true).c_str());
            abort();
          }
        }
      }
    }
  }

  void CheckConsistencyForDeletes(VersionEdit* edit, uint64_t number,
                                  int level) {
#ifdef NDEBUG
    if (!base_vstorage_->force_consistency_checks()) {
      // Dont run consistency checks in release mode except if
      // explicitly asked to
      return;
    }
#endif
    // a file to be deleted better exist in the previous version
    bool found = false;
    for (int l = 0; !found && l < num_levels_; l++) {
      const std::vector<FileMetaData*>& base_files =
          base_vstorage_->LevelFiles(l);
      for (size_t i = 0; i < base_files.size(); i++) {
        FileMetaData* f = base_files[i];
        if (f->fd.GetNumber() == number) {
          found = true;
          break;
        }
      }
    }
    // if the file did not exist in the previous version, then it
    // is possibly moved from lower level to higher level in current
    // version
    for (int l = level + 1; !found && l < num_levels_; l++) {
      auto& level_added = levels_[l].added_files;
      auto got = level_added.find(number);
      if (got != level_added.end()) {
        found = true;
        break;
      }
    }

    // maybe this file was added in a previous edit that was Applied
    if (!found) {
      auto& level_added = levels_[level].added_files;
      auto got = level_added.find(number);
      if (got != level_added.end()) {
        found = true;
      }
    }
    if (!found) {
      fprintf(stderr, "not found %" PRIu64 "\n", number);
      abort();
    }
  }

  bool CheckConsistencyForNumLevels() {
    // Make sure there are no files on or beyond num_levels().
    if (has_invalid_levels_) {
      return false;
    }
    for (auto& level : invalid_levels_) {
      if (level.second.size() > 0) {
        return false;
      }
    }
    return true;
  }

  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    CheckConsistency(base_vstorage_);

    // Delete files
    const VersionEdit::DeletedFileSet& del = edit->GetDeletedFiles();
    for (const auto& del_file : del) {
      const auto level = del_file.first;
      const auto number = del_file.second;
      if (level < num_levels_) {
        levels_[level].deleted_files.insert(number);
        CheckConsistencyForDeletes(edit, number, level);

        auto exising = levels_[level].added_files.find(number);
        if (exising != levels_[level].added_files.end()) {
          UnrefFile(exising->second);
          levels_[level].added_files.erase(number);
        }
      } else {
        if (invalid_levels_[level].count(number) > 0) {
          invalid_levels_[level].erase(number);
        } else {
          // Deleting an non-existing file on invalid level.
          has_invalid_levels_ = true;
        }
      }
    }

    // Add new files
    for (const auto& new_file : edit->GetNewFiles()) {
      const int level = new_file.first;
      if (level < num_levels_) {
        FileMetaData* f = new FileMetaData(new_file.second);
        f->refs = 1;

        assert(levels_[level].added_files.find(f->fd.GetNumber()) ==
               levels_[level].added_files.end());
        levels_[level].deleted_files.erase(f->fd.GetNumber());
        levels_[level].added_files[f->fd.GetNumber()] = f;
      } else {
        uint64_t number = new_file.second.fd.GetNumber();
        if (invalid_levels_[level].count(number) == 0) {
          invalid_levels_[level].insert(number);
        } else {
          // Creating an already existing file on invalid level.
          has_invalid_levels_ = true;
        }
      }
    }

    // Add guards
    // NOTE: VersionEdit currently stores the complete set of guards, so
    // clear new_guards and complete_guards first
    const auto& new_guards_from_edit = edit->GetNewGuards();
    const auto& complete_guards_from_edit = edit->GetCompleteGuards();
    new_guards_.clear();
    complete_guards_.clear();
    for (const auto& new_guard : new_guards_from_edit) {
      assert(new_guard.level() >= 1);
      new_guards_.emplace_back(new_guard);
    }
    for (const auto& complete_guard : complete_guards_from_edit) {
      assert(complete_guard.level() >= 1);
      complete_guards_.emplace_back(complete_guard);
    }
  }

  // Save the current state in *v.
  void SaveTo(VersionStorageInfo* vstorage) {
    CheckConsistency(base_vstorage_);
    CheckConsistency(vstorage);

    // For convenience
    std::function<bool(const GuardMetaData&, const GuardMetaData&)> equals =
        [&](const GuardMetaData& first, const GuardMetaData& second) -> bool {
      return vstorage->internal_comparator_->Compare(first.guard_key(),
                                                     second.guard_key()) == 0;
    };

    // Make copy
    auto complete_guards = complete_guards_;
    auto new_guards = new_guards_;

    // Remove guards from complete_guards and new_guards if they already exist
    // in vstorage

    for (const GuardMetaData& new_guard : new_guards) {
      int level = new_guard.level();
      const auto& new_guards_result = vstorage->new_guards_.find(level);
      if (new_guards_result == vstorage->new_guards_.end()) {
        vstorage->AddNewGuard(new_guard);
        break;
      }
      const auto& existing_new_guards = new_guards_result->second;
      bool exists =
          existing_new_guards.end() ==
          std::find_if(existing_new_guards.begin(), existing_new_guards.end(),
                       [&](const GuardMetaData& existing_new_guard) -> bool {
                         return equals(new_guard, existing_new_guard);
                       });
      if (!exists) {
        vstorage->AddNewGuard(new_guard);
      }
    }

    for (const GuardMetaData& complete_guard : complete_guards) {
      int level = complete_guard.level();
      const auto& complete_guards_result =
          vstorage->complete_guards_.find(level);
      if (complete_guards_result == vstorage->complete_guards_.end()) {
        vstorage->AddCompleteGuard(complete_guard);
        break;
      }
      const auto& existing_complete_guards = complete_guards_result->second;
      bool exists =
          existing_complete_guards.end() ==
          std::find_if(
              existing_complete_guards.begin(), existing_complete_guards.end(),
              [&](const GuardMetaData& existing_complete_guard) -> bool {
                return equals(complete_guard, existing_complete_guard);
              });
      if (!exists) {
        vstorage->AddCompleteGuard(complete_guard);
      }
    }

    for (int level = 0; level < num_levels_; level++) {
      const auto& cmp = (level == 0) ? level_zero_cmp_ : level_nonzero_cmp_;
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      const auto& base_files = base_vstorage_->LevelFiles(level);
      auto base_iter = base_files.begin();
      auto base_end = base_files.end();
      const auto& unordered_added_files = levels_[level].added_files;
      vstorage->Reserve(level,
                        base_files.size() + unordered_added_files.size());

      // Sort added files for the level.
      std::vector<FileMetaData*> added_files;
      added_files.reserve(unordered_added_files.size());
      for (const auto& pair : unordered_added_files) {
        added_files.push_back(pair.second);
      }
      std::sort(added_files.begin(), added_files.end(), cmp);

#ifndef NDEBUG
      FileMetaData* prev_file = nullptr;
#endif

      for (const auto& added : added_files) {
#ifndef NDEBUG
        if (level > 0 && prev_file != nullptr) {
          assert(base_vstorage_->InternalComparator()->Compare(
                     prev_file->smallest, added->smallest) <= 0);
        }
        prev_file = added;
#endif

        // Add all smaller files listed in base_
        for (auto bpos = std::upper_bound(base_iter, base_end, added, cmp);
             base_iter != bpos; ++base_iter) {
          MaybeAddFile(vstorage, level, *base_iter);
        }

        MaybeAddFile(vstorage, level, added);
      }

      // Add remaining base files
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(vstorage, level, *base_iter);
      }
    }

    CheckConsistency(vstorage);
  }

  void LoadTableHandlers(InternalStats* internal_stats, int max_threads,
                         bool prefetch_index_and_filter_in_cache) {
    assert(table_cache_ != nullptr);
    // <file metadata, level>
    std::vector<std::pair<FileMetaData*, int>> files_meta;
    for (int level = 0; level < num_levels_; level++) {
      for (auto& file_meta_pair : levels_[level].added_files) {
        auto* file_meta = file_meta_pair.second;
        assert(!file_meta->table_reader_handle);
        files_meta.emplace_back(file_meta, level);
      }
    }

    std::atomic<size_t> next_file_meta_idx(0);
    std::function<void()> load_handlers_func = [&]() {
      while (true) {
        size_t file_idx = next_file_meta_idx.fetch_add(1);
        if (file_idx >= files_meta.size()) {
          break;
        }

        auto* file_meta = files_meta[file_idx].first;
        int level = files_meta[file_idx].second;
        table_cache_->FindTable(env_options_,
                                *(base_vstorage_->InternalComparator()),
                                file_meta->fd, &file_meta->table_reader_handle,
                                false /*no_io */, true /* record_read_stats */,
                                internal_stats->GetFileReadHist(level), false,
                                level, prefetch_index_and_filter_in_cache);
        if (file_meta->table_reader_handle != nullptr) {
          // Load table_reader
          file_meta->fd.table_reader = table_cache_->GetTableReaderFromHandle(
              file_meta->table_reader_handle);
        }
      }
    };

    if (max_threads <= 1) {
      load_handlers_func();
    } else {
      std::vector<port::Thread> threads;
      for (int i = 0; i < max_threads; i++) {
        threads.emplace_back(load_handlers_func);
      }

      for (auto& t : threads) {
        t.join();
      }
    }
  }

  void MaybeAddFile(VersionStorageInfo* vstorage, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->fd.GetNumber()) > 0) {
      // f is to-be-delected table file
      vstorage->RemoveCurrentStats(f);
    } else {
      vstorage->AddFile(level, f, info_log_);
    }
  }
};

VersionBuilder::VersionBuilder(const EnvOptions& env_options,
                               TableCache* table_cache,
                               VersionStorageInfo* base_vstorage,
                               Logger* info_log)
    : rep_(new Rep(env_options, info_log, table_cache, base_vstorage)) {}

VersionBuilder::~VersionBuilder() { delete rep_; }

void VersionBuilder::CheckConsistency(VersionStorageInfo* vstorage) {
  rep_->CheckConsistency(vstorage);
}

void VersionBuilder::CheckConsistencyForDeletes(VersionEdit* edit,
                                                uint64_t number, int level) {
  rep_->CheckConsistencyForDeletes(edit, number, level);
}

bool VersionBuilder::CheckConsistencyForNumLevels() {
  return rep_->CheckConsistencyForNumLevels();
}

void VersionBuilder::Apply(VersionEdit* edit) { rep_->Apply(edit); }

void VersionBuilder::SaveTo(VersionStorageInfo* vstorage) {
  rep_->SaveTo(vstorage);
}

void VersionBuilder::LoadTableHandlers(
    InternalStats* internal_stats, int max_threads,
    bool prefetch_index_and_filter_in_cache) {
  rep_->LoadTableHandlers(internal_stats, max_threads,
                          prefetch_index_and_filter_in_cache);
}

void VersionBuilder::MaybeAddFile(VersionStorageInfo* vstorage, int level,
                                  FileMetaData* f) {
  rep_->MaybeAddFile(vstorage, level, f);
}

}  // namespace rocksdb
