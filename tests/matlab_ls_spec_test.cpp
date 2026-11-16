// Arranque del servidor LSP de MATLAB: de dónde sale el binario y con qué
// argumentos. No lanza el servidor (necesita Node y un MATLAB con licencia).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "lsp/language_server_spec.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::cout << "FAIL: " << what << '\n';
    ++failures;
  }
}

void write_executable(const fs::path& path) {
  std::ofstream(path) << "#!/bin/sh\nexit 0\n";
  fs::permissions(path, fs::perms::owner_all);
}

bool has_arg(const tuide::LanguageServerSpec& spec, const std::string& arg) {
  for (const auto& a : spec.args) {
    if (a == arg) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("tuide-matlab-ls-" + std::to_string(getpid()));
  const fs::path bin = root / "bin";
  fs::create_directories(bin);
  // PATH aislado: el resultado no puede depender de lo que haya instalado el host.
  setenv("PATH", bin.c_str(), 1);
  // El resolutor también busca en ~/.local/bin, ~/.cargo/bin y ~/go/bin.
  setenv("HOME", root.c_str(), 1);
  unsetenv("TUIDE_MATLAB_LS");
  unsetenv("TUIDE_MATLAB_ROOT");
  unsetenv("TUIDE_NODE_BIN");

  check(tuide::language_server_id_for_language("matlab") == tuide::kLspServerMatlabLs,
        "matlab maps to the MATLAB language server");
  check(!tuide::make_matlab_ls_spec("/ws").has_value(), "no server installed gives no spec");

  const fs::path script = root / "index.js";
  std::ofstream(script) << "// fake\n";
  setenv("TUIDE_MATLAB_LS", script.c_str(), 1);
  check(!tuide::make_matlab_ls_spec("/ws").has_value(), "a .js without node gives no spec");

  write_executable(bin / "node");
  {
    const auto spec = tuide::make_matlab_ls_spec("/ws");
    check(spec.has_value(), "index.js + node gives a spec");
    if (spec.has_value()) {
      check(spec->command == (bin / "node").string(), "the .js runs under node");
      check(spec->args.size() >= 2 && spec->args[0] == script.string() &&
                spec->args[1] == "--stdio",
            "script path comes first, then --stdio");
      check(has_arg(*spec, "--indexWorkspace"), "workspace indexing is requested");
      check(spec->language_ids.count("matlab") == 1, "spec serves matlab");
      check(spec->workspace_root == "/ws", "workspace root is forwarded");
      for (const auto& a : spec->args) {
        check(a.rfind("--matlabInstallPath", 0) != 0, "no install path unless configured");
      }
    }
  }

  setenv("TUIDE_MATLAB_ROOT", "/opt/MATLAB/R2024b", 1);
  {
    const auto spec = tuide::make_matlab_ls_spec("/ws");
    check(spec.has_value() && has_arg(*spec, "--matlabInstallPath=/opt/MATLAB/R2024b"),
          "TUIDE_MATLAB_ROOT becomes --matlabInstallPath");
  }
  unsetenv("TUIDE_MATLAB_ROOT");

  unsetenv("TUIDE_MATLAB_LS");
  write_executable(bin / "matlab-language-server");
  {
    const auto spec = tuide::make_matlab_ls_spec("/ws");
    check(spec.has_value(), "launcher on PATH gives a spec");
    if (spec.has_value()) {
      check(spec->command == (bin / "matlab-language-server").string(), "launcher is the command");
      check(!spec->args.empty() && spec->args[0] == "--stdio", "launcher gets --stdio first");
    }
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  if (failures != 0) {
    return 1;
  }
  std::cout << "matlab_ls_spec_test OK\n";
  return 0;
}
