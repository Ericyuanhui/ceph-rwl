// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG
#define CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG

//#if defined(HAVE_PMEM)
#include <libpmemobj.h>
//#endif
#include "common/RWLock.h"
#include "librbd/cache/ImageCache.h"
#include "librbd/Utils.h"
#include "librbd/cache/BlockGuard.h"
#include "librbd/cache/file/Policy.h"
#include "librbd/BlockGuard.h"
#include <functional>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <deque>
#include <list>
#include "common/Finisher.h"
#include "include/assert.h"

enum {
  l_librbd_rwl_first = 26500,

  // All read requests
  l_librbd_rwl_rd_req,           // read requests
  l_librbd_rwl_rd_bytes,         // bytes read
  l_librbd_rwl_rd_latency,       // average req completion latency

  // Read requests completed from RWL (no misses)
  l_librbd_rwl_rd_hit_req,       // read requests
  l_librbd_rwl_rd_hit_bytes,     // bytes read
  l_librbd_rwl_rd_hit_latency,   // average req completion latency

  // Reed requests with hit and miss extents
  l_librbd_rwl_rd_part_hit_req,  // read ops

  // All write requests
  l_librbd_rwl_wr_req,             // write requests
  l_librbd_rwl_wr_bytes,           // bytes written

  // Write log operations (1 .. n per write request)
  l_librbd_rwl_log_ops,            // log append ops
  l_librbd_rwl_log_op_bytes,       // average bytes written per log op

  /*

   Req and op average latencies to the beginning of and over various phases:

   +------------------------------+------+-------------------------------+
   | Phase                        | Name | Description                   |
   +------------------------------+------+-------------------------------+
   | Arrive at RWL                | arr  |Arrives as a request           |
   +------------------------------+------+-------------------------------+
   | Allocate resources           | all  |time spent in block guard for  |
   |                              |      |overlap sequencing occurs      |
   |                              |      |before this point              |
   +------------------------------+------+-------------------------------+
   | Dispatch                     | dis  |Op lifetime begins here. time  |
   |                              |      |spent in allocation waiting for|
   |                              |      |resources occurs before this   |
   |                              |      |point                          |
   +------------------------------+------+-------------------------------+
   | Payload buffer persist and   | buf  |time spent queued for          |
   |replicate                     |      |replication occurs before here |
   +------------------------------+------+-------------------------------+
   | Payload buffer persist       | bufc |bufc - buf is just the persist |
   |complete                      |      |time                           |
   +------------------------------+------+-------------------------------+
   | Log append                   | app  |time spent queued for append   |
   |                              |      |occurs before here             |
   +------------------------------+------+-------------------------------+
   | Append complete              | appc |appc - app is just the time    |
   |                              |      |spent in the append operation  |
   +------------------------------+------+-------------------------------+
   | Complete                     | cmp  |write persisted, replciated,   |
   |                              |      |and globally visible           |
   +------------------------------+------+-------------------------------+

  */

  /* Request times */
  l_librbd_rwl_req_arr_to_all_t,   // arrival to allocation elapsed time - same as time deferred in block guard
  l_librbd_rwl_req_arr_to_dis_t,   // arrival to dispatch elapsed time
  l_librbd_rwl_req_all_to_dis_t,   // Time spent allocating or waiting to allocate resources
  l_librbd_rwl_wr_latency,         // average req (persist) completion latency
  l_librbd_rwl_wr_latency_hist,    // Histogram of write req (persist) completion latency vs. bytes written
  l_librbd_rwl_wr_caller_latency,  // average req completion (to caller) latency

  /* Log operation times */
  l_librbd_rwl_log_op_dis_to_buf_t, // dispatch to buffer persist elapsed time
  l_librbd_rwl_log_op_dis_to_app_t, // diapatch to log append elapsed time
  l_librbd_rwl_log_op_dis_to_cmp_t, // dispatch to persist completion elapsed time

