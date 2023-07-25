/*
 * Copyright (c) 2023, Adam Simpkins
 *
 * Useful docs on Xlib selection behavior and requirements:
 * https://tronche.com/gui/x/icccm/sec-2.html
 */
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string_view>
#include <type_traits>
#include <vector>

#include <getopt.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <fmt/core.h>
#include <fmt/compile.h>

namespace {

constexpr uint32_t xfixes_version_major = 5;
constexpr uint32_t xfixes_version_minor = 1;
enum class LogLevel {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};
LogLevel g_log_level = LogLevel::Warn;

namespace detail {
struct free_deleter {
  template <typename T> void operator()(T *ptr) {
    std::free(const_cast<std::remove_const_t<T> *>(ptr));
  }
};
} // namespace detail

// Smart pointer that frees with std::free() rather than delete
template <typename T>
using c_unique_ptr = std::unique_ptr<T, detail::free_deleter>;

template <typename T>
c_unique_ptr<T> free_wrapper(T* ptr) {
  return c_unique_ptr<T>(ptr);
}

template <typename S, typename... Args>
inline void dbg(const S& format_str, Args&&... args) {
  if (g_log_level < LogLevel::Debug) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto now_seconds = std::chrono::floor<std::chrono::seconds>(now);
  const time_t now_time_t = std::chrono::system_clock::to_time_t(now_seconds);
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - now_seconds);

  struct tm tm;
  localtime_r(&now_time_t, &tm);
  std::array<char, 20> time_buf;
  auto time_len = strftime(time_buf.data(), time_buf.size(), "%H:%M:%S", &tm);

  const auto msg = fmt::format(format_str, std::forward<Args>(args)...);
  const auto full_msg =
      fmt::format("dbg:{}.{:03d}: {}\n",
                  std::string_view(time_buf.data(), time_len), ms.count(), msg);

  fputs(full_msg.c_str(), stderr);
}

template <typename S, typename... Args>
inline void info(const S& format_str, Args&&... args) {
  if (g_log_level < LogLevel::Info) {
    return;
  }
  fprintf(stdout, "%s\n",
          fmt::format(format_str, std::forward<Args>(args)...).c_str());
}

template <typename S, typename... Args>
inline void warn(const S& format_str, Args&&... args) {
  if (g_log_level < LogLevel::Warn) {
    return;
  }
  fprintf(stderr, "warning: %s\n",
          fmt::format(format_str, std::forward<Args>(args)...).c_str());
}

class XcbEvent : public c_unique_ptr<xcb_generic_event_t> {
public:
  using c_unique_ptr<xcb_generic_event_t>::c_unique_ptr;

  template <typename EventT> EventT *as() {
    return reinterpret_cast<EventT *>(get());
  }
};

class XcbConn {
public:
  XcbConn() {}
  ~XcbConn() { destroy(); }
  XcbConn(XcbConn &&conn) : conn_(conn.conn_) { conn.conn_ = nullptr; }
  XcbConn &operator=(XcbConn &&conn) {
    destroy();
    conn_ = conn.conn_;
    conn.conn_ = nullptr;
    return *this;
  }

  xcb_connection_t *conn() { return conn_; }

  /**
   * Connect.  Returns the preferred screen number.
   */
  int connect(const char *displayname = nullptr) {
    int preferred_screen = 0;
    auto *conn = xcb_connect(displayname, &preferred_screen);
    auto rc = xcb_connection_has_error(conn);
    if (rc != 0) {
      xcb_disconnect(conn);
      throw std::runtime_error(
          fmt::format("failed to connect to X display: {}", rc));
    }
    conn_ = conn;
    return preferred_screen;
  }

  XcbEvent wait_for_event() {
    auto *event = xcb_wait_for_event(conn_);
    if (!event) {
      throw std::runtime_error("I/O error communicating with X server");
    }
    return XcbEvent(event);
  }

  void flush();
  const xcb_screen_t &get_screen(size_t screen_num);
  xcb_atom_t get_atom(std::string_view name, bool create = true);

private:
  void destroy() {
    if (conn_) {
      xcb_disconnect(conn_);
    }
  }

  xcb_connection_t *conn_ = nullptr;
};

