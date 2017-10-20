// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ReplicatedWriteLog.h"
#include "include/buffer.h"
#include "include/Context.h"
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

namespace rwl {

typedef std::list<bufferlist> Buffers;
typedef std::map<uint64_t, bufferlist> ExtentBuffers;
typedef std::function<void(uint64_t)> ReleaseBlock;
typedef std::function<void(BlockGuard::BlockIO)> AppendDetainedBlock;

SyncPoint::SyncPoint(CephContext *cct, uint64_t sync_gen_num)
  : m_cct(cct), m_sync_gen_num(sync_gen_num), on_sync_point_persisted(NULL) {
  prior_log_entries_persisted = new C_Gather(cct, NULL);
  /* TODO: Connect prior_log_entries_persisted finisher to append this
     sync point and on persist complete call on_sync_point_persisted
     and delete this object */
}

bool is_block_aligned(const ImageCache::Extents &image_extents) {
  for (auto &extent : image_extents) {
    if (extent.first % BLOCK_SIZE != 0 || extent.second % BLOCK_SIZE != 0) {
      return false;
    }
  }
  return true;
}

struct C_BlockIORequest : public Context {
  CephContext *cct;
  C_BlockIORequest *next_block_request;

  C_BlockIORequest(CephContext *cct, C_BlockIORequest *next_block_request)
    : cct(cct), next_block_request(next_block_request) {
  }

  virtual void finish(int r) override {
    ldout(cct, 20) << "(" << get_name() << "): r=" << r << dendl;

    if (r < 0) {
      // abort the chain of requests upon failure
      next_block_request->complete(r);
    } else {
      // execute next request in chain
      next_block_request->send();
    }
  }

  virtual void send() = 0;
  virtual const char *get_name() const = 0;
};

struct C_ReleaseBlockGuard : public C_BlockIORequest {
  uint64_t block;
  ReleaseBlock &release_block;
  BlockGuard::C_BlockRequest *block_request;

  C_ReleaseBlockGuard(CephContext *cct, uint64_t block,
                      ReleaseBlock &release_block,
                      BlockGuard::C_BlockRequest *block_request)
    : C_BlockIORequest(cct, nullptr), block(block),
      release_block(release_block), block_request(block_request) {
  }

  virtual void send() override {
    complete(0);
  }
  virtual const char *get_name() const override {
    return "C_ReleaseBlockGuard";
  }

  virtual void finish(int r) override {
    ldout(cct, 20) << "(" << get_name() << "): r=" << r << dendl;

    // IO operation finished -- release guard
    release_block(block);

    // complete block request
    block_request->complete_request(r);
  }
};

template <typename I>
struct C_PromoteToCache : public C_BlockIORequest {
  ImageStore<I> &image_store;
  BlockGuard::BlockIO block_io;
  const bufferlist &bl;

  C_PromoteToCache(CephContext *cct, ImageStore<I> &image_store,
                   BlockGuard::BlockIO &block_io, const bufferlist &bl,
                   C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_store(image_store), block_io(block_io), bl(bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block=" << block_io.block << dendl;
    // promote the clean block to the cache
    bufferlist sub_bl;
    if(bl.length() > BLOCK_SIZE) {
      sub_bl.substr_of(bl, block_io.extents[0].buffer_offset, block_io.extents[0].block_length);
    } else {
      sub_bl.append(bl);
    }
    image_store.write_block(block_io.block, {{0, BLOCK_SIZE}}, std::move(sub_bl),
                            this);
  }
  virtual const char *get_name() const override {
    return "C_PromoteToCache";
  }
};

template <typename I>
struct C_DemoteFromCache : public C_BlockIORequest {
  ImageStore<I> &image_store;
  ReleaseBlock &release_block;
  uint64_t block;

  C_DemoteFromCache(CephContext *cct, ImageStore<I> &image_store,
                    ReleaseBlock &release_block, uint64_t block,
                    C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_store(image_store), release_block(release_block), block(block) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block=" << block << dendl;
    image_store.discard_block(block, this);
  }
  virtual const char *get_name() const override {
    return "C_DemoteFromCache";
  }

  virtual void finish(int r) {
    // IO against the demote block was detained -- release
    release_block(block);

    C_BlockIORequest::finish(r);
  }
};

template <typename I>
struct C_ReadFromCacheRequest : public C_BlockIORequest {
  ImageStore<I> &image_store;
  BlockGuard::BlockIO block_io;
  ExtentBuffers *extent_buffers;

  C_ReadFromCacheRequest(CephContext *cct, ImageStore<I> &image_store,
                         BlockGuard::BlockIO &&block_io,
                         ExtentBuffers *extent_buffers,
                         C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_store(image_store), block_io(block_io),
      extent_buffers(extent_buffers) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block_io=[" << block_io << "]" << dendl;
    C_Gather *ctx = new C_Gather(cct, this);
    for (auto &extent : block_io.extents) {
      image_store.read_block(block_io.block,
                             {{extent.block_offset, extent.block_length}},
                             &(*extent_buffers)[extent.buffer_offset],
                             ctx->new_sub());
    }
    ctx->activate();
  }
  virtual const char *get_name() const override {
    return "C_ReadFromCacheRequest";
  }
};

template <typename I>
struct C_ReadFromImageRequest : public C_BlockIORequest {
  ImageWriteback<I> &image_writeback;
  BlockGuard::BlockIO block_io;
  ExtentBuffers *extent_buffers;

  C_ReadFromImageRequest(CephContext *cct, ImageWriteback<I> &image_writeback,
                         BlockGuard::BlockIO &&block_io,
                         ExtentBuffers *extent_buffers,
                         C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_writeback(image_writeback), block_io(block_io),
      extent_buffers(extent_buffers) {
  }

  virtual void send() override {
   ldout(cct, 20) << "(" << get_name() << "): "
                   << "block_io=[" << block_io << "]" << dendl;

    // TODO improve scatter/gather to include buffer offsets
    uint64_t image_offset = block_io.block * BLOCK_SIZE;
    C_Gather *ctx = new C_Gather(cct, this);
    for (auto &extent : block_io.extents) {
      image_writeback.aio_read({{image_offset + extent.block_offset,
                                 extent.block_length}},
                               &(*extent_buffers)[extent.buffer_offset],
                               0, ctx->new_sub());
    }
    ctx->activate();
  }
  virtual const char *get_name() const override {
    return "C_ReadFromImageRequest";
  }
};

template <typename I>
struct C_WriteToMetaRequest : public C_BlockIORequest {
  MetaStore<I> &meta_store;
  uint64_t cache_block_id;
  Policy *policy;

