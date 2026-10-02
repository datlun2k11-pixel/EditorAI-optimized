// ── Real token streaming: runtime-bound libcurl + per-provider SSE decode ────
// See stream.hpp for the design rationale (why we bind curl at runtime rather
// than linking a second HTTP stack).

#include "stream.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>

#ifdef GEODE_IS_WINDOWS
    #include <Geode/platform/windows.hpp>
#else
    #include <dlfcn.h>
#endif

using namespace geode::prelude;

namespace eaistream {

// ── The slice of libcurl we need ─────────────────────────────────────────────
// Declared locally so no curl headers are required at build time. The ABI of
// these entry points has been stable since curl 7.x.
namespace curl {

using CURL  = void;
using SLIST = void;

// Option ids (curl.h): base + index. Hard-coded because they are part of
// curl's public ABI and never change.
constexpr long OPT_LONG   = 0;
constexpr long OPT_OBJECT = 10000;
constexpr long OPT_FUNC   = 20000;

constexpr long O_URL             = OPT_OBJECT + 2;
constexpr long O_WRITEFUNCTION   = OPT_FUNC   + 11;
constexpr long O_WRITEDATA       = OPT_OBJECT + 1;    // == CURLOPT_FILE
constexpr long O_POSTFIELDS      = OPT_OBJECT + 15;
constexpr long O_USERAGENT       = OPT_OBJECT + 18;
constexpr long O_HTTPHEADER      = OPT_OBJECT + 23;
constexpr long O_NOPROGRESS      = OPT_LONG   + 43;
constexpr long O_POST            = OPT_LONG   + 47;
constexpr long O_FOLLOWLOCATION  = OPT_LONG   + 52;
constexpr long O_POSTFIELDSIZE   = OPT_LONG   + 60;
constexpr long O_TIMEOUT         = OPT_LONG   + 13;
constexpr long O_LOW_SPEED_LIMIT = OPT_LONG   + 19;
constexpr long O_LOW_SPEED_TIME  = OPT_LONG   + 20;
constexpr long O_CONNECTTIMEOUT  = OPT_LONG   + 78;
constexpr long O_NOSIGNAL        = OPT_LONG   + 99;
constexpr long O_ACCEPT_ENCODING = OPT_OBJECT + 102;
constexpr long O_HTTP_VERSION    = OPT_LONG   + 84;
constexpr long HTTP_VERSION_1_1  = 2;          // CURL_HTTP_VERSION_1_1
constexpr long O_XFERINFOFUNCTION= OPT_FUNC   + 219;
constexpr long O_XFERINFODATA    = OPT_OBJECT + 57;   // == CURLOPT_PROGRESSDATA
constexpr long O_TCP_KEEPALIVE   = OPT_LONG   + 213;

constexpr long INFO_LONG          = 0x200000;
constexpr long INFO_RESPONSE_CODE = INFO_LONG + 2;

using easy_init_t    = CURL* (*)();
using easy_setopt_t  = int   (*)(CURL*, long, ...);
using easy_perform_t = int   (*)(CURL*);
using easy_getinfo_t = int   (*)(CURL*, long, ...);
using easy_cleanup_t = void  (*)(CURL*);
using easy_strerror_t= const char* (*)(int);
using slist_append_t = SLIST* (*)(SLIST*, const char*);
using slist_free_t   = void   (*)(SLIST*);

struct Api {
    easy_init_t     init     = nullptr;
    easy_setopt_t   setopt   = nullptr;
    easy_perform_t  perform  = nullptr;
    easy_getinfo_t  getinfo  = nullptr;
    easy_cleanup_t  cleanup  = nullptr;
    easy_strerror_t strerror = nullptr;
    slist_append_t  slistAdd = nullptr;
    slist_free_t    slistFree= nullptr;
    bool        ok = false;
    std::string why;      // why not, when !ok
};

// Resolve once. On Windows the game's own libcurl.dll is already in the
// process (libExtensions.dll imports it), so GetModuleHandleA finds it without
// us loading anything new.
const Api& api() {
    static Api a = [] {
        Api out;
        auto resolve = [&](const char* name) -> void* {
#ifdef GEODE_IS_WINDOWS
            static HMODULE mod = [] {
                HMODULE m = GetModuleHandleA("libcurl.dll");
                if (!m) m = GetModuleHandleA("libcurl-x64.dll");
                if (!m) m = LoadLibraryA("libcurl.dll");
                return m;
            }();
            if (!mod) return nullptr;
            return (void*)GetProcAddress(mod, name);
#else
            // Already-loaded image first (curl is statically linked into the
            // loader on some platforms, exported on others).
            if (void* s = dlsym(RTLD_DEFAULT, name)) return s;
            return nullptr;
#endif
        };

        out.init      = (easy_init_t)    resolve("curl_easy_init");
        out.setopt    = (easy_setopt_t)  resolve("curl_easy_setopt");
        out.perform   = (easy_perform_t) resolve("curl_easy_perform");
        out.getinfo   = (easy_getinfo_t) resolve("curl_easy_getinfo");
        out.cleanup   = (easy_cleanup_t) resolve("curl_easy_cleanup");
        out.strerror  = (easy_strerror_t)resolve("curl_easy_strerror");
        out.slistAdd  = (slist_append_t) resolve("curl_slist_append");
        out.slistFree = (slist_free_t)   resolve("curl_slist_free_all");

        out.ok = out.init && out.setopt && out.perform && out.getinfo
              && out.cleanup && out.slistAdd && out.slistFree;
        if (!out.ok)
            out.why = "libcurl entry points could not be resolved in this "
                      "process — streaming is unavailable on this platform "
                      "build; requests use the normal (non-streamed) path.";
        else
            log::info("Streaming: libcurl bound at runtime");
        return out;
    }();
    return a;
}

} // namespace curl

// ── Sink ─────────────────────────────────────────────────────────────────────
bool Sink::drain(std::string& outText, std::string& outThinking) {
    std::lock_guard lock(m_mu);
    bool any = !m_text.empty() || !m_thinking.empty();
    if (!m_text.empty())     { outText     += m_text;     m_text.clear(); }
    if (!m_thinking.empty()) { outThinking += m_thinking; m_thinking.clear(); }
    return any;
}

bool Sink::finished(Result& out) {
    std::lock_guard lock(m_mu);
    if (!m_done) return false;
    out = m_result;
    return true;
}

void Sink::pushText(std::string_view s) {
    if (s.empty()) return;
    std::lock_guard lock(m_mu);
    m_text.append(s);
}

void Sink::pushThinking(std::string_view s) {
    if (s.empty()) return;
    std::lock_guard lock(m_mu);
    m_thinking.append(s);
}

void Sink::publish(Result r) {
    std::lock_guard lock(m_mu);
    m_result = std::move(r);
    m_done   = true;
}

bool available() { return curl::api().ok; }
std::string unavailableReason() { return curl::api().ok ? "" : curl::api().why; }

bool supportsStreaming(const std::string& provider) {
    return provider == "openai"      || provider == "ministral"  ||
           provider == "huggingface" || provider == "openrouter" ||
           provider == "deepseek"    || provider == "groq"       ||
           provider == "lm-studio"   || provider == "llama-cpp"  ||
           provider == "custom"      || provider == "claude"     ||
           provider == "gemini"      || provider == "ollama";
}

std::string streamUrlFor(const std::string& provider, const std::string& url) {
    if (provider != "gemini") return url;
    // Gemini streams from a different method, and needs alt=sse to get real
    // SSE framing instead of a growing JSON array.
    std::string out = url;
    auto pos = out.find(":generateContent");
    if (pos != std::string::npos)
        out.replace(pos, std::strlen(":generateContent"), ":streamGenerateContent");
    out += (out.find('?') == std::string::npos) ? "?alt=sse" : "&alt=sse";
    return out;
}

// ── Decoders ────────────────────────────────────────────────────────────────
// One decoder instance per transfer. Feed it raw bytes; it finds event
// boundaries and pushes decoded deltas into the sink.
namespace {

// Accumulates tool-call fragments by index (OpenAI streams them piecewise).
struct ToolAcc {
    std::vector<ToolCallAcc> byIndex;
    ToolCallAcc& at(size_t i) {
        if (byIndex.size() <= i) byIndex.resize(i + 1);
        return byIndex[i];
    }
};

// Common helpers over matjson without ever inserting into a const value.
std::string strOr(const matjson::Value& v, const char* key) {
    if (!v.isObject() || !v.contains(key)) return "";
    auto r = v[key].asString();
    return r ? r.unwrap() : "";
}

class Decoder {
public:
    Decoder(std::string provider, Sink& sink)
        : m_provider(std::move(provider)), m_sink(sink) {}