void XcbConn::flush() {
  auto rc = xcb_flush(conn_);
  if (rc <= 0) {
    throw std::runtime_error("error flushing XCB requests");
  }
}

const xcb_screen_t &XcbConn::get_screen(size_t screen_num) {
  auto setup = xcb_get_setup(conn_);
  if (!setup) {
    throw std::runtime_error("unable to get xcb setup information");
  }

  auto iter = xcb_setup_roots_iterator(setup);
  size_t idx = 0;
  while (true) {
    if (!iter.rem) {
      throw std::runtime_error(
          fmt::format("screen number {} does not exist", screen_num));
    }
    if (idx == screen_num) {
      return *iter.data;
    }
    xcb_screen_next(&iter);
    ++idx;
  }
}

xcb_atom_t XcbConn::get_atom(std::string_view name, bool create) {
  xcb_generic_error_t *err = nullptr;
  auto cookie = xcb_intern_atom(conn_, create ? 0 : 1, name.size(),
                                name.data());
  auto reply = free_wrapper(xcb_intern_atom_reply(conn_, cookie, &err));
  if (!reply) {
    int error_code = err ? err->error_code : -1;
    throw std::runtime_error(
        fmt::format("failed to get atom {}: {}", name, error_code));
  }
  auto atom = reply->atom;
  return atom;
}

class Window {
public:
  Window() = default;
  Window(XcbConn &conn) : conn_(&conn), window_(xcb_generate_id(conn.conn())) {}
  ~Window() { destroy(); }
  Window(Window &&win) : conn_(win.conn_), window_(win.window_) {
    win.window_ = XCB_WINDOW_NONE;
  }
  Window &operator=(Window &&win) {
    destroy();
    conn_ = win.conn_;
    window_ = win.window_;
    win.window_ = XCB_WINDOW_NONE;
    return *this;
  }

  xcb_window_t id() const { return window_; }
  XcbConn &conn() const { return *conn_; }
  xcb_connection_t *raw_conn() const { return conn_->conn(); }

private:
  void destroy() {
    if (window_ != XCB_WINDOW_NONE) {
      xcb_destroy_window(conn_->conn(), window_);
    }
  }

  XcbConn *conn_ = nullptr;
  xcb_window_t window_ = XCB_WINDOW_NONE;
};

/**
 * This class represents a pending XConvertSelection request from a client.
 *
 * We will send a corresponding XConvertSelection request to the owner of the
 * sync'ed clipboard, and proxy the data back to the original requestor.
 */
class Request {
public:
  explicit Request(xcb_atom_t local_prop) : local_prop_(local_prop) {}
  Request(xcb_atom_t local_prop, const xcb_selection_request_event_t *req)
      : local_prop_(local_prop) {
    assign(req);
  }

  bool in_use() const { return requestor_ != XCB_WINDOW_NONE; }
  void assign(const xcb_selection_request_event_t *req) {
    remote_prop_ = req->property;
    selection_ = req->selection;
    target_ = req->target;
    requestor_ = req->requestor;
    timestamp_ = req->time;
    dbg("proxy {}: assigned to requestor {} prop {}", local_prop_, requestor_,
        remote_prop_);
  }
  void unassign() {
    dbg("proxy {}: unassigned", local_prop_);
    remote_prop_ = XCB_ATOM_NONE;
    selection_ = XCB_ATOM_NONE;
    target_ = XCB_ATOM_NONE;
    requestor_ = XCB_WINDOW_NONE;
    timestamp_ = 0;
  }

  xcb_window_t requestor() const { return requestor_; }
  xcb_atom_t local_prop() const { return local_prop_; }
  xcb_atom_t remote_prop() const { return remote_prop_; }
  xcb_atom_t selection() const { return selection_; }
  xcb_atom_t target() const { return target_; }
  xcb_timestamp_t timestamp() const { return timestamp_; }

private:
  const xcb_atom_t local_prop_ = XCB_ATOM_NONE;
  xcb_atom_t remote_prop_ = XCB_ATOM_NONE;
  xcb_atom_t selection_ = XCB_ATOM_NONE;
  xcb_atom_t target_ = XCB_ATOM_NONE;
  xcb_window_t requestor_ = XCB_WINDOW_NONE;
  xcb_timestamp_t timestamp_ = 0;
};

