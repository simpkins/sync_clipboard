/*
 * Copyright (c) 2023, Adam Simpkins
 */
#include <array>
#include <cstdint>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string_view>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <fmt/core.h>

constexpr uint32_t xfixes_version_major = 5;
constexpr uint32_t xfixes_version_minor = 1;

namespace {

class XcbConn {
public:
  XcbConn() {}
  ~XcbConn() {
    if (conn_) {
      xcb_disconnect(conn_);
    }
  }

  xcb_connection_t *conn() const { return conn_; }

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

  void flush();
  const xcb_screen_t &get_screen(size_t screen_num);
  xcb_atom_t get_atom(std::string_view name, bool create = false);

private:
  XcbConn(XcbConn &&) = delete;
  XcbConn &operator=(XcbConn &&) = delete;

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
  auto reply = xcb_intern_atom_reply(conn_, cookie, &err);
  if (!reply) {
    int error_code = err ? err->error_code : -1;
    throw std::runtime_error(
        fmt::format("failed to get atom {}: {}", name, error_code));
  }
  auto atom = reply->atom;
  free(reply);
  return atom;
}

class Window {
public:
  Window(XcbConn &conn) : conn_(conn), window_(xcb_generate_id(conn.conn())) {}
  ~Window() { xcb_destroy_window(conn_.conn(), window_); }

  xcb_window_t id() const { return window_; }
  XcbConn &conn() const { return conn_; }
  xcb_connection_t *raw_conn() const { return conn_.conn(); }

private:
  Window(Window &&) = delete;
  Window &operator=(Window &&) = delete;

  XcbConn &conn_;
  xcb_window_t window_{0};
};

class Clipboard {
public:
  Clipboard(Window &window, std::string_view name)
      : atom_(window.conn().get_atom(name)), window_(window), name_(name) {}

  xcb_atom_t atom() const { return atom_; }

  void listen_for_changes() {
    auto event_mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
    xcb_xfixes_select_selection_input(window_.raw_conn(), window_.id(), atom_,
                                      event_mask);
  }

  void sync_to(const Clipboard& other, xcb_window_t owner, xcb_timestamp_t timestamp) {
    if (owner == XCB_NONE) {
      if (ownership_end_ == 0) {
        printf("  -> dropping ownership of %s; synced clipboard lost owner\n",
               name_.c_str());
        ownership_end_ = timestamp;
        xcb_set_selection_owner(window_.raw_conn(), XCB_NONE, atom_, timestamp);
        window_.conn().flush();
      }
      return;
    }
    printf("  -> I own %s synced owner is %d\n", name_.c_str(), owner);
    ownership_start_ = timestamp;
    ownership_end_ = 0;
    synced_selection_ = other.atom();
    xcb_set_selection_owner(window_.raw_conn(), window_.id(), atom_, timestamp);
    window_.conn().flush();
  }
  void lost_ownership(xcb_window_t new_owner, xcb_timestamp_t timestamp) {
    printf("lost %s selection ownership; new owner=%d, timestamp=%d\n",
           name_.c_str(), new_owner, timestamp);
    ownership_end_ = timestamp;
  }

  void selection_request(const xcb_selection_request_event_t *req) {
    printf("selection request for %s\n", name_.c_str());

    // Forward the request to the owner of the synced selection
    xcb_convert_selection(window_.raw_conn(), req->requestor, synced_selection_,
                          req->target, req->property, req->time);
    window_.conn().flush();
#if 0
    // If req->property is XCB_ATOM_NONE, use req->target as the property
    auto property = req->property;
    if (property == XCB_ATOM_NONE) {
        // From ICCCM manual version 2, section 2.2:
        //
        // If the specified property is None, the requestor is an obsolete
        // client. Owners are encouraged to support these clients by using the
        // specified target atom as the property name to be used for the reply.
        property = req->target;
    }

    xcb_change_property(window_.raw_conn(), XCB_PROP_MODE_REPLACE,
                        req->requestor, property, type, 32, 6, "foobar");
#endif

    // fail_selection_request_(req);
  }

private:
  Clipboard(Clipboard&&) = delete;
  Clipboard& operator=(Clipboard&&) = delete;

  void fail_selection_request_(const xcb_selection_request_event_t *req) {
    constexpr uint8_t propagate = 0;
    xcb_selection_notify_event_t resp = {};
    resp.response_type = XCB_SELECTION_NOTIFY;
    resp.sequence = 0;
    resp.time = req->time;
    resp.requestor = req->requestor;
    resp.selection = atom_;
    resp.target = req->target;
    resp.property = XCB_ATOM_NONE;
    xcb_send_event(window_.raw_conn(), propagate, req->requestor,
                   XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&resp));
    window_.conn().flush();
  }

