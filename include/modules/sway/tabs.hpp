#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "client.hpp"
#include "modules/sway/ipc/client.hpp"
#include "util/icon_loader.hpp"
#include "util/json.hpp"

namespace waybar::modules::sway {

/*
 * One button per window, like wlr/taskbar, built from sway's tree.
 *
 * sway/tabs: the tabs of the top-level tabbed container on the bar's workspace
 * (the workspace itself, or a tabbed container reached through splits that
 * each hold only it), with icon and title.
 *
 * sway/away: icons of the windows on workspaces that no output is showing.
 *
 * The buttons sit in a scrolled window without scrollbars, so however many
 * windows there are the module never pushes the bar past the output; the
 * wheel scrolls and the focused tab is kept in view.
 */
class Tabs : public AModule, public sigc::trackable {
 public:
  enum class Mode { Tabs, Away };

  Tabs(const std::string& id, const Bar& bar, const Json::Value& config, Mode mode);
  ~Tabs() override = default;
  auto update() -> void override;

 private:
  struct Item {
    int64_t id;
    std::string app_id;
    std::string label;
    std::string tooltip;
    std::vector<std::string> classes;
  };

  struct Entry {
    Gtk::Button button;
    Gtk::Box content;
    Gtk::Image icon;
    Gtk::Label label;
    std::string app_id;
    std::vector<std::string> classes;
    bool icon_loaded = false;
  };

  void onEvent(const struct Ipc::ipc_response& res);
  void onCmd(const struct Ipc::ipc_response& res);
  std::vector<Item> tabItems(const Json::Value& root) const;
  std::vector<Item> awayItems(const Json::Value& root) const;
  Item makeItem(const Json::Value& con, const Json::Value& window) const;
  std::unique_ptr<Entry> createEntry(int64_t id);
  void loadIcon(Entry& entry);
  std::string mappedAppId(const std::string& app_id) const;
  void scrollToFocus();

  const Bar& bar_;
  const Mode mode_;
  const int icon_size_;
  Gtk::ScrolledWindow scroll_;
  Gtk::Box box_;
  int64_t focus_id_ = -1;
  std::map<int64_t, std::unique_ptr<Entry>> entries_;
  std::map<std::string, Glib::RefPtr<Gio::DesktopAppInfo>> app_infos_;
  IconLoader icon_loader_;
  std::vector<Item> items_;
  util::JsonParser parser_;
  std::mutex mutex_;
  Ipc ipc_;
};

}  // namespace waybar::modules::sway
