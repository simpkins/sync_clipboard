/*
 * Copyright (c) 2023, Adam Simpkins
 *
 * Useful docs on Xlib selection behavior and requirements:
 * https://tronche.com/gui/x/icccm/sec-2.html
 */
#include <array>
#include <cstdint>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string_view>
#include <type_traits>
#include <memory>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <fmt/core.h>

constexpr uint32_t xfixes_version_major = 5;
constexpr uint32_t xfixes_version_minor = 1;

namespace {

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
    printf("proxy %d: assigned to requestor %d prop %d\n", local_prop_,
           requestor_, remote_prop_);
  }
  void unassign() {
    printf("proxy %d: unassigned\n", local_prop_);
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
      printf("  -> dropping ownership of %s; synced clipboard lost owner\n",
             name_.c_str());
      ownership_end_ = timestamp;
      xcb_set_selection_owner(sync_->raw_conn(), XCB_NONE, atom_, timestamp);
      sync_->conn().flush();
    }
    return;
  }
  printf("  -> I own %s synced owner is %d\n", name_.c_str(), owner);
  ownership_start_ = timestamp;
  ownership_end_ = 0;
  synced_selection_ = other.atom();
  xcb_set_selection_owner(sync_->raw_conn(), sync_->window_id(), atom_,
                          timestamp);
  sync_->conn().flush();
}

void Clipboard::lost_ownership(xcb_window_t new_owner,
                               xcb_timestamp_t timestamp) {
  printf("lost %s selection ownership; new owner=%d, timestamp=%d\n",
         name_.c_str(), new_owner, timestamp);
  ownership_end_ = timestamp;
}

void Clipboard::selection_request(const xcb_selection_request_event_t *req) {
  printf("selection request for %s\n", name_.c_str());
  printf("  - response_type = %u\n", req->response_type);
  printf("  - pad0 = %u\n", req->pad0);
  printf("  - seq = %u\n", req->sequence);
  printf("  - time = %u\n", req->time);
  printf("  - owner = %u\n", req->owner);
  printf("  - requestor = %u\n", req->requestor);
  printf("  - selection = %u\n", req->selection);
  printf("  - target = %u\n", req->target);
  printf("  - property = %u\n", req->property);

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
  printf("Listening for clipboard events; my_id=%d\n", window_id());
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
      fprintf(stderr, "unknown event: %u\n", type);
    }
  }
}

void Syncer::handle_error(xcb_generic_error_t* err) {
  // We just print a warning for now, and do not abort the program.
  fprintf(stderr, "error: %d\n", err->error_code);
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
      printf("unknown selection changed: atom=%u\n", event->selection);
    }
  }
}

void Syncer::handle_selection_clear(xcb_selection_clear_event_t *event) {
  if (event->selection == clipboard_.atom()) {
    clipboard_.lost_ownership(event->owner, event->time);
  } else if (event->selection == primary_.atom()) {
    primary_.lost_ownership(event->owner, event->time);
  } else {
    printf("unknown selection cleared: atom=%u\n", event->selection);
  }
}

void Syncer::handle_selection_request(xcb_selection_request_event_t *event) {
  if (event->selection == clipboard_.atom()) {
    clipboard_.selection_request(event);
  } else if (event->selection == primary_.atom()) {
    primary_.selection_request(event);
  } else {
    printf("selection request for unknown selection: atom=%u\n",
           event->selection);
  }
}

void Syncer::handle_selection_notify(xcb_selection_notify_event_t *event) {
  printf("got XCB_SELECTION_NOTIFY event:\n");
  printf(" - pad = %u\n", event->pad0);
  printf(" - seq = %u\n", event->sequence);
  printf(" - time = %u\n", event->time);
  printf(" - requestor = %u\n", event->requestor);
  printf(" - selection = %u\n", event->selection);
  printf(" - target = %u\n", event->target);
  printf(" - property = %u\n", event->property);

  auto* req = find_request(event);
  if (!req) {
    fprintf(stderr, "warning: received XCB_SELECTION_NOTIFY event with no "
                    "outstanding request\n");
    return;
  }

  if (event->property == XCB_ATOM_NONE) {
    // This was a failure.
    fail_selection_request(*req);
    return;
  }

  // Read the property, and send it on to the real requestor
  try {
    proxy_response_data(*req);
  } catch (const std::exception& ex) {
    fprintf(stderr,
            "error: failed to proxy clipboard data back to original requestor: "
            "%s\n",
            ex.what());
    fail_selection_request(*req);
    return;
  }
}

void Syncer::proxy_response_data(Request& req) {
  // TODO: perhaps add a state member variable to Request to track that we are
  // now proxying the data back to the original requestor?

  // TODO: We probably should specify a smaller long_length field,
  // and make multiple calls with increasing offsets to fetch the data in
  // chunks if it is very large (and not using the INCR protocol).
  auto cookie = xcb_get_property(raw_conn(), /*delete=*/0, window_id(),
                                 req.local_prop(), XCB_GET_PROPERTY_TYPE_ANY,
                                 /*long_offset=*/0, /*long_length=*/0x1fffffff);
  xcb_generic_error_t *err = nullptr;

  // TODO: rather than calling xcb_get_property_reply immediately, it would
  // perhaps be better to use xcb_get_property_unchecked() and then process
  // this reply in the event loop, so other events could be handled in the
  // interim.
  auto reply = free_wrapper(xcb_get_property_reply(raw_conn(), cookie, &err));
  if (!reply) {
    int error_code = err ? err->error_code : -1;
    throw std::runtime_error(fmt::format("failed to get window property {}: {}",
                                         req.local_prop(), error_code));
  }

  if (reply->type == incr_) {
    throw std::runtime_error("INCR unsupported for now");
  }

  auto length = xcb_get_property_value_length(reply.get());
  printf("proxy %d: sending %d bytes\n", req.local_prop(), length);
  auto *data = xcb_get_property_value(reply.get());

  xcb_change_property(raw_conn(), XCB_PROP_MODE_REPLACE, req.requestor(),
                      req.remote_prop(), reply->type, reply->format, length,
                      data);
  notify_selection_request(req);
  req.unassign();
  conn_.flush();
}

Request *Syncer::find_request(xcb_selection_notify_event_t *event) {
  if (event->property == XCB_ATOM_NONE) {
      // This is an error response.
      // Since we don't have the source property ID, look up the entry by
      // the timestamp and other fields in the request
      for (auto &req : requests_) {
        if (req.timestamp() == event->time &&
            req.selection() == event->selection &&
            req.target() == event->target) {
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
  printf("got XCB_PROPERTY_NOTIFY event\n");
  printf(" - pad = %u\n", event->pad0);
  printf(" - seq = %u\n", event->sequence);
  printf(" - window = %u\n", event->window);
  printf(" - atom = %u\n", event->atom);
  printf(" - time = %u\n", event->time);
  printf(" - state = %u\n", event->state);
}

} // namespace

int main(int argc, char* argv[]) {
  // TODO: parse args
  (void)argc;
  (void)argv;

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
