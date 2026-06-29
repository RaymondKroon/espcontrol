#include "host_web_server.h"

namespace esphome {
namespace espcontrol_host_web_server {

static const char *const TAG = "host_web";

void HostWebServer::setup() {
  this->load_saved_config_();
  this->running_ = true;
  this->server_thread_ = std::thread([this]() { this->server_loop_(); });
}

void HostWebServer::loop() {
  std::deque<std::shared_ptr<PendingOperation>> pending;
  {
    std::lock_guard<std::mutex> lock(this->operations_mutex_);
    pending.swap(this->operations_);
  }
  for (auto &op : pending) {
    op->result = this->perform_entity_post_(op->domain, op->object_id, op->action, op->params);
    {
      std::lock_guard<std::mutex> lock(this->operations_mutex_);
      op->done = true;
    }
    this->operations_cv_.notify_all();
  }
}

void HostWebServer::dump_config() {
  ESP_LOGCONFIG(TAG, "EspControl host web server:");
  ESP_LOGCONFIG(TAG, "  URL: http://0.0.0.0:%u/", this->port_);
  ESP_LOGCONFIG(TAG, "  JS path: %s", this->js_path_.c_str());
}

void HostWebServer::on_shutdown() {
  this->running_ = false;
  if (this->server_fd_ >= 0) {
    shutdown(this->server_fd_, SHUT_RDWR);
    close(this->server_fd_);
    this->server_fd_ = -1;
  }
  if (this->server_thread_.joinable()) this->server_thread_.join();
}

void HostWebServer::server_loop_() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    ESP_LOGE(TAG, "socket() failed: %s", strerror(errno));
    return;
  }
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind(:%u) failed: %s", this->port_, strerror(errno));
    close(fd);
    return;
  }
  if (listen(fd, 8) < 0) {
    ESP_LOGE(TAG, "listen() failed: %s", strerror(errno));
    close(fd);
    return;
  }
  this->server_fd_ = fd;
  ESP_LOGI(TAG, "EspControl config page listening on http://0.0.0.0:%u/", this->port_);

  while (this->running_) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{};
    tv.tv_sec = 1;
    int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ready <= 0) continue;
    int client = accept(fd, nullptr, nullptr);
    if (client < 0) continue;
    this->handle_client_(client);
    close(client);
  }
}

void HostWebServer::handle_client_(int client_fd) {
  std::string req;
  char buf[1024];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    if (n <= 0) return;
    req.append(buf, static_cast<size_t>(n));
  }
  auto first_end = req.find("\r\n");
  if (first_end == std::string::npos) return;
  std::istringstream first(req.substr(0, first_end));
  std::string method, target, version;
  first >> method >> target >> version;
  if (method.empty() || target.empty()) return;

  size_t content_length = 0;
  size_t header_end = req.find("\r\n\r\n");
  size_t line_start = first_end + 2;
  while (line_start < header_end) {
    size_t line_end = req.find("\r\n", line_start);
    if (line_end == std::string::npos || line_end > header_end) break;
    std::string line = req.substr(line_start, line_end - line_start);
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const std::string prefix = "content-length:";
    if (lower.rfind(prefix, 0) == 0) content_length = strtoul(line.c_str() + prefix.size(), nullptr, 10);
    line_start = line_end + 2;
  }
  size_t body_start = header_end == std::string::npos ? req.size() : header_end + 4;
  while (req.size() < body_start + content_length && req.size() < body_start + content_length + 16384) {
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    req.append(buf, static_cast<size_t>(n));
  }
  std::string body;
  if (body_start < req.size()) body = req.substr(body_start, std::min(content_length, req.size() - body_start));
  this->route_(client_fd, method, target, body);
}

void HostWebServer::send_response_(int client_fd, int status, const std::string &type, const std::string &body,
                                   const std::string &extra_headers) {
  const char *reason = status == 200 ? "OK" : status == 204 ? "No Content" : status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" : "Error";
  std::ostringstream out;
  out << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
  out << "Content-Type: " << type << "\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Cache-Control: no-store\r\n";
  out << "Connection: close\r\n";
  if (!extra_headers.empty()) out << extra_headers;
  out << "\r\n" << body;
  std::string response = out.str();
  send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
}

void HostWebServer::send_not_found_(int client_fd) {
  this->send_response_(client_fd, 404, "text/plain", "Not found\n");
}

void HostWebServer::send_events_(int client_fd) {
  std::string title = json_escape_(App.get_friendly_name().str());
  std::string body = "event: ping\ndata: {\"title\":\"" + title + "\"}\n\n";
  this->send_response_(client_fd, 200, "text/event-stream", body);
}