class Syncer;

class Clipboard {
public:
  Clipboard() = default;
  Clipboard(Syncer &sync, std::string_view name);
  Clipboard(Clipboard&&) = default;
  Clipboard& operator=(Clipboard&&) = default;

  xcb_atom_t atom() const { return atom_; }
  const std::string& name() const { return name_; }

  void listen_for_changes();
  void sync_to(const Clipboard &other, xcb_window_t owner,
               xcb_timestamp_t timestamp);
  void lost_ownership(xcb_window_t new_owner, xcb_timestamp_t timestamp);
  void selection_request(const xcb_selection_request_event_t *req);

private:
  bool is_owned() const { return ownership_end_ == 0 && ownership_start_ > 0; }

  xcb_atom_t atom_ = XCB_ATOM_NONE;
  Syncer *sync_ = nullptr;
  std::string name_;
  xcb_atom_t synced_selection_ = XCB_ATOM_NONE;
  xcb_timestamp_t ownership_start_ = 0;
  xcb_timestamp_t ownership_end_ = 1;
};

class Syncer {
public:
  Syncer() = default;

  XcbConn &conn() { return conn_; }
  xcb_connection_t* raw_conn() { return conn_.conn(); }
  Window &window() { return window_; }
  xcb_window_t window_id() const { return window_.id(); }

  void init();
  [[noreturn]] void loop();

  xcb_atom_t allocate_request_entry(const xcb_selection_request_event_t* req);

private:
  Syncer(Syncer&&) = delete;
  Syncer& operator=(Syncer&&) = delete;

  void handle_error(xcb_generic_error_t *err);
  void
  handle_selection_owner_notify(xcb_xfixes_selection_notify_event_t *event);
  void handle_selection_clear(xcb_selection_clear_event_t *event);
  void handle_selection_request(xcb_selection_request_event_t *event);
  void handle_selection_notify(xcb_selection_notify_event_t *event);
  void handle_property_notify(xcb_property_notify_event_t *event);

  Request *find_request(xcb_selection_notify_event_t *event);
  void fail_selection_request(Request& req);
  void notify_selection_request(Request& req);
  void notify_selection_request_impl(Request &req, xcb_atom_t property);

  void proxy_response_data(Request &req);

  XcbConn conn_;
  Window window_;
  Clipboard clipboard_;
  Clipboard primary_;
  xcb_atom_t incr_ = XCB_ATOM_NONE;
  const xcb_query_extension_reply_t *xfixes_ext_ = nullptr;
  std::vector<Request> requests_;
};

void Syncer::init() {
  auto preferred_screen = conn_.connect(nullptr);
  auto screen = conn_.get_screen(preferred_screen);

  // Verify XFIXES is available
  auto xfixes_cookie = xcb_xfixes_query_version(
      raw_conn(), xfixes_version_major, xfixes_version_minor);
  xcb_generic_error_t *err = nullptr;
  auto xfixes_reply = free_wrapper(
      xcb_xfixes_query_version_reply(raw_conn(), xfixes_cookie, &err));
  if (!xfixes_reply || !xfixes_reply->major_version) {
    throw std::runtime_error("XFIXES extension is not available");
  }
  xfixes_ext_ = xcb_get_extension_data(raw_conn(), &xcb_xfixes_id);
  if (!xfixes_ext_) {
    throw std::runtime_error("XFIXES extension is not available");
  }

  incr_ = conn_.get_atom("INCR");

  // Create a window for events to be delivered to.
  // We don't ever map the window to the display, so it is not shown.
  window_ = Window(conn_);
  std::array<uint32_t, 1> value_list{
      XCB_EVENT_MASK_PROPERTY_CHANGE,
  };
  xcb_create_window(raw_conn(), screen.root_depth, window_id(), screen.root, -1,
                    -1, 100, 100, 0, XCB_COPY_FROM_PARENT, screen.root_visual,
                    XCB_CW_EVENT_MASK, value_list.data());

  // Set up our clipboard state
  clipboard_ = Clipboard(*this, "CLIPBOARD");
  primary_ = Clipboard(*this, "PRIMARY");
  clipboard_.listen_for_changes();
  primary_.listen_for_changes();
  conn_.flush();
}

