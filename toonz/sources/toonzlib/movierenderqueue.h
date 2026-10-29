#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>

// Pending results belong to actual requested frames. A failed frame occupies
// its own place in the sequence, allowing later successful frames to drain.
// The caller serializes access and decides whether to take in chronological
// order or process independent image clusters concurrently.
template <class Payload>
class MovieRenderQueue {
public:
  struct Result {
    Payload payload{};
    bool failed = false;
    std::wstring error;
  };

  void addFrame(double frame) {
    m_remaining.insert(frame);
  }

  void push(double frame, const Payload &payload) {
    if (!m_remaining.count(frame)) return;
    Result result;
    result.payload = payload;
    m_pending.emplace(frame, std::move(result));
  }

  void fail(double frame, const std::wstring &error) {
    if (!m_remaining.count(frame)) return;
    Result result;
    result.failed = true;
    result.error  = error;
    m_pending.emplace(frame, std::move(result));
  }

  bool take(double frame, Result &result) {
    auto it = m_pending.find(frame);
    if (it == m_pending.end()) return false;
    result = std::move(it->second);
    m_pending.erase(it);
    m_remaining.erase(frame);
    return true;
  }

  bool takeNext(double &frame, Result &result) {
    if (m_remaining.empty()) return false;
    frame = *m_remaining.begin();
    return take(frame, result);
  }

  bool complete() const { return m_remaining.empty(); }
  void discardPending() { m_pending.clear(); }

private:
  std::set<double> m_remaining;
  std::map<double, Result> m_pending;
};
