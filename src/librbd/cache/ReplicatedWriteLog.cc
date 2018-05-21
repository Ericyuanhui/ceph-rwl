// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ReplicatedWriteLog.h"
#include "include/buffer.h"
#include "include/Context.h"
#include "common/deleter.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "librbd/ImageCtx.h"
#include "librbd/cache/file/ImageStore.h"
#include "librbd/cache/file/JournalStore.h"
#include "librbd/cache/file/MetaStore.h"
#include "librbd/cache/file/StupidPolicy.h"
#include "librbd/cache/file/Types.h"
#include <map>
#include <vector>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::ReplicatedWriteLog: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {

using namespace librbd::cache::rwl;
using namespace librbd::cache::file;

BlockExtent block_extent(uint64_t offset_bytes, uint64_t length_bytes)
{
  return BlockExtent(offset_bytes / MIN_WRITE_SIZE,
		     ((offset_bytes + length_bytes) / MIN_WRITE_SIZE) - 1);
}

BlockExtent block_extent(ImageCache::Extent& image_extent)
{
  return block_extent(image_extent.first, image_extent.second);
}

ImageCache::Extent image_extent(BlockExtent block_extent)
{
  return ImageCache::Extent(block_extent.block_start * MIN_WRITE_SIZE,
			    (block_extent.block_end - block_extent.block_start + 1) * MIN_WRITE_SIZE);
}

namespace rwl {

SyncPoint::SyncPoint(CephContext *cct, uint64_t sync_gen_num)
  : m_cct(cct), m_sync_gen_num(sync_gen_num) {
  m_prior_log_entries_persisted = new C_Gather(cct, nullptr);
  ldout(cct, 6) << "sync point" << m_sync_gen_num << dendl;
  /* TODO: Connect m_prior_log_entries_persisted finisher to append this
     sync point and on persist complete call on_sync_point_persisted
     and delete this object */
}
SyncPoint::~SyncPoint() {
  /* TODO: will m_prior_log_entries_persisted always delete itself? */
}

WriteLogOperation::WriteLogOperation(WriteLogOperationSet *set, uint64_t image_offset_bytes, uint64_t write_bytes)
  : log_entry(new WriteLogEntry(image_offset_bytes, write_bytes)) {
  on_write_persist = set->m_extent_ops->new_sub();
}

WriteLogOperation::~WriteLogOperation() { }

/* Called when the write log operation is completed in all log replicas */
void WriteLogOperation::complete(int result) {
  on_write_persist->complete(result);
  delete(this);
}

WriteLogOperationSet::WriteLogOperationSet(CephContext *cct, SyncPoint *sync_point, bool persist_on_flush,
					   BlockExtent extent, Context *on_finish)
  : m_cct(cct), m_extent(extent), m_on_finish(on_finish), m_persist_on_flush(persist_on_flush) {
  m_on_ops_persist = sync_point->m_prior_log_entries_persisted->new_sub();
  m_extent_ops =
    new C_Gather(cct,
		 new FunctionContext( [this](int r) {
		     //ldout(m_cct, 6) << "m_extent_ops completed" << dendl;
		     m_on_ops_persist->complete(r);
		     m_on_finish->complete(r);
		     delete(this);
		   }));
}

WriteLogOperationSet::~WriteLogOperationSet() { }

void WriteLogEntry::add_reader() { reader_count++; }
void WriteLogEntry::remove_reader() { reader_count--; }

/**
 * Add a write log entry to the map. Subsequent queries for blocks
 * within this log entry's extent will find this log entry. Portions
 * of prior write log entries overlapping with this log entry will
 * be replaced in the map by this log entry.
 *
 * The map_entries field of the log entry object will be updated to
 * contain this map entry.
 *
 * The map_entries fields of all log entries overlapping with this
 * entry will be updated to remove the regions that overlap with
 * this.
 */
void WriteLogMap::add_log_entry(WriteLogEntry *log_entry) {
  Mutex::Locker locker(m_lock);
  add_log_entry_locked(log_entry);
}

void WriteLogMap::add_log_entries(WriteLogEntries &log_entries) {
  Mutex::Locker locker(m_lock);
  ldout(m_cct, 20) << dendl;
  for (auto &log_entry : log_entries) {
    add_log_entry_locked(log_entry);
  }
}
  
/**
 * Remove any map entries that refer to the supplied write log
 * entry.
 */
void WriteLogMap::remove_log_entry(WriteLogEntry *log_entry) {
  Mutex::Locker locker(m_lock);
  remove_log_entry_locked(log_entry);
}

void WriteLogMap::remove_log_entries(WriteLogEntries &log_entries) {
  Mutex::Locker locker(m_lock);
  ldout(m_cct, 20) << dendl;
  for (auto &log_entry : log_entries) {
    remove_log_entry_locked(log_entry);
  }
}

/**
 * Returns the list of all write log entries that overlap the specified block
 * extent. This doesn't tell you which portions of these entries overlap the
 * extent, or each other. For that, use find_map_entries(). A log entry may
 * appear in the list more than once, if multiple map entries refer to it
 * (e.g. the middle of that write log entry has been overwritten).
 */
WriteLogEntries WriteLogMap::find_log_entries(BlockExtent block_extent) {
  Mutex::Locker locker(m_lock);
  ldout(m_cct, 20) << dendl;
  return find_log_entries_locked(block_extent);
}

/**
 * Returns the list of all write log map entries that overlap the
 * specified block extent.
 */
WriteLogMapEntries WriteLogMap::find_map_entries(BlockExtent block_extent) {
  Mutex::Locker locker(m_lock);
  ldout(m_cct, 20) << dendl;
  return find_map_entries_locked(block_extent);
}
  
void WriteLogMap::add_log_entry_locked(WriteLogEntry *log_entry) {
  WriteLogMapEntry map_entry(log_entry);
  ldout(m_cct, 20) << "block_extent=" << map_entry.block_extent 
		   << dendl;
  assert(m_lock.is_locked_by_me());
  WriteLogMapEntries overlap_entries = find_map_entries_locked(map_entry.block_extent);
  if (overlap_entries.size()) {
    for (auto &entry : overlap_entries) {
      ldout(m_cct, 20) << entry << dendl;
      if (map_entry.block_extent.block_start <= entry.block_extent.block_start) {
	if (map_entry.block_extent.block_end >= entry.block_extent.block_end) {
	  ldout(m_cct, 20) << "map entry completely occluded by new log entry" << dendl;
	  remove_map_entry_locked(entry);
	} else {
	  assert(map_entry.block_extent.block_end < entry.block_extent.block_end);
	  /* The new entry occludes the beginning of the old entry */
	  BlockExtent adjusted_extent(map_entry.block_extent.block_end+1,
				      entry.block_extent.block_end);
	  adjust_map_entry_locked(entry, adjusted_extent);
	}
      } else {
	assert(map_entry.block_extent.block_start > entry.block_extent.block_start);
	if (map_entry.block_extent.block_end >= entry.block_extent.block_end) {
	  /* The new entry occludes the end of the old entry */
	  BlockExtent adjusted_extent(entry.block_extent.block_start,
				      map_entry.block_extent.block_start-1);
	  adjust_map_entry_locked(entry, adjusted_extent);
	} else {
	  /* The new entry splits the old entry */
	  split_map_entry_locked(entry, map_entry.block_extent);
	}
      }
    }
  }
  add_map_entry_locked(map_entry);
}

void WriteLogMap::remove_log_entry_locked(WriteLogEntry *log_entry) {
  ldout(m_cct, 20) << "*log_entry=" << *log_entry << dendl;
  assert(m_lock.is_locked_by_me());

  BlockExtent log_entry_extent(log_entry->block_extent());
  WriteLogMapEntries possible_hits = find_map_entries_locked(log_entry_extent);
  for (auto &possible_hit : possible_hits) {
    if (possible_hit.log_entry == log_entry) {
      /* This map entry refers to the specified log entry */
      remove_map_entry_locked(possible_hit);
    }
  }
}

void WriteLogMap::add_map_entry_locked(WriteLogMapEntry &map_entry)
{
  m_block_to_log_entry_map.insert(map_entry);
  map_entry.log_entry->referring_map_entries++;
}

void WriteLogMap::remove_map_entry_locked(WriteLogMapEntry &map_entry)
{
  auto it = m_block_to_log_entry_map.find(map_entry);
  assert(it != m_block_to_log_entry_map.end());

  WriteLogMapEntry erased = *it;
  m_block_to_log_entry_map.erase(it);
  erased.log_entry->referring_map_entries--;
  if (0 == erased.log_entry->referring_map_entries) {
    ldout(m_cct, 20) << "log entry has zero map entries: " << erased.log_entry << dendl;
  }
}

void WriteLogMap::adjust_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &new_extent)
{
  auto it = m_block_to_log_entry_map.find(map_entry);
  assert(it != m_block_to_log_entry_map.end());

  WriteLogMapEntry adjusted = *it;
  m_block_to_log_entry_map.erase(it);

  m_block_to_log_entry_map.insert(WriteLogMapEntry(new_extent, adjusted.log_entry));
}

