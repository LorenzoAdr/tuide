#include "util/hang_watchdog.hpp"

#include "util/thread_name.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <execinfo.h>
#include <sys/syscall.h>
#endif

namespace fs = std::filesystem;

namespace tuide::hang_watchdog {

namespace {

constexpr int kMaxFrames = 64;
constexpr auto kStackReplyTimeout = std::chrono::milliseconds(300);

// --- Estado compartido con el hilo de UI: solo atómicos, nada de locks. ---
std::atomic<const char*> g_phase{nullptr};
std::atomic<const char*> g_last_action{nullptr};
std::atomic<int> g_last_action_code{-1};
std::atomic<std::int64_t> g_last_action_wall_s{0};
std::atomic<std::uint64_t> g_probe_acked{0};
std::atomic<bool> g_loop_live{false};

// --- Estado del vigía. ---
std::mutex g_control_mu;  // start/stop; nunca lo toca el hilo de UI en caliente
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
int g_wake_pipe[2] = {-1, -1};
Options g_options;
UiPoster g_poster;
pid_t g_ui_tid = 0;
std::string g_directory;

// Peticiones desde señales (SIGUSR1/SIGUSR2).
std::atomic<int> g_signal_request{0};  // 1 = volcar, 2 = volcar y salir

// Recogida de backtraces: el vigía pide de uno en uno, así basta un único hueco.
int g_stack_signal = 0;
void* g_slot_frames[kMaxFrames];
std::atomic<int> g_slot_count{0};
std::atomic<bool> g_slot_ready{false};

// Terminal y fichero de crash precalculados: los usan manejadores de señal.
bool g_have_termios = false;
struct termios g_saved_termios;
char g_crash_path[512] = {0};

pid_t current_tid() {
#if defined(__linux__)
  return static_cast<pid_t>(::syscall(SYS_gettid));
#else
  return 0;
#endif
}

void write_fd(int fd, const char* text) {
  if (fd < 0 || text == nullptr) {
    return;
  }
  std::size_t left = std::strlen(text);
  while (left > 0) {
    const ssize_t n = ::write(fd, text, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    text += n;
    left -= static_cast<std::size_t>(n);
  }
}

void write_fd(int fd, const std::string& text) { write_fd(fd, text.c_str()); }

std::string read_small_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  char buf[4096];
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) {
    return {};
  }
  std::string out(buf, static_cast<std::size_t>(n));
  while (!out.empty() && (out.back() == '\n' || out.back() == '\0')) {
    out.pop_back();
  }
  return out;
}

std::string wall_timestamp(bool for_filename) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), for_filename ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

std::string default_directory() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::string(home) + "/.config/tuide/hangs";
}

void on_stack_request(int /*sig*/) {
  const int saved_errno = errno;
#if defined(__linux__)
  const int count = backtrace(g_slot_frames, kMaxFrames);
  g_slot_count.store(count, std::memory_order_relaxed);
#endif
  g_slot_ready.store(true, std::memory_order_release);
  errno = saved_errno;
}

void on_user_request(int sig) {
  const int saved_errno = errno;
  g_signal_request.store(sig == SIGUSR2 ? 2 : 1, std::memory_order_release);
  if (g_wake_pipe[1] >= 0) {
    const char byte = 1;
    (void)!::write(g_wake_pipe[1], &byte, 1);
  }
  errno = saved_errno;
}

// El estado del hilo (S/R/D) y su wchan ya cuentan medio diagnóstico: futex =
// lock, wait_for_partner = FIFO, n_tty_write/wait_woken = terminal que no lee.
void dump_thread_table(int fd) {
  write_fd(fd, "\n== hilos (tid comm estado wchan)\n");
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/proc/self/task", ec)) {
    const std::string tid = entry.path().filename().string();
    const std::string base = entry.path().string();
    const std::string comm = read_small_file(base + "/comm");
    const std::string stat = read_small_file(base + "/stat");
    std::string state = "?";
    const auto close_paren = stat.rfind(')');
    if (close_paren != std::string::npos && close_paren + 2 < stat.size()) {
      state = stat.substr(close_paren + 2, 1);
    }
    std::string wchan = read_small_file(base + "/wchan");
    if (wchan.empty() || wchan == "0") {
      wchan = "-";
    }
    char line[256];
    std::snprintf(line, sizeof(line), "%-8s %-16s %s %s%s\n", tid.c_str(), comm.c_str(),
                  state.c_str(), wchan.c_str(),
                  std::atoi(tid.c_str()) == g_ui_tid ? "   <-- UI" : "");
    write_fd(fd, line);
  }
}