  C_WriteToMetaRequest(CephContext *cct, MetaStore<I> &meta_store,
		                uint64_t cache_block_id, Policy *policy,
                        C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request), meta_store(meta_store),
    cache_block_id(cache_block_id), policy(policy) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "cache_block_id=" << cache_block_id << dendl;

    bufferlist meta_bl;
    policy->entry_to_bufferlist(cache_block_id, &meta_bl);
    ldout(cct, 20) << "entry_to_bufferlist bl:" << meta_bl << dendl;
    meta_store.write_block(cache_block_id, std::move(meta_bl), this);
  }
  virtual const char *get_name() const override {
    return "C_WriteToMetaRequest";
  }
};

template <typename I>
struct C_WriteToImageRequest : public C_BlockIORequest {
  ImageWriteback<I> &image_writeback;
  BlockGuard::BlockIO block_io;
  const bufferlist &bl;

  C_WriteToImageRequest(CephContext *cct, ImageWriteback<I> &image_writeback,
                        BlockGuard::BlockIO &&block_io, const bufferlist &bl,
                        C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_writeback(image_writeback), block_io(block_io), bl(bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block_io=[" << block_io << "]" << dendl;

    uint64_t image_offset = block_io.block * BLOCK_SIZE;

    ImageCache::Extents image_extents;
    bufferlist scatter_bl;
    for (auto &extent : block_io.extents) {
      image_extents.emplace_back(image_offset + extent.block_offset,
                                 extent.block_length);

      bufferlist sub_bl;
      sub_bl.substr_of(bl, extent.buffer_offset, extent.block_length);
      scatter_bl.claim_append(sub_bl);
    }

    image_writeback.aio_write(std::move(image_extents), std::move(scatter_bl),
                              0, this);
  }
  virtual const char *get_name() const override {
    return "C_WriteToImageRequest";
  }
};

template <typename I>
struct C_ReadBlockFromCacheRequest : public C_BlockIORequest {
  ImageStore<I> &image_store;
  uint64_t block;
  uint32_t block_size;
  bufferlist *block_bl;

  C_ReadBlockFromCacheRequest(CephContext *cct, ImageStore<I> &image_store,
                              uint64_t block, uint32_t block_size,
                               bufferlist *block_bl,
                              C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_store(image_store), block(block), block_size(block_size),
      block_bl(block_bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block=" << block << dendl;
    image_store.read_block(block, {{0, block_size}}, block_bl, this);
  }
  virtual const char *get_name() const override {
    return "C_ReadBlockFromCacheRequest";
  }
};

template <typename I>
struct C_ReadBlockFromImageRequest : public C_BlockIORequest {
  ImageWriteback<I> &image_writeback;
  uint64_t block;
  bufferlist *block_bl;

  C_ReadBlockFromImageRequest(CephContext *cct,
                              ImageWriteback<I> &image_writeback,
                              uint64_t block, bufferlist *block_bl,
                              C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      image_writeback(image_writeback), block(block), block_bl(block_bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block=" << block << dendl;

    uint64_t image_offset = block * BLOCK_SIZE;
    image_writeback.aio_read({{image_offset, BLOCK_SIZE}}, block_bl, 0, this);
  }
  virtual const char *get_name() const override {
    return "C_ReadBlockFromImageRequest";
  }
};

struct C_CopyFromBlockBuffer : public C_BlockIORequest {
  BlockGuard::BlockIO block_io;
  const bufferlist &block_bl;
  ExtentBuffers *extent_buffers;

  C_CopyFromBlockBuffer(CephContext *cct, const BlockGuard::BlockIO &block_io,
                        const bufferlist &block_bl,
                        ExtentBuffers *extent_buffers,
                        C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      block_io(block_io), block_bl(block_bl), extent_buffers(extent_buffers) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block_io=[" << block_io << "]" << dendl;

    for (auto &extent : block_io.extents) {
      bufferlist &sub_bl = (*extent_buffers)[extent.buffer_offset];
      sub_bl.substr_of(block_bl, extent.block_offset, extent.block_length);
    }
    complete(0);
  }
  virtual const char *get_name() const override {
    return "C_CopyFromBlockBuffer";
  }
};

struct C_ModifyBlockBuffer : public C_BlockIORequest {
  BlockGuard::BlockIO block_io;
  const bufferlist &bl;
  bufferlist *block_bl;

  C_ModifyBlockBuffer(CephContext *cct, const BlockGuard::BlockIO &block_io,
                      const bufferlist &bl, bufferlist *block_bl,
                      C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      block_io(block_io), bl(bl), block_bl(block_bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block_io=[" << block_io << "]" << dendl;

    for (auto &extent : block_io.extents) {
      bufferlist modify_bl;
      if (extent.block_offset > 0) {
        bufferlist sub_bl;
        sub_bl.substr_of(*block_bl, 0, extent.block_offset);
        modify_bl.claim_append(sub_bl);
      }
      if (extent.block_length > 0) {
        bufferlist sub_bl;
        sub_bl.substr_of(bl, extent.buffer_offset, extent.block_length);
        modify_bl.claim_append(sub_bl);
      }
      uint32_t remaining_offset = extent.block_offset + extent.block_length;
      if (remaining_offset < block_bl->length()) {
        bufferlist sub_bl;
        sub_bl.substr_of(*block_bl, remaining_offset,
                         block_bl->length() - remaining_offset);
        modify_bl.claim_append(sub_bl);
      }
      std::swap(*block_bl, modify_bl);
    }
    complete(0);
  }
  virtual const char *get_name() const override {
    return "C_ModifyBlockBuffer";
  }
};

template <typename I>
struct C_AppendEventToJournal : public C_BlockIORequest {
  JournalStore<I> &journal_store;
  uint64_t tid;
  uint64_t block;
  IOType io_type;