void WriteLogMap::split_map_entry_locked(WriteLogMapEntry &map_entry, BlockExtent &removed_extent)
{
  auto it = m_block_to_log_entry_map.find(map_entry);
  assert(it != m_block_to_log_entry_map.end());

  WriteLogMapEntry split = *it;
  m_block_to_log_entry_map.erase(it);

  BlockExtent left_extent(split.block_extent.block_start,
			  removed_extent.block_start-1);
  m_block_to_log_entry_map.insert(WriteLogMapEntry(left_extent, split.log_entry));

  BlockExtent right_extent(removed_extent.block_end+1,
			   split.block_extent.block_end);
  m_block_to_log_entry_map.insert(WriteLogMapEntry(right_extent, split.log_entry));

  split.log_entry->referring_map_entries++;
}

WriteLogEntries WriteLogMap::find_log_entries_locked(BlockExtent &block_extent) {
  WriteLogEntries overlaps;
  ldout(m_cct, 20) << "block_extent=" << block_extent << dendl;
  
  assert(m_lock.is_locked_by_me());
  WriteLogMapEntries map_entries = find_map_entries_locked(block_extent);
  for (auto &map_entry : map_entries) {
    overlaps.emplace_back(map_entry.log_entry);
  }
  return overlaps;
}

/**
 * TODO: Generalize this to do some arbitrary thing to each map
 * extent, instead of returning a list.
 */
WriteLogMapEntries WriteLogMap::find_map_entries_locked(BlockExtent &block_extent) {
  WriteLogMapEntries overlaps;
  
  ldout(m_cct, 20) << "block_extent=" << block_extent << dendl;
  assert(m_lock.is_locked_by_me());
  auto p = m_block_to_log_entry_map.equal_range(WriteLogMapEntry(block_extent));
  ldout(m_cct, 20) << "count=" << std::distance(p.first, p.second) << dendl;
  for ( auto i = p.first; i != p.second; ++i ) {
    WriteLogMapEntry entry = *i;
    overlaps.emplace_back(entry);
    ldout(m_cct, 20) << entry << dendl;
  }
  return overlaps;
}

bool is_block_aligned(const ImageCache::Extents &image_extents) {
  for (auto &extent : image_extents) {
    if (extent.first % MIN_WRITE_SIZE != 0 || extent.second % MIN_WRITE_SIZE != 0) {
      return false;
    }
  }
  return true;
}

/**
 * A chain of requests that stops on the first failure
 */
struct C_BlockIORequest : public Context {
  CephContext *m_cct;
  C_BlockIORequest *next_block_request;

  C_BlockIORequest(CephContext *cct, C_BlockIORequest *next_block_request = nullptr)
    : m_cct(cct), next_block_request(next_block_request) {
    ldout(m_cct, 99) << this << dendl;
  }
  ~C_BlockIORequest() {
    ldout(m_cct, 99) << this << dendl;
  }

  virtual void finish(int r) override {
    ldout(m_cct, 20) << "(" << get_name() << "): r=" << r << dendl;

    if (next_block_request) {
      if (r < 0) {
	// abort the chain of requests upon failure
	next_block_request->complete(r);
      } else {
	// execute next request in chain
	next_block_request->send();
      }
    }
  }

  virtual void send() = 0;
  virtual const char *get_name() const = 0;
};

/**
 * A request that can be deferred in a BlockGuard to sequence
 * overlapping operatoins.
 */
struct C_GuardedBlockIORequest : public C_BlockIORequest {
private:
  CephContext *m_cct;
  BlockGuardCell* m_cell = nullptr;
public:
  C_GuardedBlockIORequest(CephContext *cct, C_BlockIORequest *next_block_request = nullptr)
    : C_BlockIORequest(cct, next_block_request), m_cct(cct) {
    ldout(m_cct, 99) << this << dendl;
  }
  ~C_GuardedBlockIORequest() {
    ldout(m_cct, 99) << this << dendl;
  }
  C_GuardedBlockIORequest(const C_GuardedBlockIORequest&) = delete;
  C_GuardedBlockIORequest &operator=(const C_GuardedBlockIORequest&) = delete;

  virtual void send() = 0;
  virtual const char *get_name() const = 0;
  void set_cell(BlockGuardCell *cell) {
    ldout(m_cct, 20) << this << dendl;
    assert(cell);
    m_cell = cell;
  }
  BlockGuardCell *get_cell(void) {
    ldout(m_cct, 20) << this << dendl;
    return m_cell;
  }
};

} // namespace rwl

template <typename I>
ReplicatedWriteLog<I>::ReplicatedWriteLog(ImageCtx &image_ctx, StackingImageCache<I> *lower)
  : m_image_ctx(image_ctx),
    m_log_pool_size(DEFAULT_POOL_SIZE),
    m_image_writeback(lower), m_write_log_guard(image_ctx.cct),
    m_log_append_lock("librbd::cache::ReplicatedWriteLog::m_log_append_lock"),
    m_lock("librbd::cache::ReplicatedWriteLog::m_lock"),
    m_persist_finisher(image_ctx.cct),
    m_log_append_finisher(image_ctx.cct),
    m_on_persist_finisher(image_ctx.cct),
    m_blocks_to_log_entries(image_ctx.cct) {
  assert(lower);
  m_persist_finisher.start();
  m_log_append_finisher.start();
  m_on_persist_finisher.start();
}

template <typename I>
ReplicatedWriteLog<I>::~ReplicatedWriteLog() {
  m_persist_finisher.wait_for_empty();
  m_log_append_finisher.wait_for_empty();
  m_on_persist_finisher.wait_for_empty();
  delete m_image_writeback;
}

template <typename ExtentsType>
class ExtentsSummary {
public:
  uint64_t total_bytes;
  uint64_t first_image_byte;
  uint64_t last_image_byte;
  uint64_t first_block;
  uint64_t last_block;
  ExtentsSummary(const ExtentsType &extents) {
    total_bytes = 0;
    first_image_byte = 0;
    last_image_byte = 0;
    first_block = 0;
    last_block = 0;
    if (extents.empty()) return;
    first_image_byte = extents.front().first;
    last_image_byte = first_image_byte + extents.front().second;
    for (auto &extent : extents) {
      total_bytes += extent.second;
      if (extent.first < first_image_byte) {
	first_image_byte = extent.first;
      }
      if ((extent.first + extent.second) > last_image_byte) {
	last_image_byte = extent.first + extent.second;
      }
    }
    first_block = first_image_byte / MIN_WRITE_SIZE;
    last_block = last_image_byte / MIN_WRITE_SIZE;
  }
};

