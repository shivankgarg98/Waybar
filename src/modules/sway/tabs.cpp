#include "modules/sway/tabs.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace waybar::modules::sway {

namespace {

const std::vector<std::string> kStateClasses = {"active", "current", "urgent"};

bool isActive(const Json::Value& node) {
  if (node["focused"].asBool()) {
    return true;
  }
  for (const auto& child : node["nodes"]) {
    if (isActive(child)) {
      return true;
    }
  }
  for (const auto& child : node["floating_nodes"]) {
    if (isActive(child)) {
      return true;
    }
  }
  return false;
}

// The window shown for a container: follow the focus order down to a leaf.
const Json::Value& leaf(const Json::Value& con) {
  const Json::Value* node = &con;
  while (!(*node)["nodes"].empty()) {
    const auto& children = (*node)["nodes"];
    const Json::Value* next = &children[0];
    if (!(*node)["focus"].empty()) {
      auto focus = (*node)["focus"][0].asInt64();
      for (const auto& child : children) {
        if (child["id"].asInt64() == focus) {
          next = &child;
          break;
        }
      }
    }
    node = next;
  }
  return *node;
}

std::string appOf(const Json::Value& window) {
  if (window["app_id"].isString()) {
    return window["app_id"].asString();
  }
  return window["window_properties"]["class"].asString();
}

// {output name: workspace it shows}
std::map<std::string, const Json::Value*> visibleWorkspaces(const Json::Value& root) {
  std::map<std::string, const Json::Value*> res;
  for (const auto& output : root["nodes"]) {
    auto current = output["current_workspace"].asString();
    for (const auto& ws : output["nodes"]) {
      if (ws["type"].asString() == "workspace" && ws["name"].asString() == current) {
        res[output["name"].asString()] = &ws;
      }
    }
  }
  return res;
}

const Json::Value* focusedWorkspace(const Json::Value& root) {
  for (const auto& output : root["nodes"]) {
    for (const auto& ws : output["nodes"]) {
      if (ws["type"].asString() == "workspace" && isActive(ws)) {
        return &ws;
      }
    }
  }
  return nullptr;
}

// The workspace's top-level tabbed container, whose tab row sway's
// "tab_titles hide" (local patch) hides: the workspace itself, or one reached
// through splits that each hold only it. A tabbed or stacked ancestor would
// draw its own titles, so anything below one is nested and keeps its row.
const Json::Value* topTabbed(const Json::Value& ws) {
  const Json::Value* node = &ws;
  while ((*node)["layout"].asString() != "tabbed") {
    auto layout = (*node)["layout"].asString();
    if ((layout != "splith" && layout != "splitv") || (*node)["nodes"].size() != 1) {
      return nullptr;
    }
    node = &(*node)["nodes"][0];
  }
  return (*node)["nodes"].empty() ? nullptr : node;
}

void windows(const Json::Value& node, std::vector<const Json::Value*>& out) {
  if (!node["pid"].isNull()) {
    out.push_back(&node);
  }
  for (const auto& child : node["nodes"]) {
    windows(child, out);
  }
  for (const auto& child : node["floating_nodes"]) {
    windows(child, out);
  }
}

}  // namespace

Tabs::Tabs(const std::string& id, const Bar& bar, const Json::Value& config, Mode mode)
    : AModule(config, mode == Mode::Tabs ? "tabs" : "away", id, false, false),
      bar_(bar),
      mode_(mode),
      icon_size_(config["icon-size"].isUInt() ? config["icon-size"].asInt() : 18),
      box_(bar.orientation, 0) {
  scroll_.set_name(name_);
  if (!id.empty()) {
    scroll_.get_style_context()->add_class(id);
  }
  scroll_.get_style_context()->add_class(MODULE_CLASS);
  bool horizontal = bar.orientation == Gtk::ORIENTATION_HORIZONTAL;
  scroll_.set_policy(horizontal ? Gtk::POLICY_EXTERNAL : Gtk::POLICY_NEVER,
                     horizontal ? Gtk::POLICY_NEVER : Gtk::POLICY_EXTERNAL);
  scroll_.set_propagate_natural_width(true);
  scroll_.set_propagate_natural_height(true);
  scroll_.set_shadow_type(Gtk::SHADOW_NONE);
  scroll_.add(box_);
  box_.signal_size_allocate().connect([this](Gtk::Allocation& /*alloc*/) { scrollToFocus(); });
  event_box_.add(scroll_);

  if (config_["icon-theme"].isArray()) {
    for (const auto& theme : config_["icon-theme"]) {
      icon_loader_.add_custom_icon_theme(theme.asString());
    }
  } else if (config_["icon-theme"].isString()) {
    icon_loader_.add_custom_icon_theme(config_["icon-theme"].asString());
  }

  // Layout changes ($mod+w, $mod+e, ...) send no window event; binding events,
  // sent after the bound command ran, cover them.
  ipc_.subscribe(R"(["window","workspace","binding"])");
  ipc_.signal_event.connect(sigc::mem_fun(*this, &Tabs::onEvent));
  ipc_.signal_cmd.connect(sigc::mem_fun(*this, &Tabs::onCmd));
  ipc_.sendCmd(IPC_GET_TREE);
  ipc_.setWorker([this] {
    try {
      ipc_.handleEvent();
    } catch (const std::exception& e) {
      spdlog::error("{}: {}", name_, e.what());
    }
  });
}

