// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG
#define CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG

//#if defined(HAVE_PMEM)
#include <libpmemobj.h>
//#endif
#include "common/RWLock.h"
#include "librbd/cache/ImageCache.h"
#include "librbd/cache/FileImageCache.h"
#include "librbd/Utils.h"
#include "librbd/cache/BlockGuard.h"
#include "librbd/cache/ImageWriteback.h"
#include "librbd/cache/file/Policy.h"
#include "librbd/BlockGuard.h"
#include <functional>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <deque>
#include <list>
#include "common/Finisher.h"
#include "include/assert.h"

namespace librbd {

struct ImageCtx;

namespace cache {

static const uint32_t MIN_WRITE_SIZE = 1;
static const uint32_t BLOCK_SIZE = MIN_WRITE_SIZE;
static const uint32_t MIN_MIN_WRITE_ALLOC_SIZE = 512;
static const uint32_t MIN_WRITE_ALLOC_SIZE =
  (MIN_WRITE_SIZE > MIN_MIN_WRITE_ALLOC_SIZE ?
   MIN_WRITE_SIZE : MIN_MIN_WRITE_ALLOC_SIZE);

/* Crash consistent flusher ignores these, and must flush in FIFO ordrer */
static const int IN_FLIGHT_FLUSH_WRITE_LIMIT = 8;
static const int IN_FLIGHT_FLUSH_BYTES_LIMIT = (1 * 1024 * 1024);

BlockExtent block_extent(uint64_t offset_bytes, uint64_t length_bytes);
BlockExtent block_extent(ImageCache::Extent& image_extent);

namespace rwl {

/**** Write log entries ****/

static const uint64_t DEFAULT_POOL_SIZE = 10u<<30;
static const uint64_t MIN_POOL_SIZE = 1u<<20;
static const double USABLE_SIZE = (7.0 / 10);
static const uint64_t BLOCK_ALLOC_OVERHEAD_BYTES = 16;
static const uint8_t RWL_POOL_VERSION = 1;

POBJ_LAYOUT_BEGIN(rbd_rwl);
POBJ_LAYOUT_ROOT(rbd_rwl, struct WriteLogPoolRoot);
POBJ_LAYOUT_TOID(rbd_rwl, uint8_t);
POBJ_LAYOUT_TOID(rbd_rwl, struct WriteLogPmemEntry);
POBJ_LAYOUT_END(rbd_rwl);

struct WriteLogPmemEntry {
  uint64_t sync_gen_number = 0;
  uint64_t write_sequence_number = 0;
  uint64_t image_offset_bytes;
  uint64_t write_bytes;
  TOID(uint8_t) write_data;
  struct {
    uint8_t entry_valid :1; /* if 0, this entry is free */
    uint8_t sync_point :1;  /* No data. No write sequence
			       number. Marks sync point for this sync
			       gen number */
    uint8_t sequenced :1;   /* write sequence number is valid */
    uint8_t has_data :1;    /* write_data field is valid (else ignore) */
    uint8_t unmap :1;       /* has_data will be 0 if this
			       is an unmap */
  };
  WriteLogPmemEntry(uint64_t image_offset_bytes, uint64_t write_bytes) 
    : image_offset_bytes(image_offset_bytes), write_bytes(write_bytes), 
      entry_valid(0), sync_point(0), sequenced(0), has_data(0), unmap(0) {
  }
  BlockExtent block_extent() {
    return BlockExtent(librbd::cache::block_extent(image_offset_bytes, write_bytes));
  }
  friend std::ostream &operator<<(std::ostream &os,
				  const WriteLogPmemEntry &entry) {
    os << "entry_valid=" << (bool)entry.entry_valid << ", "
       << "sync_point=" << (bool)entry.sync_point << ", "
       << "sequenced=" << (bool)entry.sequenced << ", "
       << "has_data=" << (bool)entry.has_data << ", "
       << "unmap=" << (bool)entry.unmap << ", "
       << "sync_gen_number=" << entry.sync_gen_number << ", "
       << "write_sequence_number=" << entry.write_sequence_number << ", "
       << "image_offset_bytes=" << entry.image_offset_bytes << ", "
       << "write_bytes=" << entry.write_bytes;
    return os;
  };
};

struct WriteLogPoolRoot {
  union {
    struct {
      uint8_t layout_version;    /* Version of this structure (RWL_POOL_VERSION) */ 
    };
    uint64_t _u64;
  } header;
  TOID(struct WriteLogPmemEntry) log_entries;   /* contiguous array of log entries */
  uint32_t block_size;			         /* block size */
  uint32_t num_log_entries;
  uint32_t first_free_entry;     /* Entry following the newest valid entry */
  uint32_t first_valid_entry;    /* Index of the oldest valid entry in the log */
};

class WriteLogEntry {
public:
  WriteLogPmemEntry ram_entry;
  WriteLogPmemEntry *pmem_entry = nullptr;
  uint8_t *pmem_buffer = nullptr;
  uint32_t log_entry_index = 0;
  uint32_t referring_map_entries = 0;
  uint32_t reader_count = 0;
  /* TODO: occlusion by subsequent writes */
  /* TODO: flush state: portions flushed, in-progress flushes */
  bool flushing = false;
  bool flushed = false;
  WriteLogEntry(uint64_t image_offset_bytes, uint64_t write_bytes) 
    : ram_entry(image_offset_bytes, write_bytes) {
  }
  WriteLogEntry(const WriteLogEntry&) = delete;
  WriteLogEntry &operator=(const WriteLogEntry&) = delete;
  BlockExtent block_extent() { return ram_entry.block_extent(); }
  void add_reader();
  void remove_reader();
  friend std::ostream &operator<<(std::ostream &os,
				  WriteLogEntry &entry) {
    os << "ram_entry=[" << entry.ram_entry << "], "
       << "log_entry_index=" << entry.log_entry_index << ", "
       << "pmem_entry=" << (void*)entry.pmem_entry << ", "
       << "referring_map_entries=" << entry.referring_map_entries << ", "
       << "reader_count=" << entry.reader_count << ", "
       << "flushing=" << entry.flushing << ", "
       << "flushed=" << entry.flushed;
    return os;
  };
};
  
typedef std::list<WriteLogEntry*> WriteLogEntries;

/**** Write log entries end ****/

class SyncPoint {
public:
  CephContext *m_cct;
  const uint64_t m_sync_gen_num;
  uint64_t m_final_op_sequence_num = 0;
  /* A sync point can't appear in the log until all the writes bearing
   * it and all the prior sync points have been appended and
   * persisted.
   *
   * Writes bearing this sync gen number and the prior sync point will
   * be sub-ops of this Gather. This sync point will not be appended
   * until all these complete. */
  C_Gather *m_prior_log_entries_persisted;
  int m_prior_log_entries_persisted_status = 0;
  /* Signal this when this sync point is appended and persisted. This
   * is a sub-operation of the next sync point's
   * m_prior_log_entries_persisted Gather. */
  Context *m_on_sync_point_persisted = nullptr;
  