struct ImageExtentBuf : public ImageCache::Extent {
public:
  buffer::raw *m_buf;
  ImageExtentBuf(ImageCache::Extent extent, buffer::raw *buf = nullptr)
    : ImageCache::Extent(extent), m_buf(buf) {}
};
typedef std::vector<ImageExtentBuf> ImageExtentBufs;

struct C_ReadRequest : public Context {
  CephContext *m_cct;
  Context *m_on_finish;
  ImageCache::Extents m_miss_extents; // move back to caller
  ImageExtentBufs m_read_extents;
  bufferlist m_miss_bl;
  bufferlist *m_out_bl;
  
  C_ReadRequest(CephContext *cct, bufferlist *out_bl, Context *on_finish)
    : m_cct(cct), m_on_finish(on_finish), m_out_bl(out_bl) {
    ldout(m_cct, 99) << this << dendl;
  }
  ~C_ReadRequest() {
    ldout(m_cct, 99) << this << dendl;
  }

  virtual void finish(int r) override {
    ldout(m_cct, 20) << "(" << get_name() << "): r=" << r << dendl;
    if (r >= 0) {
      /*
       * At this point the miss read has completed. We'll iterate through
       * m_read_extents and produce *m_out_bl by assembling pieces of m_miss_bl
       * and the individual hit extent bufs in the read extents that represent
       * hits.
       */
      uint64_t miss_bl_offset = 0;
      for (auto &extent : m_read_extents) {
	if (extent.m_buf) {
	  /* This was a hit */
	  bufferlist hit_extent_bl;
	  hit_extent_bl.append(extent.m_buf);
	  m_out_bl->claim_append(hit_extent_bl);
	} else {
	  /* This was a miss. */
	  bufferlist miss_extent_bl;
	  miss_extent_bl.substr_of(m_miss_bl, miss_bl_offset, extent.second);
	  /* Add this read miss bullerlist to the output bufferlist */
	  m_out_bl->claim_append(miss_extent_bl);
	  /* Consume these bytes in the read miss bufferlist */
	  miss_bl_offset += extent.second;
	}
      }
    }
    ldout(m_cct, 20) << "(" << get_name() << "): r=" << r << " bl=" << *m_out_bl << dendl;
    m_on_finish->complete(r);
  }
  
  virtual const char *get_name() const {
    return "C_ReadRequest";
  }
}; 
 
template <typename I>
void ReplicatedWriteLog<I>::aio_read(Extents &&image_extents, bufferlist *bl,
                                 int fadvise_flags, Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  C_ReadRequest *read_ctx = new C_ReadRequest(cct, bl, on_finish);
  ldout(cct, 5) << "image_extents=" << image_extents << ", "
		<< "bl=" << bl << ", "
		<< "on_finish=" << on_finish << dendl;

  bl->clear();

  // TODO: Handle unaligned IO.
  if (!is_block_aligned(image_extents)) {
    lderr(cct) << "unaligned read fails" << dendl;
    for (auto &extent : image_extents) {
      lderr(cct) << "start: " << extent.first << " length: " << extent.second << dendl;
    }
    on_finish->complete(-EINVAL);
    return;
  }

  // TODO handle fadvise flags

  /*
   * The strategy here is to look up all the WriteLogMapEntries that overlap
   * this read, and iterate through those to separate this read into hits and
   * misses. A new Extents object is produced here with Extents for each miss
   * region. The miss Extents is then passed on to the read cache below RWL. We
   * also produce an ImageExtentBufs for all the extents (hit or miss) in this
   * read. When the read from the lower cache layer completes, we iterate
   * through the ImageExtentBufs and insert buffers for each cache hit at the
   * appropriate spot in the buferlist returned from below for the miss
   * read. The buffers we insert here refer directly to regions of various
   * write log entry data buffers.
   *
   * TBD: These buffer objects hold a reference on those write log entries to
   * prevent them from being retired from the log while the read is
   * completing. The WriteLogEntry references are released by the buffer
   * destructor.
   */
  for (auto &extent : image_extents) {
    uint64_t extent_offset = 0;
    WriteLogMapEntries map_entries = m_blocks_to_log_entries.find_map_entries(block_extent(extent));
    for (auto &entry : map_entries) {
      ImageCache::Extent entry_image_extent(image_extent(entry.block_extent));
      /* If this map entry starts after the current image extent offset ... */
      if (entry_image_extent.first > extent.first + extent_offset) {
	/* ... add range before map_entry to miss extents */
	uint64_t miss_extent_start = extent.first + extent_offset;
	uint64_t miss_extent_length = entry_image_extent.first - miss_extent_start;
	ImageCache::Extent miss_extent(miss_extent_start, miss_extent_length);
	read_ctx->m_miss_extents.push_back(miss_extent);
	/* Add miss range to read extents */
	ImageExtentBuf miss_extent_buf(miss_extent);
	read_ctx->m_read_extents.push_back(miss_extent_buf);
	extent_offset += miss_extent_length;
      }
      assert(entry_image_extent.first <= extent.first + extent_offset);
      uint64_t entry_offset = 0;
      /* If this map entry starts before the current image extent offset ... */
      if (entry_image_extent.first < extent.first + extent_offset) {
	/* ... compute offset into log entry for this read extent */
	entry_offset = (extent.first + extent_offset) - entry_image_extent.first;
      }
      /* This read hit ends at the end of the extent or the end of the log
	 entry, whichever is less. */
      uint64_t entry_hit_length = min(entry_image_extent.second - entry_offset,
				      extent.second - extent_offset);
      ImageCache::Extent hit_extent(entry_image_extent.first, entry_hit_length);
      /* Offset of the map entry into the log entry's buffer */
      uint64_t map_entry_buffer_offset = entry_image_extent.first - entry.log_entry->ram_entry.image_offset_bytes;
      /* Offset into the log entry buffer of this read hit */
      uint64_t read_buffer_offset = map_entry_buffer_offset + entry_offset;
      /* Create buffer object referring to pmem pool for this read hit */
      WriteLogEntry *log_entry = entry.log_entry;
      ldout(cct, 5) << "adding reader: log_entry=" << *log_entry << dendl;
      log_entry->add_reader();
      buffer::raw *hit_buf =
	buffer::claim_buffer(entry_hit_length,
			     (char*)(log_entry->pmem_buffer + read_buffer_offset),
			     make_deleter([this, log_entry]
					  {
					    CephContext *cct = m_image_ctx.cct;
					    ldout(cct, 5) << "removing reader: log_entry="
							  << *log_entry << dendl;
					    log_entry->remove_reader();
					  }));
      /* Add hit extent to read extents */
      ImageExtentBuf hit_extent_buf(hit_extent, hit_buf);
      read_ctx->m_read_extents.push_back(hit_extent_buf);
      /* Exclude RWL hit range from buffer and extent */
      extent_offset += entry_hit_length;
      ldout(cct, 20) << entry << dendl;
    }
    /* If the last map entry didn't consume the entire image extent ... */
    if (extent.second > extent_offset) {
      /* ... add the rest of this extent to miss extents */
      uint64_t miss_extent_start = extent.first + extent_offset;
      uint64_t miss_extent_length = extent.second - extent_offset;
      ImageCache::Extent miss_extent(miss_extent_start, miss_extent_length);
      read_ctx->m_miss_extents.push_back(miss_extent);
      /* Add miss range to read extents */
      ImageExtentBuf miss_extent_buf(miss_extent);
      read_ctx->m_read_extents.push_back(miss_extent_buf);
      extent_offset += miss_extent_length;
    }
  }
  
  ldout(cct, 5) << "miss_extents=" << read_ctx->m_miss_extents << ", "
		<< "miss_bl=" << read_ctx->m_miss_bl << dendl;

  if (read_ctx->m_miss_extents.empty()) {
    /* All of this read comes from RWL */
    read_ctx->complete(0);
  } else {
    /* Pass the read misses on to the layer below RWL */
    m_image_writeback->aio_read(std::move(read_ctx->m_miss_extents), &read_ctx->m_miss_bl, fadvise_flags, read_ctx);
  }
}

