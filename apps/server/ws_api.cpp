#include "ws_api.h"

#include "breeze/audio.h"
#include "breeze/generation.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <thread>

namespace breeze {

// the messages are small flat objects, so pulling one field out beats vendoring a json parser
static std::string json_str(const std::string & msg, const char * key) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t at = msg.find(pat);
    if (at == std::string::npos) return "";
    at = msg.find(':', at + pat.size());
    if (at == std::string::npos) return "";
    at = msg.find('"', at);
    if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + 1; i < msg.size(); i++) {
        const char c = msg[i];
        if (c == '\\' && i + 1 < msg.size()) {
            const char n = msg[++i];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else if (n == 'u') { i += 4; }
            else out += n;
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

static double json_num(const std::string & msg, const char * key, double def) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t at = msg.find(pat);
    if (at == std::string::npos) return def;
    at = msg.find(':', at + pat.size());
    if (at == std::string::npos) return def;
    try {
        return std::stod(msg.substr(at + 1));
    } catch (...) {
        return def;
    }
}

static std::string esc(const std::string & s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char) c < 0x20) continue;
        else o += c;
    }
    return o;
}

static bool sentence_end(const std::string & s, size_t i) {
    const char c = s[i];
    if (c == '.' || c == '!' || c == '?' || c == ';') {
        // a bare dot inside a number or an abbreviation is not the end of anything
        return i + 1 >= s.size() || s[i + 1] == ' ' || s[i + 1] == '\n';
    }
    // the cjk stops carry their own spacing
    static const char * stops[] = { "\xe3\x80\x82", "\xef\xbc\x81", "\xef\xbc\x9f", "\xef\xbc\x9b" };
    for (const char * st : stops)
        if (s.compare(i, 3, st) == 0) return true;
    return false;
}