  C_AppendEventToJournal(CephContext *cct, JournalStore<I> &journal_store,
                         uint64_t tid, uint64_t block, IOType io_type,
                         C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      journal_store(journal_store), tid(tid), block(block), io_type(io_type) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "tid=" << tid << ", "
                   << "block=" << block << ", "
                   << "io_type=" << io_type << dendl;
    journal_store.append_event(tid, block, io_type, this);
  }
  virtual const char *get_name() const override {
    return "C_AppendEventToJournal";
  }
};

template <typename I>
struct C_DemoteBlockToJournal : public C_BlockIORequest {
  JournalStore<I> &journal_store;
  uint64_t block;
  const bufferlist &bl;

  C_DemoteBlockToJournal(CephContext *cct, JournalStore<I> &journal_store,
                         uint64_t block, const bufferlist &bl,
                         C_BlockIORequest *next_block_request)
    : C_BlockIORequest(cct, next_block_request),
      journal_store(journal_store), block(block), bl(bl) {
  }

  virtual void send() override {
    ldout(cct, 20) << "(" << get_name() << "): "
                   << "block=" << block << dendl;

    bufferlist copy_bl;
    copy_bl.append(bl);
    journal_store.demote_block(block, std::move(copy_bl), this);
  }
  virtual const char *get_name() const override {
    return "C_DemoteBlockToJournal";
  }
};

template <typename I>
struct C_ReadBlockRequest : public BlockGuard::C_BlockRequest {
  I &image_ctx;
  ImageWriteback<I> &image_writeback;
  ImageStore<I> &image_store;
  ReleaseBlock &release_block;
  bufferlist *bl;

  ExtentBuffers extent_buffers;
  Buffers promote_buffers;

  C_ReadBlockRequest(I &image_ctx,
                     ImageWriteback<I> &image_writeback,
                     ImageStore<I> &image_store,
                     ReleaseBlock &release_block, bufferlist *bl,
                     Context *on_finish)
    : C_BlockRequest(on_finish), image_ctx(image_ctx),
      image_writeback(image_writeback), image_store(image_store),
      release_block(release_block), bl(bl) {
  }

  virtual void remap(PolicyMapResult policy_map_result,
                     BlockGuard::BlockIO &&block_io) {
    assert(block_io.tid == 0);
    CephContext *cct = image_ctx.cct;

    // TODO: consolidate multiple reads into a single request (i.e. don't
    // have 1024 4K requests to read a single object)

    // NOTE: block guard active -- must be released after IO completes
    C_BlockIORequest *req = new C_ReleaseBlockGuard(cct, block_io.block,
                                                         release_block, this);
    switch (policy_map_result) {
    case POLICY_MAP_RESULT_HIT:
      req = new C_ReadFromCacheRequest<I>(cct, image_store, std::move(block_io),
                                          &extent_buffers, req);
      break;
    case POLICY_MAP_RESULT_MISS:
      req = new C_ReadFromImageRequest<I>(cct, image_writeback,
                                          std::move(block_io), &extent_buffers,
                                          req);
      break;
    case POLICY_MAP_RESULT_NEW:
    case POLICY_MAP_RESULT_REPLACE:
      promote_buffers.emplace_back();
      req = new C_CopyFromBlockBuffer(cct, block_io, promote_buffers.back(),
                                      &extent_buffers, req);
      req = new C_PromoteToCache<I>(cct, image_store, block_io,
                                    promote_buffers.back(), req);
      req = new C_ReadBlockFromImageRequest<I>(cct, image_writeback,
                                               block_io.block,
                                               &promote_buffers.back(), req);
      if (policy_map_result == POLICY_MAP_RESULT_REPLACE) {
        req = new C_DemoteFromCache<I>(cct, image_store, release_block,
                                       block_io.block, req);
      }
      break;
    default:
      assert(false);
    }
    req->send();
  }

  virtual void finish(int r) override {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_ReadBlockRequest): r=" << r << dendl;

    if (r < 0) {
      C_BlockRequest::finish(r);
      return;
    }

    ldout(cct, 20) << "assembling read extents" << dendl;
    for (auto &extent_bl : extent_buffers) {
      ldout(cct, 20) << extent_bl.first << "~" << extent_bl.second.length()
                     << dendl;
      bl->claim_append(extent_bl.second);
    }
    C_BlockRequest::finish(0);
  }
};

template <typename I>
struct C_WriteBlockRequest : BlockGuard::C_BlockRequest {
  I &image_ctx;
  ImageWriteback<I> &image_writeback;
  Policy &policy;
  JournalStore<I> &journal_store;
  ImageStore<I> &image_store;
  MetaStore<I> &meta_store;
  ReleaseBlock &release_block;
  AppendDetainedBlock &append_detain_block;
  bufferlist bl;
  uint32_t block_size;

  Buffers promote_buffers;

  C_WriteBlockRequest(I &image_ctx, ImageWriteback<I> &image_writeback,
                      Policy &policy, JournalStore<I> &journal_store,
                      ImageStore<I> &image_store, MetaStore<I> &meta_store,
                      ReleaseBlock &release_block, AppendDetainedBlock &append_detain_block,
                      bufferlist &&bl, uint32_t block_size, Context *on_finish)
    : C_BlockRequest(on_finish),
      image_ctx(image_ctx), image_writeback(image_writeback), policy(policy),
      journal_store(journal_store), image_store(image_store),
      meta_store(meta_store), release_block(release_block),
      append_detain_block(append_detain_block),
      bl(std::move(bl)), block_size(block_size) {
  }

