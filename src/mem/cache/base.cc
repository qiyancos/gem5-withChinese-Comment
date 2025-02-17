/*
 * Copyright (c) 2012-2013, 2018 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Erik Hallnor
 *          Nikos Nikoleris
 */

/**
 * @file
 * Definition of BaseCache functions.
 */

#include "mem/cache/base.hh"

#include "base/compiler.hh"
#include "base/logging.hh"
#include "debug/Cache.hh"
#include "debug/CachePort.hh"
#include "debug/CacheRepl.hh"
#include "debug/CacheVerbose.hh"
#include "mem/cache/mshr.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/cache/prefetch_filter/base.hh"
#include "mem/cache/prefetch_filter/pref_info.hh"
#include "mem/cache/prefetch_filter/debug_flag.hh"
#include "mem/cache/queue_entry.hh"
#include "params/BaseCache.hh"
#include "params/WriteAllocator.hh"
#include "sim/core.hh"


class BaseMasterPort;
class BaseSlavePort;

using namespace std;

std::vector<std::string> BaseCache::levelName_;

uint8_t BaseCache::initPrefetcherCount_ = 0;

BaseCache::CacheSlavePort::CacheSlavePort(const std::string &_name,
                                          BaseCache *_cache,
                                          const std::string &_label)
    : QueuedSlavePort(_name, _cache, queue),
      queue(*_cache, *this, true, _label),
      blocked(false), mustSendRetry(false),
      sendRetryEvent([this]{ processSendRetry(); }, _name)
{
}

BaseCache::BaseCache(const BaseCacheParams *p, unsigned blk_size)
    : MemObject(p),
      /// 初始化
      cpuIds_(p->cpu_ids.begin(), p->cpu_ids.end()),
      cacheLevel_(p->cache_level),
      prefetchFilter_(p->prefetch_filter),
      enableHarmTable_(p->enable_harm_table),

      cpuSidePort (p->name + ".cpu_side", this, "CpuSidePort"),
      memSidePort(p->name + ".mem_side", this, "MemSidePort"),
      prefetcher(p->prefetcher),
      mshrQueue("MSHRs", p->mshrs, 0, p->demand_mshr_reserve), // see below
      writeBuffer("write buffer", p->write_buffers, p->mshrs), // see below
      tags(p->tags),
      writeAllocator(p->write_allocator),
      writebackClean(p->writeback_clean),
      tempBlockWriteback(nullptr),
      writebackTempBlockAtomicEvent([this]{ writebackTempBlockAtomic(); },
                                    name(), false,
                                    EventBase::Delayed_Writeback_Pri),
      blkSize(blk_size),
      lookupLatency(p->tag_latency),
      dataLatency(p->data_latency),
      forwardLatency(p->tag_latency),
      fillLatency(p->data_latency),
      responseLatency(p->response_latency),
      sequentialAccess(p->sequential_access),
      numTarget(p->tgts_per_mshr),
      forwardSnoops(true),
      clusivity(p->clusivity),
      isReadOnly(p->is_read_only),
      blocked(0),
      order(0),
      noTargetMSHR(nullptr),
      missCount(p->max_miss_count),
      addrRanges(p->addr_ranges.begin(), p->addr_ranges.end()),
      system(p->system)
{
    /// 只针对标准化的CPU Cache进行初始化
    if (cacheLevel_ != 255) {
        if (prefetchFilter_) {
            /// 更新全局实例化指针
            prefetchFilter_->addCache(this);
        }
        /// 初始化CacheID
        prefetcherId_ = prefetcher ? initPrefetcherCount_++ : 255;
        /// 初始化LevelName
        if (levelName_.size() < cacheLevel_ + 2) {
            levelName_.resize(cacheLevel_ + 2);
            levelName_.back() = "DRAM";
        }
        switch(cacheLevel_) {
        case 0: levelName_[cacheLevel_] = "L1ICache"; break;
        case 1: levelName_[cacheLevel_] = "L1DCache"; break;
        default: levelName_[cacheLevel_] = std::string("L") +
                std::to_string(cacheLevel_) + "Cache"; break;
        }
        /// 打印初始化信息用于后期Debug对应Cache
        std::string cpuInfo;
        for (auto id : cpuIds_) {
            cpuInfo = cpuInfo + std::to_string(id) + "_";
        }
        fprintf(stderr, "Init Cache[%p] with level %d as %s for cpu [%s]\n",
                this, cacheLevel_, levelName_[cacheLevel_].c_str(),
                cpuInfo.substr(0, cpuInfo.size() - 1).c_str());
    } else if (prefetchFilter_) {
        /// 对于非CPU的Cache删除该结构
        prefetchFilter_ = nullptr;
    }
    // the MSHR queue has no reserve entries as we check the MSHR
    // queue on every single allocation, whereas the write queue has
    // as many reserve entries as we have MSHRs, since every MSHR may
    // eventually require a writeback, and we do not check the write
    // buffer before committing to an MSHR

    // forward snoops is overridden in init() once we can query
    // whether the connected master is actually snooping or not

    tempBlock = new TempCacheBlk(blkSize);
  
    tags->tagsInit();
    if (prefetcher)
        prefetcher->setCache(this);
}

BaseCache::~BaseCache()
{
    delete tempBlock;
}

std::string
BaseCache::getName() {
    std::string baseName = levelName_[cacheLevel_];
    if (cpuIds_.size() > 1) {
        baseName += "_all";
    } else {
        baseName += "_" + std::to_string(*(cpuIds_.begin()));
    }
    return baseName;
}

void
BaseCache::setBlocked(BlockedCause cause) {
    uint8_t flag = 1 << cause;
    if (blocked == 0) {
        DPRINTF(Cache,"Start blocking for cause %d, mask=%d\n",
                cause, blocked);
        DEBUG_MEM("%s start blocking for cause %d, mask=%d",
                getName().c_str(), cause, blocked);
        blocked_causes[cause]++;
        blockedCycle = curCycle();
        cpuSidePort.setBlocked();
    } else {
        DPRINTF(Cache,"Keep blocking for cause %d, mask=%d\n",
                cause, blocked);
        DEBUG_MEM("%s keep blocking for cause %d, mask=%d",
                getName().c_str(), cause, blocked);
    }
    blocked |= flag;
}

void
BaseCache::clearBlocked(BlockedCause cause) {
    uint8_t flag = 1 << cause;
    blocked &= ~flag;
    DPRINTF(Cache,"Unblocking for cause %d, mask=%d\n", cause, blocked);
    DEBUG_MEM("%s unblocking for cause %d, mask=%d\n",
            getName().c_str(), cause, blocked);
    if (blocked == 0) {
        blocked_cycles[cause] += curCycle() - blockedCycle;
        cpuSidePort.clearBlocked();
    }
}

bool
BaseCache::readyForLevelupPref(PacketPtr pkt) {
    std::vector<Stats::Vector*>* dismissedStats = nullptr;
    /// 判断是否有足够的WriteBuffer
    int wbForPref = writeBuffer.numEntries - writeBuffer.allocated -
            mshrQueue._numInService;
    DEBUG_MEM("%s has %d available write buffer for level-up prefetch",
            getName().c_str(), wbForPref);
    dismissedStats = wbForPref <= 0 && prefetchFilter_ ? 
            &(BasePrefetchFilter::dismissedLevelUpPrefNoWB_) :
            dismissedStats;
    
    // 判断是否已存在记录
    CacheBlk *blk = tags->findBlock(pkt->getAddr(), pkt->isSecure());
    MSHR* mshr = pkt->mshr_.get();
    MSHR *oldmshr = mshrQueue.findMatch(pkt->getAddr(), pkt->isSecure());
    WriteQueueEntry *oldWBEntry = writeBuffer.findMatch(
            pkt->getAddr(), pkt->isSecure());
    /// 如果提升等级发现了已经存在MSHR/WB/Block
    if (oldmshr || oldWBEntry || blk) {
        /// 如果已存在的MSHR/WB/Block，则会直接无效化提升等级预取的响应
        assert(mshr);
        dismissedStats = prefetchFilter_ ? 
                &(BasePrefetchFilter::dismissedLevelUpPrefLate_) :
                dismissedStats;
    }

    if (dismissedStats) {
        for (auto cpuId : cpuIds_) {
            (*(*dismissedStats)[cacheLevel_])[cpuId]++;
        }
        return false;
    } else {
        return true;
    }
}

void
BaseCache::CacheSlavePort::setBlocked()
{
    assert(!blocked);
    DPRINTF(CachePort, "Port is blocking new requests\n");
    blocked = true;
    // if we already scheduled a retry in this cycle, but it has not yet
    // happened, cancel it
    if (sendRetryEvent.scheduled()) {
        owner.deschedule(sendRetryEvent);
        DPRINTF(CachePort, "Port descheduled retry\n");
        mustSendRetry = true;
    }
}

void
BaseCache::CacheSlavePort::clearBlocked()
{
    assert(blocked);
    DPRINTF(CachePort, "Port is accepting new requests\n");
    blocked = false;
    if (mustSendRetry) {
        // @TODO: need to find a better time (next cycle?)
        owner.schedule(sendRetryEvent, curTick() + 1);
    }
}

void
BaseCache::CacheSlavePort::processSendRetry()
{
    DPRINTF(CachePort, "Port is sending retry\n");

    // reset the flag and call retry
    mustSendRetry = false;
    sendRetryReq();
}

MSHR*
BaseCache::allocateMissBuffer(PacketPtr pkt, Tick time, bool sched_send)
{
    MSHR *mshr = mshrQueue.allocate(pkt->getBlockAddr(blkSize), blkSize,
                                    pkt, time, order++,
                                    allocOnFill(pkt->cmd));
    
    DEBUG_MEM("%s allocate MSHR[%p] for %s packet[%p : %s] @0x%lx",
            getName().c_str(), mshr,
            prefetch_filter::getDataTypeString(pkt->packetType_).c_str(), pkt,
            pkt->cmd.toString().c_str(), pkt->getAddr());

    if (mshrQueue.isFull()) {
        setBlocked((BlockedCause)MSHRQueue_MSHRs);
    }

    if (sched_send) {
        // schedule the send
        schedMemSideSendEvent(time);
    }

    return mshr;
}

void
BaseCache::allocateWriteBuffer(PacketPtr pkt, Tick time)
{
    // should only see writes or clean evicts here
    assert(pkt->isWrite() || pkt->cmd == MemCmd::CleanEvict);

    Addr blk_addr = pkt->getBlockAddr(blkSize);

    WriteQueueEntry *wq_entry =
        writeBuffer.findMatch(blk_addr, pkt->isSecure());
    if (wq_entry && !wq_entry->inService) {
        DPRINTF(Cache, "Potential to merge writeback %s", pkt->print());
    }

    writeBuffer.allocate(blk_addr, blkSize, pkt, time, order++);

    if (writeBuffer.isFull()) {
        setBlocked((BlockedCause)MSHRQueue_WriteBuffer);
    }

    // schedule the send
    schedMemSideSendEvent(time);
}

Addr
BaseCache::regenerateBlkAddr(CacheBlk* blk)
{
    if (blk != tempBlock) {
        return tags->regenerateBlkAddr(blk);
    } else {
        return tempBlock->getAddr();
    }
}

void
BaseCache::init()
{
    if (!cpuSidePort.isConnected() || !memSidePort.isConnected())
        fatal("Cache ports on %s are not connected\n", name());
    cpuSidePort.sendRangeChange();
    forwardSnoops = cpuSidePort.isSnooping();
}

BaseMasterPort &
BaseCache::getMasterPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side") {
        return memSidePort;
    }  else {
        return MemObject::getMasterPort(if_name, idx);
    }
}

BaseSlavePort &
BaseCache::getSlavePort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side") {
        return cpuSidePort;
    } else {
        return MemObject::getSlavePort(if_name, idx);
    }
}

bool
BaseCache::inRange(Addr addr) const
{
    for (const auto& r : addrRanges) {
        if (r.contains(addr)) {
            return true;
       }
    }
    return false;
}