  SyncPoint(CephContext *cct, uint64_t sync_gen_num);
  ~SyncPoint();
  SyncPoint(const SyncPoint&) = delete;
  SyncPoint &operator=(const SyncPoint&) = delete;
  friend std::ostream &operator<<(std::ostream &os,
				  SyncPoint &p) {
    os << "m_sync_gen_num=" << p.m_sync_gen_num << ", "
       << "m_final_op_sequence_num=" << p.m_final_op_sequence_num << ", "
       << "m_prior_log_entries_persisted=[" << p.m_prior_log_entries_persisted << "], "
       << "m_on_sync_point_persisted=[" << p.m_on_sync_point_persisted << "]";
    return os;
  };
};

class WriteLogOperationSet;
class WriteLogOperation {
public:
  WriteLogEntry *log_entry;
  bufferlist bl;
  pobj_action buffer_alloc_action;
  Context *on_write_persist; /* Completion for things waiting on this write to persist */
  WriteLogOperation(WriteLogOperationSet *set, uint64_t image_offset_bytes, uint64_t write_bytes);
  ~WriteLogOperation();
  WriteLogOperation(const WriteLogOperation&) = delete;
  WriteLogOperation &operator=(const WriteLogOperation&) = delete;
  friend std::ostream &operator<<(std::ostream &os,
				  WriteLogOperation &op) {
    os << "log_entry=[" << *op.log_entry << "], "
       << "bl=[" << op.bl << "]";
    return os;
  };
  void complete(int r);
};
typedef std::list<WriteLogOperation*> WriteLogOperations;

class WriteLogOperationSet {
public:
  CephContext *m_cct;
  BlockExtent m_extent; /* in blocks */
  Context *m_on_finish;
  bool m_persist_on_flush;
  BlockGuardCell *m_cell;
  C_Gather *m_extent_ops;
  Context *m_on_ops_persist;
  WriteLogOperations operations;
  WriteLogOperationSet(CephContext *cct, SyncPoint *sync_point, bool persist_on_flush,
		       BlockExtent extent, Context *on_finish);
  ~WriteLogOperationSet();
  WriteLogOperationSet(const WriteLogOperationSet&) = delete;
  WriteLogOperationSet &operator=(const WriteLogOperationSet&) = delete;
  friend std::ostream &operator<<(std::ostream &os,
				  WriteLogOperationSet &s) {
    os << "m_extent=[" << s.m_extent.block_start << "," << s.m_extent.block_end << "] "
       << "m_on_finish=" << s.m_on_finish << ", "
       << "m_cell=" << (void*)s.m_cell << ", "
       << "m_extent_ops=[" << s.m_extent_ops << "]";
    return os;
  };
};

class GuardedRequestFunctionContext : public Context {
private:
  std::atomic<bool> m_callback_invoked;
  boost::function<void(BlockGuardCell*)> m_callback;
public:
  GuardedRequestFunctionContext(boost::function<void(BlockGuardCell*)> &&callback)
    : m_callback_invoked(false), m_callback(std::move(callback)) { }

