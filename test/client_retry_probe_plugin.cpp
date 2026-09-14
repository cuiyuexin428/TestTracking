#include <string>

#include "common/WorkerPlugin.h"

namespace {

class RetryProbe : public common::WorkerPlugin {
   public:
    std::string pluginType() const override { return "client-retry-probe"; }
    bool init(const std::string&) override { return true; }

    bool collectRegistrationInfo(std::string* payload) override {
        *payload = "registration-" + std::to_string(++registrations_);
        return true;
    }

    common::RegisterOutcome handleValidationReply(const std::string&) override {
        return common::RegisterOutcome::kRetry;
    }

    bool execute(const common::Task&, common::TaskResult&) override { return false; }

   private:
    int registrations_ = 0;
};

}  // namespace

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT common::WorkerPlugin* CreateClientPlugin() {
    return new RetryProbe;
}

extern "C" TEST_EXPORT void DestroyClientPlugin(common::WorkerPlugin* plugin) {
    delete plugin;
}
