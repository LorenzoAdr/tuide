#include "git/git_service.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

// El fallo original era un autobloqueo del worker: una espera normal colgaría el
// test para siempre en vez de fallar, así que la comprobación corre en un hilo
// aparte y el principal decide con un plazo.
bool finishes_within(std::chrono::milliseconds limit, tuide::GitService* git,
                     std::atomic<bool>* valid) {
  std::atomic<bool> done{false};
  std::thread probe([&] {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      const tuide::GitRepoInfo info = git->repo_info();
      if (!info.last_error.empty()) {
        valid->store(info.valid);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    done.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + limit + std::chrono::seconds(2);
  while (!done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!done.load()) {
    probe.detach();
    return false;
  }
  probe.join();
  return true;
}

}  // namespace

int main() {
  const fs::path dir =
      fs::temp_directory_path() / ("tuide_git_open_" + std::to_string(::getpid()));
  fs::create_directories(dir);

  std::atomic<int> wakes{0};
  std::atomic<bool> valid{true};
  bool ok = false;
  {
    tuide::GitService git;
    git.set_update_callback([&] { wakes.fetch_add(1); });
    git.open(dir.string());
    ok = finishes_within(std::chrono::seconds(10), &git, &valid);
    if (!ok) {
      std::cerr << "FAIL: GitService::open sobre carpeta sin repo deja mutex_ tomado\n";
      std::_Exit(1);
    }
  }
  fs::remove_all(dir);

  if (valid.load()) {
    std::cerr << "FAIL: carpeta sin repo marcada como valida\n";
    return 1;
  }
  if (wakes.load() == 0) {
    std::cerr << "FAIL: no se aviso a la UI tras descubrir que no hay repo\n";
    return 1;
  }
  std::cout << "git_service_open_test OK\n";
  return 0;
}