    void feed(const char* data, size_t n) {
        m_buf.append(data, n);
        if (m_provider == "ollama") { drainNdjson(); return; }
        drainSse();
    }

    // Flush whatever is left (last event may lack a trailing blank line).
    void finish() {
        if (m_provider == "ollama") drainNdjson(true);
        else                        drainSse(true);
        // Held-back partial <think> tail: it never resolved into a tag, so it
        // was literal text after all.
        if (!m_pending.empty()) {
            std::string rest = m_pending;
            m_pending.clear();
            emitChannel(rest);
        }
    }

    void fill(Result& r) {
        r.text         = m_text;
        r.thinking     = m_thinking;
        r.finishReason = m_finish;
        r.providerError= m_error;
        for (auto& tc : m_tools.byIndex)
            if (!tc.name.empty()) r.toolCalls.push_back(tc);
    }

private:
    // ── SSE framing: events separated by a blank line, payload on "data:" ──
    void drainSse(bool flush = false) {
        size_t pos = 0;
        for (;;) {
            size_t sep = m_buf.find("\n\n", pos);
            size_t evEnd, next;
            if (sep != std::string::npos) { evEnd = sep; next = sep + 2; }
            else {
                // Also accept CRLF pairs.
                size_t sep2 = m_buf.find("\r\n\r\n", pos);
                if (sep2 != std::string::npos) { evEnd = sep2; next = sep2 + 4; }
                else if (flush && pos < m_buf.size()) { evEnd = m_buf.size(); next = m_buf.size(); }
                else break;
            }
            handleEvent(std::string_view(m_buf.data() + pos, evEnd - pos));
            pos = next;
            if (pos >= m_buf.size()) break;
        }
        if (pos > 0) m_buf.erase(0, pos);
    }

