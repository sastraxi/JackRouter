/*
 File: JackBridge.h

MIT License

Copyright (c) 2016-2018 Shunji Uno <madhatter68@linux-dtm.ivory.ne.jp>

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

#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <atomic>
#include "jackClient.hpp"
#include "JackBridge.h"
#include "jb_log.hpp"

// Set in main() before jack_activate; read by the port-registration callback to
// wake the main thread out of sigwait when slave ports come or go. Notification
// callbacks are forbidden from calling jack_connect (JACK aborts with
// "Cannot callback the server in notification thread"), so we defer the wiring
// pass to the main thread via SIGUSR1.
static pthread_t g_main_thread;
static std::atomic<bool> g_wire_dirty{false};
#ifdef _WITH_MIDI_BRIDGE_
#include <rtmidi/RtMidi.h>
#define MAX_MIDI_PORTS 256
#endif // _WITH_MIDI_BRIDGE_

/*
 * JackBridge.cpp
 */
#define NUM_INPUT_CHANNELS  (NUM_INPUT_STREAMS*2)
#define NUM_OUTPUT_CHANNELS (NUM_OUTPUT_STREAMS*2)

class JackBridge : public JackClient, public JackBridgeDriverIF {
public:
    JackBridge(const char* name, int id, int num_Min, int num_Mout) : JackClient(name, JACK_PROCESS_CALLBACK), JackBridgeDriverIF(id) {
        if (attach_shm() < 0) {
            JB_LOG_ERR(jb_log_shm(), "attach_shm failed (id=%d)", id);
            exit(1);
        }

        if (!check_protocol_version()) {
            JB_LOG_ERR(jb_log_shm(),
                "shm protocol version mismatch — driver published %llu, daemon built for %d. Reinstall the matching .pkg.",
                (unsigned long long)shmProtocolVersion->load(std::memory_order_acquire),
                JACKBRIDGE_PROTOCOL_VERSION);
            exit(1);
        }

        check_jack_backend();

        isActive = false;
        isSyncMode = true; // FIXME: should be parameterized
        isVerbose = (getenv("JACKBRIDGE_DEBUG")) ? true : false;
        FrameNumber = 0;
        FramesPerBuffer = STRBUFNUM/2;
        shmBufferSize->store(STRBUFSZ, std::memory_order_release);
        shmSyncMode->store(0, std::memory_order_release);

        config_audio_ports();
#ifdef _WITH_MIDI_BRIDGE_
        create_midi_ports(name, num_Min, num_Mout);
        register_ports((const char**)nameAin, (const char**)nameAout, (const char**)nameMin, (const char**)nameMout);
#else
        register_ports((const char**)nameAin, (const char**)nameAout, NULL, NULL);
#endif // _WITH_MIDI_BRIDGE_

        // Must be set before jack_activate (in JackClient::activate). Fires for
        // every port registration after activation — we filter for slave ports
        // and re-run auto_wire() so connections survive netmanager reloads or
        // pi restarts.
        jack_set_port_registration_callback(client, _port_registration_callback, this);

        // For DEBUG
        lastHostTime = 0;
        struct mach_timebase_info theTimeBaseInfo;
        mach_timebase_info(&theTimeBaseInfo);
        double theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
        theHostClockFrequency *= 1000000000.0;
        HostTicksPerFrame = theHostClockFrequency / SampleRate;
        JB_LOG_INFO(jb_log_daemon(),
            "JackBridge#%u: start with samplerate=%d Hz, buffersize=%u bytes",
            instance, SampleRate, (unsigned)BufSize);
    }

    ~JackBridge() {
#ifdef _WITH_MIDI_BRIDGE_
        release_midi_ports();
#endif // _WITH_MIDI_BRIDGE_
    }

