/**
 * @file        tests/unit/core/threading_wait_test.cpp
 * @brief       Tests for rex::thread::WaitAny / WaitAll (blocking WaitMultiple)
 *              and timer wake-ups through the TimerQueue.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::Timer;
using rex::thread::WaitAll;
using rex::thread::WaitAny;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;
using Clock = std::chrono::steady_clock;

TEST_CASE("WaitAny returns immediately for an already-signaled handle", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(true);
  std::vector<WaitHandle*> handles{a.get(), b.get()};

  auto start = Clock::now();
  auto [result, index] = WaitAny(handles, false, 1000ms);
  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
  REQUIRE(Clock::now() - start < 50ms);

  // Auto-reset: b was consumed, so a second zero-timeout wait times out.
  REQUIRE(WaitAny(handles, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("WaitAny wakes promptly when another thread signals", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  std::vector<WaitHandle*> handles{a.get(), b.get()};

  Clock::time_point signaled_at;
  std::thread signaler([&] {
    std::this_thread::sleep_for(30ms);
    signaled_at = Clock::now();
    b->Set();
  });
  auto [result, index] = WaitAny(handles, false, std::chrono::milliseconds::max());
  auto woke_at = Clock::now();
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
  // A polling loop would add up to ~1 ms; a blocking wait wakes in microseconds.
  // Keep the bound loose so a loaded CI box doesn't flake.
  REQUIRE(woke_at - signaled_at < 20ms);
}

TEST_CASE("WaitAll waits for every handle and consumes them", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto sem = Semaphore::Create(0, 4);
  std::vector<WaitHandle*> handles{a.get(), sem.get()};

  // Only one signaled: WaitAll must not succeed (and must not consume it).
  a->Set();
  REQUIRE(WaitAll(handles, false, 20ms) == WaitResult::kTimeout);

  std::thread signaler([&] {
    std::this_thread::sleep_for(20ms);
    sem->Release(1, nullptr);
  });
  REQUIRE(WaitAll(handles, false, 2000ms) == WaitResult::kSuccess);
  signaler.join();

  // Both consumed: the event auto-reset and the semaphore count is back to 0.
  REQUIRE(WaitAny(handles, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("WaitAny times out after roughly the requested time", "[threading]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  std::vector<WaitHandle*> handles{a.get(), b.get()};

  auto start = Clock::now();
  REQUIRE(WaitAny(handles, false, 50ms).first == WaitResult::kTimeout);
  auto elapsed = Clock::now() - start;
  REQUIRE(elapsed >= 50ms);
  REQUIRE(elapsed < 500ms);

  // Zero timeout is a pure "poll once".
  start = Clock::now();
  REQUIRE(WaitAny(handles, false, 0ms).first == WaitResult::kTimeout);
  REQUIRE(Clock::now() - start < 20ms);
}

TEST_CASE("Many threads WaitAny on overlapping handles without deadlock", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  constexpr int kRounds = 2000;
  std::atomic<int> consumed{0};

  auto worker = [&](std::vector<WaitHandle*> handles) {
    while (consumed.load() < kRounds) {
      if (WaitAny(handles, false, 5ms).first == WaitResult::kSuccess) {
        consumed.fetch_add(1);
      }
    }
  };
  // Different orderings exercise the address-sorted lock order.
  std::thread t1(worker, std::vector<WaitHandle*>{a.get(), b.get()});
  std::thread t2(worker, std::vector<WaitHandle*>{b.get(), a.get()});
  for (int i = 0; i < kRounds * 2 && consumed.load() < kRounds; ++i) {
    (i & 1 ? a : b)->Set();
    if ((i & 63) == 0) std::this_thread::yield();
  }
  // Make sure the workers can finish even if some Sets coalesced.
  while (consumed.load() < kRounds) {
    a->Set();
    b->Set();
    std::this_thread::sleep_for(1ms);
  }
  t1.join();
  t2.join();
  REQUIRE(consumed.load() >= kRounds);
}

TEST_CASE("Timer fires through the blocking TimerQueue", "[threading]") {
  auto timer = Timer::CreateSynchronizationTimer();
  auto other = Event::CreateManualResetEvent(false);
  std::vector<WaitHandle*> handles{other.get(), timer.get()};

  auto start = Clock::now();
  REQUIRE(timer->SetOnceAfter(std::chrono::duration_cast<rex::chrono::hundrednanoseconds>(30ms)));
  auto [result, index] = WaitAny(handles, false, 2000ms);
  auto elapsed = Clock::now() - start;
  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
  REQUIRE(elapsed >= 25ms);
  REQUIRE(elapsed < 500ms);
}
