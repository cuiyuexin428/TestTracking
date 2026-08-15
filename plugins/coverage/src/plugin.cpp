#include "coverage/ClientPlugin.h"
#include "coverage/SchedulerPlugin.h"

#ifdef _WIN32
#define COVERAGE_EXPORT __declspec(dllexport)
#else
#define COVERAGE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" COVERAGE_EXPORT common::TaskPlugin* CreatePlugin() {
    return new coverage_plugin::SchedulerPlugin;
}

extern "C" COVERAGE_EXPORT void DestroyPlugin(common::TaskPlugin* plugin) {
    delete plugin;
}

extern "C" COVERAGE_EXPORT common::WorkerPlugin* CreateClientPlugin() {
    return new coverage_plugin::ClientPlugin;
}

extern "C" COVERAGE_EXPORT void DestroyClientPlugin(common::WorkerPlugin* plugin) {
    delete plugin;
}