void
BaseCache::handleTimingReqHit(PacketPtr pkt, CacheBlk *blk, Tick request_time,
        const bool notifyHit)
{
    if (pkt->needsResponse()) {
        /// 如果是一个降级预取命中，没有必要回复
        if (pkt->packetType_ == prefetch_filter::Pref) {
            if (cacheLevel_ <= pkt->targetCacheLevel_) {
                delete pkt;
                return;
            } else {
                /// 更新时间戳信息
                uint8_t realCacheLevel = cacheLevel_ ? cacheLevel_ : 1;
                pkt->setTimeStamp(realCacheLevel, Packet::WhenSend, curTick());
                pkt->setTimeStamp(realCacheLevel, Packet::WhenFill, curTick());
            }
        }
        // These delays should have been consumed by now
        assert(pkt->headerDelay == 0);
        assert(pkt->payloadDelay == 0);

        pkt->makeTimingResponse();
        /// 标记最近处理的Cache
        pkt->recentCache_ = this;
        
        // In this case we are considering request_time that takes
        // into account the delay of the xbar, if any, and just
        // lat, neglecting responseLatency, modelling hit latency
        // just as the value of lat overriden by access(), which calls
        // the calculateAccessLatency() function.
        cpuSidePort.schedTimingResp(pkt, request_time);
    } else {
        DPRINTF(Cache, "%s satisfied %s, no response needed\n", __func__,
                pkt->print());

        // queue the packet for deletion, as the sending cache is
        // still relying on it; if the block is found in access(),
        // CleanEvict and Writeback messages will be deleted
        // here as well
        pendingDelete.reset(pkt);
    }
}

void
BaseCache::handleTimingReqMiss(PacketPtr pkt, MSHR *mshr, CacheBlk *blk,
                               Tick forward_time, Tick request_time,
                               bool* pktDeleted)
{
    if (writeAllocator &&
        pkt && pkt->isWrite() && !pkt->req->isUncacheable()) {
        writeAllocator->updateMode(pkt->getAddr(), pkt->getSize(),
                                   pkt->getBlockAddr(blkSize));
    }
    
    /// 记录因为合并MSHR导致无效化的Packet
    std::set<PacketPtr> deletedPacket;
    if (mshr) {
        // MSHR hit
        // @note writebacks will be checked in getNextMSHR()
        // for any conflicting requests to the same block

        //@todo remove hw_pf here

        // Coalesce unless it was a software prefetch (see above).
        if (pkt) {
            assert(!pkt->isWriteback());
            // CleanEvicts corresponding to blocks which have
            // outstanding requests in MSHRs are simply sunk here
            if (pkt->cmd == MemCmd::CleanEvict) {
                pendingDelete.reset(pkt);
            } else if (pkt->cmd == MemCmd::WriteClean) {
                // A WriteClean should never coalesce with any
                // outstanding cache maintenance requests.

                // We use forward_time here because there is an
                // uncached memory write, forwarded to WriteBuffer.
                allocateWriteBuffer(pkt, forward_time);
            } else {
                DPRINTF(Cache, "%s coalescing MSHR for %s\n", __func__,
                        pkt->print());

                assert(pkt->req->masterId() < system->maxMasters());
                mshr_hits[pkt->cmdToIndex()][pkt->req->masterId()]++;

                // We use forward_time here because it is the same
                // considering new targets. We have multiple
                // requests for the same address here. It
                // specifies the latency to allocate an internal
                // buffer and to schedule an event to the queued
                // port and also takes into account the additional
                // delay of the xbar.
                mshr->allocateTarget(pkt, forward_time, order++,
                                     allocOnFill(pkt->cmd), this,
                                     &deletedPacket);
                if (mshr->getNumTargets() == numTarget) {
                    noTargetMSHR = mshr;
                    setBlocked(Blocked_NoTargets);
                    // need to be careful with this... if this mshr isn't
                    // ready yet (i.e. time > curTick()), we don't want to
                    // move it ahead of mshrs that are ready
                    // mshrQueue.moveToFront(mshr);
                }
            }
        }
    } else {
        // no MSHR
        assert(pkt->req->masterId() < system->maxMasters());
        mshr_misses[pkt->cmdToIndex()][pkt->req->masterId()]++;

        if (pkt->isEviction() || pkt->cmd == MemCmd::WriteClean) {
            // We use forward_time here because there is an
            // writeback or writeclean, forwarded to WriteBuffer.
            allocateWriteBuffer(pkt, forward_time);
        } else {
            if (blk && blk->isValid()) {
                // If we have a write miss to a valid block, we
                // need to mark the block non-readable.  Otherwise
                // if we allow reads while there's an outstanding
                // write miss, the read could return stale data
                // out of the cache block... a more aggressive
                // system could detect the overlap (if any) and
                // forward data out of the MSHRs, but we don't do
                // that yet.  Note that we do need to leave the
                // block valid so that it stays in the cache, in
                // case we get an upgrade response (and hence no
                // new data) when the write miss completes.
                // As long as CPUs do proper store/load forwarding
                // internally, and have a sufficiently weak memory
                // model, this is probably unnecessary, but at some
                // point it must have seemed like we needed it...
                assert((pkt->needsWritable() && !blk->isWritable()) ||
                       pkt->req->isCacheMaintenance());
                blk->status &= ~BlkReadable;
            }
            // Here we are using forward_time, modelling the latency of
            // a miss (outbound) just as forwardLatency, neglecting the
            // lookupLatency component.
            allocateMissBuffer(pkt, forward_time);
        }
    }
    
    /// 进行发生Miss情况的训练操作
    /// dmd->pref; pref->pref; pref->pendingPref
    if (deletedPacket.size() && prefetchFilter_) {
        panic_if(!mshr, "This should never happen");
        for (auto combinedPkt : deletedPacket) {
            prefetch_filter::DataTypeInfo infoPair;
            infoPair.source = pkt->packetType_;
            infoPair.target = mshr->inService ? prefetch_filter::PendingPref :
                    prefetch_filter::Pref;
            prefetchFilter_->notifyCacheMiss(this, pkt, combinedPkt, infoPair);
            delete combinedPkt;
        }
    }
    if (pktDeleted) {
        *pktDeleted = deletedPacket.find(pkt) != deletedPacket.end();
    }
}

void
BaseCache::recvTimingReq(PacketPtr pkt)
{
    // anything that is merely forwarded pays for the forward latency and
    // the delay provided by the crossbar
    Tick forward_time = clockEdge(forwardLatency) + pkt->headerDelay;

    Cycles lat;
    CacheBlk *blk = nullptr;
    bool notifyHit = true;
    bool notifyMiss = true;
    bool notifyFill = true;
    bool satisfied = false;
    {
        PacketList writebacks;
        // Note that lat is passed by reference here. The function
        // access() will set the lat value.
        satisfied = access(pkt, blk, lat, writebacks, &notifyHit,
                &notifyMiss, &notifyFill);

        // After the evicted blocks are selected, they must be forwarded
        // to the write buffer to ensure they logically precede anything
        // happening below
        doWritebacks(writebacks, clockEdge(lat + forwardLatency));
    }

    // Here we charge the headerDelay that takes into account the latencies
    // of the bus, if the packet comes from it.
    // The latency charged is just the value set by the access() function.
    // In case of a hit we are neglecting response latency.
    // In case of a miss we are neglecting forward latency.
    Tick request_time = clockEdge(lat);
    // Here we reset the timing of the packet.
    pkt->headerDelay = pkt->payloadDelay = 0;

    if (satisfied) {
        // notify before anything else as later handleTimingReqHit might turn
        // the packet in a response
        ppHit->notify(pkt);

        handleTimingReqHit(pkt, blk, request_time, notifyHit);
        
        // if (prefetcher && blk && blk->wasPrefetched()) {
        /// 由于现在允许预取的位置变更，因此即使没有Prefetcher也需要处理
        if (blk && blk->wasPrefetched() &&
                pkt->packetType_ != prefetch_filter::Pref) {
            blk->status &= ~BlkHWPrefetched;
        }
    } else {
        bool pktDeleted = false;
        handleTimingReqMiss(pkt, blk, forward_time, request_time, &pktDeleted);
        if (!pktDeleted) {
            ppMiss->notify(pkt);
        }
    }

    if (prefetcher) {
        // track time of availability of next prefetch, if any
        Tick next_pf_time = prefetcher->nextPrefetchReadyTime();
        if (next_pf_time != MaxTick) {
            schedMemSideSendEvent(next_pf_time);
        }
    }
}

void
BaseCache::handleUncacheableWriteResp(PacketPtr pkt)
{
    Tick completion_time = clockEdge(responseLatency) +
        pkt->headerDelay + pkt->payloadDelay;

    // Reset the bus additional time as it is now accounted for
    pkt->headerDelay = pkt->payloadDelay = 0;

    cpuSidePort.schedTimingResp(pkt, completion_time);
}

void
BaseCache::recvTimingResp(PacketPtr pkt)
{
    assert(pkt->isResponse());
    DEBUG_MEM("%s recv timing resp packet[%p : %s] @0x%lx ",
            getName().c_str(), pkt,
            pkt->cmd.toString().c_str(), pkt->getAddr());

    if (pkt->packetType_ == prefetch_filter::Pref) {
        DEBUG_MEM("%s recv prefetch resp @0x%lx  [%s -> %s]",
                getName().c_str(), pkt->getAddr(),
                BaseCache::levelName_[pkt->srcCacheLevel_].c_str(),
                BaseCache::levelName_[pkt->targetCacheLevel_].c_str());
    }

    // all header delay should be paid for by the crossbar, unless
    // this is a prefetch response from above
    panic_if(pkt->headerDelay != 0 && pkt->cmd != MemCmd::HardPFResp,
             "%s saw a non-zero packet delay\n", name());

    const bool is_error = pkt->isError();

    if (is_error) {
        DPRINTF(Cache, "%s: Cache received %s with error\n", __func__,
                pkt->print());
    }

    DPRINTF(Cache, "%s: Handling response %s\n", __func__,
            pkt->print());

    // if this is a write, we should be looking at an uncacheable
    // write
    if (pkt->isWrite()) {
        assert(pkt->req->isUncacheable());
        handleUncacheableWriteResp(pkt);
        return;
    }

    MSHR *mshr;
    /// 表明当前的预取是否需要执行提级预取的处理
    const bool needLevelUpPrefProcess =
            pkt->packetType_ == prefetch_filter::Pref &&
            cacheLevel_ >= pkt->targetCacheLevel_ &&
            cacheLevel_ < pkt->srcCacheLevel_ &&
            pkt->caches_.find(this) == pkt->caches_.end();
    
    CacheBlk *blk = tags->findBlock(pkt->getAddr(), pkt->isSecure());

    if (needLevelUpPrefProcess) {
        /// 如果我们发现Packet是一个提升级别的预取
        /// 那么会获取Pakcet里面存放着的额外MSHR
        mshr = pkt->mshr_.get();
        /// 如果XBar处理正确这里不应该出现冲突的提级预取
        panic_if(blk, "We do not expect levelup prefetch conflict with "
                "existing cache block here.");
        if (!readyForLevelupPref(pkt)) {
            delete pkt;
            return;
        }
    } else {
        // we have dealt with any (uncacheable) writes above, from here on
        // we know we are dealing with an MSHR due to a miss or a prefetch
        mshr = dynamic_cast<MSHR*>(pkt->popSenderState());
    }

    assert(mshr);
    
    if (mshr == noTargetMSHR) {
        // we always clear at least one target
        clearBlocked(Blocked_NoTargets);
        noTargetMSHR = nullptr;
    }

    // Initial target is used just for stats
    MSHR::Target *initial_tgt = &(mshr->targets.front());
    int stats_cmd_idx = initial_tgt->pkt->cmdToIndex();
    Tick miss_latency = curTick() - initial_tgt->recvTime;

    if (pkt->req->isUncacheable()) {
        assert(pkt->req->masterId() < system->maxMasters());
        mshr_uncacheable_lat[stats_cmd_idx][pkt->req->masterId()] +=
            miss_latency;
    } else {
        assert(pkt->req->masterId() < system->maxMasters());
        mshr_miss_latency[stats_cmd_idx][pkt->req->masterId()] +=
            miss_latency;
    }

    PacketList writebacks;

    bool is_fill = !mshr->isForward &&
        (pkt->isRead() || pkt->cmd == MemCmd::UpgradeResp ||
         mshr->wasWholeLineWrite);

    // make sure that if the mshr was due to a whole line write then
    // the response is an invalidation
    assert(!mshr->wasWholeLineWrite || pkt->isInvalidate());

    if (is_fill && !is_error) {
        DPRINTF(Cache, "Block for addr %#llx being updated in Cache\n",
                pkt->getAddr());
        
        const bool allocate = (writeAllocator && mshr->wasWholeLineWrite) ?
            writeAllocator->allocate() : mshr->allocOnFill();
        blk = handleFill(pkt, blk, writebacks, allocate);
        assert(blk != nullptr);
        ppFill->notify(pkt);
    }

    if (blk && blk->isValid() && pkt->isClean() && !pkt->isInvalidate()) {
        // The block was marked not readable while there was a pending
        // cache maintenance operation, restore its flag.
        blk->status |= BlkReadable;

        // This was a cache clean operation (without invalidate)
        // and we have a copy of the block already. Since there
        // is no invalidation, we can promote targets that don't
        // require a writable copy
        mshr->promoteReadable();
    }

    if (blk && blk->isWritable() && !pkt->req->isCacheInvalidate()) {
        // If at this point the referenced block is writable and the
        // response is not a cache invalidate, we promote targets that
        // were deferred as we couldn't guarrantee a writable copy
        mshr->promoteWritable();
    }
    
    /// 这里会进行PendingPref覆盖情况的后后续处理
    if (is_fill && !is_error && blk &&
            prefetchFilter_ && mshr->needPostProcess_ &&
            mshr->prefTarget_->pkt->packetType_ == prefetch_filter::Pref) {
        std::vector<PacketPtr> postAddedPkt;
        for (auto& target : mshr->targets) {
            if (target.postAddedTarget_) {
                postAddedPkt.push_back(target.pkt);
            }
        }
        for (int i = 0; i < postAddedPkt.size(); i++) {
            PacketPtr targetPkt = postAddedPkt[i];
            panic_if(!(targetPkt->packetType_ == prefetch_filter::Dmd ||
                    (targetPkt->packetType_ == prefetch_filter::Pref &&
                    targetPkt->srcCacheLevel_ < cacheLevel_ &&
                    targetPkt->targetCacheLevel_ < cacheLevel_)),
                    "Found unexpected MSHR target status when doing"
                    " post process");
            /// 处理PendingPref被Demand/Pref覆盖的情况
            Addr hitAddr = pkt->getAddr();
            prefetch_filter::DataTypeInfo infoPair;
            infoPair.target = i == postAddedPkt.size() - 1 ?
                    prefetch_filter::Pref : prefetch_filter::PendingPref;
            infoPair.source = targetPkt->packetType_;
            prefetchFilter_->notifyCacheHit(this, targetPkt, hitAddr,
                    infoPair);
        }
    }

    serviceMSHRTargets(mshr, pkt, blk);

    if (mshr->promoteDeferredTargets()) {
        // avoid later read getting stale data while write miss is
        // outstanding.. see comment in timingAccess()
        if (blk) {
            blk->status &= ~BlkReadable;
        }
        mshrQueue.markPending(mshr);
        schedMemSideSendEvent(clockEdge() + pkt->payloadDelay);
    } else {
        if (needLevelUpPrefProcess && cacheLevel_ == pkt->targetCacheLevel_) {
            /// 针对提升等级的预取，并且是目标层级，会删除mshr
            // delete mshr;
            // pkt->mshr_ = nullptr;
        } else if (!needLevelUpPrefProcess) {
            // while we deallocate an mshr from the queue we still have to
            // check the isFull condition before and after as we might
            // have been using the reserved entries already
            const bool was_full = mshrQueue.isFull();
            DEBUG_MEM("%s deallocate MSHR[%p] for packet[%p : %s] @0x%lx",
                    getName().c_str(), mshr, pkt,
                    pkt->cmd.toString().c_str(), pkt->getAddr());
            mshrQueue.deallocate(mshr);
            if (was_full && !mshrQueue.isFull()) {
                clearBlocked(Blocked_NoMSHRs);
            }
        }

        // Request the bus for a prefetch if this deallocation freed enough
        // MSHRs for a prefetch to take place
        if (prefetcher && mshrQueue.canPrefetch()) {
            Tick next_pf_time = std::max(prefetcher->nextPrefetchReadyTime(),
                                         clockEdge());
            if (next_pf_time != MaxTick)
                schedMemSideSendEvent(next_pf_time);
        }
    }

    // if we used temp block, check to see if its valid and then clear it out
    if (blk == tempBlock && tempBlock->isValid()) {
        evictBlock(blk, writebacks);
    }

    const Tick forward_time = clockEdge(forwardLatency) + pkt->headerDelay;
    // copy writebacks to write buffer
    doWritebacks(writebacks, forward_time);

    DPRINTF(CacheVerbose, "%s: Leaving with %s\n", __func__, pkt->print());
    if (!(needLevelUpPrefProcess && cacheLevel_ != pkt->targetCacheLevel_)) {
        delete pkt;
    }
}


