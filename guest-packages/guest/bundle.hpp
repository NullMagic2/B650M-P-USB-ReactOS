// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.embedded_bundle
Purpose: Own bounded extraction and cleanup of a standalone installer's payload.
Created: 2026-10-25
Architecture: NT5 adapter for the same verified plan used on setup media.
*/
#pragma once
#include <string>
#include <vector>
namespace lightning::packages {
bool safe_relative_path(const std::wstring &path);
enum class BundleState { absent, ready, invalid };
class EmbeddedBundle {
  public:
    EmbeddedBundle() = default;
    EmbeddedBundle(const EmbeddedBundle &) = delete;
    EmbeddedBundle &operator=(const EmbeddedBundle &) = delete;
    ~EmbeddedBundle();
    BundleState unpack(const std::wstring &executable);
    // Copy verified payloads/source to a newly created directory. Never replaces
    // user files; exporting data does not require native OS installation access.
    bool export_files(const std::wstring &new_directory) const;
    const std::wstring &root() const { return root_; }

  private:
    std::wstring root_;
    std::vector<std::wstring> files_, directories_;
    bool create_parents(const std::wstring &path);
};
} // namespace lightning::packages
