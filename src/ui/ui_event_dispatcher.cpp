#include "ui/ui_event_dispatcher.hpp"

#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ui/ui_event_trace.hpp"

namespace tuide {

using namespace ftxui;

void UiEventDispatcher::set_screen(ScreenInteractive* screen) {
  // ui_thread_ antes del store con release: quien vea screen_ ve también el hilo.
  ui_thread_ = std::this_thread::get_id();
  std::vector<std::function<void()>> deferred;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    screen_.store(screen, std::memory_order_release);
    deferred.swap(pending_main_);
  }
  if (screen == nullptr) {
    return;
  }
  for (auto& fn : deferred) {
    screen->Post(std::move(fn));
  }
  // Eventos encolados antes de tener pantalla: un Custom para drenarlos ya.
  custom_pending_.store(true, std::memory_order_release);
  screen->PostEvent(Event::Custom);
}

uint64_t UiEventDispatcher::begin_input_correlation() {
  const uint64_t id = next_correlation_id_++;
  active_correlation_id_.store(id, std::memory_order_release);
  return id;
}

uint64_t UiEventDispatcher::current_correlation_id() const {
  return active_correlation_id_.load(std::memory_order_acquire);
}

bool UiEventDispatcher::correlation_still_valid(uint64_t id) const {
  return id != 0 && id == active_correlation_id_.load(std::memory_order_acquire);
}

void UiEventDispatcher::post_custom_coalesced(bool urgent) {
  (void)urgent;
  ScreenInteractive* const screen = screen_.load(std::memory_order_acquire);
  if (screen == nullptr) {
    // Se queda en pending_events_; set_screen lanza el Custom que los drena.
    return;
  }
  if (custom_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  screen->PostEvent(Event::Custom);
}

void UiEventDispatcher::enqueue(UiEvent event, bool urgent) {
  if (event.tag.empty()) {
    event.tag = "unnamed";
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_events_.push_back(std::move(event));
  }
  post_custom_coalesced(urgent);
}

void UiEventDispatcher::enqueue(UiEvent event) { enqueue(std::move(event), false); }

void UiEventDispatcher::emit(UiEvent event) { enqueue(std::move(event), false); }

void UiEventDispatcher::emit_urgent(UiEvent event) { enqueue(std::move(event), true); }

void UiEventDispatcher::post_repaint_urgent() {
  if (screen_.load(std::memory_order_acquire) == nullptr) {
    return;
  }
  UiEvent event;
  event.kind = UiEventKind::InputCorrelated;
  event.tag = "ui.repaint_urgent";
  event.src_file = __FILE__;
  event.src_line = __LINE__;
  enqueue(std::move(event), true);
  request_animation_frame();
}

void UiEventDispatcher::emit_correlated_if_valid(uint64_t correlation_id, std::string tag,
                                                 std::function<void()> pre_paint,
                                                 const char* src_file, int src_line) {
  if (!correlation_still_valid(correlation_id)) {
    if (pre_paint) {
      pre_paint();
    }
    return;
  }
  UiEvent event;
  event.kind = UiEventKind::InputCorrelated;
  event.correlation_id = correlation_id;
  event.tag = std::move(tag);
  event.pre_paint = std::move(pre_paint);
  event.src_file = src_file;
  event.src_line = src_line;
  emit(std::move(event));
}

void UiEventDispatcher::emit_terminal(std::string tag, std::function<void()> pre_paint,
                                        const char* src_file, int src_line) {
  UiEvent event;
  event.kind = UiEventKind::TerminalOutput;
  event.tag = std::move(tag);
  event.pre_paint = std::move(pre_paint);
  event.src_file = src_file;
  event.src_line = src_line;
  emit(std::move(event));
  // Same as emit_debug: coalesced Custom alone may not schedule a frame, so PTY
  // echoes and streaming compile output stay invisible until the next key/click
  // (one-character lag / frozen scrollback).
  request_animation_frame();
}

void UiEventDispatcher::emit_debug(std::string tag, std::function<void()> pre_paint,
                                     const char* src_file, int src_line) {
  UiEvent event;
  event.kind = UiEventKind::DebugCritical;
  event.tag = std::move(tag);
  event.pre_paint = std::move(pre_paint);
  event.src_file = src_file;
  event.src_line = src_line;
  emit(std::move(event));
  // DebugCritical mutates layout (mode, panels). Coalesced Custom alone may not
  // schedule a frame; without this the UI stays stale until the next key/click.
  request_animation_frame();
}

void UiEventDispatcher::post_repaint_custom() {
  ScreenInteractive* const screen = screen_.load(std::memory_order_acquire);
  if (screen == nullptr) {
    return;
  }
  screen->PostEvent(Event::Custom);
}

void UiEventDispatcher::post_on_main(std::function<void()> fn) {
  if (!fn) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (screen_.load(std::memory_order_acquire) == nullptr) {
      // Antes se ejecutaba aquí mismo, en el hilo que llamase: guardarlo para el
      // hilo de UI.
      pending_main_.push_back(std::move(fn));
      return;
    }
  }
  screen_.load(std::memory_order_acquire)->Post(std::move(fn));
}

void UiEventDispatcher::request_animation_frame() {
  ScreenInteractive* const screen = screen_.load(std::memory_order_acquire);
  if (screen == nullptr) {
    return;
  }
  if (std::this_thread::get_id() == ui_thread_) {
    screen->RequestAnimationFrame();
    return;
  }
  // Desde el lector del PTY u otro hilo: RequestAnimationFrame escribe estado del
  // bucle sin lock; Post sí es seguro y lo ejecuta en el hilo de UI.
  screen->Post([screen] { screen->RequestAnimationFrame(); });
}

UiEventDrainPlan UiEventDispatcher::drain_pending(int64_t now_ms) {
  (void)now_ms;
  custom_pending_.store(false, std::memory_order_release);

  std::vector<UiEvent> events;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    events.swap(pending_events_);
  }

  UiEventDrainPlan plan;
  for (const UiEvent& event : events) {
    trace_.record(event, events.size() > 1, now_ms);
    if (event.pre_paint) {
      event.pre_paint();
    }
    const bool is_lsp_diag = event.tag == "lsp.diagnostics";
    const bool is_lsp_completion = event.tag == "lsp.completion";
    switch (event.kind) {
      case UiEventKind::TerminalOutput:
        plan.run_terminal = true;
        break;
      case UiEventKind::DebugCritical:
        plan.run_debug = true;
        plan.run_editor = true;
        plan.run_ui_tasks = true;
        plan.run_full_background = true;
        break;
      case UiEventKind::UserInput:
      case UiEventKind::InputCorrelated:
        if (!is_lsp_diag && !is_lsp_completion) {
          plan.run_editor = true;
          plan.run_ui_tasks = true;
          plan.run_full_background = true;
        }
        break;
    }
  }
  if (!events.empty()) {
    plan.lsp_diag_only = true;
    plan.lsp_completion_only = true;
    for (const UiEvent& event : events) {
      if (event.tag != "lsp.diagnostics") {
        plan.lsp_diag_only = false;
      }
      if (event.tag != "lsp.completion") {
        plan.lsp_completion_only = false;
      }
    }
  }
  bool repost = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    repost = !pending_events_.empty();
  }
  if (repost) {
    post_custom_coalesced(false);
  }
  return plan;
}

UiEventTrace& UiEventDispatcher::trace() { return trace_; }

const UiEventTrace& UiEventDispatcher::trace() const { return trace_; }

}  // namespace tuide