    void handleEvent(std::string_view ev) {
        // Concatenate every data: line in the event (SSE allows several).
        std::string payload;
        size_t line = 0;
        while (line < ev.size()) {
            size_t eol = ev.find('\n', line);
            if (eol == std::string_view::npos) eol = ev.size();
            std::string_view l = ev.substr(line, eol - line);
            line = eol + 1;
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            if (l.rfind("data:", 0) != 0) continue;      // ignore event:/id:/:
            l.remove_prefix(5);
            while (!l.empty() && l.front() == ' ') l.remove_prefix(1);
            payload.append(l);
        }
        if (payload.empty()) return;
        if (payload == "[DONE]") return;                 // OpenAI terminator
        auto parsed = matjson::parse(payload);
        if (!parsed) return;                             // partial/keepalive
        dispatch(parsed.unwrap());
    }

    // ── Ollama: newline-delimited JSON, no "data:" prefix ─────────────────
    void drainNdjson(bool flush = false) {
        size_t pos = 0;
        for (;;) {
            size_t eol = m_buf.find('\n', pos);
            if (eol == std::string::npos) {
                if (!(flush && pos < m_buf.size())) break;
                eol = m_buf.size();
            }
            std::string_view line(m_buf.data() + pos, eol - pos);
            pos = std::min(eol + 1, m_buf.size());
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty()) {
                auto parsed = matjson::parse(line);
                if (parsed) dispatch(parsed.unwrap());
            }
            if (pos >= m_buf.size()) break;
        }
        if (pos > 0) m_buf.erase(0, pos);
    }

