#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Protocol.hpp"
#include "mini_redis/runtime/CompletionQueue.hpp"
#include "mini_redis/runtime/GroupScheduler.hpp"
#include "mini_redis/runtime/IoBudget.hpp"
#include "mini_redis/runtime/PipelineReorder.hpp"
#include "mini_redis/runtime/RequestFrontend.hpp"
#include "mini_redis/runtime/ResourcePool.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void fail(const std::string& message) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void expect_equal(const std::string& actual, const std::string& expected,
                  const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAILED: " << message << "\nExpected: " << expected
                  << "\nActual:   " << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void test_pipeline_reorder_and_watermarks() {
    runtime::IoLimits limits;
    limits.max_inflight = 8;
    limits.inflight_high_watermark = 4;
    limits.inflight_low_watermark = 2;
    runtime::PipelineReorder pipeline(limits);

    expect(pipeline.begin(1) && pipeline.begin(2) && pipeline.begin(3) &&
               pipeline.begin(4),
           "begin up to the high watermark");
    expect(pipeline.pauseReads(), "high watermark pauses further reads");
    expect(pipeline.begin(5) && pipeline.begin(6) && pipeline.begin(7) &&
               pipeline.begin(8),
           "already-parsed requests can still fill the hard cap");
    expect(!pipeline.begin(9), "the hard in-flight cap rejects additional requests");
    pipeline.reset();

    expect(pipeline.begin(1) && pipeline.begin(2) && pipeline.begin(3) &&
               pipeline.begin(4),
           "begin a contiguous pipeline after reset");

    pipeline.complete(3, "+third\r\n");
    pipeline.complete(2, "+second\r\n");
    expect(pipeline.takeReady().empty(), "gaps hold responses until seq 1 arrives");

    pipeline.complete(1, "+first\r\n");
    const std::vector<std::string> ready = pipeline.takeReady();
    expect(ready.size() == 3, "contiguous prefix is released in request order");
    expect_equal(ready[0], "+first\r\n", "seq 1 first");
    expect_equal(ready[1], "+second\r\n", "seq 2 second");
    expect_equal(ready[2], "+third\r\n", "seq 3 third");
    expect(pipeline.belowResumeWatermark(), "draining to the low watermark resumes reads");
    expect(!pipeline.pauseReads(), "reads resume after the contiguous prefix is written");
}

void test_resource_pools_are_isolated() {
    runtime::ResourcePools pools;
    runtime::ResourceQuota snapshot;
    snapshot.max_connections = 2;
    snapshot.max_buffer_bytes = 1024;
    runtime::ResourceQuota raft;
    raft.max_connections = 2;
    raft.max_buffer_bytes = 1024;
    pools.setQuota(runtime::TrafficClass::kSnapshot, snapshot);
    pools.setQuota(runtime::TrafficClass::kRaft, raft);

    expect(pools.tryAdmit(runtime::TrafficClass::kSnapshot), "admit snapshot 1");
    expect(pools.tryAdmit(runtime::TrafficClass::kSnapshot), "admit snapshot 2");
    expect(!pools.tryAdmit(runtime::TrafficClass::kSnapshot),
           "snapshot pool saturates independently");
    expect(pools.tryAdmit(runtime::TrafficClass::kRaft),
           "raft still has its own connection quota");
    expect(pools.tryReserveBytes(runtime::TrafficClass::kRaft, 512),
           "raft buffer quota is independent of snapshot");
    pools.release(runtime::TrafficClass::kSnapshot);
    expect(pools.tryAdmit(runtime::TrafficClass::kSnapshot),
           "releasing a snapshot slot does not touch raft");
}

