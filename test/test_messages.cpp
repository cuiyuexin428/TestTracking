// Unit test: protobuf message round trip (serialize -> parse).
#include "communication.pb.h"

#include <cstdio>
#include <string>

namespace {

int Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int rc = 0;

    common::Envelope env;
    env.set_type(common::RUN_WAVE);
    env.set_request_id("req-1");

    common::RunWaveCommand* cmd = env.mutable_run_wave();
    cmd->set_run_id("run-1");
    cmd->set_wave_id("wave-1");
    cmd->set_task_type("stp");

    common::TaskSpec* task = cmd->add_tasks();
    task->set_task_id("task-1");
    task->set_payload("payload-bytes");

    std::string bytes;
    rc |= Check(env.SerializeToString(&bytes), "serialize envelope");

    common::Envelope parsed;
    rc |= Check(parsed.ParseFromString(bytes), "parse envelope");
    rc |= Check(parsed.type() == common::RUN_WAVE, "type preserved");
    rc |= Check(parsed.request_id() == "req-1", "request_id preserved");
    rc |= Check(parsed.has_run_wave(), "run_wave present");
    rc |= Check(parsed.run_wave().run_id() == "run-1", "run_id preserved");
    rc |= Check(parsed.run_wave().wave_id() == "wave-1", "wave_id preserved");
    rc |= Check(parsed.run_wave().tasks_size() == 1, "task count preserved");
    rc |= Check(parsed.run_wave().tasks(0).task_id() == "task-1", "task_id preserved");
    rc |= Check(parsed.run_wave().tasks(0).payload() == "payload-bytes", "payload preserved");

    common::Envelope hb;
    hb.set_type(common::HEARTBEAT);
    common::Heartbeat* heartbeat = hb.mutable_heartbeat();
    heartbeat->set_worker_id("w1");
    heartbeat->set_status(common::Heartbeat::IDLE);
    heartbeat->set_dataset_ready(true);

    std::string hb_bytes;
    rc |= Check(hb.SerializeToString(&hb_bytes), "serialize heartbeat");

    common::Envelope hb_parsed;
    rc |= Check(hb_parsed.ParseFromString(hb_bytes), "parse heartbeat");
    rc |= Check(hb_parsed.has_heartbeat(), "heartbeat present");
    rc |= Check(hb_parsed.heartbeat().worker_id() == "w1", "worker_id preserved");
    rc |= Check(hb_parsed.heartbeat().status() == common::Heartbeat::IDLE, "status preserved");
    rc |= Check(hb_parsed.heartbeat().dataset_ready(), "dataset_ready preserved");

    if (rc == 0) {
        std::printf("PASS: messages round trip\n");
    }
    return rc;
}