template <typename I>
void ReplicatedWriteLog<I>::detain_guarded_request(GuardedRequest &&req)
{
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  
  BlockGuardCell *cell;
  int r = m_write_log_guard.detain({req.first_block_num, req.last_block_num},
				   &req, &cell);
  assert(r>=0);
  if (r > 0) { 
    ldout(cct, 6) << "detaining guarded request due to in-flight requests: "
                   << "start=" << req.first_block_num << ", "
		   << "end=" << req.last_block_num << dendl;
    return;
  }

  ldout(cct, 6) << "in-flight request cell: " << cell << dendl;
  req.on_guard_acquire->acquired(cell);
}

template <typename I>
void ReplicatedWriteLog<I>::release_guarded_request(BlockGuardCell *cell)
{
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 6) << "cell=" << cell << dendl;

  WriteLogGuard::BlockOperations block_ops;
  m_write_log_guard.release(cell, &block_ops);

  for (auto &op : block_ops) {
    detain_guarded_request(std::move(op));
  }
}

/**
 * This is the custodian of the BlockGuard cell for this write, and
 * the state information about the progress of this write. This object
 * lives until the write is persisted in all (live) log replicas.
 * User write op may be completed from here before the write
 * persists. Block guard is not released until the write persists
 * everywhere (this is how we guarantee to each log repica that they
 * will never see overlapping writes).
 */
struct C_WriteRequest : public C_GuardedBlockIORequest {
  CephContext *m_cct;
  ImageCache::Extents m_image_extents;
  bufferlist bl;
  int fadvise_flags;
  Context *user_req; /* User write request */
  Context *_on_finish = nullptr; /* Block guard release */
  std::atomic<bool> m_user_req_completed = {false};
  ExtentsSummary<ImageCache::Extents> m_image_extents_summary;
  C_WriteRequest(CephContext *cct, ImageCache::Extents &&image_extents,
		 bufferlist&& bl, int fadvise_flags, Context *user_req)
    : C_GuardedBlockIORequest(cct), m_cct(cct), m_image_extents(std::move(image_extents)),
      bl(std::move(bl)), fadvise_flags(fadvise_flags),
      user_req(user_req), m_image_extents_summary(m_image_extents) {
    ldout(m_cct, 99) << this << dendl;
  }

  ~C_WriteRequest() {
    ldout(m_cct, 99) << this << dendl;
  }

  void complete_user_request(int r) {
    bool initial = false;
    if (m_user_req_completed.compare_exchange_strong(initial, true)) {
      ldout(m_cct, 15) << this << " completing user req" << dendl;
      user_req->complete(r);
    } else {
      ldout(m_cct, 20) << this << " user req already completed" << dendl;
    }
  }

  virtual void send() override {
    /* Should never be called */
    ldout(m_cct, 6) << this << " unexpected" << dendl;
  }
  
  virtual void finish(int r) {
    ldout(m_cct, 6) << this << dendl;

    complete_user_request(r);
    _on_finish->complete(0);
  }

  virtual const char *get_name() const override {
    return "C_WriteRequest";
  }  
};

/*
 * Performs the log event append operation for all of the scheduled
 * events.
 */
template <typename I>
void ReplicatedWriteLog<I>::append_scheduled_ops(void)
{
  WriteLogOperations ops;
  int append_result = 0;

  {
    Mutex::Locker locker(m_log_append_lock);

    {
      Mutex::Locker locker(m_lock);
      std::swap(ops, m_ops_to_append);
    }
    
    if (ops.size()) {
      alloc_op_log_entries(ops);
      append_result = append_op_log_entries(ops);
    }
  }

  if (ops.size()) {
    complete_op_log_entries(ops, append_result);
    {
      Mutex::Locker locker(m_lock);
      /* New entries may be flushable */
      wake_up();
    }
  }
}

/*
 * Takes custody of ops. They'll all get their log entries appended,
 * and have their on_write_persist contexts completed once they and
 * all prior log entries are persisted everywhere.
 */
template <typename I>
void ReplicatedWriteLog<I>::schedule_append(WriteLogOperations &ops)
{
  CephContext *cct = m_image_ctx.cct;
  bool need_finisher;
  int num_to_append;
  {
    Mutex::Locker locker(m_lock);

    need_finisher = m_ops_to_append.empty();
    m_ops_to_append.splice(m_ops_to_append.end(), ops);
    num_to_append = m_ops_to_append.size();
  }

  ldout(cct, 6) << "ops_to_append=" << num_to_append << dendl;

  if (need_finisher) {
    m_log_append_finisher.queue(new FunctionContext([this](int r) {
	  append_scheduled_ops();
	}));
  }
}

/*
 * Performs the pmem buffer flush on all scheduled ops, then schedules
 * the log event append operation for all of them.
 */
template <typename I>
void ReplicatedWriteLog<I>::flush_then_append_scheduled_ops(void)
{
  WriteLogOperations ops;
  {
    Mutex::Locker locker(m_lock);
    std::swap(ops, m_ops_to_flush);
  }

  /* Ops subsequently scheduled for flush may finish before these,
   * which is fine. We're unconcerned with completion order until we
   * get to the log message append step. */
  if (ops.size()) {
    flush_pmem_buffer(ops);
    schedule_append(ops);
  }
}

/*
 * Takes custody of ops. They'll all get their pmem blocks flushed,
 * then get their log entries appended.
 */
template <typename I>
void ReplicatedWriteLog<I>::schedule_flush_and_append(WriteLogOperations &ops)
{
  CephContext *cct = m_image_ctx.cct;
  bool need_finisher;
  int num_to_flush;
  {
    Mutex::Locker locker(m_lock);

    need_finisher = m_ops_to_flush.empty();
    m_ops_to_flush.splice(m_ops_to_flush.end(), ops);
    num_to_flush = m_ops_to_flush.size();
  }

  ldout(cct, 6) << "ops_to_flush=" << num_to_flush << dendl;

  if (need_finisher) {
    m_persist_finisher.queue(new FunctionContext([this](int r) {
	  flush_then_append_scheduled_ops();
	}));
  }
}

/*
 * Flush the pmem regions for the data blocks of a set of operations
 */
template <typename I>
void ReplicatedWriteLog<I>::flush_pmem_buffer(WriteLogOperations &ops)
{
  for (auto &operation : ops) {
    pmemobj_flush(m_log_pool, operation->log_entry->pmem_buffer, operation->log_entry->ram_entry.write_bytes);
  }
  /* Drain once for all */
  pmemobj_drain(m_log_pool);
}

/*
 * Allocate the (already resrved) write log entries for a set of operations.
 *
 * Locking:
 * Acquires m_lock
 */
template <typename I>
void ReplicatedWriteLog<I>::alloc_op_log_entries(WriteLogOperations &ops)
{
  TOID(struct WriteLogPoolRoot) pool_root;
  pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);
  struct WriteLogPmemEntry *pmem_log_entries = D_RW(D_RW(pool_root)->log_entries);

  /* Allocate the (already reserved) log entries */
  {
    Mutex::Locker locker(m_lock);

    for (auto &operation : ops) {
      operation->log_entry->log_entry_index = m_first_free_entry;
      m_first_free_entry = (m_first_free_entry + 1) % m_total_log_entries;
      operation->log_entry->pmem_entry = &pmem_log_entries[operation->log_entry->log_entry_index];
      operation->log_entry->ram_entry.entry_valid = 1;
      m_log_entries.push_back(operation->log_entry);
      m_dirty_log_entries.push_back(operation->log_entry);
    }
  }
}

/*
 * Flush the persistent write log entries set of ops. The entries must
 * be contiguous in persistemt memory.
 */