// Moves coherent text out of buf. stream_chars == 0 is the legacy lowest-latency
// sentence mode. A positive target waits for several sentences, but never lets an
// unpunctuated passage grow beyond the model's safe piece budget. force always drains
// the complete remainder, including text after the final punctuation mark.
static std::vector<std::string> drain(std::string & buf, int budget, int stream_chars, bool force) {
    std::vector<std::string> out;
    size_t cut = force ? buf.size() : 0;
    if (!force) {
        for (size_t i = 0; i < buf.size(); i++)
            if (sentence_end(buf, i)) cut = i + ((unsigned char) buf[i] < 0x80 ? 1 : 3);

        if (stream_chars > 0) {
            if ((int) buf.size() < stream_chars) return out;
            // Do not emit one stale short sentence followed by a large unfinished one.
            // Wait up to the hard piece budget for a useful boundary instead.
            if ((int) cut < stream_chars / 2) cut = 0;
        }

        // Nothing useful finished but the buffer is at the model-safe limit, so
        // preserve streaming by breaking on the latest space.
        if (!cut && (int) buf.size() >= budget) {
            const size_t sp = buf.rfind(' ');
            if (sp != std::string::npos) cut = sp + 1;
        }
    }
    // flush and end mean all of it, otherwise a tail after the last full stop (a closing quote,
    // an unfinished clause) sits in the buffer and gets glued onto the front of the next message
    if (force) cut = buf.size();
    if (!cut) return out;

    std::string ready = buf.substr(0, cut);
    buf.erase(0, cut);
    for (std::string & p : split_text(ready, budget)) {
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

namespace {

// each piece remembers the instruction that was live when it was queued, otherwise a
// direction sent while the queue is deep would land on whatever piece happens to start next
struct Piece {
    std::string text;
    std::string instruction;
};

struct Session {
    GenSession gen;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Piece> queue;
    std::string buffer;
    std::string instruction = "Speak clearly and naturally.";
    int budget = 600;
    int stream_chars = 0;
    bool started = false;
    bool ending = false;
    bool quit = false;
    bool speaking = false;
    std::atomic<bool> cancel{false};
};

} // namespace

static void event(WsConn & c, const std::string & type, const std::string & extra = "") {
    c.send_text("{\"type\":\"" + type + "\"" + (extra.empty() ? "" : "," + extra) + "}");
}

// drains the queue one piece at a time, taking the gpu lock for each so other connections interleave
static void speaker(WsConn & conn, Session & s, std::mutex & gpu) {
    for (;;) {
        Piece piece;
        {
            std::unique_lock<std::mutex> lock(s.mu);
            s.speaking = false;
            s.cv.wait(lock, [&] { return s.quit || !s.queue.empty(); });
            if (s.quit) return;
            piece = std::move(s.queue.front());
            s.queue.pop_front();
            s.speaking = true;
        }
        if (s.cancel) { s.cancel = false; continue; }

        std::unique_lock<std::mutex> hold(gpu, std::try_to_lock);
        if (!hold) {
            event(conn, "queued");
            hold.lock();
        }
        if (s.cancel) { s.cancel = false; continue; }

        {
            std::lock_guard<std::mutex> lock(s.mu);
            s.gen.set_instruction(piece.instruction);
        }
        event(conn, "speaking", "\"text\":\"" + esc(piece.text) + "\"");

        int sent = 0;
        const bool ok = s.gen.speak(piece.text, [&](const float * a, int n) {
            if (s.cancel || !conn.alive()) return false;
            std::vector<uint8_t> pcm = to_pcm16(a, n);
            sent += n;
            return conn.send_binary(pcm.data(), pcm.size());
        });
        hold.unlock();

        if (s.cancel) { event(conn, "cancelled"); s.cancel = false; continue; }
        if (!ok && !conn.alive()) return;

        std::lock_guard<std::mutex> lock(s.mu);
        if (s.queue.empty() && s.ending) {
            s.ending = false;
            event(conn, "done");
        }
    }
}

static void handle_start(WsConn & conn, Session & s, const std::string & msg, BreezeModel & model,
                         MimiCodec & codec, VoiceStore & store, int chunk_first, int chunk_max,
                         int split_chars, int stream_chars) {
    GenRequest g;
    g.instruction = json_str(msg, "instruction");
    if (g.instruction.empty()) g.instruction = "Speak clearly and naturally.";
    g.ref_text = json_str(msg, "ref_text");
    g.cfg_scale = (float) json_num(msg, "cfg_scale", 1.0);
    g.seed = (int) json_num(msg, "seed", 42);
    g.temperature = (float) json_num(msg, "temperature", 0);
    g.top_k = (int) json_num(msg, "top_k", 0);
    g.chunk_first = chunk_first;
    g.chunk_max = chunk_max;

    const std::string requested_vid = json_str(msg, "voice_id");
    const bool use_default_voice = json_num(msg, "use_default_voice", 1) != 0;
    std::string vid;
    if ((!requested_vid.empty() || use_default_voice) &&
        !store.take(requested_vid, g.ref_codes, g.ref_frames, g.ref_text, &vid)) {
        event(conn, "error", "\"message\":\"unknown voice_id\"");
        return;
    }

    std::lock_guard<std::mutex> lock(s.mu);
    s.instruction = g.instruction;
    s.budget = (int) json_num(msg, "split_chars", split_chars);
    // Streaming always needs a hard model-safe limit even when HTTP splitting is disabled.
    if (s.budget <= 0) s.budget = 600;
    s.stream_chars = (int) json_num(msg, "stream_chars", stream_chars);
    if (s.stream_chars < 0) s.stream_chars = 0;
    if (s.stream_chars > s.budget) s.stream_chars = s.budget;
    s.gen.begin(model, codec, g);
    s.started = true;
    s.queue.clear();
    s.buffer.clear();
    s.cancel = false;
    event(conn, "started", "\"voice_id\":\"" + esc(vid) + "\"");
}

void ws_connection(WsConn & conn, BreezeModel & model, MimiCodec & codec, VoiceStore & store,
                   std::mutex & gpu, int chunk_first, int chunk_max, int split_chars,
                   int stream_chars) {
    Session s;
    std::thread worker([&] { speaker(conn, s, gpu); });
    event(conn, "ready", "\"sample_rate\":24000,\"format\":\"s16le\"");

    std::string msg;
    bool binary = false;
    while (conn.recv(msg, binary)) {
        if (binary) continue;
        const std::string type = json_str(msg, "type");

        if (type == "start") {
            handle_start(conn, s, msg, model, codec, store, chunk_first, chunk_max, split_chars,
                         stream_chars);
            continue;
        }
        if (!s.started) {
            event(conn, "error", "\"message\":\"send start first\"");
            continue;
        }
        if (type == "instruction") {
            std::lock_guard<std::mutex> lock(s.mu);
            // applies to everything queued from here on, pieces already waiting keep theirs
            s.instruction = json_str(msg, "instruction");
            event(conn, "instruction_set");
        } else if (type == "text" || type == "flush" || type == "end") {
            bool done_now = false;
            std::unique_lock<std::mutex> lock(s.mu);
            s.buffer += json_str(msg, "text");
            const bool force = type != "text";
            // while there is no clip to clone the opening piece doubles as the reference, and a
            // long one makes the model skip sentences later, so it stays near a normal clip length
            const int budget = s.gen.needs_anchor() && s.queue.empty() ? 200 : s.budget;
            const int stream_target = s.stream_chars > 0 ? std::min(s.stream_chars, budget) : 0;
            for (std::string & p : drain(s.buffer, budget, stream_target, force))
                s.queue.push_back({std::move(p), s.instruction});
            if (type == "end") {
                s.ending = true;
                // The final text may already have drained and finished before `end` arrives.
                // With no queued work the speaker's wait predicate stays false, so it cannot
                // observe `ending` and would never emit the terminal event. Complete here when
                // it is idle; if it is still speaking, the speaker emits done after that piece.
                if (s.queue.empty() && !s.speaking) {
                    s.ending = false;
                    done_now = true;
                }
            }
            lock.unlock();
            s.cv.notify_one();
            if (done_now) event(conn, "done");
        } else if (type == "cancel") {
            std::lock_guard<std::mutex> lock(s.mu);
            s.queue.clear();
            s.buffer.clear();
            s.ending = false;
            // only latch it while something is actually being spoken, otherwise the flag
            // sits true and swallows whatever gets sent next
            s.cancel = s.speaking;
        } else {
            event(conn, "error", "\"message\":\"unknown type\"");
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.quit = true;
        s.cancel = true;
    }
    s.cv.notify_all();
    worker.join();
}

} // namespace breeze
