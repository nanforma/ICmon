// discovery.h - finds XM/Sofia cameras on the local network (IPv4 UDP broadcast, port 34569).
//
// Wire format taken from the open-source `dvrip` project (alexshpilkin/dvrip, dvrip/io.py and discover.py;
// read through its documentation and a code excerpt, NOT yet tried on a real network by us):
//   request : the usual 20-byte DVRIP header with message id 1530 and an empty payload, sent to
//             255.255.255.255:34569 from a socket bound to port 34569;
//   reply   : message id 1531, JSON with a "NetWork.NetCommon" object: HostIP (little-endian 32-bit integer,
//             text like "0x0100A8C0"), HostName, MAC, SN, TCPPort, ChannelNum ...
// Only cameras that answer broadcasts on the same subnet are found. Cameras that do not answer (other
// firmware, other subnet, broadcast filtered by the Wi-Fi router) must be added by hand.
// Blocking: call from a worker thread. Honours a CancelToken in <=200 ms slices.
#pragma once
#include <string>
#include <vector>

#include "error.h"

namespace icmon {

struct FoundCamera {
    std::string host;  // address the reply came from
    std::string name, mac, serial;
    int tcp_port = 34567;
    int channels = 0;
};

struct DiscoverOptions {
    int timeout_ms = 2500;
    int bind_port = 34569;  // local port; the reference binds 34569 (replies come back to it)
    int dest_port = 34569;  // port the request is sent to
    std::string destination = "255.255.255.255";
};

std::string make_discover_request();  // the 20-byte request packet

// `sender_ip` is the datagram's source address. Returns false (and sets `why`) for anything that is not a valid reply.
bool parse_discover_reply(const std::string& datagram, const std::string& sender_ip, FoundCamera& out, std::string& why);

// Sends the request and collects replies until the timeout. Duplicates (same host) are merged.
// Throws xm::Error (Io) when the socket cannot be opened/bound/sent, Cancelled when cancelled.
std::vector<FoundCamera> discover(const DiscoverOptions& opt, const xm::CancelToken* cancel = nullptr);

}  // namespace icmon
