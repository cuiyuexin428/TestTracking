#include "stpcompare/ClientPlugin.h"
#include "stpcompare/SchedulerPlugin.h"

#ifdef _WIN32
#define STPCOMPARE_EXPORT __declspec(dllexport)
#else
#define STPCOMPARE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" STPCOMPARE_EXPORT common::TaskPlugin* CreatePlugin() {
    return new stpcompare::SchedulerPlugin;
}

extern "C" STPCOMPARE_EXPORT void DestroyPlugin(common::TaskPlugin* plugin) {
    delete plugin;
}

extern "C" STPCOMPARE_EXPORT common::WorkerPlugin* CreateClientPlugin() {
    return new stpcompare::ClientPlugin;
}

extern "C" STPCOMPARE_EXPORT void DestroyClientPlugin(common::WorkerPlugin* plugin) {
    delete plugin;
}