  GuardedRequestFunctionContext(const GuardedRequestFunctionContext&) = delete;
  GuardedRequestFunctionContext &operator=(const GuardedRequestFunctionContext&) = delete;

  virtual void finish(int r) override {
    assert(true == m_callback_invoked);
  }
  
  void acquired(BlockGuardCell *cell) {
    bool initial = false;
    if (m_callback_invoked.compare_exchange_strong(initial, true)) {
      m_callback(cell);
    }
  }
};

struct GuardedRequest {
  uint64_t first_block_num;
  uint64_t last_block_num;
  GuardedRequestFunctionContext *on_guard_acquire; /* Work to do when guard on range obtained */
  
  GuardedRequest(uint64_t first_block_num, uint64_t last_block_num, GuardedRequestFunctionContext *on_guard_acquire)
    : first_block_num(first_block_num), last_block_num(last_block_num), on_guard_acquire(on_guard_acquire) {
  }
};

typedef librbd::BlockGuard<GuardedRequest> WriteLogGuard;

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::rwl::WriteLogMap: " << this << " " \
                           <<  __func__ << ": "
/**
 * WriteLogMap: maps block extents to WriteLogEntries
 *
 */
/* A WriteLogMapEntry refers to a portion of a WriteLogEntry */
struct WriteLogMapEntry {
  BlockExtent block_extent;
  WriteLogEntry *log_entry;
  
  WriteLogMapEntry(BlockExtent block_extent = BlockExtent(0, 0),
		   WriteLogEntry *log_entry = nullptr)
    : block_extent(block_extent) , log_entry(log_entry) {
  }
  WriteLogMapEntry(WriteLogEntry *log_entry)
    : block_extent(log_entry->block_extent()) , log_entry(log_entry) {
  }
  friend std::ostream &operator<<(std::ostream &os,
				  WriteLogMapEntry &e) {
    os << "block_extent=" << e.block_extent << ", "
       << "log_entry=[" << e.log_entry << "]";
    return os;
  };
};

typedef std::list<WriteLogMapEntry> WriteLogMapEntries;
class WriteLogMap {
public:
  WriteLogMap(CephContext *cct)
    : m_cct(cct), m_lock("librbd::cache::rwl::WriteLogMap::m_lock") {
  }

  WriteLogMap(const WriteLogMap&) = delete;
  WriteLogMap &operator=(const WriteLogMap&) = delete;