void HostWebServer::route_(int client_fd, const std::string &method, const std::string &target, const std::string &body) {
  std::string path = target;
  std::string query;
  auto q = path.find('?');
  if (q != std::string::npos) {
    query = path.substr(q + 1);
    path = path.substr(0, q);
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    this->send_response_(client_fd, 200, "text/html; charset=utf-8", this->page_html_());
    return;
  }
  if (method == "GET" && path == "/www.js") {
    this->send_response_(client_fd, 200, "application/javascript; charset=utf-8", this->read_js_());
    return;
  }
  if (method == "GET" && path == "/events") {
    this->send_events_(client_fd);
    return;
  }
  if (method == "GET" && path == "/espcontrol/version") {
    this->send_response_(client_fd, 200, "application/json", "{\"version\":\"host\"}");
    return;
  }
  if (method == "GET" && path == "/local_sensors") {
    this->send_response_(client_fd, 200, "application/json", this->local_sensors_json_());
    return;
  }
  if (method == "GET" && path == "/local_actions") {
    this->send_response_(client_fd, 200, "application/json", this->local_actions_json_());
    return;
  }

  std::vector<std::string> parts;
  size_t start = 1;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) parts.push_back(url_decode_(part));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }

  if (parts.size() >= 2) {
    const std::string &domain = parts[0];
    const std::string &object_id = parts[1];
    if (method == "GET" && parts.size() == 2) {
      std::string json = this->entity_json_(domain, object_id);
      if (!json.empty()) this->send_response_(client_fd, 200, "application/json", json);
      else this->send_not_found_(client_fd);
      return;
    }
    if (method == "POST" && parts.size() >= 3) {
      std::string action = parts[2];
      if (parts.size() >= 4) action += "/" + parts[3];
      bool restart_requested = domain == "button" && web_id_(object_id) == "apply_configuration";
      if (this->entity_post_(domain, object_id, action, query, body)) {
        this->send_response_(client_fd, 200, "application/json", "{\"ok\":true}");
        if (restart_requested) this->restart_process_();
      } else {
        this->send_not_found_(client_fd);
      }
      return;
    }
  }

  this->send_not_found_(client_fd);
}

std::string HostWebServer::read_js_() const {
  std::vector<std::string> candidates = {this->js_path_};
  candidates.push_back("docs/public/webserver/esp32-p4-86/www.js");
  candidates.push_back("../../docs/public/webserver/esp32-p4-86/www.js");
  for (const auto &path : candidates) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) continue;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  ESP_LOGE(TAG, "Could not read EspControl web UI JS from %s", this->js_path_.c_str());
  return "console.error('EspControl web UI JS not found');";
}

std::string HostWebServer::page_html_() const {
  return "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>EspControl Desktop SDL</title></head><body><script src=\"/www.js\"></script></body></html>";
}

std::string HostWebServer::entity_json_(const std::string &domain, const std::string &object_id) const {
  if (domain == "text") {
    for (auto *e : App.get_texts()) if (same_id_(e, object_id)) {
      return "{\"id\":\"text-" + object_id_(e) + "\",\"name\":\"" + json_escape_(e->get_name().str()) +
             "\",\"state\":\"" + json_escape_(e->state) + "\",\"value\":\"" + json_escape_(e->state) + "\"}";
    }
  } else if (domain == "select") {
    for (auto *e : App.get_selects()) if (same_id_(e, object_id)) {
      std::string value = e->current_option().str();
      std::string json = "{\"id\":\"select-" + object_id_(e) + "\",\"name\":\"" + json_escape_(e->get_name().str()) +
                         "\",\"state\":\"" + json_escape_(value) + "\",\"value\":\"" + json_escape_(value) + "\",\"options\":[";
      bool first = true;
      for (auto *opt : e->traits.get_options()) {
        if (!first) json += ',';
        first = false;
        json += "\"" + json_escape_(opt ? opt : "") + "\"";
      }
      json += "]}";
      return json;
    }
  } else if (domain == "number") {
    for (auto *e : App.get_numbers()) if (same_id_(e, object_id)) {
      return "{\"id\":\"number-" + object_id_(e) + "\",\"name\":\"" + json_escape_(e->get_name().str()) +
             "\",\"state\":" + state_text_(e->state) + ",\"value\":" + state_text_(e->state) +
             ",\"min\":" + state_text_(e->traits.get_min_value()) + ",\"max\":" + state_text_(e->traits.get_max_value()) +
             ",\"step\":" + state_text_(e->traits.get_step()) + "}";
    }
  } else if (domain == "switch") {
    for (auto *e : App.get_switches()) if (same_id_(e, object_id)) {
      return "{\"id\":\"switch-" + object_id_(e) + "\",\"name\":\"" + json_escape_(e->get_name().str()) +
             "\",\"state\":\"" + std::string(e->state ? "ON" : "OFF") + "\",\"value\":" +
             std::string(e->state ? "true" : "false") + "}";
    }
  } else if (domain == "button") {
    for (auto *e : App.get_buttons()) if (same_id_(e, object_id)) {
      return "{\"id\":\"button-" + object_id_(e) + "\",\"name\":\"" + json_escape_(e->get_name().str()) + "\"}";
    }
  }
  return "";
}

