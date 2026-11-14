#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ui/ui_event_trace.hpp"
#include "ui/ui_event_types.hpp"

namespace ftxui {
class ScreenInteractive;
class Event;
}  // namespace ftxui

namespace tuide {

class UiEventDispatcher {
 public:
  void set_screen(ftxui::ScreenInteractive* screen);

  uint64_t begin_input_correlation();
  uint64_t current_correlation_id() const;
  bool correlation_still_valid(uint64_t id) const;

  void emit(UiEvent event);
  void emit_urgent(UiEvent event);
  void emit_correlated_if_valid(uint64_t correlation_id, std::string tag,
                                std::function<void()> pre_paint, const char* src_file,
                                int src_line);

  void emit_terminal(std::string tag, std::function<void()> pre_paint, const char* src_file,
                     int src_line);
  void emit_debug(std::string tag, std::function<void()> pre_paint, const char* src_file,
                  int src_line);

  UiEventDrainPlan drain_pending(int64_t now_ms);
  void post_on_main(std::function<void()> fn);
  void post_repaint_custom();
  void request_animation_frame();
  void post_repaint_urgent();
  UiEventTrace& trace();
  const UiEventTrace& trace() const;

 private:
  void enqueue(UiEvent event);
  void enqueue(UiEvent event, bool urgent);
  void post_custom_coalesced(bool urgent = false);

  // Nulo hasta que el bucle de FTXUI atiende Post() (primer render): lo que se
  // envía antes se perdería y, peor, dejaría custom_pending_ en true para siempre.
  std::atomic<ftxui::ScreenInteractive*> screen_{nullptr};
  std::vector<std::function<void()>> pending_main_;  // bajo pending_mutex_
  // Hilo que llamó a set_screen (el de UI): fuera de él no se toca el estado de
  // animación de FTXUI, que no es thread-safe.
  std::thread::id ui_thread_;
  std::atomic<bool> custom_pending_{false};
  std::mutex pending_mutex_;
  std::vector<UiEvent> pending_events_;
  uint64_t next_correlation_id_ = 1;
  // Lo escribe la UI al empezar cada entrada; lo leen los ui_wake de hilos de fondo.
  std::atomic<uint64_t> active_correlation_id_{0};
  UiEventTrace trace_;
};

}  // namespace tuide