Tick
BaseCache::recvAtomic(PacketPtr pkt)
{
    // should assert here that there are no outstanding MSHRs or
    // writebacks... that would mean that someone used an atomic
    // access in timing mode

    // We use lookupLatency here because it is used to specify the latency
    // to access.
    Cycles lat = lookupLatency;

    CacheBlk *blk = nullptr;
    PacketList writebacks;
    bool satisfied = access(pkt, blk, lat, writebacks);

    if (pkt->isClean() && blk && blk->isDirty()) {
        // A cache clean opearation is looking for a dirty
        // block. If a dirty block is encountered a WriteClean
        // will update any copies to the path to the memory
        // until the point of reference.
        DPRINTF(CacheVerbose, "%s: packet %s found block: %s\n",
                __func__, pkt->print(), blk->print());
        PacketPtr wb_pkt = writecleanBlk(blk, pkt->req->getDest(), pkt->id);
        writebacks.push_back(wb_pkt);
        pkt->setSatisfied();
    }

    // handle writebacks resulting from the access here to ensure they
    // logically precede anything happening below
    doWritebacksAtomic(writebacks);
    assert(writebacks.empty());

    if (!satisfied) {
        lat += handleAtomicReqMiss(pkt, blk, writebacks);
    }

    // Note that we don't invoke the prefetcher at all in atomic mode.
    // It's not clear how to do it properly, particularly for
    // prefetchers that aggressively generate prefetch candidates and
    // rely on bandwidth contention to throttle them; these will tend
    // to pollute the cache in atomic mode since there is no bandwidth
    // contention.  If we ever do want to enable prefetching in atomic
    // mode, though, this is the place to do it... see timingAccess()
    // for an example (though we'd want to issue the prefetch(es)
    // immediately rather than calling requestMemSideBus() as we do
    // there).

    // do any writebacks resulting from the response handling
    doWritebacksAtomic(writebacks);

    // if we used temp block, check to see if its valid and if so
    // clear it out, but only do so after the call to recvAtomic is
    // finished so that any downstream observers (such as a snoop
    // filter), first see the fill, and only then see the eviction
    if (blk == tempBlock && tempBlock->isValid()) {
        // the atomic CPU calls recvAtomic for fetch and load/store
        // sequentuially, and we may already have a tempBlock
        // writeback from the fetch that we have not yet sent
        if (tempBlockWriteback) {
            // if that is the case, write the prevoius one back, and
            // do not schedule any new event
            writebackTempBlockAtomic();
        } else {
            // the writeback/clean eviction happens after the call to
            // recvAtomic has finished (but before any successive
            // calls), so that the response handling from the fill is
            // allowed to happen first
            schedule(writebackTempBlockAtomicEvent, curTick());
        }

        tempBlockWriteback = evictBlock(blk);
    }

    if (pkt->needsResponse()) {
        pkt->makeAtomicResponse();
    }

    return lat * clockPeriod();
}

void
BaseCache::functionalAccess(PacketPtr pkt, bool from_cpu_side)
{
    Addr blk_addr = pkt->getBlockAddr(blkSize);
    bool is_secure = pkt->isSecure();
    CacheBlk *blk = tags->findBlock(pkt->getAddr(), is_secure);
    MSHR *mshr = mshrQueue.findMatch(blk_addr, is_secure);

    pkt->pushLabel(name());

    CacheBlkPrintWrapper cbpw(blk);

    // Note that just because an L2/L3 has valid data doesn't mean an
    // L1 doesn't have a more up-to-date modified copy that still
    // needs to be found.  As a result we always update the request if
    // we have it, but only declare it satisfied if we are the owner.

    // see if we have data at all (owned or otherwise)
    bool have_data = blk && blk->isValid()
        && pkt->trySatisfyFunctional(&cbpw, blk_addr, is_secure, blkSize,
                                     blk->data);

    // data we have is dirty if marked as such or if we have an
    // in-service MSHR that is pending a modified line
    bool have_dirty =
        have_data && (blk->isDirty() ||
                      (mshr && mshr->inService && mshr->isPendingModified()));

    bool done = have_dirty ||
        cpuSidePort.trySatisfyFunctional(pkt) ||
        mshrQueue.trySatisfyFunctional(pkt, blk_addr) ||
        writeBuffer.trySatisfyFunctional(pkt, blk_addr) ||
        memSidePort.trySatisfyFunctional(pkt);

    DPRINTF(CacheVerbose, "%s: %s %s%s%s\n", __func__,  pkt->print(),
            (blk && blk->isValid()) ? "valid " : "",
            have_data ? "data " : "", done ? "done " : "");

    // We're leaving the cache, so pop cache->name() label
    pkt->popLabel();

    if (done) {
        pkt->makeResponse();
        /// 标记最近处理的Cache
        pkt->recentCache_ = this;
    } else {
        // if it came as a request from the CPU side then make sure it
        // continues towards the memory side
        if (from_cpu_side) {
            memSidePort.sendFunctional(pkt);
        } else if (cpuSidePort.isSnooping()) {
            // if it came from the memory side, it must be a snoop request
            // and we should only forward it if we are forwarding snoops
            cpuSidePort.sendFunctionalSnoop(pkt);
        }
    }
}


void
BaseCache::cmpAndSwap(CacheBlk *blk, PacketPtr pkt)
{
    assert(pkt->isRequest());

    uint64_t overwrite_val;
    bool overwrite_mem;
    uint64_t condition_val64;
    uint32_t condition_val32;

    int offset = pkt->getOffset(blkSize);
    uint8_t *blk_data = blk->data + offset;

    assert(sizeof(uint64_t) >= pkt->getSize());

    overwrite_mem = true;
    // keep a copy of our possible write value, and copy what is at the
    // memory address into the packet
    pkt->writeData((uint8_t *)&overwrite_val);
    pkt->setData(blk_data);

    if (pkt->req->isCondSwap()) {
        if (pkt->getSize() == sizeof(uint64_t)) {
            condition_val64 = pkt->req->getExtraData();
            overwrite_mem = !std::memcmp(&condition_val64, blk_data,
                                         sizeof(uint64_t));
        } else if (pkt->getSize() == sizeof(uint32_t)) {
            condition_val32 = (uint32_t)pkt->req->getExtraData();
            overwrite_mem = !std::memcmp(&condition_val32, blk_data,
                                         sizeof(uint32_t));
        } else
            panic("Invalid size for conditional read/write\n");
    }

    if (overwrite_mem) {
        std::memcpy(blk_data, &overwrite_val, pkt->getSize());
        blk->status |= BlkDirty;
    }
}

QueueEntry*
BaseCache::getNextQueueEntry()
{
    // Check both MSHR queue and write buffer for potential requests,
    // note that null does not mean there is no request, it could
    // simply be that it is not ready
    MSHR *miss_mshr  = mshrQueue.getNext();
    WriteQueueEntry *wq_entry = writeBuffer.getNext();

    // If we got a write buffer request ready, first priority is a
    // full write buffer, otherwise we favour the miss requests
    if (wq_entry && (writeBuffer.isFull() || !miss_mshr)) {
        // need to search MSHR queue for conflicting earlier miss.
        MSHR *conflict_mshr =
            mshrQueue.findPending(wq_entry->blkAddr,
                                  wq_entry->isSecure);

        if (conflict_mshr && conflict_mshr->order < wq_entry->order) {
            // Service misses in order until conflict is cleared.
            return conflict_mshr;

            // @todo Note that we ignore the ready time of the conflict here
        }

        // No conflicts; issue write
        return wq_entry;
    } else if (miss_mshr) {
        // need to check for conflicting earlier writeback
        WriteQueueEntry *conflict_mshr =
            writeBuffer.findPending(miss_mshr->blkAddr,
                                    miss_mshr->isSecure);
        if (conflict_mshr) {
            // not sure why we don't check order here... it was in the
            // original code but commented out.

            // The only way this happens is if we are
            // doing a write and we didn't have permissions
            // then subsequently saw a writeback (owned got evicted)
            // We need to make sure to perform the writeback first
            // To preserve the dirty data, then we can issue the write

            // should we return wq_entry here instead?  I.e. do we
            // have to flush writes in order?  I don't think so... not
            // for Alpha anyway.  Maybe for x86?
            return conflict_mshr;

            // @todo Note that we ignore the ready time of the conflict here
        }

        // No conflicts; issue read
        return miss_mshr;
    }

    // fall through... no pending requests.  Try a prefetch.
    assert(!miss_mshr && !wq_entry);
    if (prefetcher && mshrQueue.canPrefetch()) {
        // If we have a miss queue slot, we can try a prefetch
        PacketPtr pkt = prefetcher->getPacket();
        if (pkt) {
            Addr pf_addr = pkt->getBlockAddr(blkSize);
            if (!tags->findBlock(pf_addr, pkt->isSecure()) &&
                !mshrQueue.findMatch(pf_addr, pkt->isSecure()) &&
                !writeBuffer.findMatch(pf_addr, pkt->isSecure())) {
                // Update statistic on number of prefetches issued
                // (hwpf_mshr_misses)
                assert(pkt->req->masterId() < system->maxMasters());
                mshr_misses[pkt->cmdToIndex()][pkt->req->masterId()]++;
                
                /// 在分配MSHR的时候设置时间戳
                pkt->setTimeStamp(cacheLevel_ ? cacheLevel_ : 1,
                        Packet::WhenRecv, curTick());
                /// 通知为Prefetch分配MSHR的事件（会对预取添加Index）
                if (prefetchFilter_) {
                    prefetch_filter::DataTypeInfo infoPair;
                    infoPair.source = prefetch_filter::Pref;
                    infoPair.target =  prefetch_filter::NullType;
                    prefetchFilter_->notifyCacheMiss(this, pkt, nullptr,
                            infoPair);
                }
                
                // allocate an MSHR and return it, note
                // that we send the packet straight away, so do not
                // schedule the send
                MSHR* mshr = allocateMissBuffer(pkt, curTick(), false);
                
                return mshr;
            } else {
                // free the request and packet
                delete pkt;
            }
        }
    }

    return nullptr;
}