bool HostWebServer::entity_post_(const std::string &domain, const std::string &object_id, const std::string &action,
                                 const std::string &query, const std::string &body) {
  std::string params = query;
  if (!body.empty()) params += (params.empty() ? "" : "&") + body;
  auto op = std::make_shared<PendingOperation>();
  op->domain = domain;
  op->object_id = object_id;
  op->action = action;
  op->params = params;
  {
    std::lock_guard<std::mutex> lock(this->operations_mutex_);
    this->operations_.push_back(op);
  }
  this->operations_cv_.notify_all();
  std::unique_lock<std::mutex> lock(this->operations_mutex_);
  if (!this->operations_cv_.wait_for(lock, std::chrono::seconds(2), [&]() { return op->done; })) {
    ESP_LOGW(TAG, "Timed out applying web request for %s/%s/%s", domain.c_str(), object_id.c_str(), action.c_str());
    return false;
  }
  return op->result;
}

bool HostWebServer::perform_entity_post_(const std::string &domain, const std::string &object_id, const std::string &action,
                                         const std::string &params) {
  if (domain == "text" && action == "set") {
    std::string value = query_value_(params, "value");
    for (auto *e : App.get_texts()) if (same_id_(e, object_id)) { e->make_call().set_value(value).perform(); this->save_config_value_(domain, object_id_(e), value); if (global_preferences != nullptr) global_preferences->sync(); return true; }
  } else if (domain == "select" && action == "set") {
    std::string value = query_value_(params, "option");
    for (auto *e : App.get_selects()) if (same_id_(e, object_id)) { e->make_call().set_option(value).perform(); this->save_config_value_(domain, object_id_(e), value); if (global_preferences != nullptr) global_preferences->sync(); return true; }
  } else if (domain == "number" && action == "set") {
    float value = strtof(query_value_(params, "value").c_str(), nullptr);
    for (auto *e : App.get_numbers()) if (same_id_(e, object_id)) { e->make_call().set_value(value).perform(); this->save_config_value_(domain, object_id_(e), state_text_(value)); if (global_preferences != nullptr) global_preferences->sync(); return true; }
  } else if (domain == "switch") {
    for (auto *e : App.get_switches()) if (same_id_(e, object_id)) {
      if (action == "turn_on") e->turn_on();
      else if (action == "turn_off") e->turn_off();
      else if (action == "toggle") e->toggle();
      else return false;
      this->save_config_value_(domain, object_id_(e), e->state ? "1" : "0");
      if (global_preferences != nullptr) global_preferences->sync();
      return true;
    }
  } else if (domain == "button" && action == "press") {
    if (web_id_(object_id) == "apply_configuration") {
      if (global_preferences != nullptr) global_preferences->sync();
      return true;
    }
    for (auto *e : App.get_buttons()) if (same_id_(e, object_id)) { e->press(); return true; }
  }
  return false;
}

std::string HostWebServer::local_sensors_json_() const {
  std::string json = "[";
  bool first = true;
  auto append = [&](const std::string &key, const std::string &name, const std::string &unit, const char *type, bool internal) {
    if (!first) json += ',';
    first = false;
    json += "{\"key\":\"" + json_escape_(key) + "\",\"name\":\"" + json_escape_(name) + "\",\"unit\":\"" + json_escape_(unit) +
            "\",\"type\":\"" + type + "\"";
    if (internal) json += ",\"internal\":true";
    json += "}";
  };
  for (auto *s : App.get_sensors()) append(object_id_(s), s->get_name().str(), s->get_unit_of_measurement_ref().str(), "numeric", s->is_internal());
  for (auto *ts : App.get_text_sensors()) append(object_id_(ts), ts->get_name().str(), "", "text", ts->is_internal());
  json += "]";
  return json;
}

