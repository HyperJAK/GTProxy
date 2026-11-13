#include "template_store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "json_dump.hpp"

namespace control {
TemplateStore::TemplateStore(std::filesystem::path file)
    : file_{ std::move(file) }
{
    std::ifstream in{ file_, std::ios::binary };
    if (!in) {
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto doc = JsonValue::parse(ss.str());
    if (!doc) {
        return;
    }
    const auto* list = doc->find("templates");
    if (!list || !list->is_array()) {
        return;
    }
    for (const auto& item : list->array) {
        PacketTemplate t;
        t.name = item.str("name");
        t.description = item.str("description");
        t.dir = item.str("dir", "server_bound");
        if (const auto* p = item.find("packet"); p && !t.name.empty()) {
            t.packet = *p;
            items_.push_back(std::move(t));
        }
    }
}

std::optional<PacketTemplate> TemplateStore::find(const std::string& name) const
{
    const auto it = std::ranges::find(items_, name, &PacketTemplate::name);
    return it == items_.end() ? std::nullopt : std::optional{ *it };
}

std::string TemplateStore::to_json() const
{
    JsonWriter w;
    w.begin_object().key("templates").begin_array();
    for (const auto& t : items_) {
        w.begin_object().field("name", t.name).field("description", t.description).field("dir", t.dir);
        w.key("packet");
        write_json(w, t.packet);
        w.end_object();
    }
    w.end_array().end_object();
    return w.take();
}

std::string TemplateStore::persist() const
{
    const auto tmp = file_.string() + ".tmp";
    {
        std::ofstream out{ tmp, std::ios::binary | std::ios::trunc };
        if (!out) {
            return "cannot write templates file";
        }
        out << to_json();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    return ec ? "cannot replace templates file: " + ec.message() : std::string{};
}

std::string TemplateStore::save(PacketTemplate t)
{
    if (t.name.empty() || t.name.size() > 64) {
        return "template name must be 1-64 characters";
    }
    if (t.dir != "server_bound" && t.dir != "client_bound") {
        return "dir must be server_bound or client_bound";
    }
    if (!t.packet.is_object()) {
        return "template needs a packet spec object";
    }
    const auto it = std::ranges::find(items_, t.name, &PacketTemplate::name);
    if (it == items_.end()) {
        items_.push_back(std::move(t));
    } else {
        *it = std::move(t);
    }
    return persist();
}

std::string TemplateStore::remove(const std::string& name)
{
    const auto removed = std::erase_if(items_, [&](const PacketTemplate& t) { return t.name == name; });
    if (!removed) {
        return "no template with that name";
    }
    return persist();
}
}