void dump_thread_stacks(int fd) {
#if defined(__linux__)
  if (g_stack_signal == 0) {
    return;
  }
  const pid_t pid = ::getpid();
  const pid_t self = current_tid();
  std::vector<pid_t> tids;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/proc/self/task", ec)) {
    tids.push_back(static_cast<pid_t>(std::atoi(entry.path().filename().c_str())));
  }
  // El hilo de UI primero: es el que importa si el volcado se corta.
  std::stable_partition(tids.begin(), tids.end(), [](pid_t t) { return t == g_ui_tid; });
  for (const pid_t tid : tids) {
    if (tid == self || tid <= 0) {
      continue;
    }
    const std::string comm =
        read_small_file("/proc/self/task/" + std::to_string(tid) + "/comm");
    write_fd(fd, "\n== backtrace tid=" + std::to_string(tid) + " (" + comm + ")" +
                     (tid == g_ui_tid ? " [UI]" : "") + "\n");
    g_slot_ready.store(false, std::memory_order_release);
    g_slot_count.store(0, std::memory_order_relaxed);
    if (::syscall(SYS_tgkill, pid, tid, g_stack_signal) != 0) {
      write_fd(fd, "  (no se pudo señalar el hilo)\n");
      continue;
    }
    const auto deadline = std::chrono::steady_clock::now() + kStackReplyTimeout;
    while (!g_slot_ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!g_slot_ready.load(std::memory_order_acquire)) {
      // Un hilo en estado D (E/S de disco, FUSE) no atiende señales hasta salir.
      write_fd(fd, "  (sin respuesta: probablemente en espera no interrumpible)\n");
      // Si contesta tarde escribiría sobre el hueco mientras pedimos al siguiente;
      // esperar un poco más reduce esa ventana sin bloquear el volcado.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    backtrace_symbols_fd(g_slot_frames, g_slot_count.load(std::memory_order_relaxed), fd);
  }
#else
  (void)fd;
#endif
}

void prune_old_reports() {
  std::error_code ec;
  std::vector<fs::path> reports;
  for (const auto& entry : fs::directory_iterator(g_directory, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("hang-", 0) == 0) {
      reports.push_back(entry.path());
    }
  }
  if (static_cast<int>(reports.size()) <= g_options.keep_reports) {
    return;
  }
  // El nombre lleva fecha ordenable: el orden léxico es el cronológico.
  std::sort(reports.begin(), reports.end());
  const std::size_t extra = reports.size() - static_cast<std::size_t>(g_options.keep_reports);
  for (std::size_t i = 0; i < extra; ++i) {
    fs::remove(reports[i], ec);
  }
}

std::string open_report_path() {
  if (g_directory.empty()) {
    return {};
  }
  std::error_code ec;
  fs::create_directories(g_directory, ec);
  return g_directory + "/hang-" + wall_timestamp(true) + "-" + std::to_string(::getpid()) +
         ".txt";
}

void write_report(const std::string& path, const char* reason,
                  std::chrono::milliseconds ui_silence) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    return;
  }
  char header[1024];
  const char* current_phase = g_phase.load(std::memory_order_acquire);
  const char* action = g_last_action.load(std::memory_order_acquire);
  const std::int64_t action_at = g_last_action_wall_s.load(std::memory_order_relaxed);
  char action_when[32] = "-";
  if (action_at > 0) {
    const std::time_t t = static_cast<std::time_t>(action_at);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(action_when, sizeof(action_when), "%H:%M:%S", &tm);
  }
  std::snprintf(header, sizeof(header),
                "== tuide hang report (%s)\n"
                "fecha: %s\npid: %d  ui_tid: %d\n"
                "ui sin responder: %.1fs (umbral %.1fs)\n"
                "fase ui: %s\n"
                "ultima accion: %s (code=%d, a las %s)\n",
                reason, wall_timestamp(false).c_str(), static_cast<int>(::getpid()),
                static_cast<int>(g_ui_tid), static_cast<double>(ui_silence.count()) / 1000.0,
                static_cast<double>(g_options.threshold.count()) / 1000.0,
                current_phase != nullptr ? current_phase : "(fuera de fases marcadas)",
                action != nullptr ? action : "(ninguna)",
                g_last_action_code.load(std::memory_order_relaxed), action_when);
  write_fd(fd, header);
  const std::string exe = read_small_file("/proc/self/cmdline");
  write_fd(fd, "binario: " + std::string(exe.c_str()) + "\n");
  write_fd(fd,
           "resolver offsets: addr2line -Cfpe <binario> 0x<offset>  (los '+0x..' de cada "
           "linea)\n");
  dump_thread_table(fd);
  dump_thread_stacks(fd);
  write_fd(fd, "\n== fin\n");
  ::fsync(fd);
  ::close(fd);
}

