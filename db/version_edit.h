//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "db/dbformat.h"
#include "rocksdb/cache.h"
#include "util/arena.h"
#include "util/autovector.h"

namespace rocksdb {

class VersionSet;

struct GuardMetaData;

const uint64_t kFileNumberMask = 0x3FFFFFFFFFFFFFFF;

extern uint64_t PackFileNumberAndPathId(uint64_t number, uint64_t path_id);

// A copyable structure contains information needed to read data from an SST
// file. It can contains a pointer to a table reader opened for the file, or
// file number and size, which can be used to create a new table reader for it.
// The behavior is undefined when a copied of the structure is used when the
// file is not in any live version any more.
struct FileDescriptor {
  // Table reader in table_reader_handle
  TableReader* table_reader;
  uint64_t packed_number_and_path_id;
  uint64_t file_size;  // File size in bytes

  FileDescriptor() : FileDescriptor(0, 0, 0) {}

  FileDescriptor(uint64_t number, uint32_t path_id, uint64_t _file_size)
      : table_reader(nullptr),
        packed_number_and_path_id(PackFileNumberAndPathId(number, path_id)),
        file_size(_file_size) {}

  FileDescriptor& operator=(const FileDescriptor& fd) {
    table_reader = fd.table_reader;
    packed_number_and_path_id = fd.packed_number_and_path_id;
    file_size = fd.file_size;
    return *this;
  }

  uint64_t GetNumber() const {
    return packed_number_and_path_id & kFileNumberMask;
  }
  uint32_t GetPathId() const {
    return static_cast<uint32_t>(packed_number_and_path_id /
                                 (kFileNumberMask + 1));
  }
  uint64_t GetFileSize() const { return file_size; }
};

struct FileSampledStats {
  FileSampledStats() : num_reads_sampled(0) {}
  FileSampledStats(const FileSampledStats& other) { *this = other; }
  FileSampledStats& operator=(const FileSampledStats& other) {
    num_reads_sampled = other.num_reads_sampled.load();
    return *this;
  }

  // number of user reads to this file.
  mutable std::atomic<uint64_t> num_reads_sampled;
};

struct FileMetaData {
  FileDescriptor fd;
  InternalKey smallest;           // Smallest internal key served by table
  InternalKey largest;            // Largest internal key served by table
  SequenceNumber smallest_seqno;  // The smallest seqno in this file
  SequenceNumber largest_seqno;   // The largest seqno in this file
  GuardMetaData* guard;
  int allowed_seeks;  // Seeks allowed until compaction
  uint64_t number;
  uint64_t file_size;  // File size in bytes
                       // Needs to be disposed when refs becomes 0.
  Cache::Handle* table_reader_handle;

  FileSampledStats stats;

  // Stats for compensating deletion entries during compaction

  // File size compensated by deletion entry.
  // This is updated in Version::UpdateAccumulatedStats() first time when the
  // file is created or loaded.  After it is updated (!= 0), it is immutable.
  uint64_t compensated_file_size;
  // These values can mutate, but they can only be read or written from
  // single-threaded LogAndApply thread
  uint64_t num_entries;     // the number of entries.
  uint64_t num_deletions;   // the number of deletion entries.
  uint64_t raw_key_size;    // total uncompressed key size.
  uint64_t raw_value_size;  // total uncompressed value size.

  int refs;  // Reference count

  bool being_compacted;       // Is this file undergoing compaction?
  bool init_stats_from_file;  // true if the data-entry stats of this file
                              // has initialized from file.

  bool marked_for_compaction;  // True if client asked us nicely to compact this
                               // file.

  FileMetaData()
      : smallest_seqno(kMaxSequenceNumber),
        largest_seqno(0),
        table_reader_handle(nullptr),
        compensated_file_size(0),
        num_entries(0),
        num_deletions(0),
        raw_key_size(0),
        raw_value_size(0),
        refs(0),
        being_compacted(false),
        init_stats_from_file(false),
        marked_for_compaction(false) {}

  // REQUIRED: Keys must be given to the function in sorted order (it expects
  // the last key to be the largest).
  void UpdateBoundaries(const Slice& key, SequenceNumber seqno) {
    if (smallest.size() == 0) {
      smallest.DecodeFrom(key);
    }
    largest.DecodeFrom(key);
    smallest_seqno = std::min(smallest_seqno, seqno);
    largest_seqno = std::max(largest_seqno, seqno);
  }
};

/*
guard_key is the smallesst key served by the guard file. In each level,
there can be only one guard starting with a given key, so (level, key)
uniquely identifies a guard.
*/
struct GuardMetaData {
  int refs;
  int level;
  uint64_t number_segments;
  InternalKey guard_key;
  InternalKey smallest;
  InternalKey largest;
  std::vector<uint64_t> files;
  std::vector<FileMetaData*> file_metas;