void test_group_actor_serial_and_parallel() {
    Database left(true);
    Database right(true);
    runtime::GroupScheduler scheduler(2);
    scheduler.addGroup(1, left);
    scheduler.addGroup(2, right);
    scheduler.start();

    std::atomic<int> group1_concurrent{0};
    std::atomic<int> group1_max{0};
    std::atomic<int> global_concurrent{0};
    std::atomic<int> global_max{0};

    auto bump = [](std::atomic<int>& current, std::atomic<int>& peak) {
        const int now = current.fetch_add(1) + 1;
        int observed = peak.load();
        while (now > observed && !peak.compare_exchange_weak(observed, now)) {
        }
    };
    auto drop = [](std::atomic<int>& current) { current.fetch_sub(1); };

    auto make_task = [&](consensus::GroupId, bool same_group) {
        runtime::GroupTask task;
        task.priority = runtime::TaskPriority::kClient;
        task.run = [&, same_group] {
            bump(global_concurrent, global_max);
            if (same_group) {
                bump(group1_concurrent, group1_max);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (same_group) {
                drop(group1_concurrent);
            }
            drop(global_concurrent);
        };
        return task;
    };

    expect(scheduler.post(1, make_task(1, true)), "post group 1 task A");
    expect(scheduler.post(1, make_task(1, true)), "post group 1 task B");
    expect(scheduler.post(2, make_task(2, false)), "post group 2 task");
    expect(scheduler.waitIdle(std::chrono::seconds(2)), "actors become idle");
    scheduler.stop();

    expect(group1_max.load() == 1, "one group never runs two tasks at once");
    expect(global_max.load() >= 2, "different groups run on the worker pool in parallel");
}

void test_control_traffic_jumps_bulk_queue() {
    Database database(true);
    runtime::GroupScheduler scheduler(1);
    auto& actor = scheduler.addGroup(1, database);

    std::vector<runtime::TrafficClass> order;
    std::mutex order_mu;
    auto record = [&](runtime::TrafficClass traffic) {
        runtime::GroupTask task;
        task.priority = traffic == runtime::TrafficClass::kRaft
                            ? runtime::TaskPriority::kControl
                            : runtime::TaskPriority::kBulk;
        task.traffic = traffic;
        task.run = [&, traffic] {
            std::lock_guard<std::mutex> lock(order_mu);
            order.push_back(traffic);
        };
        expect(actor.enqueue(std::move(task)), "enqueue isolation task");
    };

    record(runtime::TrafficClass::kSnapshot);
    record(runtime::TrafficClass::kSnapshot);
    record(runtime::TrafficClass::kRaft);
    scheduler.notify(1);
    scheduler.start();
    expect(scheduler.waitIdle(std::chrono::seconds(2)), "priority drain finishes");
    scheduler.stop();

    expect(order.size() == 3, "all isolation tasks ran");
    expect(order[0] == runtime::TrafficClass::kRaft,
           "raft control work is taken before snapshot bulk");
}

void test_mailbox_backpressure() {
    Database database(true);
    runtime::GroupScheduler scheduler(1);
    auto& actor = scheduler.addGroup(1, database);
    runtime::IoLimits limits = runtime::clientIoLimits();
    limits.mailbox_high_watermark = 2;
    limits.mailbox_low_watermark = 1;
    actor.setClientLimits(limits);

    runtime::GroupTask blocker;
    blocker.run = [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); };
    expect(actor.enqueue(std::move(blocker)), "first client task accepted");
    runtime::GroupTask second;
    second.run = [] {};
    expect(actor.enqueue(std::move(second)), "second client task accepted");
    runtime::GroupTask third;
    third.run = [] {};
    expect(!actor.enqueue(std::move(third)), "mailbox high watermark rejects more client work");
    expect(!actor.canAcceptClient(), "canAcceptClient reflects the high watermark");

    runtime::GroupTask vote;
    vote.priority = runtime::TaskPriority::kControl;
    vote.traffic = runtime::TrafficClass::kRaft;
    vote.run = [] {};
    expect(actor.enqueue(std::move(vote)), "raft votes are still admitted when clients are busy");
}

