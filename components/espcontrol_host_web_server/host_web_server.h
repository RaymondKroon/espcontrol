#pragma once

#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace esphome {
namespace espcontrol_host_web_server {

class HostWebServer : public Component {
 public:
  void set_port(uint16_t port) { this->port_ = port; }
  void set_js_path(const std::string &path) { this->js_path_ = path; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return 850.0f; }

 protected:
  uint16_t port_{8080};
  std::string js_path_;
  std::atomic<bool> running_{false};
  int server_fd_{-1};
  std::thread server_thread_;

  struct PendingOperation {
    std::string domain;
    std::string object_id;
    std::string action;
    std::string params;
    bool done{false};
    bool result{false};
  };
  std::mutex operations_mutex_;
  std::condition_variable operations_cv_;
  std::deque<std::shared_ptr<PendingOperation>> operations_;

  void server_loop_();
  void handle_client_(int client_fd);
  void send_response_(int client_fd, int status, const std::string &type, const std::string &body,
                      const std::string &extra_headers = "");
  void send_not_found_(int client_fd);
  void send_events_(int client_fd);
  void route_(int client_fd, const std::string &method, const std::string &target, const std::string &body);

  std::string read_js_() const;
  std::string page_html_() const;
  std::string entity_json_(const std::string &domain, const std::string &object_id) const;
  bool entity_post_(const std::string &domain, const std::string &object_id, const std::string &action,
                    const std::string &query, const std::string &body);
  bool perform_entity_post_(const std::string &domain, const std::string &object_id, const std::string &action,
                            const std::string &params);
  std::string local_sensors_json_() const;
  std::string local_actions_json_() const;
  void load_saved_config_();
  void save_config_value_(const std::string &domain, const std::string &object_id, const std::string &value) const;
  std::string config_path_() const;

  static std::string url_decode_(const std::string &in);
  static std::string url_encode_(const std::string &in);
  static std::string query_value_(const std::string &query, const std::string &key);
  static std::string json_escape_(const std::string &in);
  static std::string object_id_(const EntityBase *entity);
  static std::string web_id_(const std::string &value);
  static bool same_id_(const EntityBase *entity, const std::string &object_id);
  void restart_process_();
  static std::string state_text_(float value);
};

}  // namespace espcontrol_host_web_server
}  // namespace esphome