  l_librbd_rwl_log_op_buf_to_app_t, // data buf persist + append wait time
  l_librbd_rwl_log_op_buf_to_bufc_t,// data buf persist / replicate elapsed time
  l_librbd_rwl_log_op_buf_to_bufc_t_hist,// data buf persist time vs bytes hisogram
  l_librbd_rwl_log_op_app_to_cmp_t, // log entry append + completion wait time
  l_librbd_rwl_log_op_app_to_appc_t, // log entry append / replicate elapsed time
  l_librbd_rwl_log_op_app_to_appc_t_hist, // log entry append time (vs. op bytes) histogram

  l_librbd_rwl_discard,
  l_librbd_rwl_discard_bytes,
  l_librbd_rwl_discard_latency,

  l_librbd_rwl_aio_flush,
  l_librbd_rwl_aio_flush_latency,
  l_librbd_rwl_ws,
  l_librbd_rwl_ws_bytes,
  l_librbd_rwl_ws_latency,

  l_librbd_rwl_cmp,
  l_librbd_rwl_cmp_bytes,
  l_librbd_rwl_cmp_latency,

  l_librbd_rwl_flush,
  l_librbd_rwl_invalidate_cache,

  l_librbd_rwl_last,
};

namespace librbd {

struct ImageCtx;

namespace cache {

namespace rwl {

static const uint32_t MIN_WRITE_SIZE = 1;
static const uint32_t BLOCK_SIZE = MIN_WRITE_SIZE;
static const uint32_t MIN_MIN_WRITE_ALLOC_SIZE = 512;
static const uint32_t MIN_WRITE_ALLOC_SIZE =
  (MIN_WRITE_SIZE > MIN_MIN_WRITE_ALLOC_SIZE ?
   MIN_WRITE_SIZE : MIN_MIN_WRITE_ALLOC_SIZE);
/* Enables use of dedicated finishers for some RWL work */
static const bool use_finishers = false;

static const int IN_FLIGHT_FLUSH_WRITE_LIMIT = 8;
static const int IN_FLIGHT_FLUSH_BYTES_LIMIT = (1 * 1024 * 1024);

/**** Write log entries ****/

static const unsigned long int MAX_ALLOC_PER_TRANSACTION = 8;
static const unsigned int MAX_CONCURRENT_WRITES = 256;
static const uint64_t DEFAULT_POOL_SIZE = 1u<<30;
//static const uint64_t MIN_POOL_SIZE = 1u<<23;
// force pools to be 1G until thread::arena init issue is resolved
static const uint64_t MIN_POOL_SIZE = DEFAULT_POOL_SIZE;
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
  uint64_t _unused = 0;     /* Padding to 64 bytes */
  WriteLogPmemEntry(uint64_t image_offset_bytes, uint64_t write_bytes)
    : image_offset_bytes(image_offset_bytes), write_bytes(write_bytes),
      entry_valid(0), sync_point(0), sequenced(0), has_data(0), unmap(0) {
  }
  BlockExtent block_extent();
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

static_assert(sizeof(WriteLogPmemEntry) == 64);

struct WriteLogPoolRoot {
  union {
    struct {
      uint8_t layout_version;    /* Version of this structure (RWL_POOL_VERSION) */
    };
    uint64_t _u64;
  } header;
  TOID(struct WriteLogPmemEntry) log_entries;   /* contiguous array of log entries */
  uint32_t block_size;				 /* block size */
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
  bool completed = false;
  bool flushing = false;
  bool flushed = false;
  WriteLogEntry(uint64_t image_offset_bytes, uint64_t write_bytes)
    : ram_entry(image_offset_bytes, write_bytes) {
  }
  WriteLogEntry(const WriteLogEntry&) = delete;
  WriteLogEntry &operator=(const WriteLogEntry&) = delete;
  BlockExtent block_extent();
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

typedef std::list<shared_ptr<WriteLogEntry>> WriteLogEntries;

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
  shared_ptr<WriteLogEntry> log_entry;
  bufferlist bl;
  pobj_action *buffer_alloc_action = nullptr;
  Context *on_write_persist; /* Completion for things waiting on this write to persist */
  utime_t m_dispatch_time; // When op created
  utime_t m_buf_persist_time; // When buffer persist begins
  utime_t m_buf_persist_comp_time; // When buffer persist completes
  utime_t m_log_append_time; // When log append begins
  utime_t m_log_append_comp_time; // When log append completes
  WriteLogOperation(WriteLogOperationSet &set, uint64_t image_offset_bytes, uint64_t write_bytes);
  ~WriteLogOperation();
  WriteLogOperation(const WriteLogOperation&) = delete;
  WriteLogOperation &operator=(const WriteLogOperation&) = delete;
  friend std::ostream &operator<<(std::ostream &os,
				  WriteLogOperation &op) {
    os << "log_entry=[" << *op.log_entry << "], "
       << "bl=[" << op.bl << "],"
       << "buffer_alloc_action=" << op.buffer_alloc_action;
    return os;
  };
  void complete(int r);
};
typedef std::list<shared_ptr<WriteLogOperation>> WriteLogOperations;

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
  utime_t m_dispatch_time; // When set created
  WriteLogOperationSet(CephContext *cct, utime_t dispatched, shared_ptr<SyncPoint> sync_point, bool persist_on_flush,
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
  std::atomic<bool> m_callback_invoked = {false};
  boost::function<void(BlockGuardCell*)> m_callback;
public:
  GuardedRequestFunctionContext(boost::function<void(BlockGuardCell*)> &&callback);
  ~GuardedRequestFunctionContext(void);
  GuardedRequestFunctionContext(const GuardedRequestFunctionContext&) = delete;
  GuardedRequestFunctionContext &operator=(const GuardedRequestFunctionContext&) = delete;
  void finish(int r) override;
  void acquired(BlockGuardCell *cell);
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
  shared_ptr<WriteLogEntry> log_entry;

  WriteLogMapEntry(BlockExtent block_extent,
		   shared_ptr<WriteLogEntry> log_entry = nullptr);
  WriteLogMapEntry(shared_ptr<WriteLogEntry> log_entry);
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
  WriteLogMap(CephContext *cct);
  WriteLogMap(const WriteLogMap&) = delete;
  WriteLogMap &operator=(const WriteLogMap&) = delete;

  void add_log_entry(shared_ptr<WriteLogEntry> log_entry);
  void add_log_entries(WriteLogEntries &log_entries);
  void remove_log_entry(shared_ptr<WriteLogEntry> log_entry);
  void remove_log_entries(WriteLogEntries &log_entries);
  WriteLogEntries find_log_entries(BlockExtent block_extent);
  WriteLogMapEntries find_map_entries(BlockExtent block_extent);

private:
  void add_log_entry_locked(shared_ptr<WriteLogEntry> log_entry);
  void remove_log_entry_locked(shared_ptr<WriteLogEntry> log_entry);
  void add_map_entry_locked(WriteLogMapEntry &map_entry);
  void remove_map_entry_locked(WriteLogMapEntry &map_entry);
  void adjust_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &new_extent);
  void split_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &removed_extent);
  WriteLogEntries find_log_entries_locked(BlockExtent &block_extent);
  WriteLogMapEntries find_map_entries_locked(BlockExtent &block_extent);

