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

constexpr std::string_view clipboard_id("CLIPBOARD");
constexpr std::string_view primary_id("PRIMARY");
constexpr uint32_t xfixes_version_major = 5;
constexpr uint32_t xfixes_version_minor = 1;

namespace {
xcb_screen_t *get_screen(xcb_connection_t *conn, size_t screen_num) {
  auto setup = xcb_get_setup(conn);
  if (!setup) {
      return nullptr;
  }

  auto iter = xcb_setup_roots_iterator(setup);
  size_t idx = 0;
  while (true) {
      if (!iter.rem) {
          return nullptr;
      }
      if (idx == screen_num) {
          return iter.data;
      }
      xcb_screen_next(&iter);
      ++idx;
  }
}
} // namespace

int main(int argc, char* argv[]) {
    // TODO: parse args
    (void)argc;
    (void)argv;

    auto* conn = xcb_connect(nullptr, nullptr);
    auto rc = xcb_connection_has_error(conn);
    if (rc != 0) {
        fprintf(stderr, "error: failed to connect to X display: %d\n", rc);
        return 1;
    }

    auto screen = get_screen(conn, 0);
    if (!screen) {
      fprintf(stderr, "error: unable to get screen\n");
      return 1;
    }

    // Verify XFIXES is available
    auto xfixes_cookie = xcb_xfixes_query_version(conn, xfixes_version_major, xfixes_version_minor);
    xcb_generic_error_t* err = nullptr;
    auto *xfixes_reply = xcb_xfixes_query_version_reply(conn, xfixes_cookie, &err);
    if (!xfixes_reply || !xfixes_reply->major_version) {
      fprintf(stderr, "error: XFIXES extension is not available\n");
      return 1;
    }
    free(xfixes_reply);
    auto *xfixes_ext = xcb_get_extension_data(conn, &xcb_xfixes_id);
    if (!xfixes_ext) {
      fprintf(stderr, "error: XFIXES extension is not available\n");
      return 1;
    }

    // Get the clipboard atom
    auto cookie =
        xcb_intern_atom(conn, 1, clipboard_id.size(), clipboard_id.data());
    auto reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply) {
      fprintf(stderr, "error: failed to get clipboard atom\n");
      return 1;
    }
    auto clipboard_atom = reply->atom;
    free(reply);

    cookie = xcb_intern_atom(conn, 1, primary_id.size(), primary_id.data());
    reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply) {
      fprintf(stderr, "error: failed to get clipboard atom\n");
      return 1;
    }
    auto primary_atom = reply->atom;
    free(reply);

    // Create a window for events to be delivered to.
    // We don't ever map the window to the display, so it is not shown.
    auto win_id = xcb_generate_id(conn);
    std::array<uint32_t, 1> value_list{XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(conn, screen->root_depth, win_id, screen->root, -1, -1, 1,
                      1, 0, XCB_COPY_FROM_PARENT, screen->root_visual,
                      XCB_CW_EVENT_MASK, value_list.data());

    // Ask for selection change events to be delivered to our window
    auto event_mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
    xcb_xfixes_select_selection_input(
        conn, win_id, clipboard_atom, event_mask);
    xcb_xfixes_select_selection_input(
        conn, win_id, primary_atom, event_mask);

    xcb_flush(conn);

    // TODO: perhaps sync the clipboards on start

    printf("Listening for clipboard events\n");
    while (true) {
        auto *event = xcb_wait_for_event(conn);
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
    xcb_disconnect(conn);

    return 0;
}