    void dispatch(const matjson::Value& j) {
        if (m_provider == "claude")      { claude(j); return; }
        if (m_provider == "gemini")      { gemini(j); return; }
        if (m_provider == "ollama")      { ollama(j); return; }
        openaiCompat(j);
    }

    void addText(const std::string& s) {
        if (s.empty()) return;
        // Local reasoning models (DeepSeek-R1 distills, QwQ, Qwen3...) don't
        // have a separate reasoning channel — they wrap their chain of thought
        // in <think>...</think> inside the normal content stream. Route it to
        // the thinking channel AS IT ARRIVES so the UI shows it as thinking
        // instead of dumping raw tags into the answer. Tags can be split across
        // chunks, so the partial-tag tail is held back until it resolves.
        m_pending += s;
        for (;;) {
            static constexpr std::string_view OPEN  = "<think>";
            static constexpr std::string_view OPEN2 = "<thinking>";
            static constexpr std::string_view CLOSE = "</think>";
            static constexpr std::string_view CLOSE2= "</thinking>";
            std::string_view want  = m_inThink ? CLOSE : OPEN;
            std::string_view want2 = m_inThink ? CLOSE2 : OPEN2;
            size_t hit  = m_pending.find(want);
            size_t hit2 = m_pending.find(want2);
            size_t at   = std::min(hit, hit2);
            std::string_view tag = (hit <= hit2) ? want : want2;
            if (at == std::string::npos) {
                // No complete tag. Emit everything except a possible partial
                // tag at the tail ('<', '</think' and so on).
                size_t hold = 0;
                size_t lt = m_pending.rfind('<');
                if (lt != std::string::npos) {
                    std::string_view tail(m_pending.data() + lt, m_pending.size() - lt);
                    // Could this tail still grow into one of our tags?
                    auto prefixOf = [&](std::string_view t) {
                        return tail.size() < t.size() &&
                               t.compare(0, tail.size(), tail) == 0;
                    };
                    if (prefixOf(want) || prefixOf(want2)) hold = m_pending.size() - lt;
                }
                if (m_pending.size() > hold) {
                    std::string emit = m_pending.substr(0, m_pending.size() - hold);
                    emitChannel(emit);
                    m_pending.erase(0, emit.size());
                }
                return;
            }
            if (at > 0) emitChannel(m_pending.substr(0, at));
            m_pending.erase(0, at + tag.size());
            m_inThink = !m_inThink;
        }
    }

    void emitChannel(const std::string& s) {
        if (s.empty()) return;
        if (m_inThink) { m_thinking += s; m_sink.pushThinking(s); }
        else           { m_text     += s; m_sink.pushText(s); }
    }

    void addThinking(const std::string& s) {
        if (s.empty()) return;
        m_thinking += s;
        m_sink.pushThinking(s);
    }

    // ── OpenAI-compatible: choices[0].delta ──────────────────────────────
    void openaiCompat(const matjson::Value& j) {
        if (j.isObject() && j.contains("error")) {
            const auto e = j["error"];
            m_error = e.isObject() ? strOr(e, "message")
                                   : (e.asString() ? e.asString().unwrap() : "");
            return;
        }
        if (!j.isObject() || !j.contains("choices") || !j["choices"].isArray()
            || j["choices"].size() == 0) return;
        const auto ch = j["choices"][0];
        if (!ch.isObject()) return;
        if (auto fr = strOr(ch, "finish_reason"); !fr.empty()) m_finish = fr;
        if (!ch.contains("delta")) return;
        const auto d = ch["delta"];
        if (!d.isObject()) return;

        addText(strOr(d, "content"));
        // Reasoning models: DeepSeek "reasoning_content", OpenRouter "reasoning".
        addThinking(strOr(d, "reasoning_content"));
        addThinking(strOr(d, "reasoning"));

        if (!d.contains("tool_calls") || !d["tool_calls"].isArray()) return;
        const auto tcs = d["tool_calls"];
        for (size_t i = 0; i < tcs.size(); ++i) {
            const auto tc = tcs[i];
            if (!tc.isObject()) continue;
            size_t idx = 0;
            if (tc.contains("index")) {
                if (auto n = tc["index"].asInt()) idx = (size_t)std::max<int64_t>(0, n.unwrap());
            }
            auto& acc = m_tools.at(idx);
            if (auto id = strOr(tc, "id"); !id.empty()) acc.id = id;
            if (!tc.contains("function")) continue;
            const auto fn = tc["function"];
            if (!fn.isObject()) continue;
            if (auto nm = strOr(fn, "name"); !nm.empty()) acc.name = nm;
            acc.argsJson += strOr(fn, "arguments");
        }
    }