xcb_atom_t
Syncer::allocate_request_entry(const xcb_selection_request_event_t *req) {
  // Note that we don't need to perform any synchronization around the
  // requests_ vector here since all operation is single threaded.
  for (size_t idx = 0; idx < requests_.size(); ++idx) {
    auto &entry = requests_[idx];
    if (!entry.in_use()) {
      entry.assign(req);
      return entry.local_prop();
    }
  }

  auto next_prop_name = fmt::format("{}", requests_.size());
  auto local_prop = conn_.get_atom(next_prop_name);
  requests_.emplace_back(local_prop, req);
  return local_prop;
}

Clipboard::Clipboard(Syncer &sync, std::string_view name)
    : atom_(sync.conn().get_atom(name)), sync_(&sync), name_(name) {}

void Clipboard::listen_for_changes() {
  auto event_mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
  xcb_xfixes_select_selection_input(sync_->raw_conn(), sync_->window_id(),
                                    atom_, event_mask);
}

void Clipboard::sync_to(const Clipboard &other, xcb_window_t owner,
                        xcb_timestamp_t timestamp) {
  if (owner == XCB_NONE) {
    if (is_owned()) {
      info("{} selection cleared; dropping ownership of {}", other.name(),
           name_);
      ownership_end_ = timestamp;
      xcb_set_selection_owner(sync_->raw_conn(), XCB_NONE, atom_, timestamp);
      sync_->conn().flush();
    }
    return;
  }
  info("{} selection changed (owner={}), taking ownership of {}", other.name(),
       owner, name_);
  ownership_start_ = timestamp;
  ownership_end_ = 0;
  synced_selection_ = other.atom();
  xcb_set_selection_owner(sync_->raw_conn(), sync_->window_id(), atom_,
                          timestamp);
  sync_->conn().flush();
}

void Clipboard::lost_ownership(xcb_window_t new_owner,
                               xcb_timestamp_t timestamp) {
  dbg("lost {} selection ownership; new owner={}, timestamp={}", name_,
      new_owner, timestamp);
  ownership_end_ = timestamp;
}

void Clipboard::selection_request(const xcb_selection_request_event_t *req) {
  dbg("selection request for {}", name_);
  dbg("  - response_type = {:d}", req->response_type);
  dbg("  - pad0 = {:d}", req->pad0);
  dbg("  - seq = {:d}", req->sequence);
  dbg("  - time = {:d}", req->time);
  dbg("  - owner = {:d}", req->owner);
  dbg("  - requestor = {:d}", req->requestor);
  dbg("  - selection = {:d}", req->selection);
  dbg("  - target = {:d}", req->target);
  dbg("  - property = {:d}", req->property);

  // It would be ideal if we could simply call xcb_convert_selection()
  // with the requestor's original parameters and have the sync'ed clipboard
  // owner deliver the response directly to the requestor.
  //
  // Unfortunately, this doesn't work, as the XCB_SELECTION_NOTIFY event
  // from the original caller will have the selection field set to the ID
  // of the synced selection, and not the one that the caller asked for.
  // This matters to clients: at least gtk3 checks the selection ID and will
  // only process the notification if it came from the selection that it was
  // expecting.
  //
  // Therefore we have to ask the original clipboard to send the data to us,
  // and then we forward it along to the original requestor.

  auto prop = sync_->allocate_request_entry(req);
  xcb_convert_selection(sync_->raw_conn(), sync_->window_id(),
                        synced_selection_, req->target, prop, req->time);
  sync_->conn().flush();
}