  virtual void remap(PolicyMapResult policy_map_result,
                     BlockGuard::BlockIO &&block_io) {
    CephContext *cct = image_ctx.cct;

    // TODO: consolidate multiple writes into a single request (i.e. don't
    // have 1024 4K requests to read a single object)

    // NOTE: block guard active -- must be released after IO completes
    C_BlockIORequest *req = new C_ReleaseBlockGuard(cct, block_io.block,
                                                         release_block, this);

    if (policy_map_result == POLICY_MAP_RESULT_MISS) {
      req = new C_WriteToImageRequest<I>(cct, image_writeback,
                                         std::move(block_io), bl, req);
    } else {
      if (0 == policy.get_write_mode()) {
        //write-thru
        req = new C_WriteToImageRequest<I>(cct, image_writeback,
                                           std::move(block_io), bl, req);
      } else {
        // block is now dirty -- can't be replaced until flushed
        policy.set_dirty(block_io.block);
      }
      req = new C_WriteToMetaRequest<I>(cct, meta_store, block_io.block, &policy, req);

      IOType io_type = static_cast<IOType>(block_io.io_type);
      if ((io_type == IO_TYPE_WRITE || io_type == IO_TYPE_DISCARD) &&
          block_io.tid == 0) {
        // TODO support non-journal mode / writethrough-only
        int r = journal_store.allocate_tid(&block_io.tid);
        if (r < 0) {
          ldout(cct, 20) << "journal full -- detaining block IO" << dendl;
          append_detain_block(block_io);
          return;
        }
      }

      if (block_io.partial_block) {
        // block needs to be promoted to cache but we require a
        // read-modify-write cycle to fully populate the block

        // TODO optimize by only reading missing extents
        promote_buffers.emplace_back();

        if (block_io.tid > 0 && (1 == policy.get_write_mode())) {
          req = new C_AppendEventToJournal<I>(cct, journal_store, block_io.tid,
                                              block_io.block, IO_TYPE_WRITE,
                                              req);
        }
        req = new C_PromoteToCache<I>(cct, image_store, block_io,
                                      promote_buffers.back(), req);
        req = new C_ModifyBlockBuffer(cct, block_io, bl,
                                      &promote_buffers.back(), req);
        if (policy_map_result == POLICY_MAP_RESULT_HIT) {
          if (block_io.tid > 0 &&
              journal_store.is_demote_required(block_io.block)) {
            req = new C_DemoteBlockToJournal<I>(cct, journal_store,
                                                block_io.block,
                                                promote_buffers.back(), req);
          }
          req = new C_ReadBlockFromCacheRequest<I>(cct, image_store,
                                                   block_io.block, BLOCK_SIZE,
                                                   &promote_buffers.back(),
                                                   req);
        } else {
          req = new C_ReadBlockFromImageRequest<I>(cct, image_writeback,
                                                   block_io.block,
                                                   &promote_buffers.back(),
                                                   req);
        }
      } else {
        // full block overwrite
        if (block_io.tid > 0 && (1 == policy.get_write_mode())) {
          req = new C_AppendEventToJournal<I>(cct, journal_store, block_io.tid,
                                              block_io.block, IO_TYPE_WRITE,
                                              req);
        }

        req = new C_PromoteToCache<I>(cct, image_store, block_io,
                                      bl, req);
      }

      if (policy_map_result == POLICY_MAP_RESULT_REPLACE) {
        req = new C_DemoteFromCache<I>(cct, image_store, release_block,
                                       block_io.block, req);
      }
    }
    req->send();
  }
};

template <typename I>
struct C_WritebackRequest : public Context {
  I &image_ctx;
  ImageWriteback<I> &image_writeback;
  Policy &policy;
  JournalStore<I> &journal_store;
  ImageStore<I> &image_store;
  const ReleaseBlock &release_block;
  util::AsyncOpTracker &async_op_tracker;
  uint64_t tid;
  uint64_t block;
  IOType io_type;
  bool demoted;
  uint32_t block_size;

  bufferlist bl;

  C_WritebackRequest(I &image_ctx, ImageWriteback<I> &image_writeback,
                     Policy &policy, JournalStore<I> &journal_store,
                     ImageStore<I> &image_store,
                     const ReleaseBlock &release_block,
                     util::AsyncOpTracker &async_op_tracker, uint64_t tid,
                     uint64_t block, IOType io_type, bool demoted,
                     uint32_t block_size)
    : image_ctx(image_ctx), image_writeback(image_writeback),
      policy(policy), journal_store(journal_store), image_store(image_store),
      release_block(release_block), async_op_tracker(async_op_tracker),
      tid(tid), block(block), io_type(io_type), demoted(demoted),
      block_size(block_size) {
    async_op_tracker.start_op();
  }
  virtual ~C_WritebackRequest() {
    async_op_tracker.finish_op();
  }

  void send() {
    read_from_cache();
  }

  void read_from_cache() {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest)" << dendl;

    Context *ctx = util::create_context_callback<
      C_WritebackRequest<I>,
      &C_WritebackRequest<I>::handle_read_from_cache>(this);
    if (demoted) {
      journal_store.get_writeback_block(tid, &bl, ctx);
    } else {
      image_store.read_block(block, {{0, block_size}}, &bl, ctx);
    }
  }

  void handle_read_from_cache(int r) {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest): r=" << r << dendl;

    if (r < 0) {
      lderr(cct) << "failed to read writeback block from cache: "
                 << cpp_strerror(r) << dendl;
      complete(r);
      return;
    }

    write_to_image();
  }

  void write_to_image() {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest)" << dendl;

    Context *ctx = util::create_context_callback<
      C_WritebackRequest<I>,
      &C_WritebackRequest<I>::handle_write_to_image>(this);
    image_writeback.aio_write({{block * block_size, block_size}}, std::move(bl),
                              0, ctx);
  }

  void handle_write_to_image(int r) {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest): r=" << r << dendl;

    if (r < 0) {
      lderr(cct) << "failed to read writeback block from cache: "
                 << cpp_strerror(r) << dendl;
      complete(r);
      return;
    }

    commit_event();
  }

  void commit_event() {
    if (tid == 0) {
      complete(0);
      return;
    }

    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest)" << dendl;

    if (!demoted) {
      policy.clear_dirty(block);
    }

    Context *ctx = util::create_context_callback<
      C_WritebackRequest<I>,
      &C_WritebackRequest<I>::handle_commit_event>(this);
    journal_store.commit_event(tid, ctx);
  }