    // ── Claude: typed events (content_block_delta etc.) ───────────────────
    void claude(const matjson::Value& j) {
        std::string type = strOr(j, "type");
        if (type == "error") {
            if (j.contains("error")) m_error = strOr(j["error"], "message");
            return;
        }
        if (type == "content_block_start") {
            if (!j.contains("content_block")) return;
            const auto cb = j["content_block"];
            if (strOr(cb, "type") == "tool_use") {
                size_t idx = 0;
                if (j.contains("index"))
                    if (auto n = j["index"].asInt()) idx = (size_t)std::max<int64_t>(0, n.unwrap());
                auto& acc = m_tools.at(idx);
                acc.id   = strOr(cb, "id");
                acc.name = strOr(cb, "name");
            }
            return;
        }
        if (type == "content_block_delta") {
            if (!j.contains("delta")) return;
            const auto d = j["delta"];
            std::string dt = strOr(d, "type");
            if (dt == "text_delta")     { addText(strOr(d, "text")); return; }
            if (dt == "thinking_delta") { addThinking(strOr(d, "thinking")); return; }
            if (dt == "input_json_delta") {
                size_t idx = 0;
                if (j.contains("index"))
                    if (auto n = j["index"].asInt()) idx = (size_t)std::max<int64_t>(0, n.unwrap());
                m_tools.at(idx).argsJson += strOr(d, "partial_json");
            }
            return;
        }
        if (type == "message_delta") {
            if (j.contains("delta"))
                if (auto sr = strOr(j["delta"], "stop_reason"); !sr.empty()) m_finish = sr;
            return;
        }
    }

    // ── Gemini: same candidate shape as non-streamed, one per event ───────
    void gemini(const matjson::Value& j) {
        if (j.isObject() && j.contains("error")) {
            m_error = strOr(j["error"], "message");
            return;
        }
        if (!j.isObject() || !j.contains("candidates")
            || !j["candidates"].isArray() || j["candidates"].size() == 0) return;
        const auto cand = j["candidates"][0];
        if (auto fr = strOr(cand, "finishReason"); !fr.empty()) m_finish = fr;
        if (!cand.isObject() || !cand.contains("content")) return;
        const auto content = cand["content"];
        if (!content.isObject() || !content.contains("parts")
            || !content["parts"].isArray()) return;
        const auto parts = content["parts"];
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto p = parts[i];
            if (!p.isObject()) continue;
            // Gemini marks reasoning parts with "thought": true.
            bool isThought = false;
            if (p.contains("thought"))
                if (auto b = p["thought"].asBool()) isThought = b.unwrap();
            if (p.contains("text")) {
                std::string t = strOr(p, "text");
                if (isThought) addThinking(t); else addText(t);
            }
            if (p.contains("functionCall")) {
                const auto fc = p["functionCall"];
                auto& acc = m_tools.at(m_geminiToolIdx++);
                acc.name = strOr(fc, "name");
                acc.id   = fmt::format("{}:{}", acc.name, m_geminiToolIdx - 1);
                acc.thoughtSignature = strOr(p, "thoughtSignature");
                if (fc.isObject() && fc.contains("args"))
                    acc.argsJson = fc["args"].dump();
            }
        }
    }