void Syncer::loop() {
  info("Listening for clipboard events; my_id={}", window_id());
  while (true) {
    auto event = conn_.wait_for_event();
    const auto type = (event->response_type & 0x7f);

    if (event->response_type == 0) {
        handle_error(event.as<xcb_generic_error_t>());
    } else if (type == xfixes_ext_->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
      handle_selection_owner_notify(
          event.as<xcb_xfixes_selection_notify_event_t>());
    } else if (type == XCB_SELECTION_CLEAR) {
      handle_selection_clear(event.as<xcb_selection_clear_event_t>());
    } else if (type == XCB_SELECTION_REQUEST) {
      handle_selection_request(event.as<xcb_selection_request_event_t>());
    } else if (type == XCB_SELECTION_NOTIFY) {
      handle_selection_notify(event.as<xcb_selection_notify_event_t>());
    } else if (type == XCB_PROPERTY_NOTIFY) {
      handle_property_notify(event.as<xcb_property_notify_event_t>());
    } else {
      warn("unknown event: {:d}", type);
    }
  }
}

void Syncer::handle_error(xcb_generic_error_t* err) {
  // We just print a warning for now, and do not abort the program.
  warn("received XCB error: {:d}", err->error_code);
}

void Syncer::handle_selection_owner_notify(
    xcb_xfixes_selection_notify_event_t *event) {
  if (event->owner == window_id()) {
    // Ignore events about us taking ownership
  } else {
    if (event->selection == clipboard_.atom()) {
      primary_.sync_to(clipboard_, event->owner, event->timestamp);
    } else if (event->selection == primary_.atom()) {
      clipboard_.sync_to(primary_, event->owner, event->timestamp);
      conn_.flush();
    } else {
      warn("unknown selection changed: atom={}", event->selection);
    }
  }
}

void Syncer::handle_selection_clear(xcb_selection_clear_event_t *event) {
  if (event->selection == clipboard_.atom()) {
    clipboard_.lost_ownership(event->owner, event->time);
  } else if (event->selection == primary_.atom()) {
    primary_.lost_ownership(event->owner, event->time);
  } else {
    warn("unknown selection cleared: atom={}", event->selection);
  }
}

void Syncer::handle_selection_request(xcb_selection_request_event_t *event) {
  if (event->selection == clipboard_.atom()) {
    clipboard_.selection_request(event);
  } else if (event->selection == primary_.atom()) {
    primary_.selection_request(event);
  } else {
    warn("selection request for unknown selection: atom={}", event->selection);
  }
}

void Syncer::handle_selection_notify(xcb_selection_notify_event_t *event) {
  dbg("got XCB_SELECTION_NOTIFY event:");
  dbg(" - pad = {:d}", event->pad0);
  dbg(" - seq = {:d}", event->sequence);
  dbg(" - time = {:d}", event->time);
  dbg(" - requestor = {:d}", event->requestor);
  dbg(" - selection = {:d}", event->selection);
  dbg(" - target = {:d}", event->target);
  dbg(" - property = {:d}", event->property);

  auto* req = find_request(event);
  if (!req) {
    warn("received XCB_SELECTION_NOTIFY event with no outstanding request");
    fail_selection_request(*req);
    return;
  }

  if (event->property == XCB_ATOM_NONE) {
    // This was a failure.
    dbg("proxy {}: proxying failure", req->local_prop());
    fail_selection_request(*req);
    return;
  }

  // Read the property, and send it on to the real requestor
  try {
    proxy_response_data(*req);
  } catch (const std::exception& ex) {
    warn("failed to proxy clipboard data back to original requestor: {}",
         ex.what());
    fail_selection_request(*req);
    return;
  }
}

void Syncer::proxy_response_data(Request& req) {
  // TODO: perhaps add a state member variable to Request to track that we are
  // now proxying the data back to the original requestor?

  // Read the data in chunks of up to (max_chunk_size32 * 4) bytes at a time.
  // This just avoids consuming too much memory at once if the value is very
  // large.
  uint32_t max_chunk_size_u32 = 2000;
  uint32_t offset_u32 = 0;
  while (true) {
    auto cookie =
        xcb_get_property(raw_conn(), /*delete=*/0, window_id(),
                         req.local_prop(), XCB_GET_PROPERTY_TYPE_ANY,
                         offset_u32, max_chunk_size_u32);
    xcb_generic_error_t *err = nullptr;
    auto reply = free_wrapper(xcb_get_property_reply(raw_conn(), cookie, &err));
    if (!reply) {
      int error_code = err ? err->error_code : -1;
      throw std::runtime_error(
          fmt::format("failed to get window property {}: {}", req.local_prop(),
                      error_code));
    }

    if (reply->type == incr_) {
      throw std::runtime_error("INCR unsupported for now");
    }

    auto length = xcb_get_property_value_length(reply.get());
    dbg("proxy {}: forwarding {} bytes; {} remain", req.local_prop(), length,
        reply->bytes_after);
    auto *data = xcb_get_property_value(reply.get());
    const uint8_t mode =
        offset_u32 == 0 ? XCB_PROP_MODE_REPLACE : XCB_PROP_MODE_APPEND;
    xcb_change_property(raw_conn(), mode, req.requestor(), req.remote_prop(),
                        reply->type, reply->format, length, data);

    if (reply->bytes_after > 0) {
      offset_u32 += (length / 4);
      continue;
    }
    break;
  }

  notify_selection_request(req);
}