void test_frontend_pipeline_across_groups() {
    Database shard1(true);
    Database shard2(true);
    CommandHandler local(shard1);
    runtime::GroupScheduler scheduler(2);
    scheduler.addGroup(1, shard1);
    scheduler.addGroup(2, shard2);
    scheduler.start();

    runtime::RequestFrontend frontend(scheduler, local);
    frontend.setGroupResolver([](const std::vector<std::string>& args) {
        if (args.size() > 1 && args[1] == "b") {
            return static_cast<consensus::GroupId>(2);
        }
        return static_cast<consensus::GroupId>(1);
    });

    cluster::ClientSession session;
    runtime::PipelineReorder pipeline;
    runtime::CompletionQueue completions;
    runtime::RequestContext ctx{session, pipeline, completions, 7, 1,
                                runtime::TrafficClass::kClient};

    const auto ping = frontend.handle(ctx, {"PING"});
    expect(ping.action == runtime::FrontendAction::kImmediate, "PING stays on the reactor");
    auto ready = pipeline.takeReady();
    expect(ready.size() == 1, "local PING is immediately writable");
    expect_equal(ready[0], RespParser::encodeSimpleString("PONG"), "PING response");

    const auto set_a = frontend.handle(ctx, {"SET", "a", "1"});
    const auto set_b = frontend.handle(ctx, {"SET", "b", "2"});
    expect(set_a.action == runtime::FrontendAction::kDeferred, "SET a goes to group 1");
    expect(set_b.action == runtime::FrontendAction::kDeferred, "SET b goes to group 2");
    expect(scheduler.waitIdle(std::chrono::seconds(2)), "both group actors finish");

    auto finished = completions.popAll();
    expect(finished.size() == 2, "both writes complete via the thread-safe queue");
    // Complete out of arrival order to prove the connection reorders.
    if (finished[0].request_seq < finished[1].request_seq) {
        pipeline.complete(finished[1].request_seq, finished[1].response);
        pipeline.complete(finished[0].request_seq, finished[0].response);
    } else {
        pipeline.complete(finished[0].request_seq, finished[0].response);
        pipeline.complete(finished[1].request_seq, finished[1].response);
    }
    ready = pipeline.takeReady();
    expect(ready.size() == 2, "pipeline emits both responses once the prefix is contiguous");
    expect_equal(ready[0], RespParser::encodeSimpleString("OK"), "first SET response");
    expect_equal(ready[1], RespParser::encodeSimpleString("OK"), "second SET response");

    std::string value;
    expect(shard1.get("a", value) && value == "1", "group 1 store received SET a");
    expect(shard2.get("b", value) && value == "2", "group 2 store received SET b");
    scheduler.stop();
}

void test_frontend_holds_when_pipeline_is_full() {
    Database database(true);
    CommandHandler local(database);
    runtime::GroupScheduler scheduler(1);
    scheduler.addGroup(1, database);

    runtime::IoLimits limits;
    limits.max_inflight = 2;
    limits.inflight_high_watermark = 2;
    limits.inflight_low_watermark = 1;
    runtime::PipelineReorder pipeline(limits);
    runtime::CompletionQueue completions;
    cluster::ClientSession session;
    runtime::RequestFrontend frontend(scheduler, local);
    runtime::RequestContext ctx{session, pipeline, completions, 1, 1,
                                runtime::TrafficClass::kClient};

    expect(frontend.handle(ctx, {"SET", "k", "1"}).action ==
               runtime::FrontendAction::kDeferred,
           "first SET is submitted");
    expect(frontend.handle(ctx, {"SET", "k", "2"}).action ==
               runtime::FrontendAction::kDeferred,
           "second SET fills the in-flight window");
    const auto held = frontend.handle(ctx, {"SET", "k", "3"});
    expect(held.action == runtime::FrontendAction::kHold,
           "a full pipeline leaves the next command in the input buffer");
    expect(held.pause_reads, "the reactor must pause EPOLLIN");
}

}  // namespace

int main() {
    test_pipeline_reorder_and_watermarks();
    test_resource_pools_are_isolated();
    test_group_actor_serial_and_parallel();
    test_control_traffic_jumps_bulk_queue();
    test_mailbox_backpressure();
    test_frontend_pipeline_across_groups();
    test_frontend_holds_when_pipeline_is_full();
    std::cout << "runtime concurrency tests passed" << std::endl;
    return EXIT_SUCCESS;
}