template <typename I>
void ReplicatedWriteLog<I>::flush_op_log_entries(WriteLogOperations &ops)
{
  if (ops.empty()) return;

  if (ops.size() > 1) {
    assert(ops.front()->log_entry->pmem_entry < ops.back()->log_entry->pmem_entry);
  }
  
  pmemobj_flush(m_log_pool,
		ops.front()->log_entry->pmem_entry,
		ops.size() * sizeof(*(ops.front()->log_entry->pmem_entry)));
}

/*
 * Write and persist the (already allocated) write log entries and
 * data buffer allocations for a set of ops. The data buffer for each
 * of these must already have been persisted to its reserved area.
 */
template <typename I>
int ReplicatedWriteLog<I>::append_op_log_entries(WriteLogOperations &ops)
{
  CephContext *cct = m_image_ctx.cct;
  WriteLogOperations entries_to_flush;
  TOID(struct WriteLogPoolRoot) pool_root;
  pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);
  
  if (ops.empty()) return 0;

  /* Write log entries to ring and persist */
  for (auto &operation : ops) {
    if (!entries_to_flush.empty()) {
      /* Flush these and reset the list if the current entry wraps to the 
       * tail of the ring */
      if (entries_to_flush.back()->log_entry->log_entry_index >
	  operation->log_entry->log_entry_index) {
	flush_op_log_entries(entries_to_flush);
	entries_to_flush.clear();
      }
    }
    *operation->log_entry->pmem_entry = operation->log_entry->ram_entry;
    entries_to_flush.push_back(operation);
  }
  flush_op_log_entries(entries_to_flush);
  
  /* Drain once for all */
  pmemobj_drain(m_log_pool);

  /* 
   * Atomically advance the log head pointer and publish the
   * allocations for all the data buffers they refer to.
   */
  TX_BEGIN(m_log_pool) {
    D_RW(pool_root)->first_free_entry = m_first_free_entry;
    for (auto &operation : ops) {
      pmemobj_tx_publish(&operation->buffer_alloc_action, 1);
    }  
  } TX_ONCOMMIT {
  } TX_ONABORT {
    lderr(cct) << "failed to commit log entries (" << m_log_pool_name << ")" << dendl;
    return(-EIO);
  } TX_FINALLY {
  } TX_END;
  
  return 0;
}

/*
 * Complete a set of write ops with the result of append_op_entries.
 */
template <typename I>
void ReplicatedWriteLog<I>::complete_op_log_entries(WriteLogOperations &ops, int result)
{
  for (auto &operation : ops) {
    WriteLogOperation *op = operation;
    m_on_persist_finisher.queue(new FunctionContext([this, op, result](int r) {
	  op->complete(result);
	}));
  }
}

template <typename I>
void ReplicatedWriteLog<I>::complete_write_req(C_WriteRequest *write_req, int result)
{
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 6) << "write_req=" << write_req << dendl;
  ldout(cct, 6) << "write_req=" << write_req << " cell=" << write_req->get_cell() << dendl;
  assert(write_req->get_cell());
  release_guarded_request(write_req->get_cell());
}

/**
 * Takes custody of write_req.
 *
 * Locking:
 * Acquires m_lock
 */
template <typename I>
void ReplicatedWriteLog<I>::dispatch_aio_write(C_WriteRequest *write_req)
{
  CephContext *cct = m_image_ctx.cct;
  WriteLogEntries log_entries;
  WriteLogOperationSet *set;
  
  TOID(struct WriteLogPoolRoot) pool_root;
  pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);

  ldout(cct, 6) << "write_req=" << write_req << " cell=" << write_req->get_cell() << dendl;
  ldout(cct,6) << "bl=[" << write_req->bl << "]" << dendl;

  {
    uint64_t buffer_offset = 0;
    Mutex::Locker locker(m_lock);
    set = new WriteLogOperationSet(cct, m_current_sync_point, m_persist_on_flush,
				   BlockExtent(write_req->m_image_extents_summary.first_block,
					       write_req->m_image_extents_summary.last_block),
				   new C_OnFinisher(write_req, &m_on_persist_finisher));
    /* TODO: If there isn't space for this whole write, defer it */
    if (m_free_log_entries < write_req->m_image_extents.size()) {
      ldout(cct, 6) << "wait for " << write_req->m_image_extents.size() << " log entries" << dendl;
    } else {
      for (auto &extent : write_req->m_image_extents) {
	/* operation->on_write_persist connected to m_prior_log_entries_persisted Gather */
	WriteLogOperation *operation =
	  new WriteLogOperation(set, extent.first, extent.second);
	set->operations.emplace_back(operation);
	log_entries.emplace_back(operation->log_entry);

	/* Reserve both log entry and data block space */
	m_free_log_entries--;

	/* Allocate data blocks */
	operation->log_entry->ram_entry.has_data = 1;
	/* TODO: Handle failure */
	unsigned int alloc_bytes = MIN_WRITE_ALLOC_SIZE;
	if (operation->log_entry->ram_entry.write_bytes > alloc_bytes) {
	  alloc_bytes = operation->log_entry->ram_entry.write_bytes;
	}
	operation->log_entry->ram_entry.write_data = pmemobj_reserve(m_log_pool,
								     &operation->buffer_alloc_action,
								     alloc_bytes,
								     0 /* Object type */);
	if (TOID_IS_NULL(operation->log_entry->ram_entry.write_data)) {
	  lderr(cct) << "pmemobj_reserve() failed: " << pmemobj_errormsg() << dendl;
	}
	assert(!TOID_IS_NULL(operation->log_entry->ram_entry.write_data));
	operation->log_entry->pmem_buffer = D_RW(operation->log_entry->ram_entry.write_data);
	operation->log_entry->ram_entry.sync_gen_number = m_current_sync_gen;
	if (set->m_persist_on_flush) {
	  /* Persist on flush. Sequence #0 is never used. */
	  operation->log_entry->ram_entry.write_sequence_number = 0;
	} else {
	  /* Persist on write */
	  operation->log_entry->ram_entry.write_sequence_number = ++m_last_op_sequence_num;
	  operation->log_entry->ram_entry.sequenced = 1;
	}
	operation->log_entry->ram_entry.sync_point = 0;
	operation->log_entry->ram_entry.unmap = 0;
	operation->bl.substr_of(write_req->bl, buffer_offset,
				operation->log_entry->ram_entry.write_bytes);
	buffer_offset += operation->log_entry->ram_entry.write_bytes;
	ldout(cct, 6) << "operation=[" << *operation << "]" << dendl;
      }
    }
  }

  m_async_op_tracker.start_op();
  write_req->_on_finish =
    new FunctionContext([this, write_req](int r) {
	complete_write_req(write_req, r);
	m_async_op_tracker.finish_op();
      });

  /* All extent ops subs created */
  set->m_extent_ops->activate();
  
  /* Write data */
  for (auto &operation : set->operations) {
    bufferlist::iterator i(&operation->bl);
    ldout(cct, 6) << operation->bl << dendl;
    i.copy((unsigned)operation->log_entry->ram_entry.write_bytes, (char*)operation->log_entry->pmem_buffer);
  }

  m_blocks_to_log_entries.add_log_entries(log_entries);
  
  if (set->m_persist_on_flush) {
    /* 
     * We're done with the caller's buffer, and not guaranteeing
     * persistence until the next flush. The block guard for this
     * write_req will not be released until the write is persisted
     * everywhere.
     */
    write_req->complete_user_request(0);
  }
  
  WriteLogOperations ops_copy = set->operations;
  schedule_flush_and_append(ops_copy);
}