Request *Syncer::find_request(xcb_selection_notify_event_t *event) {
  if (event->property == XCB_ATOM_NONE) {
      // This is an error response.
      // Since we don't have the source property ID, look up the entry by
      // the timestamp and other fields in the request
      for (auto &req : requests_) {
        if (req.timestamp() == event->time && req.target() == event->target) {
          return &req;
        }
      }
  } else {
    for (auto &req : requests_) {
      if (req.local_prop() == event->property) {
        // We perhaps should also check event->selection and event->target here
        // too just to confirm they match what we expect?
        return &req;
      }
    }
  }

  return nullptr;
}

void Syncer::fail_selection_request(Request &req) {
  notify_selection_request_impl(req, XCB_ATOM_NONE);
}

void Syncer::notify_selection_request(Request &req) {
  notify_selection_request_impl(req, req.remote_prop());
}

void Syncer::notify_selection_request_impl(Request &req, xcb_atom_t property) {
  constexpr uint8_t propagate = 0;
  xcb_selection_notify_event_t resp = {};
  resp.response_type = XCB_SELECTION_NOTIFY;
  resp.sequence = 0;
  resp.time = req.timestamp();
  resp.requestor = req.requestor();
  resp.selection = req.selection();
  resp.target = req.target();
  resp.property = property;
  xcb_send_event(raw_conn(), propagate, req.requestor(),
                 XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char *>(&resp));
  req.unassign();
  conn_.flush();
}

void Syncer::handle_property_notify(xcb_property_notify_event_t *event) {
  // We don't really care about XCB_PROPERTY_NOTIFY events:
  // we will wait to process the data until we get the XCB_SELECTION_NOTIFY
  // event.
  dbg("got XCB_PROPERTY_NOTIFY event");
  dbg(" - pad = {:d}", event->pad0);
  dbg(" - seq = {:d}", event->sequence);
  dbg(" - window = {:d}", event->window);
  dbg(" - atom = {:d}", event->atom);
  dbg(" - time = {:d}", event->time);
  dbg(" - state = {:d}", event->state);
}

std::optional<int> parse_args(int argc, char** argv) {
  std::array<option, 3> long_opts{{
      {"verbose", 0, nullptr, 'v'},
      {"help", 0, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  }};

  while (true) {
    int long_index;
    auto opt = getopt_long(argc, argv, "vh", long_opts.data(), &long_index);
    if (opt == -1) {
      return std::nullopt;
    } else if (opt == '?') {
      return 2;
    } else if (opt == 'v') {
      g_log_level = static_cast<LogLevel>(static_cast<int>(g_log_level) + 1);
    } else if (opt == 'h') {
      printf("sync_clipboard: synchronize X11 selection buffers\n"
             "\n"
             "Usage:\n"
             "  -h, --help      Show this help message and exit.\n"
             "  -v, --verbose   Increase the output verbosity.\n");
      return 0;
    } else {
      throw std::runtime_error(
          fmt::format("unexpected getopt return value {:d}", opt));
    }
  }
}

} // namespace

int main(int argc, char* argv[]) {
  auto rc = parse_args(argc, argv);
  if (rc) {
    return *rc;
  }

  try {
    Syncer sync;
    sync.init();

    // TODO: perhaps sync the clipboard contents on start

    sync.loop();
  } catch (const std::exception &ex) {
    fprintf(stderr, "error: %s\n", ex.what());
    return 1;
  }

  return 0;
}
