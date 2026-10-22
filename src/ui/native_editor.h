// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/editor.h>

#include <cstdint>
#include <memory>

namespace ekphore {

enum class NativeWindowApi : std::uint8_t {
    cocoa,
    win32,
    x11,
};

struct NativeWindowParent {
    NativeWindowApi api{ NativeWindowApi::cocoa };
    void *handle{};
    void *display{};
};

// EditorSession has to stay alive until we detach
class NativeEditor {
  public:
    explicit NativeEditor(EditorSession &session);
    ~NativeEditor();
    NativeEditor(NativeEditor &&) noexcept;
    NativeEditor &operator=(NativeEditor &&) noexcept;
    NativeEditor(const NativeEditor &) = delete;
    NativeEditor &operator=(const NativeEditor &) = delete;

    [[nodiscard]] static bool supports(NativeWindowApi api);
    [[nodiscard]] bool attach(const NativeWindowParent &parent);
    void detach();
    void show();
    void hide();
    void idle();
    [[nodiscard]] bool attached() const;
    [[nodiscard]] void *native_handle() const;
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const;
    [[nodiscard]] bool set_scale(float scale);

  private:
    class Implementation;
    std::unique_ptr<Implementation> implementation;
};

} // namespace ekphore
