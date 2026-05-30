/*
 File: JackBridge.h

 MIT License
 
 Copyright (c) 2018 Shunji Uno <madhatter68@linux-dtm.ivory.ne.jp>
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */
#pragma once
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <sstream>

#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <atomic>
#include <mach/mach_time.h>
#include "jb_log.hpp"

// IPC contract version. Bump on every shm layout change (sizes, offsets, field
// types, sync semantics). Phase 2.3 wires the handshake — daemon and HAL both
// refuse to attach on mismatch.
#define JACKBRIDGE_PROTOCOL_VERSION 3

// shm sync fields are std::atomic<uint64_t> placed by reinterpret_cast over the
// mapped region. Both targets must agree that the type is lock-free and the
// representation is just an aligned uint64_t — true on every arm64 / x86_64
// target we ship to, but assert it at compile time so a future toolchain
// surprise fails loudly instead of silently corrupting the IPC.
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t),
              "std::atomic<uint64_t> must have the same layout as uint64_t");
static_assert(alignof(std::atomic<uint64_t>) == alignof(uint64_t),
              "std::atomic<uint64_t> must have the same alignment as uint64_t");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "std::atomic<uint64_t> must be lock-free on this target");

/******************************************************************************
 Audio functions (Generic/CoreAudio)
******************************************************************************/
// Shared memory map: (mapped every 1MB boundary for each instance)
// 0x0000      : Control Registers (Read/Write Pointers)
// 0x0000      :    upstream write pointer
// 0x0002      :    upstream read  pointer
// 0x0004      :    downstream write pointer
// 0x0006      :    downstream read pointer
// 0x0080      :    RingBufferSize(Default: 4K*2ch)
// 0x0100      :    TimeStamp number
// 0x0108      :    HostTime at recent TimeZero
// 0x0110      :    Seed
// 0x0118      :    SyncMode
// 0x0120      :    RingBufferSize
// 0x0128      :    Driver status
// 0x0130      :    Protocol version (handshake — refuse-on-mismatch)
// 0x0138      :    Daemon alive heartbeat counter
// 0x0180      :    Current Frame Number(coreAudio read)
// 0x0188      :    Current Frame Number(coreAudio write)
// 0x0190      :    Current Frame Number(coreAudio read)
// 0x0198      :    Current Frame Number(coreAudio write)
// 0x10000     : Upstream buffer #0 (Driver -> Application)
// 0x18000     : Downstream buffer #0 (Application -> Driver)
// 0x20000     : Upstream buffer #0 (Driver -> Application)
// 0x28000     : Downstream buffer #0 (Application -> Driver)

typedef float sample_t;
#define AUDIO_SAMPLE_SIZE (sizeof(sample_t))
#define NUM_INPUT_STREAMS   1
#define NUM_OUTPUT_STREAMS  2
#define MAX_STREAMS         2
#define MAX_CHANNELS        ((MAX_STREAMS)*2)
#define NUM_INSTANCES       1

#define STRBUFSZ            (0x8000) // 32KB Ring buffer
#define STRBUFNUM           (STRBUFSZ/AUDIO_SAMPLE_SIZE) // 1024 entries
#define REGSMAP_SIZE        (0x10000*(MAX_STREAMS)+0x10000)
#define REGSMAP_BOUNDARY    REGSMAP_SIZE
#define JACK_SHMSIZE        (REGSMAP_SIZE*NUM_INSTANCES)
#define STRBUF_U0           (0x10000)
#define STRBUF_UP(i)        (0x10000*(i)+0x10000)
#define STRBUF_DOWN(i)      (0x10000*(i)+0x18000)

#define JACK_SHMPATH        "/JackBridge"

class JackBridgeDriverIF {
protected:
    uint32_t instance;
    int shm_fd;
    sample_t *buf_up[MAX_STREAMS];
    sample_t *buf_down[MAX_STREAMS];
    uint64_t   FrameNumber;
    int        FramesPerBuffer;
    std::atomic<uint64_t> *shmNumberTimeStamps;
    std::atomic<uint64_t> *shmZeroHostTime;
    std::atomic<uint64_t> *shmSeed;
    std::atomic<uint64_t> *shmSyncMode;
    std::atomic<uint64_t> *shmBufferSize;
    std::atomic<uint64_t> *shmDriverStatus;
#define JB_DRV_STATUS_INIT      0
#define JB_DRV_STATUS_ACTIVE    1
#define JB_DRV_STATUS_STARTED   2
    std::atomic<uint64_t> *shmProtocolVersion;
    std::atomic<uint64_t> *shmDaemonAlive;
    std::atomic<uint64_t> *shmReadFrameNumber[MAX_STREAMS];
    std::atomic<uint64_t> *shmWriteFrameNumber[MAX_STREAMS];