  struct WriteLogMapEntryCompare {
    bool operator()(const WriteLogMapEntry &lhs,
		    const WriteLogMapEntry &rhs) const;
  };

  typedef std::set<WriteLogMapEntry,
		   WriteLogMapEntryCompare> BlockExtentToWriteLogMapEntries;

  WriteLogMapEntry block_extent_to_map_key(BlockExtent &block_extent);

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
class ReplicatedWriteLog : public ImageCache<ImageCtxT> {
public:
  using typename ImageCache<ImageCtxT>::Extent;
  using typename ImageCache<ImageCtxT>::Extents;
  ReplicatedWriteLog(ImageCtx &image_ctx, ImageCache<ImageCtxT> *lower);
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

private:
  typedef std::function<void(uint64_t)> ReleaseBlock;
  typedef std::function<void(BlockGuard::BlockIO)> AppendDetainedBlock;
  typedef std::list<Context *> Contexts;
  typedef std::list<C_WriteRequest *> C_WriteRequests;

  void detain_guarded_request(GuardedRequest &&req);
  void release_guarded_request(BlockGuardCell *cell);

  const char* rwl_pool_layout_name = POBJ_LAYOUT_NAME(rbd_rwl);

  ImageCtxT &m_image_ctx;

  std::string m_log_pool_name;
  PMEMobjpool *m_log_pool = nullptr;
  uint64_t m_log_pool_size;