void
BaseCache::satisfyRequest(PacketPtr pkt, CacheBlk *blk, bool, bool)
{
    assert(pkt->isRequest());

    assert(blk && blk->isValid());
    // Occasionally this is not true... if we are a lower-level cache
    // satisfying a string of Read and ReadEx requests from
    // upper-level caches, a Read will mark the block as shared but we
    // can satisfy a following ReadEx anyway since we can rely on the
    // Read requester(s) to have buffered the ReadEx snoop and to
    // invalidate their blocks after receiving them.
    // assert(!pkt->needsWritable() || blk->isWritable());
    assert(pkt->getOffset(blkSize) + pkt->getSize() <= blkSize);

    // Check RMW operations first since both isRead() and
    // isWrite() will be true for them
    if (pkt->cmd == MemCmd::SwapReq) {
        if (pkt->isAtomicOp()) {
            // extract data from cache and save it into the data field in
            // the packet as a return value from this atomic op
            int offset = tags->extractBlkOffset(pkt->getAddr());
            uint8_t *blk_data = blk->data + offset;
            pkt->setData(blk_data);

            // execute AMO operation
            (*(pkt->getAtomicOp()))(blk_data);

            // set block status to dirty
            blk->status |= BlkDirty;
        } else {
            cmpAndSwap(blk, pkt);
        }
    } else if (pkt->isWrite()) {
        // we have the block in a writable state and can go ahead,
        // note that the line may be also be considered writable in
        // downstream caches along the path to memory, but always
        // Exclusive, and never Modified
        assert(blk->isWritable());
        // Write or WriteLine at the first cache with block in writable state
        if (blk->checkWrite(pkt)) {
            pkt->writeDataToBlock(blk->data, blkSize);
        }
        // Always mark the line as dirty (and thus transition to the
        // Modified state) even if we are a failed StoreCond so we
        // supply data to any snoops that have appended themselves to
        // this cache before knowing the store will fail.
        blk->status |= BlkDirty;
        DPRINTF(CacheVerbose, "%s for %s (write)\n", __func__, pkt->print());
    } else if (pkt->isRead()) {
        if (pkt->isLLSC()) {
            blk->trackLoadLocked(pkt);
        }

        // all read responses have a data payload
        assert(pkt->hasRespData());
        pkt->setDataFromBlock(blk->data, blkSize);
    } else if (pkt->isUpgrade()) {
        // sanity check
        assert(!pkt->hasSharers());

        if (blk->isDirty()) {
            // we were in the Owned state, and a cache above us that
            // has the line in Shared state needs to be made aware
            // that the data it already has is in fact dirty
            pkt->setCacheResponding();
            blk->status &= ~BlkDirty;
        }
    } else if (pkt->isClean()) {
        blk->status &= ~BlkDirty;
    } else {
        assert(pkt->isInvalidate());
        invalidateBlock(blk);
        DPRINTF(CacheVerbose, "%s for %s (invalidation)\n", __func__,
                pkt->print());
    }
}

/////////////////////////////////////////////////////
//
// Access path: requests coming in from the CPU side
//
/////////////////////////////////////////////////////
Cycles
BaseCache::calculateTagOnlyLatency(const uint32_t delay,
                                   const Cycles lookup_lat) const
{
    // A tag-only access has to wait for the packet to arrive in order to
    // perform the tag lookup.
    return ticksToCycles(delay) + lookup_lat;
}

Cycles
BaseCache::calculateAccessLatency(const CacheBlk* blk, const uint32_t delay,
                                  const Cycles lookup_lat) const
{
    Cycles lat(0);

    if (blk != nullptr) {
        // As soon as the access arrives, for sequential accesses first access
        // tags, then the data entry. In the case of parallel accesses the
        // latency is dictated by the slowest of tag and data latencies.
        if (sequentialAccess) {
            lat = ticksToCycles(delay) + lookup_lat + dataLatency;
        } else {
            lat = ticksToCycles(delay) + std::max(lookup_lat, dataLatency);
        }

        // Check if the block to be accessed is available. If not, apply the
        // access latency on top of when the block is ready to be accessed.
        const Tick tick = curTick() + delay;
        const Tick when_ready = blk->getWhenReady();
        if (when_ready > tick &&
            ticksToCycles(when_ready - tick) > lat) {
            lat += ticksToCycles(when_ready - tick);
        }
    } else {
        // In case of a miss, we neglect the data access in a parallel
        // configuration (i.e., the data access will be stopped as soon as
        // we find out it is a miss), and use the tag-only latency.
        lat = calculateTagOnlyLatency(delay, lookup_lat);
    }

    return lat;
}

bool
BaseCache::access(PacketPtr pkt, CacheBlk *&blk, Cycles &lat,
                  PacketList &writebacks, bool* notifyHit,
                  bool* notifyMiss, bool* notifyFill)
{
    // sanity check
    assert(pkt->isRequest());

    chatty_assert(!(isReadOnly && pkt->isWrite()),
                  "Should never see a write in a read-only cache %s\n",
                  name());

    // Access block in the tags
    Cycles tag_latency(0);
    blk = tags->accessBlock(pkt->getAddr(), pkt->isSecure(), tag_latency);

    DPRINTF(Cache, "%s for %s %s\n", __func__, pkt->print(),
            blk ? "hit " + blk->print() : "miss");

    if (pkt->req->isCacheMaintenance()) {
        // A cache maintenance operation is always forwarded to the
        // memory below even if the block is found in dirty state.

        // We defer any changes to the state of the block until we
        // create and mark as in service the mshr for the downstream
        // packet.

        // Calculate access latency on top of when the packet arrives. This
        // takes into account the bus delay.
        lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

        return false;
    }

    if (pkt->isEviction()) {
        // We check for presence of block in above caches before issuing
        // Writeback or CleanEvict to write buffer. Therefore the only
        // possible cases can be of a CleanEvict packet coming from above
        // encountering a Writeback generated in this cache peer cache and
        // waiting in the write buffer. Cases of upper level peer caches
        // generating CleanEvict and Writeback or simply CleanEvict and
        // CleanEvict almost simultaneously will be caught by snoops sent out
        // by crossbar.
        WriteQueueEntry *wb_entry = writeBuffer.findMatch(pkt->getAddr(),
                                                          pkt->isSecure());
        if (wb_entry) {
            assert(wb_entry->getNumTargets() == 1);
            PacketPtr wbPkt = wb_entry->getTarget()->pkt;
            assert(wbPkt->isWriteback());

            if (pkt->isCleanEviction()) {
                // The CleanEvict and WritebackClean snoops into other
                // peer caches of the same level while traversing the
                // crossbar. If a copy of the block is found, the
                // packet is deleted in the crossbar. Hence, none of
                // the other upper level caches connected to this
                // cache have the block, so we can clear the
                // BLOCK_CACHED flag in the Writeback if set and
                // discard the CleanEvict by returning true.
                wbPkt->clearBlockCached();

                // A clean evict does not need to access the data array
                lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

                return true;
            } else {
                assert(pkt->cmd == MemCmd::WritebackDirty);
                // Dirty writeback from above trumps our clean
                // writeback... discard here
                // Note: markInService will remove entry from writeback buffer.
                markInService(wb_entry);
                delete wbPkt;
            }
        }
    }

    // Writeback handling is special case.  We can write the block into
    // the cache without having a writeable copy (or any copy at all).
    if (pkt->isWriteback()) {
        assert(blkSize == pkt->getSize());

        // we could get a clean writeback while we are having
        // outstanding accesses to a block, do the simple thing for
        // now and drop the clean writeback so that we do not upset
        // any ordering/decisions about ownership already taken
        if (pkt->cmd == MemCmd::WritebackClean &&
            mshrQueue.findMatch(pkt->getAddr(), pkt->isSecure())) {
            DPRINTF(Cache, "Clean writeback %#llx to block with MSHR, "
                    "dropping\n", pkt->getAddr());

            // A writeback searches for the block, then writes the data.
            // As the writeback is being dropped, the data is not touched,
            // and we just had to wait for the time to find a match in the
            // MSHR. As of now assume a mshr queue search takes as long as
            // a tag lookup for simplicity.
            lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);
            
            /// WritebackClean只会写下一个等级，但是如果命中的是一个预取MSHR
            /// 那么实际上当前等级的数据填充的时候，属于Demand属性
            /// 但是我们不做过于复杂的处理，只是不通知Hit
            if (notifyHit) {
                *notifyHit = false;
            }
            return true;
        }

        if (!blk) {
            // need to do a replacement
            blk = allocateBlock(pkt, writebacks);
            if (!blk) {
                // no replaceable block available: give up, fwd to next level.
                incMissCount(pkt);

                // A writeback searches for the block, then writes the data.
                // As the block could not be found, it was a tag-only access.
                lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

                return false;
            }
            
            /// 如果Writeback没有命中并且直接发生了替换，则通知一个Fill
            /// 而不是一个Hit
            if (prefetchFilter_) {
                prefetch_filter::DataTypeInfo infoPair;
                infoPair.source = pkt->packetType_;
                infoPair.target = blk->usedToBePrefetched_ ?
                        prefetch_filter::Pref : prefetch_filter::Dmd;
                infoPair.target = blk->wasValid_ ? infoPair.target :
                    prefetch_filter::NullType;
                Addr evictedAddr = blk->wasValid_ ? blk->oldAddr_ :
                        prefetch_filter::invalidBlkAddr_;
                prefetchFilter_->notifyCacheFill(this, pkt,
                        evictedAddr, infoPair);
            }
            
            if (notifyHit) {
                *notifyHit = false;
            }
            blk->wasValid_ = false;
            blk->usedToBePrefetched_ = false;
            blk->oldAddr_ = prefetch_filter::invalidBlkAddr_;
            
            blk->status |= BlkReadable;
        }
        // only mark the block dirty if we got a writeback command,
        // and leave it as is for a clean writeback
        if (pkt->cmd == MemCmd::WritebackDirty) {
            // TODO: the coherent cache can assert(!blk->isDirty());
            blk->status |= BlkDirty;
        }
        // if the packet does not have sharers, it is passing
        // writable, and we got the writeback in Modified or Exclusive
        // state, if not we are in the Owned or Shared state
        if (!pkt->hasSharers()) {
            blk->status |= BlkWritable;
        }
        // nothing else to do; writeback doesn't expect response
        assert(!pkt->needsResponse());
        pkt->writeDataToBlock(blk->data, blkSize);
        DPRINTF(Cache, "%s new state is %s\n", __func__, blk->print());
        incHitCount(pkt);

        // A writeback searches for the block, then writes the data
        lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);

        // When the packet metadata arrives, the tag lookup will be done while
        // the payload is arriving. Then the block will be ready to access as
        // soon as the fill is done
        blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
            std::max(cyclesToTicks(tag_latency), (uint64_t)pkt->payloadDelay));

        return true;
    } else if (pkt->cmd == MemCmd::CleanEvict) {
        // A CleanEvict does not need to access the data array
        lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

        if (blk) {
            // Found the block in the tags, need to stop CleanEvict from
            // propagating further down the hierarchy. Returning true will
            // treat the CleanEvict like a satisfied write request and delete
            // it.
            return true;
        }
        // We didn't find the block here, propagate the CleanEvict further
        // down the memory hierarchy. Returning false will treat the CleanEvict
        // like a Writeback which could not find a replaceable block so has to
        // go to next level.
        return false;
    } else if (pkt->cmd == MemCmd::WriteClean) {
        // WriteClean handling is a special case. We can allocate a
        // block directly if it doesn't exist and we can update the
        // block immediately. The WriteClean transfers the ownership
        // of the block as well.
        assert(blkSize == pkt->getSize());

        if (!blk) {
            if (pkt->writeThrough()) {
                // A writeback searches for the block, then writes the data.
                // As the block could not be found, it was a tag-only access.
                lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

                // if this is a write through packet, we don't try to
                // allocate if the block is not present
                return false;
            } else {
                // a writeback that misses needs to allocate a new block
                blk = allocateBlock(pkt, writebacks);
                if (!blk) {
                    // no replaceable block available: give up, fwd to
                    // next level.
                    incMissCount(pkt);

                    // A writeback searches for the block, then writes the
                    // data. As the block could not be found, it was a tag-only
                    // access.
                    lat = calculateTagOnlyLatency(pkt->headerDelay,
                                                  tag_latency);

                    return false;
                }

                /// 如果WriteClean没有命中并且直接发生了替换，则通知一个Fill
                /// 而不是一个Hit
                if (prefetchFilter_) {
                    prefetch_filter::DataTypeInfo infoPair;
                    infoPair.source = pkt->packetType_;
                    infoPair.target = blk->usedToBePrefetched_ ?
                            prefetch_filter::Pref : prefetch_filter::Dmd;
                    infoPair.target = blk->wasValid_ ? infoPair.target :
                        prefetch_filter::NullType;
                    Addr evictedAddr = blk->wasValid_ ? blk->oldAddr_ :
                            prefetch_filter::invalidBlkAddr_;
                    prefetchFilter_->notifyCacheFill(this, pkt,
                            evictedAddr, infoPair);
                }
                if (notifyHit) {
                    *notifyHit = false;
                }
                blk->wasValid_ = false;
                blk->usedToBePrefetched_ = false;
                blk->oldAddr_ = prefetch_filter::invalidBlkAddr_;
                blk->status |= BlkReadable;
            }
        }

        // at this point either this is a writeback or a write-through
        // write clean operation and the block is already in this
        // cache, we need to update the data and the block flags
        assert(blk);
        // TODO: the coherent cache can assert(!blk->isDirty());
        if (!pkt->writeThrough()) {
            blk->status |= BlkDirty;
        }
        // nothing else to do; writeback doesn't expect response
        assert(!pkt->needsResponse());
        pkt->writeDataToBlock(blk->data, blkSize);
        DPRINTF(Cache, "%s new state is %s\n", __func__, blk->print());

        incHitCount(pkt);

        // A writeback searches for the block, then writes the data
        lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);

        // When the packet metadata arrives, the tag lookup will be done while
        // the payload is arriving. Then the block will be ready to access as
        // soon as the fill is done
        blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
            std::max(cyclesToTicks(tag_latency), (uint64_t)pkt->payloadDelay));

        // if this a write-through packet it will be sent to cache
        // below
        return !pkt->writeThrough();
    } else if (blk && (pkt->needsWritable() ? blk->isWritable() :
                       blk->isReadable())) {
        // OK to satisfy access
        incHitCount(pkt);

        // Calculate access latency based on the need to access the data array
        if (pkt->isRead() || pkt->isWrite()) {
            lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);
        } else {
            lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);
        }

        satisfyRequest(pkt, blk);
        maintainClusivity(pkt->fromCache(), blk);

        return true;
    }

    // Can't satisfy access normally... either no block (blk == nullptr)
    // or have block but need writable

    incMissCount(pkt);

    lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);

    if (!blk && pkt->isLLSC() && pkt->isWrite()) {
        // complete miss on store conditional... just give up now
        pkt->req->setExtraData(0);
        return true;
    }

    return false;
}

