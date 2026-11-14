#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "ftxui/dom/elements.hpp"

namespace tuide {

enum class UiPanelId : std::uint8_t {
  FileTree = 0,
  EditorCenter = 1,
  RightSidebar = 2,
  Console = 3,
  kCount
};

struct UiPanelRenderCacheEntry {
  ftxui::Element cached = ftxui::text("");
  // Atómico: mark_dirty llega desde hilos de fondo (PTY, git, indexador, tree-sitter).
  // Con un ++ normal dos marcas simultáneas podían perder una y el panel quedarse sin
  // repintar. El resto de campos solo los toca el hilo de UI al pintar.
  std::atomic<uint64_t> dirty_generation{1};
  uint64_t last_painted_generation = 0;
  uint64_t last_colors_revision = 0;
};

struct UiPanelRenderCache {
  std::array<UiPanelRenderCacheEntry, static_cast<std::size_t>(UiPanelId::kCount)> entries;

  void mark_dirty(UiPanelId panel) {
    entries[static_cast<std::size_t>(panel)].dirty_generation.fetch_add(
        1, std::memory_order_acq_rel);
  }

  void mark_all_dirty() {
    for (auto& entry : entries) {
      entry.dirty_generation.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  ftxui::Element render(UiPanelId panel, uint64_t colors_revision,
                        const std::function<ftxui::Element()>& build) {
    UiPanelRenderCacheEntry& entry = entries[static_cast<std::size_t>(panel)];
    // Leer una sola vez: una marca que llegue durante build() queda pendiente para el
    // siguiente frame en vez de darse por pintada.
    const uint64_t generation = entry.dirty_generation.load(std::memory_order_acquire);
    if (generation != entry.last_painted_generation ||
        entry.last_colors_revision != colors_revision) {
      entry.cached = build();
      entry.last_painted_generation = generation;
      entry.last_colors_revision = colors_revision;
    }
    return entry.cached;
  }
};

}  // namespace tuide
