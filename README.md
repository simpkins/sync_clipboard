# Sync X11 Clipboards

This is a small tool to keep the 2 X11 clipboards in sync.

There are a variety of other tools out there that have more features, like
maintaining history and providing fancier UIs.  I intentionally didn't want a
tool that tracks history, since tools like password managers put sensitive
information in the clipboard.

This tool simply links the "primary" and "clipboard" selection buffers
together.  Whenever one is changed, this tool claims ownership of the other
selection buffer, and proxies all requests for the selection buffer data to the
owner of the other buffer.

The X11 selection behavior behavior is [awkwardly complex](https://www.uninformativ.de/blog/postings/2017-04-02/0/POSTING-en.html),
and can really act more like a dynamic pipe than a simple static buffer.

# Building

```
cd build && mkdir build
cmake ..
make
```