void append_line(const std::string& path, const std::string& line) {
  if (path.empty()) {
    return;
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  write_fd(fd, line);
  ::close(fd);
}

void emergency_exit() {
  restore_terminal();
  write_fd(STDERR_FILENO, "\ntuide: salida de emergencia (SIGUSR2). Informe en " +
                              g_directory + "\n");
  // Los hijos con child_die_with_parent() mueren con nosotros; no esperamos a
  // destructores que pueden ser precisamente los que están colgados.
  ::_exit(3);
}

void watchdog_loop() {
  set_current_thread_name("hang-watch");
  std::uint64_t probe_seq = 0;
  bool probe_pending = false;
  auto probe_sent_at = std::chrono::steady_clock::now();
  auto next_probe_at = probe_sent_at;
  // Episodio de cuelgue en curso.
  std::string episode_path;
  bool second_dump_done = false;
  const auto started_at = probe_sent_at;
  bool startup_episode = false;

  while (!g_stop.load(std::memory_order_acquire)) {
    pollfd pfd{g_wake_pipe[0], POLLIN, 0};
    (void)::poll(&pfd, 1, 200);
    if (pfd.revents & POLLIN) {
      char drain[16];
      (void)!::read(g_wake_pipe[0], drain, sizeof(drain));
    }
    if (g_stop.load(std::memory_order_acquire)) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();

    const int request = g_signal_request.exchange(0, std::memory_order_acq_rel);
    if (request != 0) {
      const auto silence =
          probe_pending || !g_loop_live.load(std::memory_order_acquire)
              ? std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_sent_at)
              : std::chrono::milliseconds(0);
      const std::string path = open_report_path();
      write_report(path, request == 2 ? "SIGUSR2 salida de emergencia" : "SIGUSR1 bajo demanda",
                   silence);
      prune_old_reports();
      if (request == 2) {
        emergency_exit();
      }
    }

    // FTXUI descarta lo que se envía con Post() antes de Loop: hasta el primer
    // render no hay sondas y el arranque se mide por reloj.
    if (!g_loop_live.load(std::memory_order_acquire)) {
      const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at);
      if (episode_path.empty() && silence >= g_options.startup_threshold) {
        episode_path = open_report_path();
        startup_episode = true;
        write_report(episode_path, "arranque: la UI no llega al bucle principal", silence);
        prune_old_reports();
      }
      continue;
    }
    if (startup_episode) {
      const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at);
      char line[128];
      std::snprintf(line, sizeof(line), "\n== RESUMED: el bucle arranco tras %.1fs\n",
                    static_cast<double>(took.count()) / 1000.0);
      append_line(episode_path, line);
      episode_path.clear();
      startup_episode = false;
    }

    if (probe_pending &&
        g_probe_acked.load(std::memory_order_acquire) >= probe_seq) {
      probe_pending = false;
      if (!episode_path.empty()) {
        const auto took =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_sent_at);
        char line[128];
        std::snprintf(line, sizeof(line), "\n== RESUMED: la UI volvio a responder tras %.1fs\n",
                      static_cast<double>(took.count()) / 1000.0);
        append_line(episode_path, line);
        episode_path.clear();
        second_dump_done = false;
      }
    }

    if (!probe_pending && now >= next_probe_at) {
      ++probe_seq;
      const std::uint64_t seq = probe_seq;
      probe_pending = true;
      probe_sent_at = now;
      next_probe_at = now + g_options.probe_interval;
      g_poster([seq] { g_probe_acked.store(seq, std::memory_order_release); });
      continue;
    }

    if (!probe_pending) {
      continue;
    }
    const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_sent_at);
    if (episode_path.empty() && silence >= g_options.threshold) {
      episode_path = open_report_path();
      write_report(episode_path, "UI sin responder", silence);
      prune_old_reports();
    } else if (!episode_path.empty() && !second_dump_done &&
               silence >= g_options.second_dump_after) {
      second_dump_done = true;
      write_report(episode_path, "UI sigue sin responder (segundo volcado)", silence);
    }
  }
}

void install_handler(int sig, void (*handler)(int)) {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  // SA_RESTART: la señal interrumpe a hilos que no esperan EINTR; que read/write
  // se reanuden solos (poll y nanosleep vuelven con EINTR igualmente).
  sa.sa_flags = SA_RESTART;
  ::sigaction(sig, &sa, nullptr);
}

void set_crash_path(const std::string& directory) {
  if (directory.empty()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(directory, ec);
  std::snprintf(g_crash_path, sizeof(g_crash_path), "%s/crash-%d.txt", directory.c_str(),
                static_cast<int>(::getpid()));
}

}  // namespace

void prepare_crash_report() { set_crash_path(default_directory()); }

void save_terminal_state() {
  if (::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
    g_have_termios = true;
  }
}

