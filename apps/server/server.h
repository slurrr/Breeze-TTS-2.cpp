#pragma once

#include <string>

namespace breeze {

struct ServerOptions {
    std::string model;
    std::string host = "127.0.0.1";
    int port = 8080;
    bool use_gpu = true;
    bool webui = false;
    bool verbose = false;
    std::string voices_dir = "voices";
    int ws_port = 0; // 0 puts it on port + 1, negative turns it off
    int chunk_first = 4;
    int chunk_max = 25;
    int split_chars = 600;  // maximum text per model generation; 0 keeps HTTP text in one pass
    int stream_chars = 0;   // WS text target before generation; 0 drains every completed sentence
    bool warmup = true;
    std::string cors; // empty turns cors off, "*" allows any origin, otherwise a comma separated allowlist
};

int run_server(const ServerOptions & opts);

}