  void handle_commit_event(int r) {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "(C_WritebackRequest): r=" << r << dendl;

    if (r < 0) {
      lderr(cct) << "failed to commit event in cache: "
                 << cpp_strerror(r) << dendl;
      complete(r);
      return;
    }

    complete(0);
  }

  virtual void finish(int r) {
    CephContext *cct = image_ctx.cct;
    ldout(cct, 20) << "r=" << r << dendl;

    if (r < 0) {
      lderr(cct) << "failed to writeback block " << block << ": "
                 << cpp_strerror(r) << dendl;
      policy.set_dirty(block);
    }

    release_block(block);
  }
};

} // namespace rwl

template <typename I>
ReplicatedWriteLog<I>::ReplicatedWriteLog(ImageCtx &image_ctx)
  : m_image_ctx(image_ctx), 
    m_log_pool(NULL), m_log_pool_size(DEFAULT_POOL_SIZE),
    m_total_log_entries(0), m_total_blocks(0),
    m_free_log_entries(0), m_free_blocks(0),
    m_image_writeback(image_ctx), m_image_cache(image_ctx),
    m_block_guard(image_ctx.cct, 256, BLOCK_SIZE),
    m_first_free_entry(0), m_first_valid_entry(0),
    m_next_free_block(0), m_last_free_block(0),
    m_free_entry_hint(0), m_valid_entry_hint(0),
    m_current_sync_gen(0), m_current_sync_point(NULL),
    m_last_op_sequence_num(0),
    m_persist_on_write_until_flush(true), m_persist_on_flush(false),
    m_flush_seen(false),
    m_release_block(std::bind(&ReplicatedWriteLog<I>::release_block, this,
                              std::placeholders::_1)),
    m_lock("librbd::cache::ReplicatedWriteLog::m_lock") {
  //CephContext *cct = m_image_ctx.cct;
  m_policy = new StupidPolicy<I>(m_image_ctx, m_block_guard);
  m_policy->set_write_mode(0);
}

template <typename I>
ReplicatedWriteLog<I>::~ReplicatedWriteLog() {
  delete m_policy;
}

template <typename I>
void ReplicatedWriteLog<I>::aio_read(Extents &&image_extents, bufferlist *bl,
                                 int fadvise_flags, Context *on_finish) {
  //CephContext *cct = m_image_ctx.cct;
  //ldout(cct, 20) << "image_extents=" << image_extents << ", "
  //               << "on_finish=" << on_finish << dendl;

  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_read(std::move(image_extents), bl, fadvise_flags, on_finish);
  } else {
    m_image_writeback.aio_read(std::move(image_extents), bl, fadvise_flags, on_finish);
  }
  // TODO handle fadvise flags
  /*
  BlockGuard::C_BlockRequest *req = new C_ReadBlockRequest<I>(
    m_image_ctx, m_image_writeback, *m_image_store, m_release_block, bl,
    on_finish);
  map_blocks(IO_TYPE_READ, std::move(image_extents), req);
  */
}