std::string HostWebServer::local_actions_json_() const { return "[]"; }

std::string HostWebServer::config_path_() const {
  const char *home = getenv("HOME");
  if (home == nullptr) return "/tmp/espcontrol-desktop-sdl-web-config.tsv";
  return std::string(home) + "/.esphome/prefs/" + App.get_name() + "-web-config.tsv";
}

void HostWebServer::load_saved_config_() {
  std::ifstream f(this->config_path_());
  if (!f.good()) return;
  std::string line;
  while (std::getline(f, line)) {
    size_t first = line.find('\t');
    size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    std::string domain = line.substr(0, first);
    std::string object_id = line.substr(first + 1, second - first - 1);
    std::string value = url_decode_(line.substr(second + 1));
    if (domain == "text") {
      for (auto *e : App.get_texts()) if (same_id_(e, object_id)) { e->publish_state(value); break; }
    } else if (domain == "select") {
      for (auto *e : App.get_selects()) if (same_id_(e, object_id)) { e->make_call().set_option(value).perform(); break; }
    } else if (domain == "number") {
      float number_value = strtof(value.c_str(), nullptr);
      for (auto *e : App.get_numbers()) if (same_id_(e, object_id)) { e->make_call().set_value(number_value).perform(); break; }
    } else if (domain == "switch") {
      for (auto *e : App.get_switches()) if (same_id_(e, object_id)) { if (value == "1" || value == "true" || value == "ON") e->turn_on(); else e->turn_off(); break; }
    }
  }
}

void HostWebServer::save_config_value_(const std::string &domain, const std::string &object_id, const std::string &value) const {
  std::map<std::string, std::string> values;
  std::ifstream in(this->config_path_());
  std::string line;
  while (std::getline(in, line)) {
    size_t first = line.find('\t');
    size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    values[line.substr(0, second)] = line.substr(second + 1);
  }
  values[domain + "\t" + object_id] = url_encode_(value);
  std::ofstream out(this->config_path_(), std::ios::trunc);
  for (const auto &item : values) out << item.first << '\t' << item.second << '\n';
}

std::string HostWebServer::url_decode_(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size()) {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
      i += 2;
    } else if (in[i] == '+') out.push_back(' ');
    else out.push_back(in[i]);
  }
  return out;
}

std::string HostWebServer::url_encode_(const std::string &in) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

std::string HostWebServer::query_value_(const std::string &query, const std::string &key) {
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = part.find('=');
    std::string k = url_decode_(part.substr(0, eq));
    if (k == key) return url_decode_(eq == std::string::npos ? "" : part.substr(eq + 1));
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
}

std::string HostWebServer::json_escape_(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string HostWebServer::object_id_(const EntityBase *entity) {
  char buf[OBJECT_ID_MAX_LEN] = {};
  auto ref = entity->get_object_id_to(std::span<char, OBJECT_ID_MAX_LEN>(buf, OBJECT_ID_MAX_LEN));
  return ref.str();
}

std::string HostWebServer::web_id_(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (ch == ' ') {
      out.push_back('_');
      continue;
    }
    char lower = static_cast<char>(std::tolower(ch));
    if ((lower >= 'a' && lower <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      out.push_back(lower);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

bool HostWebServer::same_id_(const EntityBase *entity, const std::string &object_id) {
  const std::string normalized = web_id_(object_id);
  const std::string entity_object_id = object_id_(entity);
  const std::string entity_name_id = web_id_(entity->get_name().str());
  return entity_object_id == object_id || entity_object_id == normalized ||
         entity_name_id == object_id || entity_name_id == normalized;
}

void HostWebServer::restart_process_() {
  std::thread([this]() {
    ESP_LOGI(TAG, "Restarting host process to apply configuration");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    this->running_ = false;
    if (this->server_fd_ >= 0) {
      shutdown(this->server_fd_, SHUT_RDWR);
      close(this->server_fd_);
      this->server_fd_ = -1;
    }
    char exe_path[4096] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
      ESP_LOGE(TAG, "Could not resolve /proc/self/exe for host restart: %s", strerror(errno));
      return;
    }
    exe_path[len] = '\0';
    char *const argv[] = {exe_path, nullptr};
    execv(exe_path, argv);
    ESP_LOGE(TAG, "execv(%s) failed: %s", exe_path, strerror(errno));
  }).detach();
}

std::string HostWebServer::state_text_(float value) {
  if (std::isnan(value)) return "null";
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6g", value);
  return buf;
}

}  // namespace espcontrol_host_web_server
}  // namespace esphome