  void add_log_entry(WriteLogEntry *log_entry);
  void add_log_entries(WriteLogEntries &log_entries);
  void remove_log_entry(WriteLogEntry *log_entry);
  void remove_log_entries(WriteLogEntries &log_entries);
  WriteLogEntries find_log_entries(BlockExtent block_extent);
  WriteLogMapEntries find_map_entries(BlockExtent block_extent);
  
private:
  void add_log_entry_locked(WriteLogEntry *log_entry);
  void remove_log_entry_locked(WriteLogEntry *log_entry);
  void add_map_entry_locked(WriteLogMapEntry &map_entry);
  void remove_map_entry_locked(WriteLogMapEntry &map_entry);
  void adjust_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &new_extent);
  void split_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &removed_extent);
  WriteLogEntries find_log_entries_locked(BlockExtent &block_extent);
  WriteLogMapEntries find_map_entries_locked(BlockExtent &block_extent);

  /* We map block extents to write log entries, or portions of write log
   * entries. These are both represented by a WriteLogMapEntry. When a
   * WriteLogEntry is added to this map, a WriteLogMapEntry is created to
   * represent the entire block extent of the WriteLogEntry, and the
   * WriteLogMapEntry is added to the set.
   *
   * The set must not contain overlapping WriteLogMapEntrys. WriteLogMapEntrys
   * in the set that overlap with one being added are adjusted (shrunk, split,
   * or removed) before the new entry is added.
   *
   * This comparison works despite the ambiguity because we ensure the set
   * contains no overlapping entries. This comparison works to find entries
   * that overlap with a given block extent because equal_range() returns the
   * first entry in which the extent doesn't end before the given extent
   * starts, and the last entry for which the extent starts before the given
   * extent ends (the first entry that the key is less than, and the last entry
   * that is less than the key).
   */
  struct WriteLogMapEntryCompare {
    bool operator()(const WriteLogMapEntry &lhs,
                    const WriteLogMapEntry &rhs) const {
      // check for range overlap (lhs < rhs)
      if (lhs.block_extent.block_end < rhs.block_extent.block_start) {
        return true;
      }
      return false;
    }
  };

  typedef std::set<WriteLogMapEntry,
		   WriteLogMapEntryCompare> BlockExtentToWriteLogMapEntries;

  WriteLogMapEntry block_extent_to_map_key(BlockExtent &block_extent) {
    return WriteLogMapEntry(block_extent);
  }

  CephContext *m_cct;

  Mutex m_lock;
  BlockExtentToWriteLogMapEntries m_block_to_log_entry_map;
};

} // namespace rwl

using namespace librbd::cache::rwl;

struct C_WriteRequest;

/**
 * Prototype pmem-based, client-side, replicated write log
 */
template <typename ImageCtxT = librbd::ImageCtx>
class ReplicatedWriteLog : public StackingImageCache<ImageCtxT> {
  using typename ImageCache::Extent;
  using typename ImageCache::Extents;
public:
  ReplicatedWriteLog(ImageCtx &image_ctx, StackingImageCache<ImageCtxT> *lower);
  ~ReplicatedWriteLog();
  ReplicatedWriteLog(const ReplicatedWriteLog&) = delete;
  ReplicatedWriteLog &operator=(const ReplicatedWriteLog&) = delete;

  /// client AIO methods
  void aio_read(Extents&& image_extents, ceph::bufferlist *bl,
                int fadvise_flags, Context *on_finish) override;
  void aio_write(Extents&& image_extents, ceph::bufferlist&& bl,
                 int fadvise_flags, Context *on_finish) override;
  void aio_discard(uint64_t offset, uint64_t length,
                   bool skip_partial_discard, Context *on_finish);
  void aio_flush(Context *on_finish) override;
  void aio_writesame(uint64_t offset, uint64_t length,
                     ceph::bufferlist&& bl,
                     int fadvise_flags, Context *on_finish) override;
  void aio_compare_and_write(Extents&& image_extents,
                             ceph::bufferlist&& cmp_bl, ceph::bufferlist&& bl,
                             uint64_t *mismatch_offset,int fadvise_flags,
                             Context *on_finish) override;

  /// internal state methods
  void init(Context *on_finish) override;
  void shut_down(Context *on_finish) override;

  void invalidate(Context *on_finish) override;
  void flush(Context *on_finish) override;

