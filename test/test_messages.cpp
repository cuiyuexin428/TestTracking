// Unit test: protobuf message round trips for the framework protocol.
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

    // TASK: single-task dispatch routed by envelope plugin_type.
    common::Envelope env;
    env.set_type(common::TASK);
    env.set_plugin_type("coverage");

    common::TaskCommand* cmd = env.mutable_task();
    cmd->set_run_id("run-1");
    common::Task* task1 = cmd->add_tasks();
    task1->set_task_id("task-1");
    task1->set_payload("payload-bytes");
    common::Task* task2 = cmd->add_tasks();
    task2->set_task_id("task-2");
    task2->set_payload("payload-2");

    std::string bytes;
    rc |= Check(env.SerializeToString(&bytes), "serialize envelope");

    common::Envelope parsed;
    rc |= Check(parsed.ParseFromString(bytes), "parse envelope");
    rc |= Check(parsed.type() == common::TASK, "type preserved");
    rc |= Check(parsed.plugin_type() == "coverage", "plugin_type preserved");
    rc |= Check(parsed.has_task(), "task present");
    rc |= Check(parsed.task().run_id() == "run-1", "run_id preserved");
    rc |= Check(parsed.task().tasks_size() == 2, "task count preserved");
    rc |= Check(parsed.task().tasks(0).task_id() == "task-1", "task_id preserved");
    rc |= Check(parsed.task().tasks(0).payload() == "payload-bytes", "payload preserved");
    rc |= Check(parsed.task().tasks(1).task_id() == "task-2", "second task_id preserved");

    // REQUEST_TASK: desired task count.
    common::Envelope req;
    req.set_type(common::REQUEST_TASK);
    req.set_plugin_type("coverage");
    common::RequestTaskCommand* request = req.mutable_request_task();
    request->set_count(4);

    std::string req_bytes;
    rc |= Check(req.SerializeToString(&req_bytes), "serialize request task");
    common::Envelope req_parsed;
    rc |= Check(req_parsed.ParseFromString(req_bytes), "parse request task");
    rc |= Check(req_parsed.has_request_task(), "request task present");
    rc |= Check(req_parsed.request_task().count() == 4, "request count preserved");

    // REGISTER: opaque business payload, routed by envelope plugin_type.
    common::Envelope reg;
    reg.set_type(common::REGISTER);
    reg.set_plugin_type("coverage");
    reg.mutable_register_()->set_payload("client-info-bytes");
    reg.mutable_register_()->set_registration_id("register-1");

    std::string reg_bytes;
    rc |= Check(reg.SerializeToString(&reg_bytes), "serialize register");
    common::Envelope reg_parsed;
    rc |= Check(reg_parsed.ParseFromString(reg_bytes), "parse register");
    rc |= Check(reg_parsed.has_register_(), "register present");
    rc |= Check(reg_parsed.plugin_type() == "coverage", "register plugin_type");
    rc |= Check(reg_parsed.register_().registration_id() == "register-1",
                "registration id preserved");
    rc |= Check(reg_parsed.register_().payload() == "client-info-bytes",
                "register payload preserved");

    // REGISTER_RESULT: opaque plugin-defined reply.
    common::Envelope result_env;
    result_env.set_type(common::REGISTER_RESULT);
    result_env.set_plugin_type("coverage");
    result_env.set_session_id("session-1");
    result_env.mutable_register_result()->set_payload("deploy-instructions");
    result_env.mutable_register_result()->set_registration_id("register-1");

    std::string result_bytes;
    rc |= Check(result_env.SerializeToString(&result_bytes), "serialize register result");
    common::Envelope result_parsed;
    rc |= Check(result_parsed.ParseFromString(result_bytes), "parse register result");
    rc |= Check(result_parsed.has_register_result(), "register result present");
    rc |= Check(result_parsed.session_id() == "session-1", "session id preserved");
    rc |= Check(result_parsed.register_result().registration_id() == "register-1",
                "registration id preserved");
    rc |= Check(result_parsed.register_result().payload() == "deploy-instructions",
                "register result payload preserved");

    result_env.mutable_register_result()->set_plugin_found(true);
    result_env.mutable_register_result()->set_accepted(false);
    result_env.mutable_register_result()->set_reason("deploy required");
    rc |= Check(result_env.SerializeToString(&result_bytes), "serialize register result fields");
    rc |= Check(result_parsed.ParseFromString(result_bytes), "parse register result fields");
    rc |= Check(result_parsed.register_result().plugin_found(), "register result plugin_found");
    rc |= Check(!result_parsed.register_result().accepted(), "register result accepted");
    rc |= Check(result_parsed.register_result().reason() == "deploy required",
                "register result reason");

    // STOP: server-initiated stop.
    common::Envelope stop;
    stop.set_type(common::STOP);
    stop.set_plugin_type("coverage");
    stop.mutable_stop()->set_run_id("run-1");
    stop.mutable_stop()->set_reason("manual");

    std::string stop_bytes;
    rc |= Check(stop.SerializeToString(&stop_bytes), "serialize stop");
    common::Envelope stop_parsed;
    rc |= Check(stop_parsed.ParseFromString(stop_bytes), "parse stop");
    rc |= Check(stop_parsed.has_stop(), "stop present");
    rc |= Check(stop_parsed.plugin_type() == "coverage", "stop plugin_type");
    rc |= Check(stop_parsed.stop().run_id() == "run-1", "stop run_id");
    rc |= Check(stop_parsed.stop().reason() == "manual", "stop reason");

    // TASK_ACK: task receipt.
    common::Envelope ack;
    ack.set_type(common::TASK_ACK);
    ack.set_plugin_type("coverage");
    ack.mutable_task_ack()->set_task_id("task-1");

    std::string ack_bytes;
    rc |= Check(ack.SerializeToString(&ack_bytes), "serialize task ack");
    common::Envelope ack_parsed;
    rc |= Check(ack_parsed.ParseFromString(ack_bytes), "parse task ack");
    rc |= Check(ack_parsed.has_task_ack(), "task ack present");
    rc |= Check(ack_parsed.plugin_type() == "coverage", "task ack plugin_type");
    rc |= Check(ack_parsed.task_ack().task_id() == "task-1", "task ack task_id");

    // TASK_REJECT: client cannot execute the task.
    common::Envelope reject;
    reject.set_type(common::TASK_REJECT);
    reject.set_plugin_type("coverage");
    reject.mutable_task_reject()->set_task_id("task-1");
    reject.mutable_task_reject()->set_reason("plugin_type mismatch");

    std::string reject_bytes;
    rc |= Check(reject.SerializeToString(&reject_bytes), "serialize task reject");
    common::Envelope reject_parsed;
    rc |= Check(reject_parsed.ParseFromString(reject_bytes), "parse task reject");
    rc |= Check(reject_parsed.has_task_reject(), "task reject present");
    rc |= Check(reject_parsed.plugin_type() == "coverage", "task reject plugin_type");
    rc |= Check(reject_parsed.task_reject().task_id() == "task-1", "task reject task_id");
    rc |= Check(reject_parsed.task_reject().reason() == "plugin_type mismatch",
                "task reject reason");

    // TASK_REPORT: execution report with opaque business result.
    common::Envelope report;
    report.set_type(common::TASK_REPORT);
    report.set_plugin_type("coverage");
    common::TaskReport* tr = report.mutable_task_report();
    tr->mutable_result()->set_task_id("task-1");
    tr->mutable_result()->set_exit_code(0);
    tr->mutable_result()->set_duration_ms(123);
    tr->mutable_result()->set_result("result-bytes");

    std::string report_bytes;
    rc |= Check(report.SerializeToString(&report_bytes), "serialize report");
    common::Envelope report_parsed;
    rc |= Check(report_parsed.ParseFromString(report_bytes), "parse report");
    rc |= Check(report_parsed.has_task_report(), "report present");
    rc |= Check(report_parsed.plugin_type() == "coverage", "report plugin_type");
    rc |= Check(report_parsed.task_report().result().task_id() == "task-1",
                "report task_id");
    rc |= Check(report_parsed.task_report().result().result() == "result-bytes",
                "report result preserved");

    // HEARTBEAT: display-only, routed by envelope plugin_type.
    common::Envelope hb;
    hb.set_type(common::HEARTBEAT);
    hb.set_plugin_type("coverage");
    common::Heartbeat* heartbeat = hb.mutable_heartbeat();
    heartbeat->set_ip("192.168.1.10");
    heartbeat->set_status(common::Heartbeat::IDLE);
    heartbeat->mutable_resource()->set_memory_bytes(1024);

    std::string hb_bytes;
    rc |= Check(hb.SerializeToString(&hb_bytes), "serialize heartbeat");
    common::Envelope hb_parsed;
    rc |= Check(hb_parsed.ParseFromString(hb_bytes), "parse heartbeat");
    rc |= Check(hb_parsed.has_heartbeat(), "heartbeat present");
    rc |= Check(hb_parsed.plugin_type() == "coverage", "heartbeat plugin_type");
    rc |= Check(hb_parsed.heartbeat().ip() == "192.168.1.10", "heartbeat ip");
    rc |= Check(hb_parsed.heartbeat().resource().memory_bytes() == 1024,
                "heartbeat resource");

    if (rc == 0) {
        std::printf("PASS: messages round trip\n");
    }
    return rc;
}