template <typename I>
void ReplicatedWriteLog<I>::aio_write(Extents &&image_extents,
                                  bufferlist&& bl,
                                  int fadvise_flags,
                                  Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  TOID(struct WriteLogPoolRoot) pool_root;
  pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);
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

  /* Make empty log entry objects for each extent */
  uint64_t total_bytes = 0;
  for (auto &extent : image_extents) {
    total_bytes += extent.second;
  }

  ldout(cct,6) << "bl=[" << bl << "]" << dendl;
  uint8_t *pmem_blocks = D_RW(D_RW(pool_root)->data_blocks);
  struct WriteLogPmemEntry *pmem_log_entries = D_RW(D_RW(pool_root)->log_entries);

  WriteLogOperations operations;
  {
    uint64_t buffer_offset = 0;
    Mutex::Locker locker(m_lock);
    /* TODO: If there isn't space for this whole write, defer it */
    if (m_free_log_entries < operations.size()) {
      ldout(cct, 6) << "wait for " << operations.size() << " log entries" << dendl;
    } else if (m_free_blocks < total_bytes / BLOCK_SIZE) {
      ldout(cct, 6) << "wait for " << total_bytes / BLOCK_SIZE << " data blocks" << dendl;
    } else {
      for (auto &extent : image_extents) {
	WriteLogOperation *operation =
	  new WriteLogOperation(m_current_sync_point, extent.first, extent.second);
	operations.emplace_back(operation);
	operation->log_entry->log_entry_index = m_first_free_entry;
	m_first_free_entry = (m_first_free_entry + 1) % m_total_log_entries;
	m_free_log_entries--;
	operation->log_entry->pmem_entry = &pmem_log_entries[operation->log_entry->log_entry_index];
	operation->log_entry->ram_entry.first_pool_block = m_next_free_block;
	operation->log_entry->ram_entry.has_data = 1;
	m_next_free_block = (m_next_free_block + 1) % m_total_blocks;
	m_free_blocks -= operation->log_entry->ram_entry.write_bytes / BLOCK_SIZE;
	operation->log_entry->pmem_block =
	  pmem_blocks + (operation->log_entry->ram_entry.first_pool_block * BLOCK_SIZE);
	operation->log_entry->ram_entry.sync_gen_number = m_current_sync_gen;
	if (m_persist_on_flush) {
	  /* Persist on flush. Sequence #0 is never used. */
	  operation->log_entry->ram_entry.write_sequence_number = 0;
	} else {
	  /* Persist on write */
	  operation->log_entry->ram_entry.write_sequence_number = ++m_last_op_sequence_num;
	  operation->log_entry->ram_entry.sequenced = 1;
	}
	operation->log_entry->ram_entry.sync_point = 0;
	operation->log_entry->ram_entry.unmap = 0;
	operation->log_entry->ram_entry.entry_valid = 1;
	operation->bl.substr_of(bl, buffer_offset,
				operation->log_entry->ram_entry.write_bytes);
	buffer_offset += operation->log_entry->ram_entry.write_bytes;
	ldout(cct, 6) << "operation=[" << *operation << "]" << dendl;
      }
    }
  }

  /* Write data - drain once for all */
  for (auto &operation : operations) {
    bufferlist::iterator i(&operation->bl);
    i.copy((unsigned)operation->log_entry->ram_entry.write_bytes, (char*)operation->log_entry->pmem_block);
    ldout(cct, 6) << operation->bl << dendl;
    pmemobj_flush(m_log_pool, operation->log_entry->pmem_block, operation->log_entry->ram_entry.write_bytes);
  }
  pmemobj_drain(m_log_pool);

  /* TODO: For persist-on-flush, complete user write here */
  /* TODO: Better yet: split the loop above at the i.copy, and push
   * the rest to another thread. Rather than pushing as a work item,
   * Let a single WF do all the pmemobj_flush for all the waiting ops
   * (after sorting them in address order and merging them into fewer
   * larger persist calls), do one pmemobj_drain, then append all
   * their log entries on one transaction. */

  /* Write data is now (locally) persisted. It's now safe to persist
   *  the log entries that refer to them */
  
  /* Write log entries - write all in a transaction */
  TX_BEGIN(m_log_pool) {
    for (auto &operation : operations) {
      TX_MEMCPY(operation->log_entry->pmem_entry,
		&operation->log_entry->ram_entry,
		sizeof(operation->log_entry->ram_entry));
    }
  } TX_ONCOMMIT {
  } TX_ONABORT {
    lderr(cct) << "failed to commit log entries (" << m_log_pool_name << ")" << dendl;
    on_finish->complete(-1);
    return;
  } TX_FINALLY {
  } TX_END;
  
  
  /*// TODO handle fadvise flags
  BlockGuard::C_BlockRequest *req = new C_WriteBlockRequest<I>(
    m_image_ctx, m_image_writeback, *m_policy, *m_journal_store, *m_image_store,
    *m_meta_store, m_release_block, m_detain_block, std::move(bl), BLOCK_SIZE, on_finish);
  map_blocks(IO_TYPE_WRITE, std::move(image_extents), req);
  */

  /* Pass write through */
  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_write(std::move(image_extents), std::move(bl), fadvise_flags, on_finish);
  } else {
    m_image_writeback.aio_write(std::move(image_extents), std::move(bl), 0, on_finish);
  }
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

  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_discard(offset, length, skip_partial_discard, on_finish);
  } else {
    m_image_writeback.aio_discard(offset, length, skip_partial_discard, on_finish);
  }
  /*
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

  m_image_writeback.aio_discard(offset, length, skip_partial_discard, ctx->new_sub());

  ctx->activate();

  Context *invalidate_ctx = new FunctionContext(
    [this, offset, length, invalidate_done_ctx](int r) {
      invalidate({{offset, length}}, invalidate_done_ctx);
    });
  flush(invalidate_ctx);
  */
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
  
  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_flush(on_finish);
  } else {
    m_image_writeback.aio_flush(on_finish);
  }
  /*
  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      if (r < 0) {
        on_finish->complete(r);
      }
      m_image_writeback.aio_flush(on_finish);
    });

  flush(ctx);
  */
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

  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_writesame(offset, length, std::move(bl), fadvise_flags, on_finish);
  } else {
    m_image_writeback.aio_writesame(offset, length, std::move(bl), fadvise_flags, on_finish);
  }

  /*
  bufferlist total_bl;

  uint64_t left = length;
  while(left) {
    total_bl.append(bl);
    left -= bl.length();
  }
  assert(length == total_bl.length());
  aio_write({{offset, length}}, std::move(total_bl), fadvise_flags, on_finish);
  */
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

  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.aio_compare_and_write(
      std::move(image_extents), std::move(cmp_bl), std::move(bl), mismatch_offset,
      fadvise_flags, on_finish);
  } else {
    m_image_writeback.aio_compare_and_write(
      std::move(image_extents), std::move(cmp_bl), std::move(bl), mismatch_offset,
      fadvise_flags, on_finish);
  }
}

/**
 * Begin a new sync point
 */
template <typename I>
void ReplicatedWriteLog<I>::new_sync_point(void) {
  CephContext *cct = m_image_ctx.cct;
  SyncPoint *old_sync_point = m_current_sync_point;
  ldout(cct, 20) << dendl;

  assert(m_lock.is_locked());

  old_sync_point->m_final_op_sequence_num = m_last_op_sequence_num;
  m_current_sync_point = new SyncPoint(cct, ++m_current_sync_gen);
  /* Append of new sync point deferred until this sync point is persisted */
  old_sync_point->on_sync_point_persisted = m_current_sync_point->prior_log_entries_persisted->new_sub();
  /* This sync point will acquire no more sub-ops */
  old_sync_point->prior_log_entries_persisted->activate();

  ldout(cct,6) << "new sync point = [" << m_current_sync_point
	       << "], prior = [" << old_sync_point << "]" << dendl;
}

