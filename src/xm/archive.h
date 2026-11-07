// archive.h - recordings stored on the camera's microSD, read over DVRIP (no card removal).
//
// What is verified against a real camera (XM530V200, firmware V5.04.R02) and what is not:
//  * listing: OPFileQuery (msg 1440) answers at most 64 entries per reply; list_files() pages by
//    re-querying from the last BeginTime and removing duplicates.
//  * download: Claim (1424), DownloadStart (1420), data in 1426 packets (32 KiB), an empty 1426 packet
//    ends the data, then DownloadStop. Narrowing StartTime/EndTime inside a file gives a time window.
//  * a window starts and ends mid-frame; decoding can only begin at the first I-frame.
//  * a window across two files (fetch_range) was checked once on a real camera (21:34:50..21:35:10: no
//    visible glitch at the join, audio in sync, no resyncs); one case only.
//  NOT verified: other file tags than [R], the unit
//  of FileLength, recordings of other channels, behaviour of other firmware versions.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "camtime.h"
#include "dvrip.h"
#include "xmdemux.h"

namespace xm {

struct RecordingFile {
    std::string name;       // as reported, e.g. "/idea0/2026-10-31/001/00.50.00-00.55.00[R][@1][0].h264"
    CamTime begin = 0, end = 0;
    uint64_t length_raw = 0;  // FileLength as sent (hex string); unit not verified
    int disk = 0, serial = 0;
};

// All recordings overlapping [begin, end] (camera clock), oldest first, duplicates removed.
// Throws xm::Error (Cancelled / Timeout / Camera ...). A day of footage needs a few queries.
std::vector<RecordingFile> list_files(Dvrip& c, CamTime begin, CamTime end, const CancelToken* cancel = nullptr);

// One OPFileQuery: is there at least one recording in [begin, end]? (used for the calendar marks)
bool has_files(Dvrip& c, CamTime begin, CamTime end, const CancelToken* cancel = nullptr);

// The file that contains time t (begin <= t < end), or nullptr.
const RecordingFile* find_file(const std::vector<RecordingFile>& files, CamTime t);

struct DownloadStats {
    size_t bytes = 0, packets = 0;
    bool stopped_by_sink = false;
};
using DataSink = std::function<bool(const uint8_t*, size_t)>;  // return false to stop the download

// Downloads [start, end] of one file and hands the raw bytes to `sink` as they arrive. The connection
// `c` must be logged in and is used exclusively until the call returns. To cancel from another thread
// set the CancelToken given to Dvrip::connect(); the connection is then unusable and must be closed.
DownloadStats download_window(Dvrip& c, const RecordingFile& f, CamTime start, CamTime end, const DataSink& sink,
                              int idle_timeout_ms = 10000);

struct FetchResult {
    DownloadStats download;
    Stats demux;
};
// download_window() + StreamParser: `on_frame` receives every complete frame (check Frame::after_i
// before decoding video: frames before the first I-frame cannot be decoded).
FetchResult fetch_frames(Dvrip& c, const RecordingFile& f, CamTime start, CamTime end,
                         const std::function<void(const Frame&)>& on_frame, int idle_timeout_ms = 10000);

// A window may cross file boundaries (recordings are 5-minute files). One fresh connection per file;
// each connection is made by `connect` (it must log in, and should carry the CancelToken).
// Windows are clipped to every file; only the first file gets the `lead_secs` run-up (the camera cuts the
// first 32 KiB chunk mid-frame, so the I-frame before the target can be lost: 6 s = two GOPs is safe on
// the observed camera). `on_frame` also gets the index (into the selected files) the frame came from.
// NOT verified on a real camera: that a window starting exactly at a file's begin starts with an I-frame.
using Connector = std::function<std::unique_ptr<Dvrip>()>;
struct RangeResult {
    size_t files_used = 0;
    DownloadStats download;
    size_t i_frames = 0, p_frames = 0, audio_frames = 0, resyncs = 0, skipped_bytes = 0;
    std::vector<std::string> notes;  // gaps between files, etc.
};
RangeResult fetch_range(const Connector& connect, const std::vector<RecordingFile>& files, CamTime start, CamTime end,
                        int lead_secs, const std::function<void(const Frame&, size_t file_index)>& on_frame,
                        int idle_timeout_ms = 10000);

}  // namespace xm