  xcb_atom_t atom_ = XCB_ATOM_NONE;
  Window& window_;
  std::string name_;
  xcb_atom_t synced_selection_ = XCB_ATOM_NONE;
  xcb_timestamp_t ownership_start_ = 0;
  xcb_timestamp_t ownership_end_ = 1;
};

} // namespace

int main(int argc, char* argv[]) {
    // TODO: parse args
    (void)argc;
    (void)argv;

    XcbConn conn;
    auto preferred_screen = conn.connect(nullptr);
    auto screen = conn.get_screen(preferred_screen);

    // Verify XFIXES is available
    auto xfixes_cookie = xcb_xfixes_query_version(conn.conn(), xfixes_version_major,
                                                  xfixes_version_minor);
    xcb_generic_error_t* err = nullptr;
    auto *xfixes_reply =
        xcb_xfixes_query_version_reply(conn.conn(), xfixes_cookie, &err);
    if (!xfixes_reply || !xfixes_reply->major_version) {
      fprintf(stderr, "error: XFIXES extension is not available\n");
      return 1;
    }
    free(xfixes_reply);
    auto *xfixes_ext = xcb_get_extension_data(conn.conn(), &xcb_xfixes_id);
    if (!xfixes_ext) {
      fprintf(stderr, "error: XFIXES extension is not available\n");
      return 1;
    }

    // Create a window for events to be delivered to.
    // We don't ever map the window to the display, so it is not shown.
    Window window(conn);
    std::array<uint32_t, 1> value_list{
        XCB_EVENT_MASK_PROPERTY_CHANGE,
    };
    xcb_create_window(conn.conn(), screen.root_depth, window.id(), screen.root,
                      -1, -1, 100, 100, 0, XCB_COPY_FROM_PARENT,
                      screen.root_visual, XCB_CW_EVENT_MASK, value_list.data());

    // Set up our clipboard state
    Clipboard clipboard(window, "CLIPBOARD");
    Clipboard primary(window, "PRIMARY");
    clipboard.listen_for_changes();
    primary.listen_for_changes();
    conn.flush();

    // TODO: perhaps sync the clipboards on start

    printf("Listening for clipboard events; my_id=%d\n", window.id());
    while (true) {
        auto *event = xcb_wait_for_event(conn.conn());
        if (!event) {
            break;
        }
        auto type = (event->response_type & 0x7f);
        if (event->response_type == 0) {
          // Error
          auto err = reinterpret_cast<xcb_generic_error_t *>(event);
          fprintf(stderr, "error: %d\n", err->error_code);
        } else if (type ==
                   xfixes_ext->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
          auto ev =
              reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event);
          if (ev->owner == window.id()) {
            // Ignore events about us taking ownership
          } else {
            if (ev->selection == clipboard.atom()) {
              primary.sync_to(clipboard, ev->owner, ev->timestamp);
            } else if (ev->selection == primary.atom()) {
              clipboard.sync_to(primary, ev->owner, ev->timestamp);
            conn.flush();
            } else {
              printf("unknown selection changed: atom=%u\n", ev->selection);
            }
          }
        } else if (type == XCB_SELECTION_CLEAR) {
          auto ev = reinterpret_cast<xcb_selection_clear_event_t *>(event);
          if (ev->selection == clipboard.atom()) {
            clipboard.lost_ownership(ev->owner, ev->time);
          } else if (ev->selection == primary.atom()) {
            primary.lost_ownership(ev->owner, ev->time);
          } else {
            printf("unknown selection cleared: atom=%u\n", ev->selection);
          }
        } else if (type == XCB_SELECTION_REQUEST) {
          auto ev = reinterpret_cast<xcb_selection_request_event_t *>(event);
          if (ev->selection == clipboard.atom()) {
            clipboard.selection_request(ev);
          } else if (ev->selection == primary.atom()) {
            primary.selection_request(ev);
          } else {
            printf("selection request for unknown selection: atom=%u\n",
                   ev->selection);
          }
        } else if (type == XCB_SELECTION_NOTIFY) {
          auto ev = reinterpret_cast<xcb_selection_notify_event_t *>(event);
          printf("unhandled XCB_SELECTION_NOTIFY event:\n");
          printf(" - pad = %u\n", ev->pad0);
          printf(" - seq = %u\n", ev->sequence);
          printf(" - time = %u\n", ev->time);
          printf(" - requestor = %u\n", ev->requestor);
          printf(" - selection = %u\n", ev->selection);
          printf(" - target = %u\n", ev->target);
          printf(" - property = %u\n", ev->property);
        } else if (type == XCB_PROPERTY_NOTIFY) {
          printf("unhandled XCB_PROPERTY_NOTIFY event\n");
        } else {
          printf("unknown event: %u\n", (0x7f & event->response_type));
        }
        free(event);
    }

    printf("done\n");

    return 0;
}
