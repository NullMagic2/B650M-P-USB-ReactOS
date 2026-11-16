// sndtest: list the wave devices and play or record through the ordinary
// Windows multimedia API (winmm), writing everything it sees to a log file.
//
//   sndtest list                          log
//   sndtest sleep <seconds>               log
//   sndtest play <device> <seconds>       log   (device: index, part of its name, or "mapper")
//   sndtest rec  <device> <seconds> file  log
//   sndtest file <wave file>              log   (PlaySound, as the desktop does for its sounds)
//   sndtest mixer                         log   (lists the controls of mixer 0)
//   sndtest volume <0..65535>             log   (sets every volume control of mixer 0, reads it back)
//   sndtest mute <0|1>                    log   (the same for every mute control)
//
// Playback is 16 bit stereo at RATE Hz (environment, default 48000): the sample
// number times 1/48 of a turn on the left and 1/16 on the right, which is
// 1000 Hz and 3000 Hz at 48 kHz.
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *report;

static void say(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(report, format, arguments);
    va_end(arguments);
    fflush(report);
}

static void list(void)
{
    UINT count = waveOutGetNumDevs();
    say("out_devices=%u\n", count);
    for (UINT index = 0; index < count; ++index) {
        WAVEOUTCAPSA caps;
        MMRESULT result = waveOutGetDevCapsA(index, &caps, sizeof(caps));
        say("out[%u] result=%u name=\"%s\" formats=%08lx channels=%u support=%08lx\n", index, result,
            result ? "" : caps.szPname, result ? 0 : caps.dwFormats, result ? 0 : caps.wChannels,
            result ? 0 : caps.dwSupport);
    }
    count = waveInGetNumDevs();
    say("in_devices=%u\n", count);
    for (UINT index = 0; index < count; ++index) {
        WAVEINCAPSA caps;
        MMRESULT result = waveInGetDevCapsA(index, &caps, sizeof(caps));
        say("in[%u] result=%u name=\"%s\" formats=%08lx channels=%u\n", index, result,
            result ? "" : caps.szPname, result ? 0 : caps.dwFormats, result ? 0 : caps.wChannels);
    }
    count = mixerGetNumDevs();
    say("mixer_devices=%u\n", count);
    for (UINT index = 0; index < count; ++index) {
        MIXERCAPSA caps;
        MMRESULT result = mixerGetDevCapsA(index, &caps, sizeof(caps));
        say("mixer[%u] result=%u name=\"%s\" destinations=%lu\n", index, result,
            result ? "" : caps.szPname, result ? 0 : caps.cDestinations);
    }
}