    // Refuse to start if jackd's backend is anything other than CoreAudio,
    // or if jackd's CoreAudio backend is pointed at JackBridge itself
    // (clock-device feedback loop).
    void check_jack_backend() {
        jack_port_t* port = jack_port_by_name(client, "system:playback_1");
        if (!port) {
            JB_LOG_ERR(jb_log_jack(),
                "no system:playback_1 port — jackd has no backend or backend has no playback. "
                "JackBridge requires a CoreAudio backend (-d coreaudio). See docs/macos-setup.md.");
            exit(1);
        }

        size_t alias_size = jack_port_name_size();
        char* alias_storage[2] = {
            (char*)calloc(1, alias_size),
            (char*)calloc(1, alias_size),
        };
        int n = jack_port_get_aliases(port, alias_storage);

        if (n <= 0) {
            JB_LOG_ERR(jb_log_jack(),
                "system:playback_1 has no aliases — jackd backend is likely 'net' or a "
                "non-CoreAudio driver. Required: coreaudio. See docs/macos-setup.md.");
            free(alias_storage[0]);
            free(alias_storage[1]);
            exit(1);
        }

        // Feedback-loop check: if jackd's clock device is JackBridge itself
        // (directly or via an aggregate whose name contains "JackBridge"),
        // CoreAudio doesn't detect the cycle — output is silence or runaway.
        // The HAL device's display name is "JackBridge" (see Localizable.strings).
        for (int i = 0; i < n; i++) {
            if (strstr(alias_storage[i], "JackBridge") != NULL) {
                JB_LOG_ERR(jb_log_jack(),
                    "jackd is clocked off JackBridge itself (alias=%{public}s). "
                    "This creates a CoreAudio feedback loop. Set ClockDeviceUID in "
                    "/Library/Application Support/JackBridge/config.plist to a different "
                    "device (e.g. built-in output). See docs/idiosyncrasies.md.",
                    alias_storage[i]);
                free(alias_storage[0]);
                free(alias_storage[1]);
                exit(1);
            }
        }

        JB_LOG_INFO(jb_log_jack(), "backend check OK (alias=%{public}s)", alias_storage[0]);
        free(alias_storage[0]);
        free(alias_storage[1]);
    }

    int process_callback(jack_nframes_t nframes) override {
        sample_t *ain[NUM_INPUT_CHANNELS];
        sample_t *aout[NUM_OUTPUT_CHANNELS];

        // Heartbeat — HAL watches this counter; if it stops advancing the HAL
        // flips DeviceIsAlive=0 so the DAW disconnects instead of getting
        // forever-silence. relaxed is fine: the staleness check only cares
        // that the value moves, not what the value is.
        shmDaemonAlive->fetch_add(1, std::memory_order_relaxed);

#ifdef _WITH_MIDI_BRIDGE_
        process_midi_message(nframes);
#endif // _WITH_MIDI_BRIDGE_

        if (shmDriverStatus->load(std::memory_order_acquire) != JB_DRV_STATUS_STARTED) {
            // Driver isn't working. Just return zero buffer;
            for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
                aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
                bzero(aout[i], STRBUFSZ);
            }
            return 0;
        }

        // For DEBUG
        check_progress();

        if (!isActive) {
            ncalls = 0;
            FrameNumber = 0;

            if (isSyncMode) {
                shmSyncMode->store(1, std::memory_order_relaxed);
                shmNumberTimeStamps->store(0, std::memory_order_relaxed);
                shmSeed->fetch_add(1, std::memory_order_release);
            }

            isActive = true;
            // FIXME(rt-safety): os_log on the JACK process callback path is
            // not strictly RT-safe (may take internal locks). Fires once per
            // activation, so the practical cost is bounded — revisit if it
            // shows up under load.
            JB_LOG_INFO(jb_log_jack(),
                "JackBridge#%u: activated SyncMode=%{public}s ZeroHostTime=0x%llx",
                instance, isSyncMode ? "yes" : "no",
                (unsigned long long)shmZeroHostTime->load(std::memory_order_acquire));
        }