template <typename I>
void ReplicatedWriteLog<I>::aio_write(Extents &&image_extents,
                                  bufferlist&& bl,
                                  int fadvise_flags,
                                  Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  //ldout(cct, 20) << "image_extents=" << image_extents << ", "
  //               << "on_finish=" << on_finish << dendl;

  {
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.snap_id != CEPH_NOSNAP || m_image_ctx.read_only) {
      on_finish->complete(-EROFS);
      return;
    }
  }

  // TODO: Handle unaligned IO. RMW must block overlapping writes (to
  // the limits of the read phase) and wait for prior overlaping
  // writes to complete.
  if (!is_block_aligned(image_extents)) {
    lderr(cct) << "unaligned write fails" << dendl;
    for (auto &extent : image_extents) {
      lderr(cct) << "start: " << extent.first << " length: " << extent.second << dendl;
    }
    on_finish->complete(-EINVAL);
    return;
  }

  C_WriteRequest *write_req =
    new C_WriteRequest(cct, std::move(image_extents), std::move(bl), fadvise_flags, on_finish);

  /* The lambda below will be called when the block guard for all
   * blocks affected by this write is obtained */
  GuardedRequestFunctionContext *guarded_ctx =
    new GuardedRequestFunctionContext([this, write_req](BlockGuardCell *cell) {
      CephContext *cct = m_image_ctx.cct;
      ldout(cct, 6) << "write_req=" << write_req << " cell=" << cell << dendl;

      assert(cell);
      write_req->set_cell(cell);

      /* TODO: Defer write_req until log resources available */
      dispatch_aio_write(write_req);
    });

  GuardedRequest guarded(write_req->m_image_extents_summary.first_block,
			 write_req->m_image_extents_summary.last_block,
			 guarded_ctx);
  detain_guarded_request(std::move(guarded));
}

template <typename I>
void ReplicatedWriteLog<I>::aio_discard(uint64_t offset, uint64_t length,
					bool skip_partial_discard, Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "on_finish=" << on_finish << dendl;

  {
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.snap_id != CEPH_NOSNAP || m_image_ctx.read_only) {
      on_finish->complete(-EROFS);
      return;
    }
  }

  if (!is_block_aligned({{offset, length}})) {
    // For clients that don't use LBA extents, re-align the discard request
    // to work with the cache
    ldout(cct, 20) << "aligning discard to block size" << dendl;

    // TODO: for non-aligned extents, invalidate the associated block-aligned
    // regions in the cache (if any), send the aligned extents to the cache
    // and the un-aligned extents directly to back to librbd
  }

  // TODO invalidate discard blocks until writethrough/back support added
  C_Gather *ctx = new C_Gather(cct, on_finish);
  Context *invalidate_done_ctx = ctx->new_sub();

  m_image_writeback->aio_discard(offset, length, skip_partial_discard, ctx->new_sub());

  ctx->activate();

  Context *invalidate_ctx = new FunctionContext(
    [this, offset, length, invalidate_done_ctx](int r) {
      invalidate({{offset, length}}, invalidate_done_ctx);
    });
  flush(invalidate_ctx);
}

template <typename I>
void ReplicatedWriteLog<I>::aio_flush(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "on_finish=" << on_finish << dendl;

  {
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.snap_id != CEPH_NOSNAP || m_image_ctx.read_only) {
      on_finish->complete(-EROFS);
      return;
    }
  }

  if (!m_flush_seen) {
    Mutex::Locker locker(m_lock);
    if (!m_flush_seen) {
      ldout(cct, 5) << "flush seen" << dendl;
      m_flush_seen = true;
      if (!m_persist_on_flush && m_persist_on_write_until_flush) {
	m_persist_on_flush = true;
	ldout(cct, 5) << "now persisting on flush" << dendl;
      }
    }
  }

  /* 
   * TODO: if persist_on_flush, create a new sync point if there have been
   * writes since the last one. If The current sync point isn't persisted,
   * complete this flush when it is. Otherwise complete this flush now.
   *
   * We do not flush the caches below the RWL here.
   */

  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      if (r < 0) {
        on_finish->complete(r);
      }
      m_image_writeback->aio_flush(on_finish);
    });

  flush(ctx);
}

template <typename I>
void ReplicatedWriteLog<I>::aio_writesame(uint64_t offset, uint64_t length,
                                      bufferlist&& bl, int fadvise_flags,
                                      Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "data_len=" << bl.length() << ", "
                 << "on_finish=" << on_finish << dendl;
  {

    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.snap_id != CEPH_NOSNAP || m_image_ctx.read_only) {
      on_finish->complete(-EROFS);
      return;
    }
  }

  m_image_writeback->aio_writesame(offset, length, std::move(bl), fadvise_flags, on_finish);

  bufferlist total_bl;

  uint64_t left = length;
  while(left) {
    total_bl.append(bl);
    left -= bl.length();
  }
  assert(length == total_bl.length());
  aio_write({{offset, length}}, std::move(total_bl), fadvise_flags, on_finish);
}

template <typename I>
void ReplicatedWriteLog<I>::aio_compare_and_write(Extents &&image_extents,
                                                     bufferlist&& cmp_bl,
                                                     bufferlist&& bl,
                                                     uint64_t *mismatch_offset,
                                                     int fadvise_flags,
                                                     Context *on_finish) {

  // TODO:
  // Compare source may be RWL, image cache, or image.
  // Write will be to RWL

  //CephContext *cct = m_image_ctx.cct;

  //ldout(cct, 20) << "image_extents=" << image_extents << ", "
  //               << "on_finish=" << on_finish << dendl;

  m_image_writeback->aio_compare_and_write(
    std::move(image_extents), std::move(cmp_bl), std::move(bl), mismatch_offset,
    fadvise_flags, on_finish);
}

/**
 * Called when the specified sync point can be appended to the log
 */
template <typename I>
void ReplicatedWriteLog<I>::append_sync_point(SyncPoint *sync_point, int status) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  assert(m_lock.is_locked_by_me());
  sync_point->m_prior_log_entries_persisted_status = status;
  /* TODO: Write sync point, complete and free
     m_on_sync_point_persisted, and delete this sync point (log entry
     object for sync point will remain until redired). */
}

/**
 * Begin a new sync point
 */
template <typename I>
void ReplicatedWriteLog<I>::new_sync_point(void) {
  CephContext *cct = m_image_ctx.cct;
  SyncPoint *old_sync_point = m_current_sync_point;
  SyncPoint *new_sync_point;
  ldout(cct, 20) << dendl;

  assert(m_lock.is_locked_by_me());

  if (old_sync_point) {
    /* Advance the sync gen num unless this is the first sync point */
    ++m_current_sync_gen;
  }
  new_sync_point = new SyncPoint(cct, m_current_sync_gen);
  m_current_sync_point = new_sync_point;
  
  if (old_sync_point) {
    old_sync_point->m_final_op_sequence_num = m_last_op_sequence_num;
    /* Append of new sync point deferred until this sync point is persisted */
    old_sync_point->m_on_sync_point_persisted = m_current_sync_point->m_prior_log_entries_persisted->new_sub();
    /* This sync point will acquire no more sub-ops */
    old_sync_point->m_prior_log_entries_persisted->activate();
  }

  /* TODO: Make this part of the SyncPoint object. */
  new_sync_point->m_prior_log_entries_persisted->
    set_finisher(new FunctionContext([this, new_sync_point](int r) {
	  CephContext *cct = m_image_ctx.cct;
	  ldout(cct, 20) << "Prior log entries persisted for sync point =[" << new_sync_point << "]" << dendl;
	  append_sync_point(new_sync_point, r);
	}));
  
  if (old_sync_point) {
    ldout(cct,6) << "new sync point = [" << m_current_sync_point
		 << "], prior = [" << old_sync_point << "]" << dendl;
  } else {
    ldout(cct,6) << "first sync point = [" << m_current_sync_point
		 << "]" << dendl;
  }
}

