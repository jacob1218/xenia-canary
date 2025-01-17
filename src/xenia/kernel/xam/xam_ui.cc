/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/window.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

std::atomic<int> xam_dialogs_shown_ = {0};

dword_result_t XamIsUIActive() { return xam_dialogs_shown_ > 0 ? 1 : 0; }
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

class MessageBoxDialog : public xe::ui::ImGuiDialog {
 public:
  MessageBoxDialog(xe::ui::Window* window, std::wstring title,
                   std::wstring description, std::vector<std::wstring> buttons,
                   uint32_t default_button, uint32_t* out_chosen_button)
      : ImGuiDialog(window),
        title_(xe::to_string(title)),
        description_(xe::to_string(description)),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        out_chosen_button_(out_chosen_button) {
    if (out_chosen_button) {
      *out_chosen_button = default_button;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("%s", description_.c_str());
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        auto button_name = xe::to_string(buttons_[i]);
        if (ImGui::Button(button_name.c_str())) {
          if (out_chosen_button_) {
            *out_chosen_button_ = static_cast<uint32_t>(i);
          }
          ImGui::CloseCurrentPopup();
          Close();
        }
        ImGui::SameLine();
      }
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::wstring> buttons_;
  uint32_t default_button_ = 0;
  uint32_t* out_chosen_button_ = nullptr;
};

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI(dword_t user_index, lpwstring_t title_ptr,
                                   lpwstring_t text_ptr, dword_t button_count,
                                   lpdword_t button_ptrs, dword_t active_button,
                                   dword_t flags, lpdword_t result_ptr,
                                   pointer_t<XAM_OVERLAPPED> overlapped) {
  std::wstring title;
  if (title_ptr) {
    title = title_ptr.value();
  } else {
    title = L"";  // TODO(gibbed): default title based on flags?
  }
  auto text = text_ptr.value();

  std::vector<std::wstring> buttons;
  std::wstring all_buttons;
  for (uint32_t j = 0; j < button_count; ++j) {
    uint32_t button_ptr = button_ptrs[j];
    auto button = xe::load_and_swap<std::wstring>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));
    all_buttons.append(button);
    if (j + 1 < button_count) {
      all_buttons.append(L" | ");
    }
    buttons.push_back(button);
  }

  XELOGI(
      "XamShowMessageBoxUI(%d, %.8X(%S), %.8X(%S), %d, %.8X(%S), %d, %X, %.8X, "
      "%.8X)",
      user_index, title_ptr, title.c_str(), text_ptr, text.c_str(),
      button_count, button_ptrs, all_buttons.c_str(), active_button, flags,
      result_ptr, overlapped);

  // Set overlapped result to X_ERROR_IO_PENDING
  if (overlapped) {
    XOverlappedSetResult((void*)overlapped.host_address(), X_ERROR_IO_PENDING);
  }

  // Broadcast XN_SYS_UI = true
  kernel_state()->BroadcastNotification(0x9, true);

  // Auto-pick the focused button.
  //uint32_t chosen_button = active_button;

  if (!cvars::headless) {
    auto display_window = kernel_state()->emulator()->display_window();
    ++xam_dialogs_shown_;
    display_window->loop()->PostSynchronous([&]() {
      // TODO(benvanik): setup icon states.
      switch (flags & 0xF) {
        case 0:
          // config.pszMainIcon = nullptr;
          break;
        case 1:
          // config.pszMainIcon = TD_ERROR_ICON;
          break;
        case 2:
          // config.pszMainIcon = TD_WARNING_ICON;
          break;
        case 3:
          // config.pszMainIcon = TD_INFORMATION_ICON;
          break;
      }

      auto chosen_button = new uint32_t();      
      auto fence = new xe::threading::Fence();
      (new MessageBoxDialog(display_window, title, text, buttons, active_button,
                            chosen_button))
          ->Then(fence);

      // The function to be run once dialog has finished
      auto ui_fn = [fence, result_ptr, chosen_button, overlapped]() {
        fence->Wait();
        delete fence;
        --xam_dialogs_shown_;

        *result_ptr = *chosen_button;
        delete chosen_button;

        if (overlapped) {
          // TODO: this will set overlapped's context to ui_threads thread
          // ID, is that a good idea?
          kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                      X_ERROR_SUCCESS);
        }

        // Broadcast XN_SYS_UI = false
        kernel_state()->BroadcastNotification(0x9, false);

        return 0;
      };

      // Create a host thread to run the function above
      auto ui_thread = kernel::object_ref<kernel::XHostThread>(
          new kernel::XHostThread(kernel_state(), 128 * 1024, 0, ui_fn));
      ui_thread->set_name("XamShowMessageBoxUI Thread");
      ui_thread->Create();
    });
  } else {
    // Auto-pick the focused button.
    *result_ptr = (uint32_t)active_button;

    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    }

    // Broadcast XN_SYS_UI = false
    kernel_state()->BroadcastNotification(0x9, false);
  }
  uint32_t result = X_ERROR_SUCCESS;
  if (overlapped) {
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

class KeyboardInputDialog : public xe::ui::ImGuiDialog {
 public:
  KeyboardInputDialog(xe::ui::Window* window, std::wstring title,
                      std::wstring description, std::wstring default_text,
                      std::wstring* out_text, size_t max_length)
      : ImGuiDialog(window),
        title_(xe::to_string(title)),
        description_(xe::to_string(description)),
        default_text_(xe::to_string(default_text)),
        out_text_(out_text),
        max_length_(max_length) {
    if (out_text_) {
      *out_text_ = default_text;
    }
    text_buffer_.resize(max_length);
    std::strncpy(text_buffer_.data(), default_text_.c_str(),
                 std::min(text_buffer_.size() - 1, default_text_.size()));
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s", description_.c_str());
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (out_text_) {
          *out_text_ = xe::to_wstring(text_buffer_.data());
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        if (out_text_) {
          *out_text_ = xe::to_wstring(text_buffer_.data());
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  std::wstring* out_text_ = nullptr;
  std::vector<char> text_buffer_;
  size_t max_length_ = 0;
};

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI(dword_t user_index, dword_t flags,
                                 lpwstring_t default_text, lpwstring_t title,
                                 lpwstring_t description, lpwstring_t buffer,
                                 dword_t buffer_length,
                                 pointer_t<XAM_OVERLAPPED> overlapped) {
  // overlapped should always be set, xam seems to check for this specifically
  if (!overlapped) {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  } else {
    // Set overlapped result to X_ERROR_IO_PENDING
    XOverlappedSetResult((void*)overlapped.host_address(), X_ERROR_IO_PENDING);
  }

  // Broadcast XN_SYS_UI = true
  kernel_state()->BroadcastNotification(0x9, true);

  if (cvars::headless) {
    // Redirect default_text back into the buffer.
    std::memset(buffer, 0, buffer_length * 2);
    if (default_text) {
      xe::store_and_swap<std::wstring>(buffer, default_text.value());
    }

    // TODO: we should probably setup a thread to complete the overlapped a few
    // seconds after this returns, to simulate the user taking a few seconds to
    // enter text
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    }

    // Broadcast XN_SYS_UI = false
    kernel_state()->BroadcastNotification(0x9, false);

    return X_ERROR_IO_PENDING;
  }
  
  // Instead of waiting for the keyboard dialog to finish before returning,
  // we'll create a thread that'll wait for it instead, and return immediately.
  // This way we can let the game run any "code-to-run-while-UI-is-active" code
  // that it might need to.

  ++xam_dialogs_shown_;

  auto display_window = kernel_state()->emulator()->display_window();
  display_window->loop()->PostSynchronous([&]() {
    auto out_text = new std::wstring();
    auto fence = new xe::threading::Fence();

    // Create the dialog
    (new KeyboardInputDialog(display_window, title ? title.value() : L"",
                             description ? description.value() : L"",
                             default_text ? default_text.value() : L"",
                             out_text, buffer_length))
        ->Then(fence);
    
    // The function to be run once dialog has finished
    auto ui_fn = [fence, out_text, buffer, buffer_length, overlapped]() {
      fence->Wait();
      delete fence;
      --xam_dialogs_shown_;

    // Zero the output buffer.
    std::memset(buffer, 0, buffer_length * 2);

      // Copy the string.
      size_t size = buffer_length;
      if (size > out_text->size()) {
        size = out_text->size();
      }

      xe::copy_and_swap((wchar_t*)buffer.host_address(), out_text->c_str(),
                        size);
      delete out_text;

      // TODO: this will set overlapped's context to ui_threads thread ID
      // is that a good idea?
      if (overlapped) {
        kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
      }

      // Broadcast XN_SYS_UI = false
      kernel_state()->BroadcastNotification(0x9, false);

      return 0;
    };

    // Create a host thread to run the function above
    auto ui_thread = kernel::object_ref<kernel::XHostThread>(
        new kernel::XHostThread(kernel_state(), 128 * 1024, 0, ui_fn));
    ui_thread->set_name("XamShowKeyboardUI Thread");
    ui_thread->Create();
  });

  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI(dword_t user_index, dword_t content_type,
                                       dword_t content_flags,
                                       qword_t total_requested,
                                       lpdword_t device_id_ptr,
                                       pointer_t<XAM_OVERLAPPED> overlapped) {

  // Set overlapped to X_ERROR_IO_PENDING
  if (overlapped) {
    XOverlappedSetResult((void*)overlapped.host_address(), X_ERROR_IO_PENDING);
  }

  // Broadcast XN_SYS_UI = true
  kernel_state()->BroadcastNotification(0x9, true);

  auto ui_fn = [content_type, device_id_ptr, overlapped]() {
    XELOGI("XamShowDeviceSelectorUI Content_type:(%X) device_id_ptr: %.8X overlapped:(%X)",
           content_type, device_id_ptr, (bool)overlapped);

    // NOTE: 0xF00D0000 magic from xam_content.cc
    switch (content_type) {
      case 1:  // save game
        *device_id_ptr = 0xF00D0000 | 0x0001;
        break;
      case 2:  // marketplace
        *device_id_ptr = 0xF00D0000 | 0x0002;
        break;
      case 3:  // title/publisher update?
        *device_id_ptr = 0xF00D0000 | 0x0003;
        break;
      default:
        assert_unhandled_case(content_type);
        *device_id_ptr = 0xF00D0000 | 0x0001;
        break;
    }

    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    }

    // Sleep for 1 second, act like user is making a choice
    xe::threading::Sleep(std::chrono::milliseconds(500));

    // Broadcast XN_SYS_UI = true followed by XN_SYS_UI = false
    kernel_state()->BroadcastNotification(0x9, true);
    kernel_state()->BroadcastNotification(0x9, false);

    // return 0;
    return X_ERROR_SUCCESS;
  };

  if (overlapped) {
     // Create a host thread to run the function above
    auto ui_thread = kernel::object_ref<kernel::XHostThread>(
        new kernel::XHostThread(kernel_state(), 128 * 1024, 0, ui_fn));
    ui_thread->set_name("XamShowDeviceSelectorUI Thread");
    ui_thread->Create();
    while (ui_thread->last_error() != X_ERROR_SUCCESS) {
      xe::threading::Sleep(std::chrono::milliseconds(101));
    }
    return X_ERROR_IO_PENDING;
  } else {
    ui_fn();
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

void XamShowDirtyDiscErrorUI(dword_t user_index) {
  if (cvars::headless) {
    assert_always();
    exit(1);
    return;
  }

  auto display_window = kernel_state()->emulator()->display_window();
  xe::threading::Fence fence;
  display_window->loop()->PostSynchronous([&]() {
    xe::ui::ImGuiDialog::ShowMessageBox(
        display_window, "Disc Read Error",
        "There's been an issue reading content from the game disc.\nThis is "
        "likely caused by bad or unimplemented file IO calls.")
        ->Then(&fence);
  });
  ++xam_dialogs_shown_;
  fence.Wait();
  --xam_dialogs_shown_;

  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

void RegisterUIExports(xe::cpu::ExportResolver* export_resolver,
                       KernelState* kernel_state) {}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