        if ((FrameNumber % FramesPerBuffer) == 0) {
            if(shmSyncMode->load(std::memory_order_acquire) == 1) {
                shmZeroHostTime->store(mach_absolute_time(), std::memory_order_relaxed);
                shmNumberTimeStamps->store(FrameNumber / FramesPerBuffer,
                                           std::memory_order_release);
            }

            if ((!isSyncMode) && isVerbose && ((ncalls++) % 100) == 0) {
                uint64_t zht = shmZeroHostTime->load(std::memory_order_acquire);
                printf("JackBridge#%d: ZeroHostTime: %llx, %llu, diff:%d\n",
                    instance, zht,
                    shmNumberTimeStamps->load(std::memory_order_acquire),
                    ((int)(mach_absolute_time()+1000000-zht))-1000000);
            }
        }

        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            ain[i] = (sample_t*)jack_port_get_buffer(audioIn[i], nframes);
        }
        sendToCoreAudio(ain, nframes);


        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
        }
        receiveFromCoreAudio(aout, nframes);

        FrameNumber += nframes;

        return 0;
    }

    void setVerbose(bool flag) {
        JB_LOG_INFO(jb_log_daemon(),
            "JackBridge#%u: verbose mode %{public}s", instance, flag ? "on" : "off");
        isVerbose = flag;
    }

    // Auto-wire netmanager slave ports to our HAL bridge ports. Called once
    // after activate() to pick up slaves that registered before us, and again
    // from the port-registration callback when a new slave shows up later
    // (netadapter reload, pi reboot, etc.). Idempotent: jack_connect returns
    // EEXIST for already-connected pairs, which we treat as success.
    //
    // Policy is "first match per channel": the first *:from_slave_<n> seen is
    // wired to JackBridge input_<n>, etc. Multi-slave fan-out and an opt-out
    // are deferred to config.plist (PLAN.md §3.4.3).
    void auto_wire() {
        wire_direction("from_slave", JackPortIsOutput, audioIn, nAudioIn);
        wire_direction("to_slave",   JackPortIsInput,  audioOut, nAudioOut);
    }

    void on_shutdown() override {
        // Called by jackd when it goes away (intentional stop, crash, whatever).
        // Zero the heartbeat so the HAL's staleness watchdog flips DeviceIsAlive
        // immediately rather than waiting for the 5-cycle threshold, then nudge
        // main()'s sigwait so we exit cleanly. LaunchAgent KeepAlive brings us
        // back when jackd is back.
        shmDaemonAlive->store(0, std::memory_order_release);
        shmDriverStatus->store(JB_DRV_STATUS_INIT, std::memory_order_release);
        JB_LOG_DEFAULT(jb_log_jack(), "jackd shut down — exiting for LaunchAgent restart");
        kill(getpid(), SIGTERM);
    }

    static void _port_registration_callback(jack_port_id_t port_id, int registered, void* arg) {
        // Only react to port appearances, not departures — we don't need to
        // disconnect anything when a slave goes away (jackd handles that).
        if (!registered) return;
        JackBridge* self = (JackBridge*)arg;
        jack_port_t* port = jack_port_by_id(self->client, port_id);
        if (!port) return;
        const char* shortname = jack_port_short_name(port);
        if (!shortname) return;
        if (strncmp(shortname, "from_slave_", 11) == 0 ||
            strncmp(shortname, "to_slave_", 9)   == 0) {
            // Defer to main thread — jack_connect is illegal from here.
            g_wire_dirty.store(true, std::memory_order_release);
            pthread_kill(g_main_thread, SIGUSR1);
        }
    }

private:
    // Walks all ports of the given direction, filters by short-name == "<role>_<n>",
    // and connects the first match per channel to our local port `local[n-1]`.
    // `flags` selects the *remote* port direction: JackPortIsOutput when we're
    // looking for sources to feed our inputs, JackPortIsInput when we're looking
    // for sinks for our outputs.
    void wire_direction(const char* role, unsigned long flags,
                        jack_port_t* const* local, int n_local) {
        const char** ports = jack_get_ports(client, NULL,
                                            JACK_DEFAULT_AUDIO_TYPE, flags);
        if (!ports) return;

        for (int ch = 1; ch <= n_local; ch++) {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), "%s_%d", role, ch);

            const char* match = NULL;
            for (const char** p = ports; *p; p++) {
                const char* colon = strrchr(*p, ':');
                if (!colon) continue;
                if (strcmp(colon + 1, suffix) != 0) continue;
                match = *p;
                break;
            }
            if (!match) continue;

            // jack_connect takes source first, then destination.
            const char* local_name = jack_port_name(local[ch - 1]);
            const char* src = (flags & JackPortIsOutput) ? match      : local_name;
            const char* dst = (flags & JackPortIsOutput) ? local_name : match;

            int rc = jack_connect(client, src, dst);
            if (rc == 0) {
                JB_LOG_INFO(jb_log_jack(),
                    "auto-wire: %{public}s -> %{public}s", src, dst);
            } else if (rc != EEXIST) {
                JB_LOG_DEFAULT(jb_log_jack(),
                    "auto-wire: jack_connect %{public}s -> %{public}s failed rc=%d",
                    src, dst, rc);
            }
        }

        jack_free(ports);
    }

    bool isActive, isSyncMode, isVerbose;
    bool showmsg;
    uint64_t lastHostTime;
    double HostTicksPerFrame;
    int64_t ncalls;
    char** nameAin;
    char** nameAout;

    int sendToCoreAudio(float** in,int nframes) {
        unsigned int offset = FrameNumber % FramesPerBuffer;
        // FIXME: should be consider buffer overwrapping
        for(int i=0; i<nframes; i++) {
            for(int j=0; j<NUM_INPUT_STREAMS; j++) {
                *(buf_down[j]+(offset+i)*2) = in[j*2][i];
                *(buf_down[j]+(offset+i)*2+1) = in[j*2+1][i];
            }
        }
        return nframes;
    }

    int receiveFromCoreAudio(float** out, int nframes) {
        //unsigned int offset = FrameNumber % FramesPerBuffer;
        unsigned int offset = (FrameNumber - nframes) % FramesPerBuffer;
        // FIXME: should be consider buffer overwrapping
        for(int i=0; i<nframes; i++) {
            for(int j=0; j<NUM_OUTPUT_STREAMS; j++) {
                out[j*2][i] = *(buf_up[j]+(offset+i)*2);
                out[j*2+1][i] = *(buf_up[j]+(offset+i)*2+1);
                *(buf_up[j]+(offset+i)*2) = 0.0f;
                *(buf_up[j]+(offset+i)*2+1) = 0.0f;
            }
        }
        return nframes;
    }

    void config_audio_ports() {
        nameAin = (char**)malloc(sizeof(char*)*(NUM_INPUT_CHANNELS+1));
        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            nameAin[i] = (char*)malloc(256);
            snprintf(nameAin[i], 256, "input_%d", i+1);
        }
        nameAin[NUM_INPUT_CHANNELS] = nullptr;

        nameAout = (char**)malloc(sizeof(char*)*(NUM_OUTPUT_CHANNELS+1));
        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            nameAout[i] = (char*)malloc(256);
            snprintf(nameAout[i], 256, "output_%d", i+1);
        }
        nameAout[NUM_OUTPUT_CHANNELS] = nullptr;
    }

