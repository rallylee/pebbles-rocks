//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#ifndef ROCKSDB_LITE

#include <functional>
#include <string>
#include <vector>
#include "rocksdb/db.h"
#include "rocksdb/status.h"
#include "rocksdb/utilities/stackable_db.h"

namespace rocksdb {

namespace blob_db {

class TTLExtractor;

// A wrapped database which puts values of KV pairs in a separate log
// and store location to the log in the underlying DB.
// It lacks lots of importatant functionalities, e.g. DB restarts,
// garbage collection, iterators, etc.
//
// The factory needs to be moved to include/rocksdb/utilities to allow
// users to use blob DB.

struct BlobDBOptions {
  // name of the directory under main db, where blobs will be stored.
  // default is "blob_dir"
  std::string blob_dir = "blob_dir";

  // whether the blob_dir path is relative or absolute.
  bool path_relative = true;

  // is the eviction strategy fifo based
  bool is_fifo = false;

  // maximum size of the blob dir. Once this gets used, up
  // evict the blob file which is oldest (is_fifo )
  // 0 means no limits
  uint64_t blob_dir_size = 0;

  // a new bucket is opened, for ttl_range. So if ttl_range is 600seconds
  // (10 minutes), and the first bucket starts at 1471542000
  // then the blob buckets will be
  // first bucket is 1471542000 - 1471542600
  // second bucket is 1471542600 - 1471543200
  // and so on
  uint64_t ttl_range_secs = 3600;

  // at what bytes will the blob files be synced to blob log.
  uint64_t bytes_per_sync = 0;

  // the target size of each blob file. File will become immutable
  // after it exceeds that size
  uint64_t blob_file_size = 256 * 1024 * 1024;

  // how many files to use for simple blobs at one time
  uint32_t num_concurrent_simple_blobs = 1;

  // Instead of setting TTL explicitly by calling PutWithTTL or PutUntil,
  // applications can set a TTLExtractor which can extract TTL from key-value
  // pairs.
  std::shared_ptr<TTLExtractor> ttl_extractor = nullptr;

  // eviction callback.
  // this function will be called for every blob that is getting
  // evicted.
  std::function<void(const ColumnFamilyHandle*, const Slice&, const Slice&)>
      gc_evict_cb_fn;

  // what compression to use for Blob's
  CompressionType compression = kNoCompression;

  // Disable all background job.
  bool disable_background_tasks = false;

  void Dump(Logger* log) const;
};

class BlobDB : public StackableDB {
 public:
  using rocksdb::StackableDB::Put;

  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) override = 0;

  using rocksdb::StackableDB::Delete;
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family,
                        const Slice& key) override = 0;

  virtual Status PutWithTTL(const WriteOptions& options,
                            ColumnFamilyHandle* column_family, const Slice& key,
                            const Slice& value, uint64_t ttl) = 0;

  virtual Status PutWithTTL(const WriteOptions& options, const Slice& key,
                            const Slice& value, uint64_t ttl) {
    return PutWithTTL(options, DefaultColumnFamily(), key, value, ttl);
  }

  // Put with expiration. Key with expiration time equal to
  // std::numeric_limits<uint64_t>::max() means the key don't expire.
  virtual Status PutUntil(const WriteOptions& options,
                          ColumnFamilyHandle* column_family, const Slice& key,
                          const Slice& value, uint64_t expiration) = 0;

  virtual Status PutUntil(const WriteOptions& options, const Slice& key,
                          const Slice& value, uint64_t expiration) {
    return PutUntil(options, DefaultColumnFamily(), key, value, expiration);
  }

  using rocksdb::StackableDB::Get;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     PinnableSlice* value) override = 0;

  using rocksdb::StackableDB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys,
      std::vector<std::string>* values) override = 0;

  using rocksdb::StackableDB::SingleDelete;
  virtual Status SingleDelete(const WriteOptions& wopts,
                              ColumnFamilyHandle* column_family,
                              const Slice& key) override = 0;

  using rocksdb::StackableDB::Merge;
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) override {
    return Status::NotSupported("Not supported operation in blob db.");
  }

  virtual Status Write(const WriteOptions& opts,
                       WriteBatch* updates) override = 0;

  // Starting point for opening a Blob DB.
  // changed_options - critical. Blob DB loads and inserts listeners
  // into options which are necessary for recovery and atomicity
  // Use this pattern if you need control on step 2, i.e. your
  // BaseDB is not just a simple rocksdb but a stacked DB
  // 1. ::OpenAndLoad
  // 2. Open Base DB with the changed_options
  // 3. ::LinkToBaseDB
  static Status OpenAndLoad(const Options& options,
                            const BlobDBOptions& bdb_options,
                            const std::string& dbname, BlobDB** blob_db,
                            Options* changed_options);

  // This is another way to open BLOB DB which do not have other
  // Stackable DB's in play
  // Steps.
  // 1. ::Open
  static Status Open(const Options& options, const BlobDBOptions& bdb_options,
                     const std::string& dbname, BlobDB** blob_db);

  static Status Open(const DBOptions& db_options,
                     const BlobDBOptions& bdb_options,
                     const std::string& dbname,
                     const std::vector<ColumnFamilyDescriptor>& column_families,
                     std::vector<ColumnFamilyHandle*>* handles,
                     BlobDB** blob_db, bool no_base_db = false);

  virtual BlobDBOptions GetBlobDBOptions() const = 0;

  virtual ~BlobDB() {}

  virtual Status LinkToBaseDB(DB* db_base) = 0;

 protected:
  explicit BlobDB(DB* db);
};

// Destroy the content of the database.
Status DestroyBlobDB(const std::string& dbname, const Options& options,
                     const BlobDBOptions& bdb_options);

// TTLExtractor allow applications to extract TTL from key-value pairs.
// This useful for applications using Put or WriteBatch to write keys and
// don't intend to migrate to PutWithTTL or PutUntil.
//
// Applications can implement either ExtractTTL or ExtractExpiration. If both
// are implemented, ExtractExpiration will take precedence.
class TTLExtractor {
 public:
  // Extract TTL from key-value pair.
  // Return true if the key has TTL, false otherwise. If key has TTL,
  // TTL is pass back through ttl. The method can optionally modify the value,
  // pass the result back through new_value, and also set value_changed to true.
  virtual bool ExtractTTL(const Slice& key, const Slice& value, uint64_t* ttl,
                          std::string* new_value, bool* value_changed);

  // Extract expiration time from key-value pair.
  // Return true if the key has expiration time, false otherwise. If key has
  // expiration time, it is pass back through expiration. The method can
  // optionally modify the value, pass the result back through new_value,
  // and also set value_changed to true.
  virtual bool ExtractExpiration(const Slice& key, const Slice& value,
                                 uint64_t now, uint64_t* expiration,
                                 std::string* new_value, bool* value_changed);

  virtual ~TTLExtractor() = default;
};

}  // namespace blob_db
}  // namespace rocksdb
#endif  // ROCKSDB_LITE
