/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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
 * Authors: Javier Bueno
 */

#include <cmath>

#include "mem/cache/prefetch/slim_ampm.hh"

#include "params/SlimAMPMPrefetcher.hh"

SlimAMPMPrefetcher::SlimAMPMPrefetcher(const SlimAMPMPrefetcherParams* p)
  : QueuedPrefetcher(p), ampm(*p->ampm), dcpt(*p->dcpt)
{
    /// 初始化父类的预取度 
    originDegree_ = std::max(dcpt.degree_,
            static_cast<uint16_t>(ampm.degree));
    throttlingDegree_ = originDegree_;
}

void
SlimAMPMPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                  std::vector<AddrPriority> &addresses)
{
    dcpt.calculatePrefetch(pfi, addresses);
    if (addresses.empty()) {
        ampm.calculatePrefetch(pfi, addresses);
    }
}

SlimAMPMPrefetcher*
SlimAMPMPrefetcherParams::create()
{
    return new SlimAMPMPrefetcher(this);
}