template <typename I>
void ReplicatedWriteLog<I>::rwl_init(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  TOID(struct WriteLogPoolRoot) pool_root;

  ldout(cct,5) << "rwl_enabled:" << m_image_ctx.rwl_enabled << dendl;
  ldout(cct,5) << "rwl_size:" << m_image_ctx.rwl_size << dendl;
  std::string rwl_path = m_image_ctx.rwl_path;
  ldout(cct,5) << "rwl_path:" << m_image_ctx.rwl_path << dendl;

  std::string log_pool_name = rwl_path + "/rbd-rwl." + m_image_ctx.id + ".pool";
  std::string log_poolset_name = rwl_path + "/rbd-rwl." + m_image_ctx.id + ".poolset";
  m_log_pool_size = max(cct->_conf->get_val<uint64_t>("rbd_rwl_size"), MIN_POOL_SIZE);

  if (access(log_poolset_name.c_str(), F_OK) == 0) {
    m_log_pool_name = log_poolset_name;
  } else {
    m_log_pool_name = log_pool_name;
    lderr(cct) << "failed to open poolset" << log_poolset_name
	       << ":" << pmemobj_errormsg()
	       << ". Opening/creating simple/unreplicated pool" << dendl;
  }
  
  if (access(m_log_pool_name.c_str(), F_OK) != 0) {
    if ((m_log_pool =
	 pmemobj_create(m_log_pool_name.c_str(),
			rwl_pool_layout_name,
			m_log_pool_size,
			(S_IWUSR | S_IRUSR))) == NULL) {
      lderr(cct) << "failed to create pool (" << m_log_pool_name << ")"
                 << pmemobj_errormsg() << dendl;
      /* TODO: filter/replace errnos that are meaningless to the caller */
      on_finish->complete(-errno);
      return;
    }
    pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);

    /* new pool, calculate and store metadata */
    size_t effective_pool_size = (size_t)(m_log_pool_size * USABLE_SIZE);
    size_t small_write_size = MIN_WRITE_ALLOC_SIZE + BLOCK_ALLOC_OVERHEAD_BYTES + sizeof(struct WriteLogPmemEntry);
    uint64_t num_small_writes = (uint64_t)(effective_pool_size / small_write_size);
    /* Log ring empty */
    m_first_free_entry = 0;
    m_first_valid_entry = 0;
    TX_BEGIN(m_log_pool) {
      TX_ADD(pool_root);
      D_RW(pool_root)->header.layout_version = RWL_POOL_VERSION;
      D_RW(pool_root)->log_entries = TX_ZALLOC(struct WriteLogPmemEntry, num_small_writes);
      D_RW(pool_root)->block_size = MIN_WRITE_ALLOC_SIZE;
      D_RW(pool_root)->num_log_entries = num_small_writes-1; // leave one free
      D_RW(pool_root)->first_free_entry = m_first_free_entry;
      D_RW(pool_root)->first_valid_entry = m_first_valid_entry;
    } TX_ONCOMMIT {
      m_total_log_entries = D_RO(pool_root)->num_log_entries;
      m_free_log_entries = D_RO(pool_root)->num_log_entries;
    } TX_ONABORT {
      m_total_log_entries = 0;
      m_free_log_entries = 0;
      lderr(cct) << "failed to initialize pool (" << m_log_pool_name << ")" << dendl;
      on_finish->complete(-pmemobj_tx_errno());
      return;
    } TX_FINALLY {
    } TX_END;
  } else {
    if ((m_log_pool =
	 pmemobj_open(m_log_pool_name.c_str(),
		      rwl_pool_layout_name)) == NULL) {
      lderr(cct) << "failed to open pool (" << m_log_pool_name << "): "
                 << pmemobj_errormsg() << dendl;
      on_finish->complete(-errno);
      return;
    }
    pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);
    if (D_RO(pool_root)->header.layout_version != RWL_POOL_VERSION) {
      lderr(cct) << "Pool layout version is " << D_RO(pool_root)->header.layout_version
		 << " expected " << RWL_POOL_VERSION << dendl;
      on_finish->complete(-EINVAL);
      return;
    }
    if (D_RO(pool_root)->block_size != MIN_WRITE_ALLOC_SIZE) {
      lderr(cct) << "Pool block size is " << D_RO(pool_root)->block_size << " expected " << MIN_WRITE_ALLOC_SIZE << dendl;
      on_finish->complete(-EINVAL);
      return;
    }
    m_total_log_entries = D_RO(pool_root)->num_log_entries;
    m_free_log_entries = D_RO(pool_root)->num_log_entries;
    m_first_free_entry = D_RO(pool_root)->first_free_entry;
    m_first_valid_entry = D_RO(pool_root)->first_valid_entry;
    /* TODO: Actually load all the log entries already persisted */
    /* TODO: Set m_current_sync_gen to the successor of the last one seen in the log */
    ldout(cct,5) << "pool " << m_log_pool_name << "has " << D_RO(pool_root)->num_log_entries <<
      " log entries" << dendl;
    if (m_first_free_entry == m_first_valid_entry) {
      ldout(cct,5) << "write log is empy" << dendl;
    }
  }

  /* Start the sync point following the last one seen in the log */
  m_current_sync_point = new SyncPoint(cct, m_current_sync_gen);
  ldout(cct,6) << "new sync point = [" << m_current_sync_point << "]" << dendl;

  on_finish->complete(0);
}

template <typename I>
void ReplicatedWriteLog<I>::init(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      if (r >= 0) {
	rwl_init(on_finish);
      } else {
	/* Don't init RWL if layer below failed to init */
	on_finish->complete(r);
      }
    });
  /* Initialize the cache layer below first */
  m_image_writeback->init(ctx);
}

template <typename I>
void ReplicatedWriteLog<I>::shut_down(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  // TODO flush all in-flight IO and pending writeback prior to shut down

  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      Context *next_ctx = on_finish;
      if (r < 0) {
	/* Override on_finish status with this error */
        next_ctx = new FunctionContext(
          [r, on_finish](int _r) {
            on_finish->complete(r);
          });
      }
      /* Shut down the cache layer below */
      m_image_writeback->shut_down(next_ctx);
    });
  ctx = new FunctionContext(
    [this, ctx](int r) {
      Context *next_ctx = ctx;
      if (r < 0) {
	/* Override next_ctx status with this error */
        next_ctx = new FunctionContext(
          [r, ctx](int _r) {
            ctx->complete(r);
          });
      }
      if (m_log_pool) {
	pmemobj_close(m_log_pool);
	r = -errno;
      }
      next_ctx->complete(r);
    });
  ctx = new FunctionContext(
    [this, ctx](int r) {
      Context *next_ctx = ctx;
      if (r < 0) {
	/* Override next_ctx status with this error */
        next_ctx = new FunctionContext(
          [r, ctx](int _r) {
            ctx->complete(r);
          });
      }
      // flush all writes to OSDs
      flush(next_ctx);
    });
  {
    Mutex::Locker locker(m_lock);
    m_async_op_tracker.wait(m_image_ctx, ctx);
  }
}

template <typename I>
void ReplicatedWriteLog<I>::invalidate(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      if (m_image_ctx.persistent_cache_enabled) {
	m_image_writeback->invalidate(on_finish);
      } else {
	on_finish->complete(0);
      }
    });
  // TODO
  invalidate({{0, m_image_ctx.size}}, ctx);
}

template <typename I>
void ReplicatedWriteLog<I>::wake_up() {
  CephContext *cct = m_image_ctx.cct;
  assert(m_lock.is_locked());
  if (m_wake_up_requested || m_async_op_tracker.is_waiting()) {
    return;
  }

  ldout(cct, 20) << dendl;

  /* Wake-up can be requested while it's already scheduled */
  m_wake_up_requested = true;

  /* Wake-up cannot be scheduled if it's already scheduled */
  if (m_wake_up_scheduled) {
    return;
  }
  m_wake_up_scheduled = true;
  m_async_op_tracker.start_op();
  m_image_ctx.op_work_queue->queue(new FunctionContext(
    [this](int r) {
      process_work();
    }), 0);
}

