/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OSGUI_HPP
#define OSGUI_HPP

#include "osgui.h"

#include <utility>

namespace osgui {

class Frame {
public:
  Frame() = default;
  explicit Frame(osgui_frame *frame) : frame_(frame) {}
  ~Frame() { osgui_cancel_frame(frame_); }
  Frame(const Frame &) = delete;
  Frame &operator=(const Frame &) = delete;
  Frame(Frame &&other) noexcept
      : frame_(std::exchange(other.frame_, nullptr)) {}
  Frame &operator=(Frame &&other) noexcept {
    if (this != &other) {
      osgui_cancel_frame(frame_);
      frame_ = std::exchange(other.frame_, nullptr);
    }
    return *this;
  }
  osgui_frame *get() const { return frame_; }
  enum osgui_result end() {
    osgui_frame *frame = std::exchange(frame_, nullptr);
    return osgui_end_frame(frame);
  }

private:
  osgui_frame *frame_ = nullptr;
};

class Context {
public:
  Context() = default;
  ~Context() { osgui_context_destroy(context_); }
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&other) noexcept
      : context_(std::exchange(other.context_, nullptr)) {}
  Context &operator=(Context &&other) noexcept {
    if (this != &other) {
      osgui_context_destroy(context_);
      context_ = std::exchange(other.context_, nullptr);
    }
    return *this;
  }
  enum osgui_result create(uint32_t flags = 0) {
    struct osgui_context_options options = {sizeof(options), OSGUI_API_VERSION,
                                            flags};
    return osgui_context_create(&options, &context_);
  }
  enum osgui_result begin(int width, int height, Frame *out) {
    if (out == nullptr || out->get() != nullptr)
      return OSGUI_INVALID_ARGUMENT;
    osgui_frame *frame = nullptr;
    enum osgui_result result =
        osgui_begin_frame(context_, width, height, &frame);
    if (result == OSGUI_OK)
      *out = Frame(frame);
    return result;
  }
  osgui_context *get() const { return context_; }

private:
  osgui_context *context_ = nullptr;
};

} // namespace osgui

#endif