void
BaseCache::maintainClusivity(bool from_cache, CacheBlk *blk)
{
    if (from_cache && blk && blk->isValid() && !blk->isDirty() &&
        clusivity == Enums::mostly_excl) {
        // if we have responded to a cache, and our block is still
        // valid, but not dirty, and this cache is mostly exclusive
        // with respect to the cache above, drop the block
        invalidateBlock(blk);
    }
}

CacheBlk*
BaseCache::handleFill(PacketPtr pkt, CacheBlk *blk, PacketList &writebacks,
                      bool allocate)
{
    assert(pkt->isResponse());
    Addr addr = pkt->getAddr();
    bool is_secure = pkt->isSecure();
#if TRACING_ON
    CacheBlk::State old_state = blk ? blk->status : 0;
#endif

    // When handling a fill, we should have no writes to this line.
    assert(addr == pkt->getBlockAddr(blkSize));
    assert(!writeBuffer.findMatch(addr, is_secure));

    if (!blk) {
        // better have read new data...
        assert(pkt->hasData() || pkt->cmd == MemCmd::InvalidateResp);

        // need to do a replacement if allocating, otherwise we stick
        // with the temporary storage
        blk = allocate ? allocateBlock(pkt, writebacks) : nullptr;
        
        /// 检查预取反馈的场景正确性
        if (pkt->packetType_ == prefetch_filter::Pref) {
            /// 我们应该确保所有的预取反馈都是allocOnFill的
            assert(allocate);
            /// 我们暂时不考虑预取反馈没有可以替换的Block的情况
            assert(blk);
        }

        if (!blk) {
            // No replaceable block or a mostly exclusive
            // cache... just use temporary storage to complete the
            // current request and then get rid of it
            blk = tempBlock;
            tempBlock->insert(addr, is_secure);
            DPRINTF(Cache, "using temp block for %#llx (%s)\n", addr,
                    is_secure ? "s" : "ns");
        }
    } else {
        // existing block... probably an upgrade
        // don't clear block status... if block is already dirty we
        // don't want to lose that
    }

    // Block is guaranteed to be valid at this point
    assert(blk->isValid());
    assert(blk->isSecure() == is_secure);
    assert(regenerateBlkAddr(blk) == addr);

    blk->status |= BlkReadable;

    // sanity check for whole-line writes, which should always be
    // marked as writable as part of the fill, and then later marked
    // dirty as part of satisfyRequest
    if (pkt->cmd == MemCmd::InvalidateResp) {
        assert(!pkt->hasSharers());
    }

    // here we deal with setting the appropriate state of the line,
    // and we start by looking at the hasSharers flag, and ignore the
    // cacheResponding flag (normally signalling dirty data) if the
    // packet has sharers, thus the line is never allocated as Owned
    // (dirty but not writable), and always ends up being either
    // Shared, Exclusive or Modified, see Packet::setCacheResponding
    // for more details
    if (!pkt->hasSharers()) {
        // we could get a writable line from memory (rather than a
        // cache) even in a read-only cache, note that we set this bit
        // even for a read-only cache, possibly revisit this decision
        blk->status |= BlkWritable;

        // check if we got this via cache-to-cache transfer (i.e., from a
        // cache that had the block in Modified or Owned state)
        if (pkt->cacheResponding()) {
            // we got the block in Modified state, and invalidated the
            // owners copy
            blk->status |= BlkDirty;

            chatty_assert(!isReadOnly, "Should never see dirty snoop response "
                          "in read-only cache %s\n", name());
        }
    }

    DPRINTF(Cache, "Block addr %#llx (%s) moving from state %x to %s\n",
            addr, is_secure ? "s" : "ns", old_state, blk->print());

    // if we got new data, copy it in (checking for a read response
    // and a response that has data is the same in the end)
    if (pkt->isRead()) {
        // sanity checks
        assert(pkt->hasData());
        assert(pkt->getSize() == blkSize);
        
        /// 这里我们会更新预取请求的Fill时间戳
        if (pkt->packetType_ == prefetch_filter::Pref) {
            pkt->setTimeStamp(cacheLevel_ ? cacheLevel_ : 1,
                    Packet::WhenFill, curTick());
        }

        /// 针对Fill事件进行处理
        if (prefetchFilter_) {
            assert(allocate);
            prefetch_filter::DataTypeInfo infoPair;
            
            infoPair.source = pkt->packetType_;
            infoPair.target = blk->usedToBePrefetched_ ?
                    prefetch_filter::Pref : prefetch_filter::Dmd;
            infoPair.target = blk->wasValid_ ? infoPair.target :
                    prefetch_filter::NullType;
            Addr evictedAddr = blk->wasValid_ ? blk->oldAddr_ :
                    prefetch_filter::invalidBlkAddr_;
            prefetchFilter_->notifyCacheFill(this, pkt, evictedAddr, infoPair);
        }
        
        pkt->writeDataToBlock(blk->data, blkSize);
        /// 清除对应Block的关键数据，避免出现错误
    }
    
    blk->wasValid_ = false;
    blk->usedToBePrefetched_ = false;
    blk->oldAddr_ = prefetch_filter::invalidBlkAddr_;
    
    // The block will be ready when the payload arrives and the fill is done
    blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
                      pkt->payloadDelay);

    return blk;
}

CacheBlk*
BaseCache::allocateBlock(const PacketPtr pkt, PacketList &writebacks)
{
    // Get address
    const Addr addr = pkt->getAddr();

    // Get secure bit
    const bool is_secure = pkt->isSecure();

    // Find replacement victim
    std::vector<CacheBlk*> evict_blks;
    CacheBlk *victim = tags->findVictim(addr, is_secure, evict_blks);
    // evict_blks只会存放victim

    // It is valid to return nullptr if there is no victim
    if (!victim)
        return nullptr;

    // Print victim block's information
    DPRINTF(CacheRepl, "Replacement victim: %s\n", victim->print());

    // Check for transient state allocations. If any of the entries listed
    // for eviction has a transient state, the allocation fails
    for (const auto& blk : evict_blks) {
        if (blk->isValid()) {
            Addr repl_addr = regenerateBlkAddr(blk);
            blk->oldAddr_ = repl_addr;
            DEBUG_MEM("%s Fill @0x%lx replace valid block[%p] @0x%lx",
                    getName().c_str(), pkt->getAddr(), blk, repl_addr);
            MSHR *repl_mshr = mshrQueue.findMatch(repl_addr, blk->isSecure());
            if (repl_mshr) {
                // must be an outstanding upgrade or clean request
                // on a block we're about to replace...
                assert((!blk->isWritable() && repl_mshr->needsWritable()) ||
                       repl_mshr->isCleaning());

                // too hard to replace block with transient state
                // allocation failed, block not inserted
                return nullptr;
            }
            if (blk == victim) {
                blk->wasValid_ = true;
            }
        }
    }

    // The victim will be replaced by a new entry, so increase the replacement
    // counter if a valid block is being replaced
    if (victim->isValid()) {
        DPRINTF(Cache, "replacement: replacing %#llx (%s) with %#llx "
                "(%s): %s\n", regenerateBlkAddr(victim),
                victim->isSecure() ? "s" : "ns",
                addr, is_secure ? "s" : "ns",
                victim->isDirty() ? "writeback" : "clean");

        replacements++;
    }

    // Evict valid blocks associated to this victim block
    for (const auto& blk : evict_blks) {
        if (blk->isValid()) {
            if (blk->wasPrefetched()) {
                blk->usedToBePrefetched_ = true;
                unusedPrefetches++;
            }

            evictBlock(blk, writebacks);
        }
    }

    // Insert new block at victimized entry
    tags->insertBlock(pkt, victim);

    return victim;
}

void
BaseCache::invalidateBlock(CacheBlk *blk)
{
    Addr blkAddr = regenerateBlkAddr(blk);
    DEBUG_MEM("%s invlidate cache block @0x%lx",
            getName().c_str(), blkAddr);
    // If handling a block present in the Tags, let it do its invalidation
    // process, which will update stats and invalidate the block itself
    if (blk != tempBlock) {
        /// 添加无效化预取的相关通知（如果是因为Fill导致的Invalidate不会通知）
        if (prefetchFilter_ && blk->wasPrefetched() && !blk->wasValid_) {
            prefetchFilter_->invalidatePrefetch(this, blkAddr);
        }
        tags->invalidate(blk);
    } else {
        tempBlock->invalidate();
    }
}