  void detain_guarded_request(GuardedRequest &&req);
  void release_guarded_request(BlockGuardCell *cell);
private:
  typedef std::function<void(uint64_t)> ReleaseBlock;
  typedef std::function<void(BlockGuard::BlockIO)> AppendDetainedBlock;
  typedef std::list<Context *> Contexts;

  const char* rwl_pool_layout_name = POBJ_LAYOUT_NAME(rbd_rwl);

  ImageCtxT &m_image_ctx;

  std::string m_log_pool_name;
  PMEMobjpool *m_log_pool = nullptr;
  uint64_t m_log_pool_size;
  
  uint32_t m_total_log_entries = 0;
  uint32_t m_free_log_entries = 0;

  StackingImageCache<ImageCtxT> *m_image_writeback;
  WriteLogGuard m_write_log_guard;
  
  /* 
   * When m_first_free_entry == m_first_valid_entry, the log is
   * empty. There is always at least one free entry, which can't be
   * used.
   */
  uint64_t m_first_free_entry = 0;  /* Entries from here to m_first_valid_entry-1 are free */
  uint64_t m_first_valid_entry = 0; /* Entries from here to m_first_free_entry-1 are valid */

  /* Starts at 0 for a new write log. Incremented on every flush. */
  uint64_t m_current_sync_gen = 0;
  SyncPoint *m_current_sync_point = nullptr;
  /* Starts at 0 on each sync gen increase. Incremented before applied
     to an operation */
  uint64_t m_last_op_sequence_num = 0;

  bool m_persist_on_write_until_flush = true;
  bool m_persist_on_flush = false; /* If false, persist each write before completion */
  bool m_flush_seen = false;
  
  util::AsyncOpTracker m_async_op_tracker;

  mutable Mutex m_log_append_lock;
  mutable Mutex m_lock;
  
  BlockGuard::BlockIOs m_deferred_block_ios;
  BlockGuard::BlockIOs m_detained_block_ios;

  bool m_wake_up_requested = false;
  bool m_wake_up_scheduled = false;

  Contexts m_post_work_contexts;
  Contexts m_flush_complete_contexts;
  Finisher m_persist_finisher;
  Finisher m_log_append_finisher;
  Finisher m_on_persist_finisher;
  
  WriteLogOperations m_ops_to_flush; /* Write ops needing flush in local log */
  WriteLogOperations m_ops_to_append; /* Write ops needing event append in local log */

  WriteLogMap m_blocks_to_log_entries;

  /* New entries are at the back. Oldest at the front */
  WriteLogEntries m_log_entries;
  WriteLogEntries m_dirty_log_entries;

  int m_flush_ops_in_flight = 0;
  int m_flush_bytes_in_flight = 0;
  
  void rwl_init(Context *on_finish);
  void wake_up();
  void process_work();
  bool drain_context_list(Contexts &contexts, Mutex &contexts_lock);

  bool is_work_available() const;
  bool can_flush_entry(WriteLogEntry *log_entry);
  void flush_entry(WriteLogEntry *log_entry);
  void process_writeback_dirty_blocks();
  void process_detained_block_ios();
  void process_deferred_block_ios();

  void invalidate(Extents&& image_extents, Context *on_finish);

  void append_sync_point(SyncPoint *sync_point, int prior_write_status);
  void new_sync_point(void);

  void complete_write_req(C_WriteRequest *write_req, int result);
  void dispatch_aio_write(C_WriteRequest *write_req);
  void append_scheduled_ops(void);
  void schedule_append(WriteLogOperations &ops);
  void flush_then_append_scheduled_ops(void);
  void schedule_flush_and_append(WriteLogOperations &ops);
  void flush_pmem_buffer(WriteLogOperations &ops);
  void alloc_op_log_entries(WriteLogOperations &ops);
  void flush_op_log_entries(WriteLogOperations &ops);
  int append_op_log_entries(WriteLogOperations &ops);
  void complete_op_log_entries(WriteLogOperations &ops, int r);
};

} // namespace cache
} // namespace librbd

extern template class librbd::cache::ReplicatedWriteLog<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG

/* Local Variables: */
/* eval: (c-set-offset 'innamespace 0) */
/* End: */