template <typename I>
void ReplicatedWriteLog<I>::init(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  TOID(struct WriteLogPoolRoot) pool_root;

  ldout(cct,5) << "rbd_rwl_enabled:" << cct->_conf->get_val<bool>("rbd_rwl_enabled") << dendl;
  ldout(cct,5) << "rbd_rwl_size:" << cct->_conf->get_val<uint64_t>("rbd_rwl_size") << dendl;
  std::string rwl_path = cct->_conf->get_val<std::string>("rbd_rwl_path");
  ldout(cct,5) << "rbd_rwl_path:" << rwl_path << dendl;

  m_log_pool_name = rwl_path + "/rbd-rwl." + m_image_ctx.id + ".pool";
  m_log_pool_size = ceph::max(cct->_conf->get_val<uint64_t>("rbd_rwl_size"), MIN_POOL_SIZE);
  m_policy->set_block_count(m_image_ctx.size / BLOCK_SIZE);
  
  if (access(m_log_pool_name.c_str(), F_OK) != 0) {
    if ((m_log_pool =
	 pmemobj_create(m_log_pool_name.c_str(),
			rwl_pool_layout_name,
			m_log_pool_size,
			(S_IWUSR | S_IRUSR))) == NULL) {
      lderr(cct) << "failed to create pool (" << m_log_pool_name << ")"
                 << pmemobj_errormsg() << dendl;
      on_finish->complete(-1);
      return;
    }
    pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);

    /* new pool, calculate and store metadata */
    size_t effective_pool_size = (size_t)(m_log_pool_size * USABLE_SIZE);
    size_t small_write_size = BLOCK_SIZE + sizeof(struct WriteLogPmemEntry);
    uint64_t num_small_writes = (uint64_t)(effective_pool_size / small_write_size);
    m_valid_entry_hint = 0;
    m_free_entry_hint = 0;
    /* Log ring empty */
    m_first_free_entry = 0;
    m_first_valid_entry = 0;
    /* Block pool empty */
    m_next_free_block = 0;
    m_last_free_block = 0;
    TX_BEGIN(m_log_pool) {
      TX_ADD(pool_root);
      D_RW(pool_root)->header.layout_version = RWL_POOL_VERSION;
      D_RW(pool_root)->block_size = BLOCK_SIZE;
      D_RW(pool_root)->num_blocks = num_small_writes-1; // leave one free
      D_RW(pool_root)->num_log_entries = num_small_writes-1; // leave one free
      D_RW(pool_root)->data_blocks = TX_ALLOC(uint8_t, num_small_writes * BLOCK_SIZE);
      D_RW(pool_root)->log_entries = TX_ZALLOC(struct WriteLogPmemEntry, num_small_writes);
      D_RW(pool_root)->valid_entry_hint = m_valid_entry_hint;
      D_RW(pool_root)->free_entry_hint = m_free_entry_hint;
    } TX_ONCOMMIT {
      m_total_log_entries = D_RO(pool_root)->num_log_entries;
      m_free_log_entries = D_RO(pool_root)->num_log_entries;
      m_total_blocks = D_RO(pool_root)->num_blocks;
      m_free_blocks = D_RO(pool_root)->num_blocks;
    } TX_ONABORT {
      m_total_log_entries = 0;
      m_total_blocks = 0;
      m_free_log_entries = 0;
      m_free_blocks = 0;
      lderr(cct) << "failed to initialize pool (" << m_log_pool_name << ")" << dendl;
      on_finish->complete(-1);
      return;
    } TX_FINALLY {
    } TX_END;
  } else {
    if ((m_log_pool =
	 pmemobj_open(m_log_pool_name.c_str(),
		      rwl_pool_layout_name)) == NULL) {
      lderr(cct) << "failed to open pool (" << m_log_pool_name << "): "
                 << pmemobj_errormsg() << dendl;
      on_finish->complete(-1);
      return;
    }
    pool_root = POBJ_ROOT(m_log_pool, struct WriteLogPoolRoot);
    if (D_RO(pool_root)->header.layout_version != RWL_POOL_VERSION) {
      lderr(cct) << "Pool layout version is " << D_RO(pool_root)->header.layout_version
		 << " expected " << RWL_POOL_VERSION << dendl;
      on_finish->complete(-1);
      return;
    }
    if (D_RO(pool_root)->block_size != BLOCK_SIZE) {
      lderr(cct) << "Pool block size is " << D_RO(pool_root)->block_size << " expected " << BLOCK_SIZE << dendl;
      on_finish->complete(-1);
      return;
    }
    m_total_log_entries = D_RO(pool_root)->num_log_entries;
    m_free_log_entries = D_RO(pool_root)->num_log_entries;
    m_total_blocks = D_RO(pool_root)->num_blocks;
    m_free_blocks = D_RO(pool_root)->num_blocks;
    m_valid_entry_hint = D_RO(pool_root)->valid_entry_hint;
    m_free_entry_hint = D_RO(pool_root)->free_entry_hint;
    /* TODO: Actually load all the log entries already persisted */
    /* TODO: Set m_current_sync_gen to the successor of the last one seen in the log */
    ldout(cct,5) << "pool " << m_log_pool_name << "has " << D_RO(pool_root)->num_log_entries <<
      " log entries and " << D_RO(pool_root)->num_blocks << " data blocks" << dendl;
    if (m_valid_entry_hint == m_free_entry_hint) {
      ldout(cct,5) << "write log is empy" << dendl;
    }
  }

  /* Start the sync point following the last one seen in the log */
  m_current_sync_point = new SyncPoint(cct, m_current_sync_gen);
  ldout(cct,6) << "new sync point = [" << m_current_sync_point << "]" << dendl;
  
  // chain the initialization of the meta, image, and journal stores
  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      if (r >= 0) {
	// Init succeeded
      }
      on_finish->complete(r);
    });
  if (m_image_ctx.persistent_cache_enabled) {
    m_image_cache.init(ctx);
  } else {
    ctx->complete(0);
  }
}

template <typename I>
void ReplicatedWriteLog<I>::shut_down(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  // TODO flush all in-flight IO and pending writeback prior to shut down

  Context *ctx = new FunctionContext(
    [this, on_finish](int r) {
      pmemobj_close(m_log_pool);
      on_finish->complete(r);
    });
  if (m_image_ctx.persistent_cache_enabled) {
    ctx = new FunctionContext(
      [this, ctx](int r) {
        if (r < 0) ctx->complete(r);
	m_image_cache.shut_down(ctx);
      });
  }
  ctx = new FunctionContext(
    [this, ctx](int r) {
      // flush writeback journal to OSDs
      if (r < 0) ctx->complete(r);
      flush(ctx);
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
	m_image_cache.invalidate(on_finish);
      } else {
	on_finish->complete(0);
      }
    });
  // TODO
  invalidate({{0, m_image_ctx.size}}, ctx);
}

template <typename I>
void ReplicatedWriteLog<I>::map_blocks(IOType io_type, Extents &&image_extents,
                                   BlockGuard::C_BlockRequest *block_request) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  BlockGuard::BlockIOs block_ios;
  m_block_guard.create_block_ios(io_type, image_extents, &block_ios,
                                 block_request);

  // map block IO requests to the cache or backing image based upon policy
  for (auto &block_io : block_ios) {
    map_block(true, std::move(block_io));
  }

  // advance the policy statistics
  m_policy->tick();
  block_request->activate();
}