    // ── Ollama: /api/generate ("response") and /api/chat ("message") ──────
    // Platinum's coordinator speaks this shape too: keepalive lines
    // {"response":"","done":false} while the request sits in the queue, then a
    // final line carrying the whole answer (or {"error":...}).
    void ollama(const matjson::Value& j) {
        if (auto e = strOr(j, "error"); !e.empty()) { m_error = e; return; }
        // /api/generate. Newer Ollama also splits reasoning into "thinking".
        addThinking(strOr(j, "thinking"));
        addText(strOr(j, "response"));
        if (j.isObject() && j.contains("message")) {
            const auto m = j["message"];
            addText(strOr(m, "content"));
            addThinking(strOr(m, "thinking"));
            if (m.isObject() && m.contains("tool_calls") && m["tool_calls"].isArray()) {
                const auto tcs = m["tool_calls"];
                for (size_t i = 0; i < tcs.size(); ++i) {
                    const auto tc = tcs[i];
                    if (!tc.isObject() || !tc.contains("function")) continue;
                    const auto fn = tc["function"];
                    auto& acc = m_tools.at(m_ollamaToolIdx++);
                    acc.name = strOr(fn, "name");
                    acc.id   = fmt::format("call_{}", m_ollamaToolIdx - 1);
                    if (fn.isObject() && fn.contains("arguments"))
                        acc.argsJson = fn["arguments"].isObject()
                            ? fn["arguments"].dump() : strOr(fn, "arguments");
                }
            }
        }
        if (j.isObject() && j.contains("done"))
            if (auto b = j["done"].asBool(); b && b.unwrap()) m_finish = "stop";
    }

    std::string m_provider;
    Sink&       m_sink;
    std::string m_buf;
    std::string m_text, m_thinking, m_finish, m_error;
    // <think> demux state (see addText).
    std::string m_pending;
    bool        m_inThink = false;
    ToolAcc     m_tools;
    size_t      m_geminiToolIdx = 0;
    size_t      m_ollamaToolIdx = 0;
};

// Worker-thread context handed to the curl callbacks.
struct XferCtx {
    SinkPtr  sink;
    Decoder* dec = nullptr;
    std::string* raw = nullptr;
};

size_t writeCb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* ctx = (XferCtx*)ud;
    size_t n = sz * nm;
    if (!ctx || !ctx->sink) return 0;
    if (ctx->sink->cancelled()) return 0;    // non-full return aborts curl
    ctx->sink->addBytes(n);
    // Keep a bounded copy of the raw stream for logging / error parsing.
    if (ctx->raw && ctx->raw->size() < 2'000'000) {
        size_t room = 2'000'000 - ctx->raw->size();
        ctx->raw->append(ptr, std::min(n, room));
    }
    if (ctx->dec) ctx->dec->feed(ptr, n);
    return n;
}

// Progress callback exists solely so a cancelled generation aborts promptly
// even while the provider is silent (thinking models can idle for a minute).
int xferCb(void* ud, std::int64_t, std::int64_t, std::int64_t, std::int64_t) {
    auto* ctx = (XferCtx*)ud;
    if (ctx && ctx->sink && ctx->sink->cancelled()) return 1;   // abort
    return 0;
}

} // namespace

