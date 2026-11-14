#include "util/hang_watchdog.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << '\n';
    ++g_failures;
  }
}

std::vector<fs::path> reports_in(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().filename().string().rfind("hang-", 0) == 0) {
      out.push_back(entry.path());
    }
  }
  return out;
}

std::string read_all(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Cola que imita la de FTXUI: otro hilo la vacía, salvo cuando está "colgado".
struct FakeUiLoop {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  std::atomic<bool> frozen{false};
  std::atomic<bool> quit{false};
  std::thread worker;

  void start() {
    worker = std::thread([this] {
      while (!quit.load()) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv.wait_for(lock, 10ms, [this] { return !tasks.empty() || quit.load(); });
          if (frozen.load() || tasks.empty()) {
            continue;
          }
          task = std::move(tasks.front());
          tasks.pop_front();
        }
        task();
      }
    });
  }
  void post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mu);
      tasks.push_back(std::move(task));
    }
    cv.notify_one();
  }
  void finish() {
    quit.store(true);
    cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }
};

tuide::hang_watchdog::Options fast_options(const fs::path& dir) {
  tuide::hang_watchdog::Options options;
  options.threshold = 300ms;
  options.probe_interval = 50ms;
  options.second_dump_after = 60s;
  options.startup_threshold = 300ms;
  options.directory = dir.string();
  return options;
}

void test_healthy_ui_writes_nothing(const fs::path& dir) {
  FakeUiLoop loop;
  loop.start();
  tuide::hang_watchdog::start(fast_options(dir), [&](std::function<void()> t) {
    loop.post(std::move(t));
  });
  tuide::hang_watchdog::mark_loop_running();
  std::this_thread::sleep_for(1200ms);
  tuide::hang_watchdog::stop();
  loop.finish();
  expect(reports_in(dir).empty(), "una UI sana no debe dejar informes");
}

void test_frozen_ui_dumps_and_resume_is_noted(const fs::path& dir) {
  FakeUiLoop loop;
  loop.start();
  tuide::hang_watchdog::start(fast_options(dir), [&](std::function<void()> t) {
    loop.post(std::move(t));
  });
  tuide::hang_watchdog::mark_loop_running();
  std::this_thread::sleep_for(200ms);
  tuide::hang_watchdog::note_key_action(7, "save_file");
  tuide::hang_watchdog::set_phase("tick.git");
  loop.frozen.store(true);
  std::this_thread::sleep_for(1500ms);

  auto reports = reports_in(dir);
  expect(reports.size() == 1, "un cuelgue debe dejar exactamente un informe");
  if (!reports.empty()) {
    const std::string text = read_all(reports.front());
    expect(text.find("UI sin responder") != std::string::npos, "motivo en el informe");
    expect(text.find("fase ui: tick.git") != std::string::npos, "fase en el informe");
    expect(text.find("ultima accion: save_file") != std::string::npos, "accion en el informe");
    expect(text.find("== hilos") != std::string::npos, "tabla de hilos");
    expect(text.find("== backtrace tid=") != std::string::npos, "backtraces por hilo");
    expect(text.find("[UI]") != std::string::npos, "hilo de UI identificado");
  }

  loop.frozen.store(false);
  std::this_thread::sleep_for(500ms);
  tuide::hang_watchdog::set_phase(nullptr);
  tuide::hang_watchdog::stop();
  loop.finish();
  reports = reports_in(dir);
  expect(reports.size() == 1, "la recuperacion no abre un informe nuevo");
  if (!reports.empty()) {
    expect(read_all(reports.front()).find("RESUMED") != std::string::npos,
           "la recuperacion queda anotada");
  }
}

// Antes de Loop las sondas se perderían (FTXUI descarta Post): se mide por reloj.
void test_startup_without_loop_is_reported(const fs::path& dir) {
  std::atomic<int> posted{0};
  tuide::hang_watchdog::start(fast_options(dir), [&](std::function<void()>) { posted++; });
  std::this_thread::sleep_for(700ms);
  expect(posted.load() == 0, "no se envian sondas antes de que el bucle viva");
  auto reports = reports_in(dir);
  expect(reports.size() == 1, "arranque atascado deja un informe");
  tuide::hang_watchdog::mark_loop_running();
  std::this_thread::sleep_for(400ms);
  tuide::hang_watchdog::stop();
  reports = reports_in(dir);
  if (!reports.empty()) {
    const std::string text = read_all(reports.front());
    expect(text.find("arranque") != std::string::npos, "motivo de arranque");
    expect(text.find("RESUMED: el bucle arranco") != std::string::npos,
           "la llegada al bucle cierra el episodio");
  }
  expect(posted.load() > 0, "con el bucle vivo empiezan las sondas");
}

}  // namespace

int main() {
  const fs::path base =
      fs::temp_directory_path() / ("tuide_hang_watchdog_" + std::to_string(::getpid()));
  const fs::path healthy = base / "healthy";
  const fs::path frozen = base / "frozen";
  const fs::path startup = base / "startup";
  fs::create_directories(startup);
  fs::create_directories(healthy);
  fs::create_directories(frozen);

  test_healthy_ui_writes_nothing(healthy);
  // El hilo de UI del vigía es el que llama a start(): aquí, main, que está vivo.
  test_frozen_ui_dumps_and_resume_is_noted(frozen);
  test_startup_without_loop_is_reported(startup);

  fs::remove_all(base);
  if (g_failures != 0) {
    return 1;
  }
  std::cout << "hang_watchdog_test OK\n";
  return 0;
}