template <typename I>
void ReplicatedWriteLog<I>::map_block(bool detain_block,
                                  BlockGuard::BlockIO &&block_io) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "block_io=[" << block_io << "]" << dendl;

  int r;
  if (detain_block) {
    r = m_block_guard.detain(block_io.block, &block_io);
    if (r < 0) {
      Mutex::Locker locker(m_lock);
      ldout(cct, 20) << "block guard full -- deferring block IO" << dendl;
      m_deferred_block_ios.emplace_back(std::move(block_io));
      return;
    } else if (r > 0) {
      ldout(cct, 20) << "block already detained" << dendl;
      return;
    }
  }

  IOType io_type = static_cast<IOType>(block_io.io_type);
  PolicyMapResult policy_map_result;
  uint64_t replace_cache_block;
  r = m_policy->map(io_type, block_io.block, block_io.partial_block,
                    &policy_map_result, &replace_cache_block);
  if (r < 0) {
    // fail this IO and release any detained IOs to the block
    lderr(cct) << "failed to map block via cache policy: " << cpp_strerror(r)
               << dendl;
    block_io.block_request->fail(r);
    release_block(block_io.block);
    return;
  }

  block_io.block_request->remap(policy_map_result, std::move(block_io));
}

template <typename I>
void ReplicatedWriteLog<I>::release_block(uint64_t block) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "block=" << block << dendl;

  Mutex::Locker locker(m_lock);
  m_block_guard.release(block, &m_detained_block_ios);
  wake_up();
}

template <typename I>
void ReplicatedWriteLog<I>::append_detain_block(BlockGuard::BlockIO &block_io) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "block=" << block_io.block << dendl;

  Mutex::Locker locker(m_lock);
  m_detained_block_ios.emplace_back(std::move(block_io));
}

template <typename I>
void ReplicatedWriteLog<I>::wake_up() {
  assert(m_lock.is_locked());
  if (m_wake_up_scheduled || m_async_op_tracker.is_waiting()) {
    return;
  }

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

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
  ldout(cct, 20) << dendl;

  do {
    process_writeback_dirty_blocks();
    process_detained_block_ios();
    process_deferred_block_ios();

#if 0
    // TODO
    Contexts post_work_contexts;
    {
      Mutex::Locker locker(m_lock);
      post_work_contexts.swap(m_post_work_contexts);
    }
    if (!post_work_contexts.empty()) {
      for (auto ctx : post_work_contexts) {
        //TODO: fix flush post work
        //ctx->complete(0);
      }
      continue;
    }
#endif
  } while (false); // TODO need metric to only perform X amount of work per cycle

  // process delayed shut down request (if any)
  {
    Mutex::Locker locker(m_lock);
    m_wake_up_scheduled = false;
    m_async_op_tracker.finish_op();
  }
}

template <typename I>
bool ReplicatedWriteLog<I>::is_work_available() const {
  Mutex::Locker locker(m_lock);
  return (!m_detained_block_ios.empty() ||
          !m_deferred_block_ios.empty());
}

template <typename I>
void ReplicatedWriteLog<I>::process_writeback_dirty_blocks() {
#if 0
  CephContext *cct = m_image_ctx.cct;

  // TODO throttle the amount of in-flight writebacks
  while (true) {
    uint64_t tid = 0;
    uint64_t block;
    IOType io_type;
    bool demoted;
    int r = m_journal_store->get_writeback_event(&tid, &block, &io_type,
                                                 &demoted);
    //int r = m_policy->get_writeback_block(&block);
    if (r == -ENODATA || r == -EBUSY) {
      // nothing to writeback
      return;
    } else if (r < 0) {
      lderr(cct) << "failed to retrieve writeback block: "
                 << cpp_strerror(r) << dendl;
      return;
    }

    // block is now detained -- safe for writeback
    C_WritebackRequest<I> *req = new C_WritebackRequest<I>(
      m_image_ctx, m_image_writeback, *m_policy, *m_journal_store,
      *m_image_store, m_release_block, m_async_op_tracker, tid, block, io_type,
      demoted, BLOCK_SIZE);
    req->send();
  }
#endif
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
    map_block(false, std::move(block_io));
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
    map_block(true, std::move(block_io));
  }
}

template <typename I>
void ReplicatedWriteLog<I>::invalidate(Extents&& image_extents,
                                   Context *on_finish) {
  //CephContext *cct = m_image_ctx.cct;
  //ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  // TODO - ensure sync with in-flight flushes
  for (auto &extent : image_extents) {
    uint64_t image_offset = extent.first;
    uint64_t image_length = extent.second;
    while (image_length > 0) {
      //uint64_t block = m_meta_store->offset_to_block(image_offset);
      uint32_t block_start_offset = image_offset % BLOCK_SIZE;
      uint32_t block_end_offset = MIN(block_start_offset + image_length,
                                      BLOCK_SIZE);
      uint32_t block_length = block_end_offset - block_start_offset;

      //m_policy->invalidate(block);
      image_offset += block_length;
      image_length -= block_length;
    }
  }

  on_finish->complete(0);
}

template <typename I>
void ReplicatedWriteLog<I>::flush(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  // TODO -- need deferred set for tracking op ordering
#if 0
  if (m_journal_store->is_writeback_pending()) {
    Mutex::Locker locker(m_lock);
    m_post_work_contexts.push_back(new FunctionContext(
      [this, on_finish](int r) {
        flush(on_finish);
      }));
  }
#endif
  
  // image cache will be write-through, so no flush passed on
  on_finish->complete(0);

  // TODO
  // internal flush -- nothing to writeback but make sure
  // in-flight IO is flushed
  //aio_flush(on_finish);
}

} // namespace cache
} // namespace librbd

template class librbd::cache::ReplicatedWriteLog<librbd::ImageCtx>;

/* Local Variables: */
/* eval: (c-set-offset 'innamespace 0) */
/* End: */