void start(const Options& options, UiPoster poster) {
  std::lock_guard<std::mutex> lock(g_control_mu);
  if (g_running.load() || !poster) {
    return;
  }
  g_options = options;
  if (g_options.threshold.count() <= 0) {
    g_options.threshold = std::chrono::milliseconds(3000);
  }
  if (g_options.probe_interval.count() <= 0) {
    g_options.probe_interval = std::chrono::milliseconds(1000);
  }
  if (g_options.startup_threshold < g_options.threshold) {
    g_options.startup_threshold = g_options.threshold;
  }
  if (g_options.second_dump_after < g_options.threshold) {
    g_options.second_dump_after = g_options.threshold * 5;
  }
  g_poster = std::move(poster);
  g_directory = g_options.directory.empty() ? default_directory() : g_options.directory;
  g_ui_tid = current_tid();
  set_crash_path(g_directory);

#if defined(__linux__)
  // La primera llamada a backtrace() carga libgcc_s con malloc: hacerla aquí y no
  // dentro de un manejador de señal.
  void* warm[4];
  (void)backtrace(warm, 4);
  g_stack_signal = SIGRTMIN + 8;
  install_handler(g_stack_signal, on_stack_request);
#endif
  install_handler(SIGUSR1, on_user_request);
  install_handler(SIGUSR2, on_user_request);

  if (::pipe(g_wake_pipe) != 0) {
    g_wake_pipe[0] = g_wake_pipe[1] = -1;
  } else {
    for (const int fd : g_wake_pipe) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
  }
  g_stop.store(false);
  g_loop_live.store(false, std::memory_order_release);
  g_running.store(true);
  g_thread = std::thread(watchdog_loop);
}

void stop() {
  std::lock_guard<std::mutex> lock(g_control_mu);
  if (!g_running.load()) {
    return;
  }
  g_stop.store(true, std::memory_order_release);
  if (g_wake_pipe[1] >= 0) {
    const char byte = 1;
    (void)!::write(g_wake_pipe[1], &byte, 1);
  }
  if (g_thread.joinable()) {
    g_thread.join();
  }
  // Señales de vuelta a su acción por defecto salvo la de pilas, que se ignora: un
  // tgkill en vuelo no debe matar el proceso durante el cierre.
  std::signal(SIGUSR1, SIG_DFL);
  std::signal(SIGUSR2, SIG_DFL);
  if (g_stack_signal != 0) {
    std::signal(g_stack_signal, SIG_IGN);
  }
  for (int& fd : g_wake_pipe) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  g_poster = nullptr;
  g_running.store(false);
}

bool running() { return g_running.load(std::memory_order_acquire); }

void mark_loop_running() { g_loop_live.store(true, std::memory_order_release); }

void set_phase(const char* literal) { g_phase.store(literal, std::memory_order_release); }

const char* phase() { return g_phase.load(std::memory_order_acquire); }

void note_key_action(int action, const char* id_literal_or_null) {
  g_last_action_code.store(action, std::memory_order_relaxed);
  g_last_action.store(id_literal_or_null, std::memory_order_release);
  g_last_action_wall_s.store(static_cast<std::int64_t>(std::time(nullptr)),
                             std::memory_order_relaxed);
}

std::string reports_directory() {
  return g_directory.empty() ? default_directory() : g_directory;
}

void restore_terminal() {
  // Salir de pantalla alternativa, ratón, pegado entre corchetes y teclado kitty,
  // y mostrar el cursor: lo mínimo para que la shell vuelva a ser usable.
  static const char kReset[] =
      "\033[<u\033[?2004l\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l"
      "\033[?25h\033[0m\033[?1049l\r\n";
  (void)!::write(STDOUT_FILENO, kReset, sizeof(kReset) - 1);
  if (g_have_termios) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  }
}

void write_crash_report(const char* reason) {
  if (g_crash_path[0] == '\0') {
    return;
  }
  const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    return;
  }
  write_fd(fd, "== tuide crash: ");
  write_fd(fd, reason);
  write_fd(fd, "\n");
  const char* current_phase = g_phase.load(std::memory_order_acquire);
  write_fd(fd, "fase ui: ");
  write_fd(fd, current_phase != nullptr ? current_phase : "-");
  write_fd(fd, "\nultima accion: ");
  const char* action = g_last_action.load(std::memory_order_acquire);
  write_fd(fd, action != nullptr ? action : "-");
  write_fd(fd, "\n");
#if defined(__linux__)
  void* frames[kMaxFrames];
  const int count = backtrace(frames, kMaxFrames);
  backtrace_symbols_fd(frames, count, fd);
#endif
  ::close(fd);
}

}  // namespace tuide::hang_watchdog
