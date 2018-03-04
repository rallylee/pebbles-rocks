#pragma once

#include <atomic>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/column_family.h"
#include "db/dbformat.h"
#include "db/version_edit.h"



namespace rocksdb {

class VersionStorageInfo;

class GuardSetComparator {
 public:
  explicit GuardSetComparator(const InternalKeyComparator* comparator) : comparator_(comparator) {}
  const InternalKeyComparator* comparator_;
  bool operator()(const GuardMetaData& first, const GuardMetaData& second) const {
    assert(comparator_ != nullptr);
    if (first.guard_key().size() == 0 && second.guard_key().size() != 0) {
      return true;
    } else if (first.guard_key().size() != 0 && second.guard_key().size() == 0) {
      return false;
    } else if (first.guard_key().size() == 0 && second.guard_key().size() == 0) {
      return false;
    } else {
      return comparator_->Compare(first.guard_key(),
                                  second.guard_key()) < 0;
    }
  }
};

class GuardSet {
  std::set<std::reference_wrapper<const GuardMetaData>, GuardSetComparator> entire_set_;

  GuardSet(const GuardSetComparator& comparator, GuardMetaData& sentinel,
           std::set<GuardMetaData, GuardSetComparator>::iterator
           primary_guards_begin,
           std::set<GuardMetaData, GuardSetComparator>::iterator
           primary_guards_end,
           std::set<GuardMetaData, GuardSetComparator>::iterator
           secondary_guards_begin,
           std::set<GuardMetaData, GuardSetComparator>::iterator
           secondary_guards_end) : entire_set_(comparator) {
    entire_set_.emplace(std::cref(sentinel));
    for (auto it = primary_guards_begin; it != primary_guards_end; it++) {
      assert((*it).guard_key().size() > 0);
      entire_set_.emplace(std::cref(*it));
    }
    for (auto it = secondary_guards_begin; it != secondary_guards_end; it++) {
      assert((*it).guard_key().size() > 0);
      entire_set_.emplace(std::cref(*it));
    }
  }

 public:
  typedef std::set<std::reference_wrapper<const GuardMetaData>, GuardSetComparator>::iterator iterator;
  iterator begin() const {
    return entire_set_.begin();
  }

  iterator end() const {
    return entire_set_.end();
  }

  int size() const {
      return entire_set_.size();
  }

  friend VersionStorageInfo;
};

}  // namespace rocksdb