void Tabs::onEvent(const struct Ipc::ipc_response& /*res*/) {
  try {
    ipc_.sendCmd(IPC_GET_TREE);
  } catch (const std::exception& e) {
    spdlog::error("{}: {}", name_, e.what());
  }
}

void Tabs::onCmd(const struct Ipc::ipc_response& res) {
  if (res.type != IPC_GET_TREE) {
    return;
  }
  try {
    auto root = parser_.parse(res.payload);
    auto items = mode_ == Mode::Tabs ? tabItems(root) : awayItems(root);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      items_ = std::move(items);
    }
    dp.emit();
  } catch (const std::exception& e) {
    spdlog::error("{}: {}", name_, e.what());
  }
}

Tabs::Item Tabs::makeItem(const Json::Value& con, const Json::Value& window) const {
  Item item;
  item.id = con["id"].asInt64();
  item.app_id = appOf(window);
  auto title = window["name"].isString() ? window["name"].asString() : item.app_id;
  item.label = title.empty() ? "?" : title;
  item.tooltip = item.label;
  if (con["urgent"].asBool()) {
    item.classes.emplace_back("urgent");
  }
  return item;
}

std::vector<Tabs::Item> Tabs::tabItems(const Json::Value& root) const {
  auto visible = visibleWorkspaces(root);
  auto it = visible.find(bar_.output->name);
  const Json::Value* ws = it != visible.end() ? it->second : focusedWorkspace(root);
  const Json::Value* con = ws ? topTabbed(*ws) : nullptr;
  if (!con) {
    return {};
  }

  // The tab on top; a workspace's focus list also holds its floating windows.
  int64_t top = -1;
  for (const auto& id : (*con)["focus"]) {
    auto found = std::ranges::any_of((*con)["nodes"], [&](const Json::Value& tab) {
      return tab["id"].asInt64() == id.asInt64();
    });
    if (found) {
      top = id.asInt64();
      break;
    }
  }

  std::vector<Item> items;
  for (const auto& tab : (*con)["nodes"]) {
    auto item = makeItem(tab, leaf(tab));
    if (isActive(tab)) {
      item.classes.emplace_back("active");
    } else if (item.id == top) {
      item.classes.emplace_back("current");
    }
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<Tabs::Item> Tabs::awayItems(const Json::Value& root) const {
  auto visible = visibleWorkspaces(root);
  std::vector<Item> items;
  for (const auto& output : root["nodes"]) {
    for (const auto& ws : output["nodes"]) {
      auto name = ws["name"].asString();
      bool shown = std::ranges::any_of(visible, [&](const auto& v) { return v.second == &ws; });
      if (ws["type"].asString() != "workspace" || shown || name.starts_with("__")) {
        continue;
      }
      std::vector<const Json::Value*> wins;
      windows(ws, wins);
      for (const auto* win : wins) {
        auto item = makeItem(*win, *win);
        item.tooltip = fmt::format("{}: {}", name, item.label);
        item.label.clear();
        items.push_back(std::move(item));
      }
    }
  }
  return items;
}

std::string Tabs::mappedAppId(const std::string& app_id) const {
  const auto& mapping = config_["app_ids-mapping"];
  if (mapping.isObject() && mapping[app_id].isString()) {
    return mapping[app_id].asString();
  }
  return app_id;
}

void Tabs::loadIcon(Entry& entry) {
  auto app_id = mappedAppId(entry.app_id);
  auto found = app_infos_.find(app_id);
  if (found == app_infos_.end()) {
    found = app_infos_.emplace(app_id, IconLoader::get_app_info_from_app_id_list(app_id)).first;
  }
  if (!found->second || !icon_loader_.image_load_icon(entry.icon, found->second, icon_size_)) {
    entry.icon.set_from_icon_name("application-x-executable", Gtk::ICON_SIZE_BUTTON);
    entry.icon.set_pixel_size(icon_size_);
  }
  entry.icon_loaded = true;
}

std::unique_ptr<Tabs::Entry> Tabs::createEntry(int64_t id) {
  auto entry = std::make_unique<Entry>();
  entry->button.set_relief(Gtk::RELIEF_NONE);
  entry->content.set_spacing(mode_ == Mode::Tabs ? 6 : 0);
  entry->content.pack_start(entry->icon, false, false);
  if (mode_ == Mode::Tabs) {
    entry->label.set_ellipsize(Pango::ELLIPSIZE_END);
    entry->label.set_xalign(0);
    entry->label.set_single_line_mode(true);
    entry->content.pack_start(entry->label, true, true);
  }
  entry->button.add(entry->content);

  entry->button.signal_clicked().connect(
      [this, id] { ipc_.sendCmd(IPC_COMMAND, fmt::format("[con_id={}] focus", id)); });
  entry->button.signal_button_press_event().connect(
      [this, id](GdkEventButton* ev) {
        if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
          ipc_.sendCmd(IPC_COMMAND, fmt::format("[con_id={}] kill", id));
          return true;
        }
        return false;
      },
      false);

  // Icons render through the widget's window, so load them once realized.
  auto* raw = entry.get();
  entry->icon.signal_realize().connect([this, raw] {
    if (!raw->icon_loaded) {
      loadIcon(*raw);
    }
  });
  return entry;
}

void Tabs::scrollToFocus() {
  auto it = entries_.find(focus_id_);
  if (it == entries_.end()) {
    return;
  }
  auto alloc = it->second->button.get_allocation();
  bool horizontal = bar_.orientation == Gtk::ORIENTATION_HORIZONTAL;
  auto adj = horizontal ? scroll_.get_hadjustment() : scroll_.get_vadjustment();
  auto start = horizontal ? alloc.get_x() : alloc.get_y();
  auto size = horizontal ? alloc.get_width() : alloc.get_height();
  adj->clamp_page(start, start + size);
}

auto Tabs::update() -> void {
  std::vector<Item> items;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    items = items_;
  }

  for (auto it = entries_.begin(); it != entries_.end();) {
    bool keep = std::ranges::any_of(items, [&](const Item& i) { return i.id == it->first; });
    if (keep) {
      ++it;
    } else {
      box_.remove(it->second->button);
      it = entries_.erase(it);
    }
  }

  bool expand = mode_ == Mode::Tabs && expandEnabled();
  int position = 0;
  int64_t focus_id = -1;
  for (const auto& item : items) {
    if (std::ranges::find(item.classes, "active") != item.classes.end() ||
        (focus_id < 0 && std::ranges::find(item.classes, "current") != item.classes.end())) {
      focus_id = item.id;
    }
    auto& slot = entries_[item.id];
    if (!slot) {
      slot = createEntry(item.id);
      box_.pack_start(slot->button, expand, expand);
      slot->button.show_all();
    }
    auto& entry = *slot;
    if (entry.app_id != item.app_id) {
      entry.app_id = item.app_id;
      entry.icon_loaded = false;
      if (entry.icon.get_realized()) {
        loadIcon(entry);
      }
    }
    if (mode_ == Mode::Tabs && entry.label.get_text() != item.label) {
      entry.label.set_text(item.label);
    }
    if (tooltipEnabled() && entry.button.get_tooltip_text() != item.tooltip) {
      entry.button.set_tooltip_text(item.tooltip);
    }
    if (entry.classes != item.classes) {
      auto style = entry.button.get_style_context();
      for (const auto& cls : kStateClasses) {
        style->remove_class(cls);
      }
      for (const auto& cls : item.classes) {
        style->add_class(cls);
      }
      entry.classes = item.classes;
    }
    box_.reorder_child(entry.button, position++);
  }

  if (focus_id != focus_id_) {
    focus_id_ = focus_id;
    scrollToFocus();
  }

  // Hide the whole module when there is nothing to show, so its margins go too.
  event_box_.set_visible(!items.empty());
  AModule::update();
}

}  // namespace waybar::modules::sway