#ifdef _WITH_MIDI_BRIDGE_
    RtMidiOut  **midiout;
    RtMidiIn   **midiin;
    int nOutPorts, nInPorts;
    char** nameMin;
    char** nameMout;

    int get_num_ports(unsigned long flags) {
        int num;
        const char** ports = jack_get_ports(client, "system", ".*raw midi", flags);
        if (!ports) {
            return 0;
        }

        for(num=0;*ports != NULL; ports++,num++) {
#if 0 // For DEBUG
            jack_port_t* p = jack_port_by_name(client, *ports);
            std::cout << ";" << *ports << ";" << jack_port_short_name(p) << ";" << jack_port_type(p) << std::endl;
#endif
        }
        return num;
    }

    void create_midi_ports(const char* name, int num_Min, int num_Mout) {
        char buf[256];

        // create bridge from Jack to CoreMIDI
        nOutPorts = (num_Mout < 0) ? get_num_ports(JackPortIsOutput) : num_Mout;
        midiout = (RtMidiOut**)malloc(sizeof(RtMidiOut*)*nOutPorts);
        nameMin = (char**)malloc(sizeof(char*)*(nOutPorts+1));

        for(int n=0; n<nOutPorts; n++) {
            try {
                midiout[n] = new RtMidiOut(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiout[n]->openVirtualPort(buf);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMin[n] = (char*)malloc(256);
            snprintf(nameMin[n], 256, "event_in_%d", n+1);
        }
        nameMin[nOutPorts] = NULL;

        // create bridge from CoreMIDI to Jack
        nInPorts = (num_Min < 0) ? get_num_ports(JackPortIsInput) : num_Min;
        midiin = (RtMidiIn**)malloc(sizeof(RtMidiIn*)*nInPorts);
        nameMout = (char**)malloc(sizeof(char*)*(nInPorts+1));

        for(int n=0; n<nInPorts; n++) {
            try {
                midiin[n] = new RtMidiIn(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiin[n]->openVirtualPort(buf);
                midiin[n]->ignoreTypes(false, false, false);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMout[n] = (char*)malloc(256);
            snprintf(nameMout[n], 256, "event_out_%d", n+1);
        }
        nameMout[nInPorts] = NULL;
    }

    void release_midi_ports() {
        // release bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            delete midiout[n];
            free(nameMin[n]);
        }
        free(midiout);
        free(nameMin);

        // release bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            delete midiin[n];
            free(nameMout[n]);
        }
        free(midiin);
        free(nameMout);
    }

    void process_midi_message(jack_nframes_t nframes) {
        void *min, *mout;
        int count;
        jack_midi_event_t event;
        std::vector< unsigned char > message;
        jack_midi_data_t* buf;

        // process bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            min = jack_port_get_buffer(midiIn[n], nframes);
            count = jack_midi_get_event_count(min);
            for(int i=0; i<count; i++) {
                jack_midi_event_get(&event, min, i);
                message.clear();
                for (int j=0; j<event.size; j++) {
                    message.push_back(event.buffer[j]);
                }
                if (message.size() > 0) {
                    midiout[n]->sendMessage(&message);
                }
            }
        }

        // process bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            mout = jack_port_get_buffer(midiOut[n], nframes);
            jack_midi_clear_buffer(mout);
            midiin[n]->getMessage(&message);
            while(message.size() > 0) {
                buf = jack_midi_event_reserve(mout, 0, message.size());
                if (buf != NULL) {
                    for(int i=0; i<message.size(); i++) {
                        buf[i] = message[i];
                    }
                } else {
                    JB_LOG_ERR(jb_log_jack(), "jack_midi_event_reserve failed");
                }
                midiin[n]->getMessage(&message);
            }
        }
    }
