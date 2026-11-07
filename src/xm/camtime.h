// camtime.h - camera clock times.
// The camera reports "naive" local times ("2026-10-31 00:52:32", no zone). They are represented here
// as int64 seconds computed with timegm(), i.e. as if the camera clock were UTC. Never feed them to
// localtime()/mktime(): differences and ordering are right, absolute instants are not.
#pragma once
#include <cstdint>
#include <string>

#include "xmdemux.h"

namespace xm {

using CamTime = int64_t;

CamTime to_cam_time(const Time& t);
Time from_cam_time(CamTime t);
uint32_t encode_time(const Time& t);          // inverse of decode_time(): the packed 32-bit field in I-frames
CamTime packed_to_cam_time(uint32_t packed);  // decode_time() + to_cam_time()

// "YYYY-MM-DD HH:MM:SS"; parse throws xm::Error(Protocol) on anything else.
CamTime parse_cam_time(const std::string& s);
std::string format_cam_time(CamTime t);
std::string format_cam_clock(CamTime t);  // "HH:MM:SS"

}  // namespace xm