void
BaseCache::evictBlock(CacheBlk *blk, PacketList &writebacks)
{
    PacketPtr pkt = evictBlock(blk);
    if (pkt) {
        writebacks.push_back(pkt);
    }
}

PacketPtr
BaseCache::writebackBlk(CacheBlk *blk)
{
    chatty_assert(!isReadOnly || writebackClean,
                  "Writeback from read-only cache");
    assert(blk && blk->isValid() && (blk->isDirty() || writebackClean));

    writebacks[Request::wbMasterId]++;

    RequestPtr req = std::make_shared<Request>(
        regenerateBlkAddr(blk), blkSize, 0, Request::wbMasterId);

    if (blk->isSecure())
        req->setFlags(Request::SECURE);

    req->taskId(blk->task_id);

    PacketPtr pkt =
        new Packet(req, blk->isDirty() ?
                   MemCmd::WritebackDirty : MemCmd::WritebackClean);
    /// 初始化信息（对于这类操作添加Demand属性）
    pkt->addSrcCache(this);
    pkt->packetType_ = prefetch_filter::Dmd;

    DPRINTF(Cache, "Create Writeback %s writable: %d, dirty: %d\n",
            pkt->print(), blk->isWritable(), blk->isDirty());

    if (blk->isWritable()) {
        // not asserting shared means we pass the block in modified
        // state, mark our own block non-writeable
        blk->status &= ~BlkWritable;
    } else {
        // we are in the Owned state, tell the receiver
        pkt->setHasSharers();
    }

    // make sure the block is not marked dirty
    blk->status &= ~BlkDirty;

    pkt->allocate();
    pkt->setDataFromBlock(blk->data, blkSize);

    return pkt;
}

PacketPtr
BaseCache::writecleanBlk(CacheBlk *blk, Request::Flags dest, PacketId id)
{
    RequestPtr req = std::make_shared<Request>(
        regenerateBlkAddr(blk), blkSize, 0, Request::wbMasterId);

    if (blk->isSecure()) {
        req->setFlags(Request::SECURE);
    }
    req->taskId(blk->task_id);

    PacketPtr pkt = new Packet(req, MemCmd::WriteClean, blkSize, id);
    /// 初始化信息（对于这类写回操作，设置Demand属性）
    pkt->addSrcCache(this);
    pkt->packetType_ = prefetch_filter::Dmd;

    if (dest) {
        req->setFlags(dest);
        pkt->setWriteThrough();
    }

    DPRINTF(Cache, "Create %s writable: %d, dirty: %d\n", pkt->print(),
            blk->isWritable(), blk->isDirty());

    if (blk->isWritable()) {
        // not asserting shared means we pass the block in modified
        // state, mark our own block non-writeable
        blk->status &= ~BlkWritable;
    } else {
        // we are in the Owned state, tell the receiver
        pkt->setHasSharers();
    }

    // make sure the block is not marked dirty
    blk->status &= ~BlkDirty;

    pkt->allocate();
    pkt->setDataFromBlock(blk->data, blkSize);

    return pkt;
}


void
BaseCache::memWriteback()
{
    tags->forEachBlk([this](CacheBlk &blk) { writebackVisitor(blk); });
}

void
BaseCache::memInvalidate()
{
    tags->forEachBlk([this](CacheBlk &blk) { invalidateVisitor(blk); });
}

bool
BaseCache::isDirty() const
{
    return tags->anyBlk([](CacheBlk &blk) { return blk.isDirty(); });
}

bool
BaseCache::coalesce() const
{
    return writeAllocator && writeAllocator->coalesce();
}

void
BaseCache::writebackVisitor(CacheBlk &blk)
{
    if (blk.isDirty()) {
        assert(blk.isValid());

        RequestPtr request = std::make_shared<Request>(
            regenerateBlkAddr(&blk), blkSize, 0, Request::funcMasterId);

        request->taskId(blk.task_id);
        if (blk.isSecure()) {
            request->setFlags(Request::SECURE);
        }

        Packet packet(request, MemCmd::WriteReq);
        packet.dataStatic(blk.data);

        memSidePort.sendFunctional(&packet);

        blk.status &= ~BlkDirty;
    }
}

void
BaseCache::invalidateVisitor(CacheBlk &blk)
{
    if (blk.isDirty())
        warn_once("Invalidating dirty cache lines. " \
                  "Expect things to break.\n");

    if (blk.isValid()) {
        assert(!blk.isDirty());
        invalidateBlock(&blk);
    }
}

Tick
BaseCache::nextQueueReadyTime() const
{
    Tick nextReady = std::min(mshrQueue.nextReadyTime(),
                              writeBuffer.nextReadyTime());

    // Don't signal prefetch ready time if no MSHRs available
    // Will signal once enoguh MSHRs are deallocated
    if (prefetcher && mshrQueue.canPrefetch()) {
        nextReady = std::min(nextReady,
                             prefetcher->nextPrefetchReadyTime());
    }

    return nextReady;
}


bool
BaseCache::sendMSHRQueuePacket(MSHR* mshr)
{
    assert(mshr);

    // use request from 1st target
    PacketPtr tgt_pkt = mshr->getTarget()->pkt;

    DPRINTF(Cache, "%s: MSHR %s\n", __func__, tgt_pkt->print());

    // if the cache is in write coalescing mode or (additionally) in
    // no allocation mode, and we have a write packet with an MSHR
    // that is not a whole-line write (due to incompatible flags etc),
    // then reset the write mode
    if (writeAllocator && writeAllocator->coalesce() && tgt_pkt->isWrite()) {
        if (!mshr->isWholeLineWrite()) {
            // if we are currently write coalescing, hold on the
            // MSHR as many cycles extra as we need to completely
            // write a cache line
            if (writeAllocator->delay(mshr->blkAddr)) {
                Tick delay = blkSize / tgt_pkt->getSize() * clockPeriod();
                DPRINTF(CacheVerbose, "Delaying pkt %s %llu ticks to allow "
                        "for write coalescing\n", tgt_pkt->print(), delay);
                mshrQueue.delay(mshr, delay);
                return false;
            } else {
                writeAllocator->reset();
            }
        } else {
            writeAllocator->resetDelay(mshr->blkAddr);
        }
    }

    CacheBlk *blk = tags->findBlock(mshr->blkAddr, mshr->isSecure);

    // either a prefetch that is not present upstream, or a normal
    // MSHR request, proceed to get the packet to send downstream
    PacketPtr pkt = createMissPacket(tgt_pkt, blk, mshr->needsWritable(),
                                     mshr->isWholeLineWrite());
    
    /// 获取一个额外的时间戳信息
    if (pkt && tgt_pkt && pkt->packetType_ == prefetch_filter::Pref) {
        pkt->combineTimeStamp(tgt_pkt);
        /*
        pkt->setTimeStamp(cacheLevel_ ? cacheLevel_ - 1 : 0, Packet::WhenSend,
                tgt_pkt->getTimeStamp(cacheLevel_ ? cacheLevel_ - 1 : 0,
                Packet::WhenSend));
        */
    }

    mshr->isForward = (pkt == nullptr);

    if (mshr->isForward) {
        // not a cache block request, but a response is expected
        // make copy of current packet to forward, keep current
        // copy for response handling
        /// 设置最近处理Packet的Cache指针
        pkt = new Packet(tgt_pkt, false, true);
        // 由于是一个Forward，不会添加为源Cache
        pkt->recentCache_ = this;
        
        assert(!pkt->isWrite());
    }
    
    /// 进行预取信息的合并，无论是Demand还是预取都会进行合并
    if (pkt) {
        for (auto& target : mshr->targets) {
            pkt->combinePacket(target.pkt);
        }
    }

    /// 如果是一个提升级别的预取会设置一个虚拟MSHR供高层级响应
    if (pkt->packetType_ == prefetch_filter::Pref &&
            cacheLevel_ == pkt->srcCacheLevel_ &&
            pkt->srcCacheLevel_ > pkt->targetCacheLevel_) {
        assert(pkt->targetCacheLevel_ != 255);
        pkt->mshr_ = std::make_shared<MSHR>(*mshr);
        /// 更新prefTraget
        pkt->mshr_->initLevelUpPrefMSHR();
        pkt->mshr_->getTarget()->source = MSHR::Target::FromCPU;
        DEBUG_MEM("%s push uplevel mshr %p for prefetch @0x%lx [%s -> %s]",
                getName().c_str(), pkt->mshr_, pkt->getAddr(),
                BaseCache::levelName_[pkt->srcCacheLevel_].c_str(),
                BaseCache::levelName_[pkt->targetCacheLevel_].c_str());
    }

    /// 如果是一个降级别的预取，可以不push sender state
    if (!(pkt->packetType_ == prefetch_filter::Pref &&
            cacheLevel_ >= pkt->srcCacheLevel_ &&
            cacheLevel_ < pkt->targetCacheLevel_)) {
        if (pkt->packetType_ == prefetch_filter::Pref) {
            DEBUG_MEM("%s push sender state MSHR[%p] @0x%lx",
                    getName().c_str(), mshr, pkt->getAddr());
        }
        // play it safe and append (rather than set) the sender state,
        // as forwarded packets may already have existing state
        pkt->pushSenderState(mshr);
    } else {
        DEBUG_MEM("%s push no sender state for prefetch @0x%lx",
                getName().c_str(), pkt->getAddr());
    }
    
    if (pkt->isClean() && blk && blk->isDirty()) {
        // A cache clean opearation is looking for a dirty block. Mark
        // the packet so that the destination xbar can determine that
        // there will be a follow-up write packet as well.
        pkt->setSatisfied();
    }

    /// 对于一个发射的预取请求标记时间戳
    if (pkt->packetType_ == prefetch_filter::Pref) {
        pkt->setTimeStamp(cacheLevel_ ? cacheLevel_ : 1,
                Packet::WhenSend, curTick());
    }
    
    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(Cache, "Failed to send packet @0x%lx\n", pkt->getAddr());

        // we are awaiting a retry, but we
        // delete the packet and will be creating a new packet
        // when we get the opportunity
        delete pkt;

        // note that we have now masked any requestBus and
        // schedSendEvent (we will wait for a retry before
        // doing anything), and this is so even if we do not
        // care about this packet and might override it before
        // it gets retried
        return true;
    } else {
        // As part of the call to sendTimingReq the packet is
        // forwarded to all neighbouring caches (and any caches
        // above them) as a snoop. Thus at this point we know if
        // any of the neighbouring caches are responding, and if
        // so, we know it is dirty, and we can determine if it is
        // being passed as Modified, making our MSHR the ordering
        // point
        bool pending_modified_resp = !pkt->hasSharers() &&
            pkt->cacheResponding();
        markInService(mshr, pending_modified_resp);

        if (pkt->isClean() && blk && blk->isDirty()) {
            // A cache clean opearation is looking for a dirty
            // block. If a dirty block is encountered a WriteClean
            // will update any copies to the path to the memory
            // until the point of reference.
            DPRINTF(CacheVerbose, "%s: packet %s found block: %s\n",
                    __func__, pkt->print(), blk->print());
            PacketPtr wb_pkt = writecleanBlk(blk, pkt->req->getDest(),
                                             pkt->id);
            PacketList writebacks;
            writebacks.push_back(wb_pkt);
            doWritebacks(writebacks, 0);
        }

        return false;
    }
}

bool
BaseCache::sendWriteQueuePacket(WriteQueueEntry* wq_entry)
{
    assert(wq_entry);

    // always a single target for write queue entries
    PacketPtr tgt_pkt = wq_entry->getTarget()->pkt;

    DPRINTF(Cache, "%s: write %s\n", __func__, tgt_pkt->print());

    // forward as is, both for evictions and uncacheable writes
    if (!memSidePort.sendTimingReq(tgt_pkt)) {
        // note that we have now masked any requestBus and
        // schedSendEvent (we will wait for a retry before
        // doing anything), and this is so even if we do not
        // care about this packet and might override it before
        // it gets retried
        return true;
    } else {
        markInService(wq_entry);
        return false;
    }
}

void
BaseCache::serialize(CheckpointOut &cp) const
{
    bool dirty(isDirty());

    if (dirty) {
        warn("*** The cache still contains dirty data. ***\n");
        warn("    Make sure to drain the system using the correct flags.\n");
        warn("    This checkpoint will not restore correctly " \
             "and dirty data in the cache will be lost!\n");
    }

    // Since we don't checkpoint the data in the cache, any dirty data
    // will be lost when restoring from a checkpoint of a system that
    // wasn't drained properly. Flag the checkpoint as invalid if the
    // cache contains dirty data.
    bool bad_checkpoint(dirty);
    SERIALIZE_SCALAR(bad_checkpoint);
}