// Walks the controls of every line of mixer 0. kind: 0 lists, 1 sets volumes, 2 sets mutes.
static void mixer(int kind, DWORD value)
{
    HMIXER handle = NULL;
    MMRESULT result = mixerOpen(&handle, 0, 0, 0, MIXER_OBJECTF_MIXER);
    say("mixer_open=%u\n", result);
    if (result) {
        return;
    }
    MIXERCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    mixerGetDevCapsA(0, &caps, sizeof(caps));
    for (DWORD destination = 0; destination < caps.cDestinations; ++destination) {
        MIXERLINEA line;
        memset(&line, 0, sizeof(line));
        line.cbStruct = sizeof(line);
        line.dwDestination = destination;
        result = mixerGetLineInfoA((HMIXEROBJ)handle, &line, MIXER_GETLINEINFOF_DESTINATION);
        DWORD sources = result ? 0 : line.cConnections;
        for (DWORD source = 0; source <= sources; ++source) {
            if (source) {
                memset(&line, 0, sizeof(line));
                line.cbStruct = sizeof(line);
                line.dwDestination = destination;
                line.dwSource = source - 1;
                result = mixerGetLineInfoA((HMIXEROBJ)handle, &line, MIXER_GETLINEINFOF_SOURCE);
            }
            say("line dest=%lu source=%ld result=%u name=\"%s\" channels=%lu controls=%lu\n", destination,
                (long)source - 1, result, result ? "" : line.szName, result ? 0 : line.cChannels,
                result ? 0 : line.cControls);
            if (result || !line.cControls) {
                continue;
            }
            MIXERCONTROLA controls[16];
            MIXERLINECONTROLSA query;
            memset(&query, 0, sizeof(query));
            memset(controls, 0, sizeof(controls));
            query.cbStruct = sizeof(query);
            query.dwLineID = line.dwLineID;
            query.cControls = line.cControls > 16 ? 16 : line.cControls;
            query.cbmxctrl = sizeof(MIXERCONTROLA);
            query.pamxctrl = controls;
            result = mixerGetLineControlsA((HMIXEROBJ)handle, &query, MIXER_GETLINECONTROLSF_ALL);
            if (result) {
                say("  controls result=%u\n", result);
                continue;
            }
            for (DWORD index = 0; index < query.cControls; ++index) {
                MIXERCONTROLA *control = &controls[index];
                int volume = control->dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME;
                int mute = control->dwControlType == MIXERCONTROL_CONTROLTYPE_MUTE;
                MIXERCONTROLDETAILS_UNSIGNED values[2];
                MIXERCONTROLDETAILS details;
                memset(&details, 0, sizeof(details));
                memset(values, 0, sizeof(values));
                details.cbStruct = sizeof(details);
                details.dwControlID = control->dwControlID;
                details.cChannels = 1;
                details.cbDetails = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
                details.paDetails = values;
                MMRESULT set = 0xffff;
                if ((kind == 1 && volume) || (kind == 2 && mute)) {
                    values[0].dwValue = value;
                    set = mixerSetControlDetails((HMIXEROBJ)handle, &details, MIXER_SETCONTROLDETAILSF_VALUE);
                    values[0].dwValue = 0xdead;
                }
                MMRESULT get = (volume || mute)
                                   ? mixerGetControlDetailsA((HMIXEROBJ)handle, &details, MIXER_GETCONTROLDETAILSF_VALUE)
                                   : 0xffff;
                say("  control \"%s\" type=%08lx set=%u get=%u value=%lu\n", control->szName, control->dwControlType,
                    set, get, values[0].dwValue);
            }
        }
    }
    mixerClose(handle);
}

static int find_out(const char *wanted)
{
    char *end;
    long index = strtol(wanted, &end, 10);
    if (!*end) {
        return (int)index;
    }
    if (!strcmp(wanted, "mapper")) {
        return -1;
    }
    UINT count = waveOutGetNumDevs();
    for (UINT device = 0; device < count; ++device) {
        WAVEOUTCAPSA caps;
        if (!waveOutGetDevCapsA(device, &caps, sizeof(caps)) && strstr(caps.szPname, wanted)) {
            return (int)device;
        }
    }
    return -2;
}

static int find_in(const char *wanted)
{
    char *end;
    long index = strtol(wanted, &end, 10);
    if (!*end) {
        return (int)index;
    }
    UINT count = waveInGetNumDevs();
    for (UINT device = 0; device < count; ++device) {
        WAVEINCAPSA caps;
        if (!waveInGetDevCapsA(device, &caps, sizeof(caps)) && strstr(caps.szPname, wanted)) {
            return (int)device;
        }
    }
    return -2;
}

static unsigned rate = 48000;

static WAVEFORMATEX format(void)
{
    WAVEFORMATEX wave;
    memset(&wave, 0, sizeof(wave));
    wave.wFormatTag = WAVE_FORMAT_PCM;
    wave.nChannels = 2;
    wave.nSamplesPerSec = rate;
    wave.wBitsPerSample = 16;
    wave.nBlockAlign = 4;
    wave.nAvgBytesPerSec = rate * 4;
    return wave;
}

#define BUFFERS 8
#define FRAMES 4800