    int create_shm() {
        struct stat stat;
        JB_LOG_INFO(jb_log_shm(), "JackBridge: Initializing shared memory to communicate with jack(%d).", 0);
        shm_fd = shm_open(JACK_SHMPATH, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
        if (shm_fd < 0) {
            JB_LOG_ERR(jb_log_shm(), "shm cannot be opened with %{public}s.", strerror(errno));
            return -1;
        }
        
        if (fstat(shm_fd, &stat) < 0) {
            JB_LOG_ERR(jb_log_shm(), "Couldn't get shm stat with %{public}s.", strerror(errno));
            close(shm_fd);
            return -1;
        }
        
        if (stat.st_size != JACK_SHMSIZE) {
            if (ftruncate(shm_fd, JACK_SHMSIZE) == -1) {
                JB_LOG_INFO(jb_log_shm(), "shm cannot be truncated with %{public}s. Try to recreate shm.", strerror(errno));
                close(shm_fd);
                shm_unlink(JACK_SHMPATH);
                shm_fd = shm_open(JACK_SHMPATH, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
                if (shm_fd < 0) {
                    JB_LOG_ERR(jb_log_shm(), "shm cannot be recreated with %{public}s.", strerror(errno));
                    return -1;
                }
                if (ftruncate(shm_fd, JACK_SHMSIZE) == -1) {
                    JB_LOG_INFO(jb_log_shm(), "shm cannot be truncated with %{public}s.", strerror(errno));
                }
            }
            JB_LOG_INFO(jb_log_shm(), "Recreated shm because shm size is not matched as expected. (%d)", 0);
        }
        close(shm_fd);
        return 0;
    }
    
    int attach_shm() {
        struct stat stat;
        
        shm_fd = shm_open(JACK_SHMPATH, O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
        if (shm_fd < 0) {
            JB_LOG_ERR(jb_log_shm(), "shm_open() failed with %{public}s.", strerror(errno));
            return -1;
        }
        
        if (fstat(shm_fd, &stat) < 0) {
            JB_LOG_ERR(jb_log_shm(), "fstat() failed with %{public}s.", strerror(errno));
            return -1;
        } else {
            if (stat.st_size != JACK_SHMSIZE) {
                JB_LOG_ERR(jb_log_shm(), "does not match shmsize(%lld). May be driver version mismatch", stat.st_size);
            }
        }
        
        char* shm_base = (char*)mmap(NULL, REGSMAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, instance*REGSMAP_BOUNDARY);
        //char* shm_base = (char*)mmap(NULL, JACK_SHMSIZE, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_base == MAP_FAILED) {
            JB_LOG_ERR(jb_log_shm(), "mmap() failed with %{public}s", strerror(errno));
            return -1;
        }

        shmNumberTimeStamps = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x100);
        shmZeroHostTime = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x108);
        shmSeed = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x110);
        shmSyncMode = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x118);
        shmBufferSize = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x120);
        shmDriverStatus = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x128);
        shmProtocolVersion = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x130);
        shmDaemonAlive = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x138);

        for(int i=0; i<MAX_STREAMS; i++) {
            buf_up[i]   = (sample_t*)(shm_base + STRBUF_UP(i));
            buf_down[i] = (sample_t*)(shm_base + STRBUF_DOWN(i));
            shmReadFrameNumber[i] = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x180);
            shmWriteFrameNumber[i] = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+0x188+i*0x10);
        }
        
        return 0;
    }
    
    // Cooperative version handshake. Whichever side attaches to a fresh shm
    // first publishes its version; the second side validates. Returns true on
    // match (or first-writer), false on mismatch — caller should log + exit.
    // The race window between the two CAS-like reads is benign: both sides
    // are pinned to the same JACKBRIDGE_PROTOCOL_VERSION at build time, so any
    // disagreement implies a stale shm from a previous install.
    bool check_protocol_version() {
        uint64_t observed = shmProtocolVersion->load(std::memory_order_acquire);
        if (observed == 0) {
            shmProtocolVersion->store(JACKBRIDGE_PROTOCOL_VERSION,
                                      std::memory_order_release);
            return true;
        }
        return observed == JACKBRIDGE_PROTOCOL_VERSION;
    }

public:
    JackBridgeDriverIF(uint32_t _instance) : instance(_instance) {
    }

    ~JackBridgeDriverIF() {
    }
};
