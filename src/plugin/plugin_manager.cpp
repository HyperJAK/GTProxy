#include "plugin_manager.hpp"

namespace plugin {
PluginManager::PluginManager(std::filesystem::path dir, control::EventHub& hub)
    : dir_{ std::move(dir) }
    , hub_{ hub }
{ }
}
