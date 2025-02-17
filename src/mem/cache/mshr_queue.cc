/*
 * Copyright (c) 2012-2013, 2015-2016, 2018 ARM Limited
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
 *          Andreas Sandberg
 */

/** @file
 * Definition of MSHRQueue class functions.
 */

#include "mem/cache/mshr_queue.hh"

#include <cassert>

#include "mem/cache/mshr.hh"
#include "mem/cache/prefetch_filter/pref_info.hh"
#include "mem/cache/prefetch_filter/debug_flag.hh"

MSHRQueue::MSHRQueue(const std::string &_label,
                     int num_entries, int reserve, int demand_reserve)
    : Queue<MSHR>(_label, num_entries, reserve),
      demandReserve(demand_reserve)
{}

MSHR *
MSHRQueue::allocate(Addr blk_addr, unsigned blk_size, PacketPtr pkt,
                    Tick when_ready, Counter order, bool alloc_on_fill)
{
    assert(!freeList.empty());
    MSHR *mshr = freeList.front();
    assert(mshr->getNumTargets() == 0);
    freeList.pop_front();

    mshr->allocate(blk_addr, blk_size, pkt, when_ready, order, alloc_on_fill);
    mshr->allocIter = allocatedList.insert(allocatedList.end(), mshr);
    mshr->readyIter = addToReadyList(mshr);
    
    allocated += 1;
    /// 更新处于等待状态的Demand MSHR数量
    numWaitingDemands_ += pkt->packetType_ == prefetch_filter::Dmd;
    panic_if(numWaitingDemands_ > allocated,
            "Unexpected number of waiting demand mshr");
    return mshr;
}

void
MSHRQueue::deallocate(MSHR *mshr) {
    Queue::deallocate(mshr);
    /// 更新处于等待状态的Demand MSHR数量
    numWaitingDemands_ = 0;
    for (auto singleMSHR : allocatedList) {
        if (!singleMSHR->inService && singleMSHR->getTarget()->pkt &&
                singleMSHR->getTarget()->pkt->packetType_ ==
                prefetch_filter::Dmd) {
            numWaitingDemands_++;
        }
    }
    panic_if(numWaitingDemands_ > allocated,
            "Unexpected number of waiting demand mshr");
}

void
MSHRQueue::moveToFront(MSHR *mshr)
{
    if (!mshr->inService) {
        assert(mshr == *(mshr->readyIter));
        readyList.erase(mshr->readyIter);
        mshr->readyIter = readyList.insert(readyList.begin(), mshr);
    }
}

void
MSHRQueue::delay(MSHR *mshr, Tick delay_ticks)
{
    mshr->delay(delay_ticks);
    auto it = std::find_if(mshr->readyIter, readyList.end(),
                            [mshr] (const MSHR* _mshr) {
                                return mshr->readyTime >= _mshr->readyTime;
                            });
    readyList.splice(it, readyList, mshr->readyIter);
}

void
MSHRQueue::markInService(MSHR *mshr, bool pending_modified_resp)
{
    mshr->markInService(pending_modified_resp);
    readyList.erase(mshr->readyIter);
    _numInService += 1;
    /// 更新处于等待状态的Demand MSHR数量
    numWaitingDemands_ = 0;
    for (auto singleMSHR : allocatedList) {
        if (!singleMSHR->inService && singleMSHR->getTarget()->pkt &&
                singleMSHR->getTarget()->pkt->packetType_ ==
                prefetch_filter::Dmd) {
            numWaitingDemands_++;
        }
    }
    panic_if(numWaitingDemands_ > allocated,
            "Unexpected number of waiting demand mshr");
}

void
MSHRQueue::markPending(MSHR *mshr)
{
    assert(mshr->inService);
    mshr->inService = false;
    --_numInService;
    /// 更新处于等待状态的Demand MSHR数量
    numWaitingDemands_ = 0;
    for (auto singleMSHR : allocatedList) {
        if (!singleMSHR->inService && singleMSHR->getTarget()->pkt &&
                singleMSHR->getTarget()->pkt->packetType_ ==
                prefetch_filter::Dmd) {
            numWaitingDemands_++;
        }
    }
    panic_if(numWaitingDemands_ > allocated,
            "Unexpected number of waiting demand mshr");
    /**
     * @ todo might want to add rerequests to front of pending list for
     * performance.
     */
    mshr->readyIter = addToReadyList(mshr);
}

bool
MSHRQueue::forceDeallocateTarget(MSHR *mshr)
{
    bool was_full = isFull();
    assert(mshr->hasTargets());
    // Pop the prefetch off of the target list
    mshr->popTarget();
    // Delete mshr if no remaining targets
    if (!mshr->hasTargets() && !mshr->promoteDeferredTargets()) {
        deallocate(mshr);
    }

    // Notify if MSHR queue no longer full
    return was_full && !isFull();
}
