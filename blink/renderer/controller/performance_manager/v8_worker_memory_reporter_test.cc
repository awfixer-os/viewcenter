// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/controller/performance_manager/v8_worker_memory_reporter.h"

#include "base/byte_count.h"
#include "base/time/time.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/renderer/core/testing/sim/sim_request.h"
#include "third_party/blink/renderer/core/testing/sim/sim_test.h"
#include "third_party/blink/renderer/core/workers/dedicated_worker_test.h"
#include "third_party/blink/renderer/core/workers/worker_thread.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class V8WorkerMemoryReporterTest : public ::testing::Test {
 public:
  using Result = V8WorkerMemoryReporter::Result;
  using WorkerMemoryUsage = V8WorkerMemoryReporter::WorkerMemoryUsage;
};

class V8WorkerMemoryReporterTestWithDedicatedWorker
    : public DedicatedWorkerTest {
 public:
  V8WorkerMemoryReporterTestWithDedicatedWorker()
      : DedicatedWorkerTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}
};

class V8WorkerMemoryReporterTestWithMockPlatform
    : public V8WorkerMemoryReporterTestWithDedicatedWorker {
 public:
  void SetUp() override {
    EnablePlatform();
    V8WorkerMemoryReporterTestWithDedicatedWorker::SetUp();
  }
};

class MockCallback {
 public:
  MOCK_METHOD(void, Callback, (const V8WorkerMemoryReporter::Result&));
};

bool operator==(const V8WorkerMemoryReporter::WorkerMemoryUsage& lhs,
                const V8WorkerMemoryReporter::WorkerMemoryUsage& rhs) {
  return lhs.token == rhs.token && lhs.memory == rhs.memory;
}

bool operator==(const V8WorkerMemoryReporter::Result& lhs,
                const V8WorkerMemoryReporter::Result& rhs) {
  return lhs.workers == rhs.workers;
}

class MemoryUsageChecker {
 public:
  enum class CallbackAction { kExitRunLoop, kNone };

  MemoryUsageChecker(size_t worker_count,
                     base::ByteCount memory_per_worker_lower_bound,
                     CallbackAction callback_action)
      : worker_count_(worker_count),
        memory_per_worker_lower_bound_(memory_per_worker_lower_bound),
        callback_action_(callback_action) {}

  void Callback(const V8WorkerMemoryReporter::Result& result) {
    EXPECT_EQ(worker_count_, result.workers.size());
    size_t expected_counts[2] = {0, 1};
    EXPECT_THAT(expected_counts, testing::Contains(worker_count_));
    if (worker_count_ == 1) {
      EXPECT_LE(memory_per_worker_lower_bound_, result.workers[0].memory);
      EXPECT_EQ(KURL("http://fake.url/"), result.workers[0].url);
    }
    called_ = true;
    if (callback_action_ == CallbackAction::kExitRunLoop) {
      loop_.Quit();
    }
  }

  void Run() { loop_.Run(); }

  bool IsCalled() { return called_; }

 private:
  bool called_ = false;
  size_t worker_count_;
  base::ByteCount memory_per_worker_lower_bound_;
  CallbackAction callback_action_;
  base::RunLoop loop_;
};

TEST_F(V8WorkerMemoryReporterTest, OnMeasurementSuccess) {
  MockCallback mock_callback;
  V8WorkerMemoryReporter reporter(
      BindOnce(&MockCallback::Callback, Unretained(&mock_callback)));
  reporter.SetWorkerCount(6);
  Result result = {Vector<WorkerMemoryUsage>(
      {WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(1)},
       WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(2)},
       WorkerMemoryUsage{WorkerToken(SharedWorkerToken()), base::ByteCount(3)},
       WorkerMemoryUsage{WorkerToken(SharedWorkerToken()), base::ByteCount(4)},
       WorkerMemoryUsage{WorkerToken(ServiceWorkerToken()), base::ByteCount(4)},
       WorkerMemoryUsage{WorkerToken(ServiceWorkerToken()),
                         base::ByteCount(5)}})};

  EXPECT_CALL(mock_callback, Callback(result)).Times(1);
  for (auto& worker : result.workers) {
    reporter.OnMeasurementSuccess(std::make_unique<WorkerMemoryUsage>(worker));
  }
}