  GuardMetaData()
      : refs(0),
        level(-1),
        number_segments(0),
        guard_key(),
        smallest(),
        largest() {
    files.clear();
  }
};

// A compressed copy of file meta data that just contain minimum data needed
// to server read operations, while still keeping the pointer to full metadata
// of the file in case it is needed.
struct FdWithKeyRange {
  FileDescriptor fd;
  FileMetaData* file_metadata;  // Point to all metadata
  Slice smallest_key;           // slice that contain smallest key
  Slice largest_key;            // slice that contain largest key

  FdWithKeyRange() : fd(), smallest_key(), largest_key() {}

  FdWithKeyRange(FileDescriptor _fd, Slice _smallest_key, Slice _largest_key,
                 FileMetaData* _file_metadata)
      : fd(_fd),
        file_metadata(_file_metadata),
        smallest_key(_smallest_key),
        largest_key(_largest_key) {}
};

// Data structure to store an array of FdWithKeyRange in one level
// Actual data is guaranteed to be stored closely
struct LevelFilesBrief {
  size_t num_files;
  FdWithKeyRange* files;
  LevelFilesBrief() {
    num_files = 0;
    files = nullptr;
  }
};

class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() {}

  void Clear();

  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetMaxColumnFamily(uint32_t max_column_family) {
    has_max_column_family_ = true;
    max_column_family_ = max_column_family;
  }

  // Add the specified file at the specified number.
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  void AddFile(int level, uint64_t file, uint32_t file_path_id,
               uint64_t file_size, const InternalKey& smallest,
               const InternalKey& largest, const SequenceNumber& smallest_seqno,
               const SequenceNumber& largest_seqno,
               bool marked_for_compaction) {
    assert(smallest_seqno <= largest_seqno);
    FileMetaData f;
    f.fd = FileDescriptor(file, file_path_id, file_size);
    f.smallest = smallest;
    f.largest = largest;
    f.smallest_seqno = smallest_seqno;
    f.largest_seqno = largest_seqno;
    f.marked_for_compaction = marked_for_compaction;
    new_files_.emplace_back(level, std::move(f));
  }

  void AddFile(int level, const FileMetaData& f) {
    assert(f.smallest_seqno <= f.largest_seqno);
    new_files_.emplace_back(level, f);
  }

  void AddNewGuard(GuardMetaData* g) { new_guards_.emplace_back(*g); }

  void AddCompleteGuard(GuardMetaData* g) { complete_guards_.emplace_back(*g); }

  // Delete the specified "file" from the specified "level".
  void DeleteFile(int level, uint64_t file) {
    deleted_files_.insert({level, file});
  }

  // Number of edits
  size_t NumEntries() { return new_files_.size() + deleted_files_.size(); }

  bool IsColumnFamilyManipulation() {
    return is_column_family_add_ || is_column_family_drop_;
  }

  void SetColumnFamily(uint32_t column_family_id) {
    column_family_ = column_family_id;
  }

  // set column family ID by calling SetColumnFamily()
  void AddColumnFamily(const std::string& name) {
    assert(!is_column_family_drop_);
    assert(!is_column_family_add_);
    assert(NumEntries() == 0);
    is_column_family_add_ = true;
    column_family_name_ = name;
  }

  // set column family ID by calling SetColumnFamily()
  void DropColumnFamily() {
    assert(!is_column_family_drop_);
    assert(!is_column_family_add_);
    assert(NumEntries() == 0);
    is_column_family_drop_ = true;
  }

  // return true on success.
  bool EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  const char* DecodeNewFile4From(Slice* input);

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

  const DeletedFileSet& GetDeletedFiles() { return deleted_files_; }
  const std::vector<std::pair<int, FileMetaData>>& GetNewFiles() {
    return new_files_;
  }

  std::string DebugString(bool hex_key = false) const;
  std::string DebugJSON(int edit_num, bool hex_key = false) const;

  const std::vector<GuardMetaData>& GetNewGuards() { return new_guards_; }

  const std::vector<GuardMetaData>& GetCompleteGuards() {
    return complete_guards_;
  }

 private:
  friend class VersionSet;
  friend class Version;

  bool GetLevel(Slice* input, int* level, const char** msg);

  int max_level_;
  std::string comparator_;
  uint64_t log_number_;
  uint64_t prev_log_number_;
  uint64_t next_file_number_;
  uint32_t max_column_family_;
  SequenceNumber last_sequence_;
  bool has_comparator_;
  bool has_log_number_;
  bool has_prev_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;
  bool has_max_column_family_;

  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;

  // Each version edit record should have column_family_id set
  // If it's not set, it is default (0)
  uint32_t column_family_;
  // a version edit can be either column_family add or
  // column_family drop. If it's column family add,
  // it also includes column family name.
  bool is_column_family_drop_;
  bool is_column_family_add_;
  std::string column_family_name_;
  std::vector<std::vector<FileMetaData>> sentinel_files_;
  std::vector<std::vector<uint64_t>> sentinel_file_nos_;
  std::vector<GuardMetaData> new_guards_;
  std::vector<GuardMetaData> complete_guards_;
};

}  // namespace rocksdb
