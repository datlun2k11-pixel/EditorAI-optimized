#pragma once
// ── Real token streaming for EditorAI ────────────────────────────────────────
//
// Geode's web API cannot stream: `WebFuture` resolves exactly once with a
// complete body, and `onProgress` only reports byte counters. To show tokens
// as the model writes them we talk to libcurl directly — but WITHOUT adding a
// second HTTP stack to the binary:
//
//   * Windows: the game already ships (and libExtensions.dll already loads)
//     `libcurl.dll`. We resolve the handful of symbols we need at runtime with
//     GetModuleHandle/GetProcAddress. Its TLS backend is schannel, so the
//     Windows certificate store is used — no CA bundle to ship.
//   * Other platforms: we try dlsym on the already-loaded process image. If
//     curl isn't reachable there, streaming reports itself unavailable and
//     every caller silently falls back to the normal Geode request path.
//
// Threading model: no game object ever crosses a thread boundary. The worker
// thread only touches a `Sink` (mutex + plain strings + atomics) which both
// sides hold by shared_ptr. The engine polls the sink from a main-thread tick.
// If the popup dies it drops its handle and sets `cancel`, the worker notices,
// aborts the transfer, and the last shared_ptr release frees the sink.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace eaistream {

// A tool call reassembled from streamed fragments. `argsJson` is the raw JSON
// text of the arguments object (providers stream it as partial JSON).
struct ToolCallAcc {
    std::string id;
    std::string name;
    std::string argsJson;
    // Gemini 3 signs functionCall parts. The signature must be replayed with
    // the assistant tool call on the next request or Gemini rejects the round.
    std::string thoughtSignature;
};

// Everything the worker produced, published once when the stream ends.
struct Result {
    bool        transportOk = false;  // curl actually completed a transfer
    long        httpCode    = 0;
    std::string transportError;       // curl-level failure text (empty if ok)
    std::string rawBody;              // full raw response (error bodies, logs)
    std::string text;                 // accumulated visible assistant text
    std::string thinking;             // accumulated reasoning / thinking text
    std::vector<ToolCallAcc> toolCalls;
    std::string finishReason;
    std::string providerError;        // error object surfaced inside the stream
};

// Shared worker↔main-thread mailbox.
class Sink {
public:
    // Main thread: drain whatever arrived since the last call. Returns true if
    // anything was appended to either out-parameter.
    bool drain(std::string& outText, std::string& outThinking);
    // Main thread: has the transfer finished? `out` is filled when it has.
    bool finished(Result& out);
    // Main thread: ask the worker to stop (also called when the owner dies).
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
    // Bytes received so far — drives a "receiving..." status before the first
    // decodable token arrives.
    size_t bytes() const { return m_bytes.load(std::memory_order_relaxed); }

    // ── worker side ──
    void pushText(std::string_view s);
    void pushThinking(std::string_view s);
    void addBytes(size_t n) { m_bytes.fetch_add(n, std::memory_order_relaxed); }
    void publish(Result r);

private:
    std::mutex  m_mu;
    std::string m_text;
    std::string m_thinking;
    bool        m_done = false;
    Result      m_result;
    std::atomic<bool>   m_cancel{false};
    std::atomic<size_t> m_bytes{0};
};

using SinkPtr = std::shared_ptr<Sink>;

// True when the curl symbols resolved, i.e. streaming can be attempted.
bool available();
// Human-readable reason streaming is off (empty when available).
std::string unavailableReason();

// Which providers we know how to decode a stream from.
bool supportsStreaming(const std::string& provider);

// Body/URL adjustments the caller must apply before handing us a request.
// (Kept here so the request builders and the decoders can never disagree.)
//   openai-compat / claude / ollama : add "stream": true to the JSON body
//   gemini                          : :streamGenerateContent?alt=sse URL
std::string streamUrlFor(const std::string& provider, const std::string& url);

// Start a streaming POST on a detached worker thread. Returns the sink to
// poll, or nullptr when streaming is unavailable (caller falls back).
// `headers` entries are complete "Name: value" strings.
SinkPtr post(const std::string& provider,
             const std::string& url,
             const std::vector<std::string>& headers,
             std::string body,
             int timeoutSeconds);

} // namespace eaistream