template <typename I>
void ReplicatedWriteLog<I>::process_work() {
  CephContext *cct = m_image_ctx.cct;
  int max_iterations = 4;
  bool wake_up_requested = false;
  ldout(cct, 20) << dendl;

  do {
    {
      Mutex::Locker locker(m_lock);
      m_wake_up_requested = false;
    }
    process_writeback_dirty_blocks();
    process_detained_block_ios();
    process_deferred_block_ios();

    /* Do the work postponed from the work functions above */
    if (drain_context_list(m_post_work_contexts, m_lock)) {
      continue;
    }
    
    {
      Mutex::Locker locker(m_lock);
      wake_up_requested = m_wake_up_requested;
    }
  } while (wake_up_requested && --max_iterations > 0);

  {
    Mutex::Locker locker(m_lock);
    m_wake_up_scheduled = false;
    m_async_op_tracker.finish_op();
    /* Reschedule if it's still requested */
    if (m_wake_up_requested) {
      wake_up();
    }
  }
}

/* Empties contexts list while holding lock, completes them, and returns true if there were any */
template <typename I>
bool ReplicatedWriteLog<I>::drain_context_list(Contexts &contexts, Mutex &contexts_lock) {
  Contexts local_contexts;

  {
    Mutex::Locker locker(contexts_lock);
    local_contexts.swap(contexts);
  }

  if (!local_contexts.empty()) {
    for (auto ctx : local_contexts) {
      ctx->complete(0);
    }
    return true;
  }
  return false;
}

template <typename I>
bool ReplicatedWriteLog<I>::is_work_available() const {
  Mutex::Locker locker(m_lock);
  return (!m_detained_block_ios.empty() ||
          !m_deferred_block_ios.empty());
}

template <typename I>
bool ReplicatedWriteLog<I>::can_flush_entry(WriteLogEntry *log_entry) {
  CephContext *cct = m_image_ctx.cct;

  ldout(cct, 20) << "" << dendl;
  assert(m_lock.is_locked_by_me());
  return (0 == m_flush_ops_in_flight);
}

template <typename I>
void ReplicatedWriteLog<I>::flush_entry(WriteLogEntry *log_entry) {
  CephContext *cct = m_image_ctx.cct;

  ldout(cct, 20) << "" << dendl;
  assert(m_lock.is_locked_by_me());
  m_flush_ops_in_flight += 1;
  m_flush_bytes_in_flight += log_entry->ram_entry.write_bytes;

  /* Construct bl for pmem buffer */
  log_entry->add_reader();
  log_entry->flushing = true;
  buffer::raw *entry_buf =
    buffer::claim_buffer(log_entry->ram_entry.write_bytes,
			 (char*)log_entry->pmem_buffer,
			 make_deleter([this, log_entry]
				      {
					CephContext *cct = m_image_ctx.cct;
					ldout(cct, 5) << "removing reader: log_entry="
						      << *log_entry << dendl;
					log_entry->remove_reader();
				      }));
  /* We'll send the flush write later when we're not holding m_lock */
  m_post_work_contexts.push_back(new FunctionContext(
    [this, cct, log_entry, entry_buf](int r) {
      /* Flush write completion action */
      Context *ctx = new FunctionContext(
        [this, cct, log_entry](int r) {
	  {
	    Mutex::Locker locker(m_lock);
	    m_flush_ops_in_flight -= 1;
	    m_flush_bytes_in_flight -= log_entry->ram_entry.write_bytes;
	    log_entry->flushing = false;
	    if (r < 0) {
	      lderr(cct) << "failed to flush write log entry"
			 << cpp_strerror(r) << dendl;
	      m_dirty_log_entries.push_front(log_entry);
	      return;
	    } else {
	      log_entry->flushed = true;
	      ldout(cct, 20) << "flushed:" << log_entry << dendl;	    
	    }
	    wake_up();
	  }
	});
	
	bufferlist entry_bl;
	entry_bl.push_back(entry_buf);
      ldout(cct, 20) << "flushing:" << log_entry << dendl;	    
      m_image_writeback->aio_write({{log_entry->ram_entry.image_offset_bytes,
	                             log_entry->ram_entry.write_bytes}},
	                           std::move(entry_bl), 0, ctx);
    }));
}

template <typename I>
void ReplicatedWriteLog<I>::process_writeback_dirty_blocks() {
  CephContext *cct = m_image_ctx.cct;
  bool all_clean = false;
  
  ldout(cct, 20) << "Look for dirty entries" << dendl;
  {
    Mutex::Locker locker(m_lock);
    while (true) {
      if (m_dirty_log_entries.empty()) {
	ldout(cct, 20) << "Nothing new to flush" << dendl;
	break;
      }
      if (can_flush_entry(m_dirty_log_entries.front())) {
	flush_entry(m_dirty_log_entries.front());
	m_dirty_log_entries.pop_front();
      } else {
	ldout(cct, 20) << "Next dirty entry isn't flushable yet" << dendl;
	break;      
      }
    }
    
    /* Check for and take flush complete actions */
    all_clean = (0 == m_flush_ops_in_flight &&
		 m_dirty_log_entries.empty());
  }

  if (all_clean) {
    /* All flushing complete, drain outside lock */
    drain_context_list(m_flush_complete_contexts, m_lock);
  }
}

template <typename I>
void ReplicatedWriteLog<I>::process_detained_block_ios() {
  BlockGuard::BlockIOs block_ios;
  {
    Mutex::Locker locker(m_lock);
    std::swap(block_ios, m_detained_block_ios);
  }

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "block_ios=" << block_ios.size() << dendl;
  for (auto &block_io : block_ios) {
    ldout(cct, 20) << "block_io=" << &block_io << dendl;
    //map_block(false, std::move(block_io));
  }
}

template <typename I>
void ReplicatedWriteLog<I>::process_deferred_block_ios() {
  BlockGuard::BlockIOs block_ios;
  {
    Mutex::Locker locker(m_lock);
    std::swap(block_ios, m_deferred_block_ios);
  }

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "block_ios=" << block_ios.size() << dendl;
  for (auto &block_io : block_ios) {
    ldout(cct, 20) << "block_io=" << &block_io << dendl;
    //map_block(true, std::move(block_io));
  }
}

template <typename I>
void ReplicatedWriteLog<I>::invalidate(Extents&& image_extents,
                                   Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  // TODO - ensure sync with in-flight flushes
  for (auto &extent : image_extents) {
    uint64_t image_offset = extent.first;
    uint64_t image_length = extent.second;
    while (image_length > 0) {
      uint32_t block_start_offset = image_offset % MIN_WRITE_SIZE;
      uint32_t block_end_offset = min(block_start_offset + image_length,
                                      uint64_t(MIN_WRITE_SIZE));
      uint32_t block_length = block_end_offset - block_start_offset;

      image_offset += block_length;
      image_length -= block_length;
    }
  }

  on_finish->complete(0);
}

/*
 * Internal flush - will actually flush the RWL.
 *
 * User flushes should arrive at aio_flush(), and only flush prior
 * writes to all log replicas.
 */
template <typename I>
void ReplicatedWriteLog<I>::flush(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  bool all_clean = false;
  
  {
    Mutex::Locker locker(m_lock);
    all_clean = (0 == m_flush_ops_in_flight &&
		 m_dirty_log_entries.empty());
  }

  if (all_clean) {
    /* Complete without holding m_lock */
    ldout(cct, 20) << "no dirty entries" << dendl;
    on_finish->complete(0);
  } else {
    ldout(cct, 20) << "dirty entries remain" << dendl;
    Mutex::Locker locker(m_lock);
    /* on_finish can't be completed yet */
    m_flush_complete_contexts.push_back(new FunctionContext(
      [this, on_finish](int r) {
	flush(on_finish);
      }));
    wake_up();
  }
}

} // namespace cache
} // namespace librbd

template class librbd::cache::ReplicatedWriteLog<librbd::ImageCtx>;

/* Local Variables: */
/* eval: (c-set-offset 'innamespace 0) */
/* End: */
