// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace AgentServer
{
// Tool calls arrive on threads of their own (HttpServer), but most must not run together: they
// pause and resume emulation, hold input, arm traces. The gate lets one call in at a time. A call
// that finds another one running waits while that one is quick, so quick calls sent in parallel
// still work. Once the running call lets emulation run for a while (an input hold, a trace, frame
// steps), waiting and later calls are refused at once instead of running after it: an agent that
// sent them alongside it expects them to see the game while it runs, and they would not. The
// tools marked `anytime` (status, screenshots, the log, cancel_tool and the recovery tools) do not
// go through the gate.
class CallGate final
{
public:
  // Defined in CallGate.cpp.
  struct Call;

  // Holds the gate for one call, on the thread that entered; destroying it lets the next call in.
  class Ticket final
  {
  public:
    Ticket(Ticket&& other) noexcept;
    Ticket& operator=(Ticket&& other) noexcept;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    ~Ticket();

    bool WasCancelled() const;
    double GetSeconds() const;

  private:
    friend class CallGate;
    Ticket(CallGate* gate, std::shared_ptr<Call> call);
    void Release();

    CallGate* m_gate = nullptr;
    std::shared_ptr<Call> m_call;
  };

  struct RunningCall
  {
    std::string tool;
    double seconds = 0;
    bool runs_emulation = false;
    bool cancel_requested = false;
  };

  // How long a call waits for a quick one to finish before it is refused.
  static constexpr std::chrono::milliseconds MAX_QUEUE_WAIT = std::chrono::seconds(60);

  // A ticket for `tool`, or nullopt when the call is refused; `busy` then describes the call
  // that holds the gate. `request_id` is the JSON-RPC id, serialized, for Cancel.
  std::optional<Ticket> Enter(std::string tool, std::string request_id, RunningCall* busy,
                              std::chrono::milliseconds max_wait = MAX_QUEUE_WAIT);
  std::optional<RunningCall> GetRunning() const;
  // Asks the running call to stop (only the call with that JSON-RPC id when `request_id` is
  // given). False when no call matched.
  bool Cancel(const std::string* request_id = nullptr);

private:
  friend void MarkCallRunsEmulation();

  void Release(const std::shared_ptr<Call>& call);
  void NotifyWaiters();
  static RunningCall Describe(const Call& call);

  mutable std::mutex m_mutex;
  std::condition_variable m_changed;
  std::shared_ptr<Call> m_running;
};

// For the gated call on the calling thread; no-ops on other threads (anytime tools, tests).
// The call lets emulation run for a while from now on: calls sent alongside it are refused. The
// emulation waits of the tool helpers call it.
void MarkCallRunsEmulation();
// cancel_tool, stop_game, quit_dolphin or the client asked the call to stop: emulation waits end
// early, and the tool returns what it has.
bool IsCallCancelled();
}  // namespace AgentServer
