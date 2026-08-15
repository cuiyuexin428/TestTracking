# Concrete task plugins. Each plugin is a DLL exporting both the scheduler
# side (common::TaskPlugin) and the client side (common::WorkerPlugin) factories.
add_subdirectory(demo)
add_subdirectory(coverage)