#endif // _WITH_MIDI_BRIDGE_

    void check_progress() {
#if 0
        if (isVerbose && ((ncalls++) % 500) == 0) {
            printf("JackBridge#%d: FRAME %llu : Write0: %llu Read0: %llu Write1: %llu Read0: %llu\n",
                 instance, FrameNumber,
                 shmWriteFrameNumber[0]->load(std::memory_order_acquire),
                 shmReadFrameNumber[0]->load(std::memory_order_acquire),
                 shmWriteFrameNumber[1]->load(std::memory_order_acquire),
                 shmReadFrameNumber[1]->load(std::memory_order_acquire));
        }
#endif

        int diff = shmWriteFrameNumber[0]->load(std::memory_order_acquire) - FrameNumber;
        int interval = (mach_absolute_time() - lastHostTime) / HostTicksPerFrame;
        if (showmsg) {
            if ((diff >= (STRBUFNUM/2))||(interval >= BufSize*2))  {
                if (isVerbose) {
                    printf("WARNING: miss synchronization detected at FRAME %llu (diff=%d, interval=%d)\n",
                        FrameNumber, diff, interval);
                    fflush(stdout);
                }
                showmsg = false;
            }
        } else {
            if (diff < (STRBUFNUM/2)) {
                showmsg = true;
            }
        }
        lastHostTime = mach_absolute_time();
    }
};

int
main(int argc, char** argv)
{
    JackBridge* jackBridge[NUM_INSTANCES];
    int ch, num_midiIn=-1, num_midiOut=-1;
    bool vflag=false;

    // Block SIGINT/SIGTERM on every thread so they get delivered exclusively
    // via sigwait() below. JACK threads inherit this mask, so the on_shutdown
    // callback can raise SIGTERM and we'll catch it here for clean teardown.
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    // Capture before any JACK threads spawn so the port-registration callback
    // can pthread_kill us awake.
    g_main_thread = pthread_self();

    while ((ch = getopt(argc, argv, "vi:o:")) != -1) {
        switch (ch) {
            case 'v':
                vflag = true;
                break;
#ifdef _WITH_MIDI_BRIDGE_
            case 'i':
                num_midiIn = atoi(optarg);
                if (num_midiIn > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Inputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;

            case 'o':
                num_midiOut = atoi(optarg);
                if (num_midiOut > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Outputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;
#endif
             default:
                fprintf(stderr, "Usage: %s [-v] [-i <# of MIDI-In>] [-o <# of MIDI-Out>]\n", argv[0]);
                return -1;
        }
    }

    // Create instances of jack client
    jackBridge[0] = new JackBridge("JackBridge #1", 0, num_midiIn, num_midiOut);
    if (vflag) {
        jackBridge[0]->setVerbose(vflag);
    }
    //jackBridge[1] = new JackBridge("JackBridge #2", 1);

    // activate gateway from/to jack ports
    jackBridge[0]->activate();
    //jackBridge[1]->activate();

    // After activation, pick up any slave ports that registered before us.
    // Slaves that connect later are picked up by the port-registration callback.
    jackBridge[0]->auto_wire();

    // Event loop. SIGUSR1 = slave ports changed, run auto_wire on the main
    // thread (legal context for jack_connect). SIGINT/SIGTERM = teardown.
    int sig = 0;
    while (true) {
        sigwait(&sigset, &sig);
        if (sig == SIGUSR1) {
            if (g_wire_dirty.exchange(false, std::memory_order_acq_rel)) {
                jackBridge[0]->auto_wire();
            }
            continue;
        }
        break;
    }
    JB_LOG_DEFAULT(jb_log_daemon(), "caught signal %d, shutting down", sig);

    delete jackBridge[0];
    // Don't shm_unlink — the HAL is the shm owner; unlinking would force a
    // recreate cycle on its side. The HAL's staleness watchdog handles our
    // departure via the zeroed heartbeat.
    return 0;
}
