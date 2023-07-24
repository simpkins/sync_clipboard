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
    XcbConn(XcbConn &&conn) : conn_(conn.conn_) { conn.conn_ = nullptr; }
    XcbConn &operator=(XcbConn &&conn) {
      if (conn_) {
        xcb_disconnect(conn_);
      }
      conn_ = conn.conn_;
      conn.conn_ = nullptr;
      return *this;
    }
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

    // Get the clipboard atom
    auto clipboard_atom = conn.get_atom("CLIPBOARD");
    auto primary_atom = conn.get_atom("PRIMARY");

    // Create a window for events to be delivered to.
    // We don't ever map the window to the display, so it is not shown.
    auto win_id = xcb_generate_id(conn.conn());
    std::array<uint32_t, 1> value_list{XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(conn.conn(), screen.root_depth, win_id, screen.root, -1,
                      -1, 1, 1, 0, XCB_COPY_FROM_PARENT, screen.root_visual,
                      XCB_CW_EVENT_MASK, value_list.data());

    // Ask for selection change events to be delivered to our window
    auto event_mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
    xcb_xfixes_select_selection_input(conn.conn(), win_id, clipboard_atom,
                                      event_mask);
    xcb_xfixes_select_selection_input(conn.conn(), win_id, primary_atom,
                                      event_mask);

    conn.flush();

    // TODO: perhaps sync the clipboards on start

    printf("Listening for clipboard events\n");
    while (true) {
        auto *event = xcb_wait_for_event(conn.conn());
        if (!event) {
            break;
        }
        auto type = (event->response_type & 0x7f);
        if (type == xfixes_ext->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
          auto ev =
              reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event);
          if (ev->selection == clipboard_atom) {
            printf("clipboard selection changed\n");
          } else if (ev->selection == primary_atom) {
            printf("primary selection changed\n");
          } else {
            printf("unknown selection changed: atom=%u\n", ev->selection);
          }
        } else {
          printf("unknown event: %#x\n", event->response_type);
        }
        free(event);
    }

    printf("done\n");

    return 0;
}