void
BaseCache::unserialize(CheckpointIn &cp)
{
    bool bad_checkpoint;
    UNSERIALIZE_SCALAR(bad_checkpoint);
    if (bad_checkpoint) {
        fatal("Restoring from checkpoints with dirty caches is not "
              "supported in the classic memory system. Please remove any "
              "caches or drain them properly before taking checkpoints.\n");
    }
}

void
BaseCache::regStats()
{
    MemObject::regStats();

    using namespace Stats;

    // Hit statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        hits[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_hits")
            .desc("number of " + cstr + " hits")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            hits[access_idx].subname(i, system->getMasterName(i));
        }
    }

// These macros make it easier to sum the right subset of commands and
// to change the subset of commands that are considered "demand" vs
// "non-demand"
#define SUM_DEMAND(s) \
    (s[MemCmd::ReadReq] + s[MemCmd::WriteReq] + s[MemCmd::WriteLineReq] + \
     s[MemCmd::ReadExReq] + s[MemCmd::ReadCleanReq] + s[MemCmd::ReadSharedReq])

// should writebacks be included here?  prior code was inconsistent...
#define SUM_NON_DEMAND(s) \
    (s[MemCmd::SoftPFReq] + s[MemCmd::HardPFReq] + s[MemCmd::SoftPFExReq])

    demandHits
        .name(name() + ".demand_hits")
        .desc("number of demand (read+write) hits")
        .flags(total | nozero | nonan)
        ;
    demandHits = SUM_DEMAND(hits);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandHits.subname(i, system->getMasterName(i));
    }

    overallHits
        .name(name() + ".overall_hits")
        .desc("number of overall hits")
        .flags(total | nozero | nonan)
        ;
    overallHits = demandHits + SUM_NON_DEMAND(hits);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallHits.subname(i, system->getMasterName(i));
    }

    // Miss statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        misses[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_misses")
            .desc("number of " + cstr + " misses")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            misses[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMisses
        .name(name() + ".demand_misses")
        .desc("number of demand (read+write) misses")
        .flags(total | nozero | nonan)
        ;
    demandMisses = SUM_DEMAND(misses);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMisses.subname(i, system->getMasterName(i));
    }

    overallMisses
        .name(name() + ".overall_misses")
        .desc("number of overall misses")
        .flags(total | nozero | nonan)
        ;
    overallMisses = demandMisses + SUM_NON_DEMAND(misses);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMisses.subname(i, system->getMasterName(i));
    }

    // Miss latency statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        missLatency[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_miss_latency")
            .desc("number of " + cstr + " miss cycles")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            missLatency[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMissLatency
        .name(name() + ".demand_miss_latency")
        .desc("number of demand (read+write) miss cycles")
        .flags(total | nozero | nonan)
        ;
    demandMissLatency = SUM_DEMAND(missLatency);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMissLatency.subname(i, system->getMasterName(i));
    }

    overallMissLatency
        .name(name() + ".overall_miss_latency")
        .desc("number of overall miss cycles")
        .flags(total | nozero | nonan)
        ;
    overallMissLatency = demandMissLatency + SUM_NON_DEMAND(missLatency);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMissLatency.subname(i, system->getMasterName(i));
    }

    // access formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        accesses[access_idx]
            .name(name() + "." + cstr + "_accesses")
            .desc("number of " + cstr + " accesses(hits+misses)")
            .flags(total | nozero | nonan)
            ;
        accesses[access_idx] = hits[access_idx] + misses[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            accesses[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandAccesses
        .name(name() + ".demand_accesses")
        .desc("number of demand (read+write) accesses")
        .flags(total | nozero | nonan)
        ;
    demandAccesses = demandHits + demandMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        demandAccesses.subname(i, system->getMasterName(i));
    }

    overallAccesses
        .name(name() + ".overall_accesses")
        .desc("number of overall (read+write) accesses")
        .flags(total | nozero | nonan)
        ;
    overallAccesses = overallHits + overallMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallAccesses.subname(i, system->getMasterName(i));
    }

    // miss rate formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        missRate[access_idx]
            .name(name() + "." + cstr + "_miss_rate")
            .desc("miss rate for " + cstr + " accesses")
            .flags(total | nozero | nonan)
            ;
        missRate[access_idx] = misses[access_idx] / accesses[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            missRate[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMissRate
        .name(name() + ".demand_miss_rate")
        .desc("miss rate for demand accesses")
        .flags(total | nozero | nonan)
        ;
    demandMissRate = demandMisses / demandAccesses;
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMissRate.subname(i, system->getMasterName(i));
    }

    overallMissRate
        .name(name() + ".overall_miss_rate")
        .desc("miss rate for overall accesses")
        .flags(total | nozero | nonan)
        ;
    overallMissRate = overallMisses / overallAccesses;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMissRate.subname(i, system->getMasterName(i));
    }

    // miss latency formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        avgMissLatency[access_idx]
            .name(name() + "." + cstr + "_avg_miss_latency")
            .desc("average " + cstr + " miss latency")
            .flags(total | nozero | nonan)
            ;
        avgMissLatency[access_idx] =
            missLatency[access_idx] / misses[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            avgMissLatency[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandAvgMissLatency
        .name(name() + ".demand_avg_miss_latency")
        .desc("average overall miss latency")
        .flags(total | nozero | nonan)
        ;
    demandAvgMissLatency = demandMissLatency / demandMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        demandAvgMissLatency.subname(i, system->getMasterName(i));
    }

    overallAvgMissLatency
        .name(name() + ".overall_avg_miss_latency")
        .desc("average overall miss latency")
        .flags(total | nozero | nonan)
        ;
    overallAvgMissLatency = overallMissLatency / overallMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallAvgMissLatency.subname(i, system->getMasterName(i));
    }

    blocked_cycles.init(NUM_BLOCKED_CAUSES);
    blocked_cycles
        .name(name() + ".blocked_cycles")
        .desc("number of cycles access was blocked")
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;


    blocked_causes.init(NUM_BLOCKED_CAUSES);
    blocked_causes
        .name(name() + ".blocked")
        .desc("number of cycles access was blocked")
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;

    avg_blocked
        .name(name() + ".avg_blocked_cycles")
        .desc("average number of cycles each access was blocked")
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;

    avg_blocked = blocked_cycles / blocked_causes;

    unusedPrefetches
        .name(name() + ".unused_prefetches")
        .desc("number of HardPF blocks evicted w/o reference")
        .flags(nozero)
        ;

    writebacks
        .init(system->maxMasters())
        .name(name() + ".writebacks")
        .desc("number of writebacks")
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < system->maxMasters(); i++) {
        writebacks.subname(i, system->getMasterName(i));
    }

    // MSHR statistics
    // MSHR hit statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshr_hits[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_mshr_hits")
            .desc("number of " + cstr + " MSHR hits")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            mshr_hits[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMshrHits
        .name(name() + ".demand_mshr_hits")
        .desc("number of demand (read+write) MSHR hits")
        .flags(total | nozero | nonan)
        ;
    demandMshrHits = SUM_DEMAND(mshr_hits);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMshrHits.subname(i, system->getMasterName(i));
    }

    overallMshrHits
        .name(name() + ".overall_mshr_hits")
        .desc("number of overall MSHR hits")
        .flags(total | nozero | nonan)
        ;
    overallMshrHits = demandMshrHits + SUM_NON_DEMAND(mshr_hits);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrHits.subname(i, system->getMasterName(i));
    }

    // MSHR miss statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshr_misses[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_mshr_misses")
            .desc("number of " + cstr + " MSHR misses")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            mshr_misses[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMshrMisses
        .name(name() + ".demand_mshr_misses")
        .desc("number of demand (read+write) MSHR misses")
        .flags(total | nozero | nonan)
        ;
    demandMshrMisses = SUM_DEMAND(mshr_misses);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMshrMisses.subname(i, system->getMasterName(i));
    }

    overallMshrMisses
        .name(name() + ".overall_mshr_misses")
        .desc("number of overall MSHR misses")
        .flags(total | nozero | nonan)
        ;
    overallMshrMisses = demandMshrMisses + SUM_NON_DEMAND(mshr_misses);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrMisses.subname(i, system->getMasterName(i));
    }

    // MSHR miss latency statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshr_miss_latency[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_mshr_miss_latency")
            .desc("number of " + cstr + " MSHR miss cycles")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            mshr_miss_latency[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMshrMissLatency
        .name(name() + ".demand_mshr_miss_latency")
        .desc("number of demand (read+write) MSHR miss cycles")
        .flags(total | nozero | nonan)
        ;
    demandMshrMissLatency = SUM_DEMAND(mshr_miss_latency);
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMshrMissLatency.subname(i, system->getMasterName(i));
    }

    overallMshrMissLatency
        .name(name() + ".overall_mshr_miss_latency")
        .desc("number of overall MSHR miss cycles")
        .flags(total | nozero | nonan)
        ;
    overallMshrMissLatency =
        demandMshrMissLatency + SUM_NON_DEMAND(mshr_miss_latency);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrMissLatency.subname(i, system->getMasterName(i));
    }

    // MSHR uncacheable statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshr_uncacheable[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_mshr_uncacheable")
            .desc("number of " + cstr + " MSHR uncacheable")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            mshr_uncacheable[access_idx].subname(i, system->getMasterName(i));
        }
    }

    overallMshrUncacheable
        .name(name() + ".overall_mshr_uncacheable_misses")
        .desc("number of overall MSHR uncacheable misses")
        .flags(total | nozero | nonan)
        ;
    overallMshrUncacheable =
        SUM_DEMAND(mshr_uncacheable) + SUM_NON_DEMAND(mshr_uncacheable);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrUncacheable.subname(i, system->getMasterName(i));
    }

    // MSHR miss latency statistics
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshr_uncacheable_lat[access_idx]
            .init(system->maxMasters())
            .name(name() + "." + cstr + "_mshr_uncacheable_latency")
            .desc("number of " + cstr + " MSHR uncacheable cycles")
            .flags(total | nozero | nonan)
            ;
        for (int i = 0; i < system->maxMasters(); i++) {
            mshr_uncacheable_lat[access_idx].subname(
                i, system->getMasterName(i));
        }
    }

    overallMshrUncacheableLatency
        .name(name() + ".overall_mshr_uncacheable_latency")
        .desc("number of overall MSHR uncacheable cycles")
        .flags(total | nozero | nonan)
        ;
    overallMshrUncacheableLatency =
        SUM_DEMAND(mshr_uncacheable_lat) +
        SUM_NON_DEMAND(mshr_uncacheable_lat);
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrUncacheableLatency.subname(i, system->getMasterName(i));
    }

#if 0
    // MSHR access formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshrAccesses[access_idx]
            .name(name() + "." + cstr + "_mshr_accesses")
            .desc("number of " + cstr + " mshr accesses(hits+misses)")
            .flags(total | nozero | nonan)
            ;
        mshrAccesses[access_idx] =
            mshr_hits[access_idx] + mshr_misses[access_idx]
            + mshr_uncacheable[access_idx];
    }

    demandMshrAccesses
        .name(name() + ".demand_mshr_accesses")
        .desc("number of demand (read+write) mshr accesses")
        .flags(total | nozero | nonan)
        ;
    demandMshrAccesses = demandMshrHits + demandMshrMisses;

    overallMshrAccesses
        .name(name() + ".overall_mshr_accesses")
        .desc("number of overall (read+write) mshr accesses")
        .flags(total | nozero | nonan)
        ;
    overallMshrAccesses = overallMshrHits + overallMshrMisses
        + overallMshrUncacheable;