static int play(const char *wanted, unsigned seconds)
{
    int device = find_out(wanted);
    say("play device=%d seconds=%u rate=%u\n", device, seconds, rate);
    if (device == -2) {
        say("play_result=no_such_device\n");
        return 2;
    }
    WAVEFORMATEX wave = format();
    HANDLE done = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT out = NULL;
    MMRESULT result = waveOutOpen(&out, (UINT)device, &wave, (DWORD_PTR)done, 0, CALLBACK_EVENT);
    say("open=%u\n", result);
    if (result) {
        say("play_result=open_failed\n");
        return 3;
    }
    static short data[BUFFERS][FRAMES * 2];
    WAVEHDR header[BUFFERS];
    memset(header, 0, sizeof(header));
    unsigned total = seconds * rate / FRAMES, written = 0, failed = 0;
    unsigned long frame = 0;
    DWORD start = GetTickCount();
    for (unsigned index = 0; index < BUFFERS; ++index) {
        header[index].lpData = (LPSTR)data[index];
        header[index].dwBufferLength = sizeof(data[index]);
        header[index].dwFlags = WHDR_DONE;
        result = waveOutPrepareHeader(out, &header[index], sizeof(WAVEHDR));
        if (result) {
            say("prepare[%u]=%u\n", index, result);
        }
        header[index].dwFlags |= WHDR_DONE;
    }
    while (written < total) {
        int queued = 0;
        for (unsigned index = 0; index < BUFFERS && written < total; ++index) {
            if (!(header[index].dwFlags & WHDR_DONE)) {
                continue;
            }
            for (unsigned sample = 0; sample < FRAMES; ++sample, ++frame) {
                data[index][sample * 2] = (short)(12000.0 * sin(frame * 2.0 * 3.14159265358979 / 48.0));
                data[index][sample * 2 + 1] = (short)(12000.0 * sin(frame * 2.0 * 3.14159265358979 / 16.0));
            }
            header[index].dwFlags &= ~WHDR_DONE;
            result = waveOutWrite(out, &header[index], sizeof(WAVEHDR));
            if (result) {
                ++failed;
                say("write[%u]=%u\n", written, result);
                header[index].dwFlags |= WHDR_DONE;
            }
            ++written;
            queued = 1;
        }
        if (!queued && WaitForSingleObject(done, 5000) == WAIT_TIMEOUT) {
            say("write_stalled after=%u ms=%lu\n", written, GetTickCount() - start);
            break;
        }
        if (GetTickCount() - start > seconds * 1000 + 15000) {
            say("gave_up ms=%lu\n", GetTickCount() - start);
            break;
        }
    }
    unsigned pending = 0;
    for (unsigned wait = 0; wait < 100; ++wait) {
        pending = 0;
        for (unsigned index = 0; index < BUFFERS; ++index) {
            pending += !(header[index].dwFlags & WHDR_DONE);
        }
        if (!pending) {
            break;
        }
        WaitForSingleObject(done, 100);
    }
    DWORD elapsed = GetTickCount() - start;
    MMTIME position;
    memset(&position, 0, sizeof(position));
    position.wType = TIME_BYTES;
    result = waveOutGetPosition(out, &position, sizeof(position));
    say("position_result=%u type=%u value=%lu\n", result, position.wType, position.u.cb);
    say("buffers_written=%u buffers_failed=%u buffers_pending=%u elapsed_ms=%lu\n", written, failed, pending,
        elapsed);
    result = waveOutReset(out);
    say("reset=%u\n", result);
    for (unsigned index = 0; index < BUFFERS; ++index) {
        waveOutUnprepareHeader(out, &header[index], sizeof(WAVEHDR));
    }
    result = waveOutClose(out);
    say("close=%u\n", result);
    say("play_result=%s\n", written == total && !failed && !pending ? "ok" : "incomplete");
    return 0;
}

