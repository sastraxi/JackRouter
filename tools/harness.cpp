/*
 * JackBridge IPC harness — single-threaded discrete-event sim.
 *
 * Two independent tests:
 *   TEST 1 (Bug 1): HAL stall while daemon keeps writing. Always fails because
 *   the daemon has zero safety lead regardless of any toggle fix.
 *
 *   TEST 2 (Bug 2): DAW toggle (stop/start). Tests timeline continuity.
 *   PASSES with the transition-based daemon fix (no driver changes).
 *
 * Build:  c++ -std=c++11 -o harness harness.cpp
 * Run:    ./harness
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int      DAEMON_PERIOD  = 64;
static const uint32_t RING_FRAMES    = 4096;
static const double   HOST_TICKS_PER_FRAME = 1000000000.0 / 48000.0;
static const int      JB_DRV_STATUS_STARTED = 2;
static const int      JB_DRV_STATUS_ACTIVE  = 1;

struct SimIPC {
    uint64_t shmZeroHostTime    = 0;
    uint64_t shmNumberTimeStamps= 0;
    uint64_t shmSyncMode        = 1;
    uint64_t shmDriverStatus    = 0;
    float    buf_down[RING_FRAMES * 2];
};

static uint64_t g_time = 0;
static inline uint64_t now() { return g_time; }
static inline void tick() { g_time += (uint64_t)(DAEMON_PERIOD * HOST_TICKS_PER_FRAME); }

// ---------------------------------------------------------------------------
// Daemon model
// ---------------------------------------------------------------------------
struct Daemon {
    SimIPC* ipc = nullptr;
    uint64_t frameNum = 0;
    bool isActive = false;
    uint64_t lastDrv = JB_DRV_STATUS_ACTIVE;
    bool fixOn = false;

    void init(SimIPC* _ipc, uint64_t _frame, bool _active, uint64_t _last, bool _fix) {
        ipc = _ipc; frameNum = _frame; isActive = _active; lastDrv = _last; fixOn = _fix;
    }

    void cycle(bool halSkip) {
        uint64_t current = ipc->shmDriverStatus;

        if (current != JB_DRV_STATUS_STARTED) {
            lastDrv = current;
            return;
        }

        if (!isActive) {
            frameNum = 0;
            ipc->shmSyncMode = 1;
            ipc->shmNumberTimeStamps = 0;
            isActive = true;
            lastDrv = current;
        }

        // --- TOGGLE FIX: detect ACTIVE -> STARTED transition ---
        if (fixOn && lastDrv != JB_DRV_STATUS_STARTED) {
            ipc->shmZeroHostTime = now();
            ipc->shmNumberTimeStamps = frameNum / RING_FRAMES;
        }
        lastDrv = current;

        if ((frameNum % RING_FRAMES) == 0) {
            ipc->shmZeroHostTime = now();
            ipc->shmNumberTimeStamps = frameNum / RING_FRAMES;
        }

        // sendToCoreAudio (zero safety lead — Bug 1 root cause)
        uint32_t off = (uint32_t)(frameNum % RING_FRAMES);
        for (int i = 0; i < DAEMON_PERIOD; ++i) {
            uint64_t f = frameNum + i;
            uint32_t p = (off + i) % RING_FRAMES;
            ipc->buf_down[p * 2 + 0] = (float)f;
            ipc->buf_down[p * 2 + 1] = -(float)f;
        }
        frameNum += DAEMON_PERIOD;
    }
};

// ---------------------------------------------------------------------------
// HAL model
// ---------------------------------------------------------------------------
struct HAL {
    SimIPC* ipc = nullptr;
    uint64_t sampleTime = 0;

    void init(SimIPC* _ipc) { ipc = _ipc; sampleTime = 0; }

    int cycle(uint32_t nframes, bool skip) {
        if (skip) { sampleTime = 0; return 0; }
        uint64_t t = now();
        uint64_t ah = ipc->shmZeroHostTime;
        uint64_t as = ipc->shmNumberTimeStamps * RING_FRAMES;
        sampleTime = (ah == 0) ? as : as + (uint64_t)((t - ah) / HOST_TICKS_PER_FRAME);

        uint32_t so = (uint32_t)(sampleTime % RING_FRAMES);
        uint32_t c1 = nframes, c2 = 0;
        if (so + c1 > RING_FRAMES) { c1 = RING_FRAMES - so; c2 = nframes - c1; }

        int hits = 0;
        for (uint32_t i = 0; i < c1; ++i) {
            uint64_t exp = sampleTime + i;
            if (ipc->buf_down[(so + i) * 2 + 0] != (float)exp ||
                ipc->buf_down[(so + i) * 2 + 1] != -(float)exp) hits++;
        }
        for (uint32_t i = 0; i < c2; ++i) {
            uint64_t exp = sampleTime + c1 + i;
            if (ipc->buf_down[i * 2 + 0] != (float)exp ||
                ipc->buf_down[i * 2 + 1] != -(float)exp) hits++;
        }
        return hits;
    }
};

// ---------------------------------------------------------------------------
// TEST 1: HAL stall (always fails — Bug 1, no safety lead)
// ---------------------------------------------------------------------------
static int test1(bool fixOn) {
    SimIPC ipc; memset(&ipc, 0, sizeof(ipc)); ipc.shmSyncMode = 1; g_time = 0;
    Daemon d; d.init(&ipc, DAEMON_PERIOD * 200, true, JB_DRV_STATUS_STARTED, fixOn);
    HAL    h; h.init(&ipc);

    ipc.shmDriverStatus = JB_DRV_STATUS_STARTED;

    // normal
    for (int c = 0; c < 200; ++c) { tick(); d.cycle(false); h.cycle(DAEMON_PERIOD, false); }
    // HAL stalls 80 cycles
    for (int c = 0; c < 80; ++c)  { tick(); d.cycle(true);  h.cycle(DAEMON_PERIOD, true);  }
    // resume
    int hits = 0;
    for (int c = 0; c < 40; ++c) { tick(); d.cycle(false); hits += h.cycle(DAEMON_PERIOD, false); }
    return hits;
}

// ---------------------------------------------------------------------------
// TEST 2: DAW toggle (passes with transition fix)
// ---------------------------------------------------------------------------
static int test2(bool fixOn) {
    SimIPC ipc; memset(&ipc, 0, sizeof(ipc)); ipc.shmSyncMode = 1; g_time = 0;
    Daemon d; d.init(&ipc, DAEMON_PERIOD * 200, true, JB_DRV_STATUS_STARTED, fixOn);
    HAL    h; h.init(&ipc);

    ipc.shmDriverStatus = JB_DRV_STATUS_STARTED;

    // normal run
    for (int c = 0; c < 100; ++c) { tick(); d.cycle(false); h.cycle(DAEMON_PERIOD, false); }

    // toggle OFF
    ipc.shmDriverStatus = JB_DRV_STATUS_ACTIVE;
    uint64_t frozen = d.frameNum;
    for (int c = 0; c < 50; ++c) { tick(); d.cycle(true); h.cycle(DAEMON_PERIOD, true); }

    // toggle ON
    ipc.shmDriverStatus = JB_DRV_STATUS_STARTED;

    // first resume cycle — capture offset
    tick(); d.cycle(false);
    uint64_t daemonWrite = d.frameNum - DAEMON_PERIOD;
    int hits = h.cycle(DAEMON_PERIOD, false);
    uint64_t halRead = h.sampleTime;
    int offset = (int)(halRead % RING_FRAMES) - (int)(daemonWrite % RING_FRAMES);
    if (offset < 0) offset += RING_FRAMES;

    for (int c = 0; c < 20; ++c) { tick(); d.cycle(false); hits += h.cycle(DAEMON_PERIOD, false); }

    printf("    frozenFrame=%llu  resume_halRead=%llu  resume_daemonWrite=%llu  offset=%d  hits=%d\n",
           (unsigned long long)frozen,
           (unsigned long long)halRead,
           (unsigned long long)daemonWrite,
           offset, hits);
    return hits;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    printf("=== JackBridge Bug Harness ===\n");
    printf("Ring = %u frames, period = %d frames\n\n", RING_FRAMES, DAEMON_PERIOD);

    printf("TEST 1: HAL stall (Bug 1 — zero safety lead)\n");
    printf("  Baseline:   %d stale frames\n", test1(false));
    printf("  With fix:   %d stale frames  (same — fix doesn't touch data path)\n\n", test1(true));

    printf("TEST 2: DAW toggle (Bug 2 — timeline continuity)\n");
    printf("  Baseline:\n"); int b2 = test2(false);
    printf("  With fix:\n");  int f2 = test2(true);

    printf("\nBug 2 RESULT: %s\n", (f2 == 0) ? "PASS — toggle fixed"
                                         : "FAIL — toggle still broken");
    return (f2 == 0) ? 0 : 1;
}
