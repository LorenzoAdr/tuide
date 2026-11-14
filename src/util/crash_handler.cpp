#include "util/crash_handler.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>

#include "i18n/tr.hpp"
#include "util/hang_watchdog.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace tuide {

namespace {

#if defined(__linux__) || defined(__APPLE__)
void print_backtrace_to_stderr(const char* reason) {
  void* frames[128];
  const int count = backtrace(frames, 128);
  char** symbols = backtrace_symbols(frames, count);

  std::cerr << i18n::tr_fmt("crash.backtrace_header", {reason});
  if (symbols != nullptr) {
    for (int i = 0; i < count; ++i) {
      std::cerr << "  #" << i << " " << symbols[i] << '\n';
    }
    std::free(symbols);
  } else {
    std::cerr << i18n::tr("crash.symbols_unresolved");
  }
  std::cerr << std::flush;
}

std::atomic<bool> g_in_fatal_handler{false};

void write_stderr(const char* text) {
  (void)!::write(STDERR_FILENO, text, std::strlen(text));
}

// Solo funciones async-signal-safe. Si el crash viene de una corrupción del heap,
// glibc aborta con el lock de la arena tomado: cualquier malloc aquí (iostream,
// backtrace_symbols, i18n) se queda esperando ese lock para siempre y el crash se
// convierte en un cuelgue con el terminal en modo raw.
void on_fatal_signal(int sig) {
  if (g_in_fatal_handler.exchange(true)) {
    // Un fallo dentro del propio manejador: salir sin intentar nada más.
    _exit(128 + sig);
  }
  const char* name = "signal";
  switch (sig) {
    case SIGSEGV:
      name = "SIGSEGV";
      break;
    case SIGABRT:
      name = "SIGABRT";
      break;
    case SIGFPE:
      name = "SIGFPE";
      break;
    case SIGILL:
      name = "SIGILL";
      break;
    case SIGBUS:
      name = "SIGBUS";
      break;
    default:
      break;
  }
  // Primero el fichero (stderr suele ser la pantalla alternativa y se pierde) y
  // luego el terminal, para que lo impreso abajo sea legible y la shell usable.
  hang_watchdog::write_crash_report(name);
  hang_watchdog::restore_terminal();
  write_stderr("\n=== tuide crash (");
  write_stderr(name);
  write_stderr(") ===\n");
  void* frames[128];
  const int count = backtrace(frames, 128);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  write_stderr("informe: ~/.config/tuide/hangs/crash-<pid>.txt\n");
  _exit(128 + sig);
}

void on_tty_job_signal(int /*sig*/) {
  // Ignore: a DAP child stealing the TTY must not stop tuide (default SIGTTIN/SIGTTOU).
}
#endif

}  // namespace

void install_crash_handlers() {
#if defined(__linux__) || defined(__APPLE__)
  // backtrace() carga libgcc_s con malloc en su primera llamada: hacerla aquí, no
  // dentro del manejador.
  void* warm[4];
  (void)backtrace(warm, 4);
#endif
  hang_watchdog::prepare_crash_report();
  std::set_terminate([] {
    hang_watchdog::write_crash_report("std::terminate");
    hang_watchdog::restore_terminal();
    print_current_backtrace("std::terminate");
    std::abort();
  });
#if defined(__linux__) || defined(__APPLE__)
  // Writing to a closed DAP/stdio pipe raises SIGPIPE by default and kills the
  // process with no backtrace. Ignore it so Writer::write can return false.
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGTTIN, on_tty_job_signal);
  std::signal(SIGTTOU, on_tty_job_signal);
  std::signal(SIGSEGV, on_fatal_signal);
  std::signal(SIGABRT, on_fatal_signal);
  std::signal(SIGFPE, on_fatal_signal);
  std::signal(SIGILL, on_fatal_signal);
  std::signal(SIGBUS, on_fatal_signal);
#endif
}

void reinstall_fault_handlers() {
#if defined(__linux__) || defined(__APPLE__)
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_fatal_signal;
  sigemptyset(&sa.sa_mask);
  for (const int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    ::sigaction(sig, &sa, nullptr);
  }
#endif
}

void print_current_backtrace(const char* reason) {
#if defined(__linux__) || defined(__APPLE__)
  print_backtrace_to_stderr(reason);
#else
  std::cerr << i18n::tr_fmt("crash.unavailable", {reason});
#endif
}

}  // namespace tuide
