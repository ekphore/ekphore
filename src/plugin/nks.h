// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <libnks/client.h>
namespace ekphore::nks {
using libnks::daemon::Sound;
using State = libnks::ClientState;
using Action = libnks::ClientAction;
using libnks::apply_action;
using libnks::daemon::apply; // the diagnostic fixtures still use this
using libnks::daemon::browser_model;
using libnks::daemon::decode;
using libnks::daemon::encode;
using libnks::daemon::generation;
using libnks::daemon::Instance;
using libnks::daemon::instances;
using libnks::daemon::run_host;
using libnks::daemon::send_action;
std::filesystem::path config_file();
std::filesystem::path runtime_directory();
class Client {
  public:
    explicit Client(std::filesystem::path root = runtime_directory(),
            libnks::OdrInstanceType type = libnks::OdrInstanceType::vst3);
    ~Client();
    bool available() const { return client.available(); }
    void focus() { client.focus(); }
    const std::string &id() const { return client.id(); }
    void poll(State &, const std::function<void(const Action &)> &);
    void publish(State &);
    void restore_browser(State &);
    void discover(State &);

  private:
    libnks::Client client;
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ekphore::nks