static int record(const char *wanted, unsigned seconds, const char *path)
{
    int device = find_in(wanted);
    say("rec device=%d seconds=%u\n", device, seconds);
    if (device == -2) {
        say("rec_result=no_such_device\n");
        return 2;
    }
    WAVEFORMATEX wave = format();
    HANDLE done = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEIN in = NULL;
    MMRESULT result = waveInOpen(&in, (UINT)device, &wave, (DWORD_PTR)done, 0, CALLBACK_EVENT);
    say("open=%u\n", result);
    if (result) {
        say("rec_result=open_failed\n");
        return 3;
    }
    FILE *file = fopen(path, "wb");
    static short data[BUFFERS][FRAMES * 2];
    WAVEHDR header[BUFFERS];
    memset(header, 0, sizeof(header));
    for (unsigned index = 0; index < BUFFERS; ++index) {
        header[index].lpData = (LPSTR)data[index];
        header[index].dwBufferLength = sizeof(data[index]);
        result = waveInPrepareHeader(in, &header[index], sizeof(WAVEHDR));
        if (!result) {
            result = waveInAddBuffer(in, &header[index], sizeof(WAVEHDR));
        }
        if (result) {
            say("add[%u]=%u\n", index, result);
        }
    }
    DWORD start = GetTickCount();
    result = waveInStart(in);
    say("start=%u\n", result);
    unsigned long bytes = 0;
    unsigned buffers = 0;
    while (GetTickCount() - start < seconds * 1000) {
        WaitForSingleObject(done, 200);
        for (unsigned index = 0; index < BUFFERS; ++index) {
            if (!(header[index].dwFlags & WHDR_DONE)) {
                continue;
            }
            if (file) {
                fwrite(header[index].lpData, 1, header[index].dwBytesRecorded, file);
            }
            bytes += header[index].dwBytesRecorded;
            ++buffers;
            header[index].dwFlags &= ~WHDR_DONE;
            header[index].dwBytesRecorded = 0;
            result = waveInAddBuffer(in, &header[index], sizeof(WAVEHDR));
            if (result) {
                say("readd[%u]=%u\n", index, result);
            }
        }
    }
    DWORD elapsed = GetTickCount() - start;
    say("buffers=%u bytes=%lu elapsed_ms=%lu\n", buffers, bytes, elapsed);
    say("stop=%u\n", waveInStop(in));
    say("reset=%u\n", waveInReset(in));
    for (unsigned index = 0; index < BUFFERS; ++index) {
        waveInUnprepareHeader(in, &header[index], sizeof(WAVEHDR));
    }
    say("close=%u\n", waveInClose(in));
    if (file) {
        fclose(file);
    }
    say("rec_result=%s\n", bytes ? "ok" : "no_data");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 1;
    }
    const char *log = argv[argc - 1];
    report = fopen(log, "a");
    if (!report) {
        return 1;
    }
    if (getenv("RATE")) {
        rate = (unsigned)atoi(getenv("RATE"));
    }
    say("--- %s\n", argv[1]);
    int result = 0;
    if (!strcmp(argv[1], "list")) {
        list();
    } else if (!strcmp(argv[1], "sleep") && argc == 4) {
        Sleep((DWORD)atoi(argv[2]) * 1000);
    } else if (!strcmp(argv[1], "mixer")) {
        mixer(0, 0);
    } else if (!strcmp(argv[1], "volume") && argc == 4) {
        mixer(1, (DWORD)atoi(argv[2]));
    } else if (!strcmp(argv[1], "mute") && argc == 4) {
        mixer(2, (DWORD)atoi(argv[2]));
    } else if (!strcmp(argv[1], "file") && argc == 4) {
        DWORD start = GetTickCount();
        BOOL played = PlaySoundA(argv[2], NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
        say("file=%s played=%d elapsed_ms=%lu\n", argv[2], played, GetTickCount() - start);
    } else if (!strcmp(argv[1], "play") && argc == 5) {
        list();
        result = play(argv[2], (unsigned)atoi(argv[3]));
    } else if (!strcmp(argv[1], "rec") && argc == 6) {
        list();
        result = record(argv[2], (unsigned)atoi(argv[3]), argv[4]);
    } else {
        say("bad arguments\n");
        result = 1;
    }
    say("--- end %d\n", result);
    fclose(report);
    return result;
}