#endif

    // MSHR miss rate formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        mshrMissRate[access_idx]
            .name(name() + "." + cstr + "_mshr_miss_rate")
            .desc("mshr miss rate for " + cstr + " accesses")
            .flags(total | nozero | nonan)
            ;
        mshrMissRate[access_idx] =
            mshr_misses[access_idx] / accesses[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            mshrMissRate[access_idx].subname(i, system->getMasterName(i));
        }
    }

    demandMshrMissRate
        .name(name() + ".demand_mshr_miss_rate")
        .desc("mshr miss rate for demand accesses")
        .flags(total | nozero | nonan)
        ;
    demandMshrMissRate = demandMshrMisses / demandAccesses;
    for (int i = 0; i < system->maxMasters(); i++) {
        demandMshrMissRate.subname(i, system->getMasterName(i));
    }

    overallMshrMissRate
        .name(name() + ".overall_mshr_miss_rate")
        .desc("mshr miss rate for overall accesses")
        .flags(total | nozero | nonan)
        ;
    overallMshrMissRate = overallMshrMisses / overallAccesses;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallMshrMissRate.subname(i, system->getMasterName(i));
    }

    // mshrMiss latency formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        avgMshrMissLatency[access_idx]
            .name(name() + "." + cstr + "_avg_mshr_miss_latency")
            .desc("average " + cstr + " mshr miss latency")
            .flags(total | nozero | nonan)
            ;
        avgMshrMissLatency[access_idx] =
            mshr_miss_latency[access_idx] / mshr_misses[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            avgMshrMissLatency[access_idx].subname(
                i, system->getMasterName(i));
        }
    }

    demandAvgMshrMissLatency
        .name(name() + ".demand_avg_mshr_miss_latency")
        .desc("average overall mshr miss latency")
        .flags(total | nozero | nonan)
        ;
    demandAvgMshrMissLatency = demandMshrMissLatency / demandMshrMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        demandAvgMshrMissLatency.subname(i, system->getMasterName(i));
    }

    overallAvgMshrMissLatency
        .name(name() + ".overall_avg_mshr_miss_latency")
        .desc("average overall mshr miss latency")
        .flags(total | nozero | nonan)
        ;
    overallAvgMshrMissLatency = overallMshrMissLatency / overallMshrMisses;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallAvgMshrMissLatency.subname(i, system->getMasterName(i));
    }

    // mshrUncacheable latency formulas
    for (int access_idx = 0; access_idx < MemCmd::NUM_MEM_CMDS; ++access_idx) {
        MemCmd cmd(access_idx);
        const string &cstr = cmd.toString();

        avgMshrUncacheableLatency[access_idx]
            .name(name() + "." + cstr + "_avg_mshr_uncacheable_latency")
            .desc("average " + cstr + " mshr uncacheable latency")
            .flags(total | nozero | nonan)
            ;
        avgMshrUncacheableLatency[access_idx] =
            mshr_uncacheable_lat[access_idx] / mshr_uncacheable[access_idx];

        for (int i = 0; i < system->maxMasters(); i++) {
            avgMshrUncacheableLatency[access_idx].subname(
                i, system->getMasterName(i));
        }
    }

    overallAvgMshrUncacheableLatency
        .name(name() + ".overall_avg_mshr_uncacheable_latency")
        .desc("average overall mshr uncacheable latency")
        .flags(total | nozero | nonan)
        ;
    overallAvgMshrUncacheableLatency =
        overallMshrUncacheableLatency / overallMshrUncacheable;
    for (int i = 0; i < system->maxMasters(); i++) {
        overallAvgMshrUncacheableLatency.subname(i, system->getMasterName(i));
    }

    replacements
        .name(name() + ".replacements")
        .desc("number of replacements")
        ;
}

void
BaseCache::regProbePoints()
{
    ppHit = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Hit");
    ppMiss = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Miss");
    ppFill = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Fill");
}

///////////////
//
// CpuSidePort
//
///////////////
bool
BaseCache::CpuSidePort::recvTimingSnoopResp(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    assert(pkt->isResponse());

    // Express snoop responses from master to slave, e.g., from L1 to L2
    cache->recvTimingSnoopResp(pkt);
    return true;
}


bool
BaseCache::CpuSidePort::tryTiming(PacketPtr pkt)
{
    if (cache->system->bypassCaches() || pkt->isExpressSnoop()) {
        // always let express snoop packets through even if blocked
        return true;
    } else if (blocked || mustSendRetry) {
        // either already committed to send a retry, or blocked
        mustSendRetry = true;
        return false;
    }
    mustSendRetry = false;
    return true;
}

bool
BaseCache::CpuSidePort::recvTimingReq(PacketPtr pkt)
{
    assert(pkt->isRequest());

    if (cache->system->bypassCaches()) {
        // Just forward the packet if caches are disabled.
        // @todo This should really enqueue the packet rather
        bool M5_VAR_USED success = cache->memSidePort.sendTimingReq(pkt);
        assert(success);
        return true;
    } else if (tryTiming(pkt)) {
        cache->recvTimingReq(pkt);
        return true;
    }
    return false;
}

Tick
BaseCache::CpuSidePort::recvAtomic(PacketPtr pkt)
{
    if (cache->system->bypassCaches()) {
        // Forward the request if the system is in cache bypass mode.
        return cache->memSidePort.sendAtomic(pkt);
    } else {
        return cache->recvAtomic(pkt);
    }
}

void
BaseCache::CpuSidePort::recvFunctional(PacketPtr pkt)
{
    if (cache->system->bypassCaches()) {
        // The cache should be flushed if we are in cache bypass mode,
        // so we don't need to check if we need to update anything.
        cache->memSidePort.sendFunctional(pkt);
        return;
    }

    // functional request
    cache->functionalAccess(pkt, true);
}

AddrRangeList
BaseCache::CpuSidePort::getAddrRanges() const
{
    return cache->getAddrRanges();
}


BaseCache::
CpuSidePort::CpuSidePort(const std::string &_name, BaseCache *_cache,
                         const std::string &_label)
    : CacheSlavePort(_name, _cache, _label), cache(_cache)
{
}

///////////////
//
// MemSidePort
//
///////////////
bool
BaseCache::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    cache->recvTimingResp(pkt);
    return true;
}

// Express snooping requests to memside port
void
BaseCache::MemSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    // handle snooping requests
    cache->recvTimingSnoopReq(pkt);
}

Tick
BaseCache::MemSidePort::recvAtomicSnoop(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    return cache->recvAtomicSnoop(pkt);
}

void
BaseCache::MemSidePort::recvFunctionalSnoop(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    // functional snoop (note that in contrast to atomic we don't have
    // a specific functionalSnoop method, as they have the same
    // behaviour regardless)
    cache->functionalAccess(pkt, false);
}

void
BaseCache::CacheReqPacketQueue::sendDeferredPacket()
{
    // sanity check
    assert(!waitingOnRetry);

    // there should never be any deferred request packets in the
    // queue, instead we resly on the cache to provide the packets
    // from the MSHR queue or write queue
    assert(deferredPacketReadyTime() == MaxTick);

    // check for request packets (requests & writebacks)
    QueueEntry* entry = cache.getNextQueueEntry();
    
    if (!entry) {
        // can happen if e.g. we attempt a writeback and fail, but
        // before the retry, the writeback is eliminated because
        // we snoop another cache's ReadEx.
    } else {
        // let our snoop responses go first if there are responses to
        // the same addresses
        if (checkConflictingSnoop(entry->blkAddr)) {
            return;
        }

        waitingOnRetry = entry->sendPacket(cache);
    }

    // if we succeeded and are not waiting for a retry, schedule the
    // next send considering when the next queue is ready, note that
    // snoop responses have their own packet queue and thus schedule
    // their own events
    if (!waitingOnRetry) {
        DPRINTF(Cache, "%s schedule send event with entry"
                "[%p, MSHR? %d] successfully\n",
                cache.getName().c_str(), entry, entry ? entry->isMSHR_ : -1);
        
        DEBUG_MEM("%s schedule send event with entry"
                "[%p, MSHR? %d] successfully",
                cache.getName().c_str(), entry, entry ? entry->isMSHR_ : -1);

        schedSendEvent(cache.nextQueueReadyTime());
      
        /// 针对提升或者降低预取Cache层级的处理
        MSHR* mshr = nullptr;
        if (entry && entry->isMSHR_) {
            mshr = dynamic_cast<MSHR*>(entry);
        }
        /// 设置改判断以便避免在send过程中MSHR被deallocate
        if (mshr && mshr->hasTargets()) {
            PacketPtr tgt_pkt = mshr->getTarget()->pkt;
            if (tgt_pkt->packetType_ == prefetch_filter::Pref) {
                DEBUG_MEM("%s packet @0x%lx [%s -> %s] sent",
                        cache.getName().c_str(), tgt_pkt->getAddr(),
                        BaseCache::levelName_[tgt_pkt->srcCacheLevel_].c_str(),
                        BaseCache::levelName_[tgt_pkt->targetCacheLevel_].c_str());
                if (cache.cacheLevel_ < tgt_pkt->targetCacheLevel_) {
                    /// 如果目标的MSHR是一个降级别预取，发送之后立即清空MSHR
                    DEBUG_MEM("%s deallocate MSHR[%p] for low level "
                            "prefetch @0x%lx ", cache.getName().c_str(),
                            mshr, tgt_pkt->getAddr());
                    MSHR::TargetList targets =
                            mshr->extractServiceableTargets(tgt_pkt);
                    for (auto target : targets) {
                        /// 删除所有降级别的预取操作
                        delete target.pkt;
                    }
                    const bool was_full = cache.mshrQueue.isFull();
                    cache.mshrQueue.deallocate(mshr);
                    
                    /// 释放MSHR之后处理Block状态
                    if (was_full && !cache.mshrQueue.isFull()) {
                        cache.clearBlocked(Blocked_NoMSHRs);
                    }

                    if (cache.prefetcher && cache.mshrQueue.canPrefetch()) {
                        Tick next_pf_time = std::max(
                                cache.prefetcher->nextPrefetchReadyTime(),
                                cache.clockEdge());
                        if (next_pf_time != MaxTick)
                            cache.schedMemSideSendEvent(next_pf_time);
                    }
                } else if (cache.cacheLevel_ >= tgt_pkt->targetCacheLevel_) {
                    /// 如果合并为一个提级/平级预取，
                    DEBUG_MEM("MSHR changed for none low level prefetch "
                            "@0x%lx", tgt_pkt->getAddr());
                    /// 为所有平级/提级预取修改属性
                    for (auto& target : mshr->targets) {
                        panic_if(target.pkt->packetType_ !=
                                prefetch_filter::Pref, "Prefetch mshr should "
                                "not have demand target");
                        if (target.pkt->targetCacheLevel_ <=
                                cache.cacheLevel_) {
                            target.source = MSHR::Target::FromCPU;
                        }
                    }
                }
            }
        }
    } else {
        DPRINTF(Cache, "%s schedule send event with entry"
                "[%p, MSHR? %d] failed\n",
                cache.getName().c_str(), entry, entry ? entry->isMSHR_ : -1);
        
        DEBUG_MEM("%s schedule send event with entry"
                "[%p, MSHR? %d] failed",
                cache.getName().c_str(), entry, entry ? entry->isMSHR_ : -1);
        
        if (cache.prefetchFilter_ && cache.prefetcher) {
            uint8_t newDegree;
            cache.prefetchFilter_->notifyCacheReqSentFailed(&cache,
                    cache.writeBuffer.numEntries + cache.mshrQueue.numEntries,
                    cache.writeBuffer.numWaitingDemands_ +
                    cache.mshrQueue.numWaitingDemands_,
                    cache.prefetcher->originDegree_, &newDegree);
            // cache.prefetcher->throttlingDegree_ = newDegree;
        }
    }
}

BaseCache::MemSidePort::MemSidePort(const std::string &_name,
                                    BaseCache *_cache,
                                    const std::string &_label)
    : CacheMasterPort(_name, _cache, _reqQueue, _snoopRespQueue),
      _reqQueue(*_cache, *this, _snoopRespQueue, _label),
      _snoopRespQueue(*_cache, *this, true, _label), cache(_cache)
{
}

void
WriteAllocator::updateMode(Addr write_addr, unsigned write_size,
                           Addr blk_addr)
{
    // check if we are continuing where the last write ended
    if (nextAddr == write_addr) {
        delayCtr[blk_addr] = delayThreshold;
        // stop if we have already saturated
        if (mode != WriteMode::NO_ALLOCATE) {
            byteCount += write_size;
            // switch to streaming mode if we have passed the lower
            // threshold
            if (mode == WriteMode::ALLOCATE &&
                byteCount > coalesceLimit) {
                mode = WriteMode::COALESCE;
                DPRINTF(Cache, "Switched to write coalescing\n");
            } else if (mode == WriteMode::COALESCE &&
                       byteCount > noAllocateLimit) {
                // and continue and switch to non-allocating mode if we
                // pass the upper threshold
                mode = WriteMode::NO_ALLOCATE;
                DPRINTF(Cache, "Switched to write-no-allocate\n");
            }
        }
    } else {
        // we did not see a write matching the previous one, start
        // over again
        byteCount = write_size;
        mode = WriteMode::ALLOCATE;
        resetDelay(blk_addr);
    }
    nextAddr = write_addr + write_size;
}

WriteAllocator*
WriteAllocatorParams::create()
{
    return new WriteAllocator(this);
}
