/******************************************************************************
 *                                                                            *
 * Copyright 2021 Jan Henrik Weinstock                                        *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License");            *
 * you may not use this file except in compliance with the License.           *
 * You may obtain a copy of the License at                                    *
 *                                                                            *
 *     http://www.apache.org/licenses/LICENSE-2.0                             *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 *                                                                            *
 ******************************************************************************/

#include "vcml/models/meta/throttle.h"

namespace vcml {
namespace meta {

static u64 do_usleep(u64 delta) {
    u64 start = timestamp_us();
    usleep(delta);
    u64 end = timestamp_us();
    if (start >= end)
        return 0;
    u64 d = end - start;
    return d > delta ? d - delta : 0;
}

void throttle::update() {
    sc_time quantum = tlm::tlm_global_quantum::instance().get();
    sc_time interval = max<sc_time>(quantum, update_interval);
    next_trigger(interval);

    if (rtf > 0.0) {
        u64 actual = realtime_us() - m_start + m_extra;
        u64 target = time_to_us(interval) / rtf;

        if (actual < target) {
            m_extra = do_usleep(target - actual);
            if (!m_throttling)
                log_debug("throttling started");
            m_throttling = true;
        } else {
            m_extra = actual - target;
            if (m_throttling)
                log_debug("throttling stopped");
            m_throttling = false;
        }
    }

    m_start = realtime_us();
}

throttle::throttle(const sc_module_name& nm):
    module(nm),
    m_throttling(false),
    m_start(realtime_us()),
    m_extra(0),
    update_interval("update_interval", sc_time(10.0, SC_MS)),
    rtf("rtf", 0.0) {
    SC_HAS_PROCESS(throttle);
    SC_METHOD(update);
}

void throttle::session_suspend() {
    m_start -= realtime_us();
}

void throttle::session_resume() {
    m_start += realtime_us();
    m_extra = 0;
}

} // namespace meta
} // namespace vcml