TEST_F(V8WorkerMemoryReporterTest, OnMeasurementFailure) {
  MockCallback mock_callback;
  V8WorkerMemoryReporter reporter(
      BindOnce(&MockCallback::Callback, Unretained(&mock_callback)));
  reporter.SetWorkerCount(3);
  Result result = {Vector<WorkerMemoryUsage>(
      {WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(1)},
       WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(2)}})};

  EXPECT_CALL(mock_callback, Callback(result)).Times(1);
  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[0]));
  reporter.OnMeasurementFailure();
  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[1]));
}

TEST_F(V8WorkerMemoryReporterTest, OnTimeout) {
  MockCallback mock_callback;
  V8WorkerMemoryReporter reporter(
      BindOnce(&MockCallback::Callback, Unretained(&mock_callback)));
  reporter.SetWorkerCount(4);
  Result result = {Vector<WorkerMemoryUsage>(
      {WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(1)},
       WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(2)}})};

  EXPECT_CALL(mock_callback, Callback(result)).Times(1);

  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[0]));
  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[1]));
  reporter.OnTimeout();
  reporter.OnMeasurementSuccess(std::make_unique<WorkerMemoryUsage>(
      WorkerMemoryUsage{WorkerToken(SharedWorkerToken()), base::ByteCount(2)}));
  reporter.OnMeasurementFailure();
}

TEST_F(V8WorkerMemoryReporterTest, OnTimeoutNoop) {
  MockCallback mock_callback;
  V8WorkerMemoryReporter reporter(
      BindOnce(&MockCallback::Callback, Unretained(&mock_callback)));
  reporter.SetWorkerCount(2);
  Result result = {Vector<WorkerMemoryUsage>(
      {WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(1)},
       WorkerMemoryUsage{WorkerToken(DedicatedWorkerToken()),
                         base::ByteCount(2)}})};

  EXPECT_CALL(mock_callback, Callback(result)).Times(1);
  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[0]));
  reporter.OnMeasurementSuccess(
      std::make_unique<WorkerMemoryUsage>(result.workers[1]));
  reporter.OnTimeout();
}

TEST_F(V8WorkerMemoryReporterTestWithDedicatedWorker, GetMemoryUsage) {
  const String source_code = "globalThis.array = new Array(1000000).fill(0);";
  StartWorker();
  EvaluateClassicScript(source_code);
  WaitUntilWorkerIsRunning();
  constexpr base::ByteCount kBytesPerArrayElement = base::ByteCount(4);
  constexpr size_t kArrayLength = 1000000;
  MemoryUsageChecker checker(1, kBytesPerArrayElement * kArrayLength,
                             MemoryUsageChecker::CallbackAction::kExitRunLoop);
  V8WorkerMemoryReporter::GetMemoryUsage(
      BindOnce(&MemoryUsageChecker::Callback, Unretained(&checker)),
      v8::MeasureMemoryExecution::kEager);
  checker.Run();
  EXPECT_TRUE(checker.IsCalled());
}

TEST_F(V8WorkerMemoryReporterTestWithMockPlatform, GetMemoryUsageTimeout) {
  const String source_code = "while(true);";
  StartWorker();
  EvaluateClassicScript(source_code);
  // Since the worker is in infinite loop and does not process tasks,
  // we cannot call WaitUntilWorkerIsRunning here as that would block.
  MemoryUsageChecker checker(0, base::ByteCount(0),
                             MemoryUsageChecker::CallbackAction::kNone);
  V8WorkerMemoryReporter::GetMemoryUsage(
      BindOnce(&MemoryUsageChecker::Callback, Unretained(&checker)),
      v8::MeasureMemoryExecution::kEager);
  FastForwardBy(
      base::Seconds(V8WorkerMemoryReporter::kTimeout.InSeconds() + 1));
  EXPECT_TRUE(checker.IsCalled());
}

}  // namespace blink