  uint32_t m_total_log_entries = 0;
  uint32_t m_free_log_entries = 0;

  ImageCache<ImageCtxT> *m_image_writeback;
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
  shared_ptr<SyncPoint> m_current_sync_point = nullptr;
  /* Starts at 0 on each sync gen increase. Incremented before applied
     to an operation */
  uint64_t m_last_op_sequence_num = 0;

  bool m_persist_on_write_until_flush = true;
  bool m_persist_on_flush = false; /* If false, persist each write before completion */
  bool m_flush_seen = false;

  util::AsyncOpTracker m_async_op_tracker;

  mutable Mutex m_log_append_lock;
  mutable Mutex m_lock;

  bool m_wake_up_requested = false;
  bool m_wake_up_scheduled = false;
  bool m_wake_up_enabled = true;

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

  /* Writes that have left the block guard, but are waiting for resources */
  C_WriteRequests m_deferred_writes;
  /* Throttle writes concurrently allocating & replicating */
  unsigned int m_free_lanes = MAX_CONCURRENT_WRITES;
  unsigned int m_unpublished_reserves = 0;
  PerfCounters *m_perfcounter = nullptr;

  void perf_start(const std::string name);
  void perf_stop();
  void log_perf();

  void rwl_init(Context *on_finish);
  void wake_up();
  void process_work();
  bool drain_context_list(Contexts &contexts, Mutex &contexts_lock);

  bool can_flush_entry(const shared_ptr<WriteLogEntry> log_entry);
  Context *construct_flush_entry_ctx(shared_ptr<WriteLogEntry> log_entry);
  void process_writeback_dirty_entries();
  bool can_retire_entry(const shared_ptr<WriteLogEntry> log_entry);
  bool retire_entries();

  void invalidate(Extents&& image_extents, Context *on_finish);

  void append_sync_point(shared_ptr<SyncPoint> sync_point, const int prior_write_status);
  void new_sync_point(void);

  void complete_write_req(C_WriteRequest *write_req, const int result);
  void dispatch_deferred_writes(void);
  bool alloc_write_resources(C_WriteRequest *write_req);
  void release_write_lanes(C_WriteRequest *write_req);
  void alloc_and_dispatch_aio_write(C_WriteRequest *write_req);
  void dispatch_aio_write(C_WriteRequest *write_req);
  void append_scheduled_ops(void);
  void schedule_append(WriteLogOperations &ops);
  void flush_then_append_scheduled_ops(void);
  void schedule_flush_and_append(WriteLogOperations &ops);
  void flush_pmem_buffer(WriteLogOperations &ops);
  void alloc_op_log_entries(WriteLogOperations &ops);
  void flush_op_log_entries(WriteLogOperations &ops);
  int append_op_log_entries(WriteLogOperations &ops);
  void complete_op_log_entries(WriteLogOperations &ops, const int r);
};

} // namespace cache
} // namespace librbd

extern template class librbd::cache::ReplicatedWriteLog<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_REPLICATED_WRITE_LOG

/* Local Variables: */
/* eval: (c-set-offset 'innamespace 0) */
/* End: */