SinkPtr post(const std::string& provider,
             const std::string& url,
             const std::vector<std::string>& headers,
             std::string body,
             int timeoutSeconds)
{
    const auto& c = curl::api();
    if (!c.ok) return nullptr;

    auto sink = std::make_shared<Sink>();
    std::thread([provider, url, headers, body = std::move(body),
                 timeoutSeconds, sink]() mutable {
        const auto& c = curl::api();
        Result res;
        auto* h = c.init();
        if (!h) {
            res.transportError = "curl_easy_init failed";
            sink->publish(std::move(res));
            return;
        }
        Decoder dec(provider, *sink);
        std::string raw;
        XferCtx ctx{sink, &dec, &raw};

        curl::SLIST* list = nullptr;
        for (auto& hdr : headers) list = c.slistAdd(list, hdr.c_str());
        // Advertise the framing we actually decode. Ollama (and Platinum's
        // coordinator, which mimics it) answers NDJSON; everyone else uses SSE.
        // Real servers ignore Accept here, but a strict one could 406 on a
        // mismatch, and being honest costs nothing.
        list = c.slistAdd(list, provider == "ollama"
            ? "Accept: application/x-ndjson"
            : "Accept: text/event-stream");
        // Nothing on the way may buffer or transform the response.
        list = c.slistAdd(list, "Cache-Control: no-cache");

        c.setopt(h, curl::O_URL, url.c_str());
        c.setopt(h, curl::O_POST, 1L);
        c.setopt(h, curl::O_POSTFIELDS, body.c_str());
        c.setopt(h, curl::O_POSTFIELDSIZE, (long)body.size());
        c.setopt(h, curl::O_HTTPHEADER, list);
        c.setopt(h, curl::O_WRITEFUNCTION, +writeCb);
        c.setopt(h, curl::O_WRITEDATA, &ctx);
        c.setopt(h, curl::O_NOPROGRESS, 0L);
        c.setopt(h, curl::O_XFERINFOFUNCTION, +xferCb);
        c.setopt(h, curl::O_XFERINFODATA, &ctx);
        c.setopt(h, curl::O_FOLLOWLOCATION, 1L);
        c.setopt(h, curl::O_NOSIGNAL, 1L);
        c.setopt(h, curl::O_TCP_KEEPALIVE, 1L);
        c.setopt(h, curl::O_CONNECTTIMEOUT, 30L);
        // Timeouts, streaming-aware. A single total timeout is wrong here: a
        // long generation that is actively producing tokens would be killed
        // mid-answer, while a silently dead connection would hold the thread
        // for the whole window. So:
        //   stall  = the user's configured timeout, applied as "no bytes at all
        //            for this long" (LOW_SPEED_LIMIT 1 byte/s) — this is what
        //            "give up waiting" actually means for a stream, and
        //            keepalive lines legitimately reset it.
        //   total  = a generous hard ceiling so a pathological case can't leak
        //            the worker thread forever.
        long stall = (long)std::max(30, timeoutSeconds);
        c.setopt(h, curl::O_LOW_SPEED_LIMIT, 1L);
        c.setopt(h, curl::O_LOW_SPEED_TIME, stall);
        c.setopt(h, curl::O_TIMEOUT, std::min(std::max(stall * 3, 900L), 5400L));
        c.setopt(h, curl::O_USERAGENT, "EditorAI (Geode mod)");
        // HTTP/1.1 only. SSE gains nothing from HTTP/2 multiplexing, and h2
        // adds framing that has to work perfectly for a long-lived streamed
        // body — ALPN + h2 over the game's schannel-based libcurl is exactly
        // the kind of thing that misbehaves under Wine/Proton. 1.1 chunked
        // transfer is the simplest thing that can possibly work here.
        c.setopt(h, curl::O_HTTP_VERSION, curl::HTTP_VERSION_1_1);
        // Identity only: a compressed SSE stream would arrive in blocks and
        // defeat the point of streaming.
        c.setopt(h, curl::O_ACCEPT_ENCODING, "identity");

        int rc = c.perform(h);
        long code = 0;
        c.getinfo(h, curl::INFO_RESPONSE_CODE, &code);

        dec.finish();
        dec.fill(res);
        res.httpCode    = code;
        res.rawBody     = std::move(raw);
        res.transportOk = (rc == 0);
        if (rc != 0) {
            res.transportError = c.strerror
                ? fmt::format("curl error {}: {}", rc, c.strerror(rc))
                : fmt::format("curl error {}", rc);
            if (sink->cancelled()) res.transportError = "cancelled";
        }

        if (list) c.slistFree(list);
        c.cleanup(h);
        sink->publish(std::move(res));
    }).detach();

    return sink;
}

} // namespace eaistream
