#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ftxui/screen/color.hpp"

namespace tuide {

struct TerminalStyledSpan {
  std::string text;
  ftxui::Color fg = ftxui::Color::RGB(200, 230, 255);
  ftxui::Color bg = ftxui::Color::RGB(0, 0, 0);
  // True when SGR left the background at ANSI "default" (reset / 49). Renderers
  // should map this to the theme terminal background instead of opaque black.
  bool bg_default = true;
};

using TerminalStyledRow = std::vector<TerminalStyledSpan>;

// Qué parte del scrollback tiene ya una copia sincronizada con
// RawPtyScreen::sync_styled_rows. Las líneas cerradas no cambian nunca (solo se
// recortan por delante), así que basta con saber cuántas se copiaron y cuántas
// se habían descartado: el resto del historial no se vuelve a copiar.
struct TerminalRowsSyncCursor {
  std::uint64_t epoch = 0;
  std::uint64_t dropped = 0;
  std::size_t committed = 0;
};

// Lightweight PTY screen buffer. Handles text layout, basic cursor control and
// ANSI SGR colours used by shell prompts (16 / 256 / truecolor).
class RawPtyScreen {
 public:
  RawPtyScreen(int rows = 24, int cols = 80);

  void reset(int rows, int cols);
  void resize(int rows, int cols);
  void feed(const char* data, std::size_t len);

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  int cursor_col() const { return cursor_col_; }
  int cursor_row() const { return static_cast<int>(lines_.size()); }
  int total_rows() const { return static_cast<int>(lines_.size()) + 1; }
  std::string text() const;
  // Copia completa (historial + línea activa). Cara con mucho scrollback: la UI
  // debe usar sync_styled_rows.
  std::vector<TerminalStyledRow> styled_rows() const;
  // Deja *dst igual que styled_rows() copiando solo lo nuevo desde la última
  // sincronización con el mismo cursor: en una tecla, la línea activa.
  void sync_styled_rows(std::vector<TerminalStyledRow>* dst,
                        TerminalRowsSyncCursor* cursor) const;

 private:
  struct ScreenCell {
    char ch = ' ';
    ftxui::Color fg = ftxui::Color::RGB(200, 230, 255);
    ftxui::Color bg = ftxui::Color::RGB(0, 0, 0);
    bool bg_default = true;
  };

  void append_char(char ch);
  void newline();
  void carriage_return();
  void backspace();
  void clear_current_line();
  void clear_to_end_of_line();
  void clear_to_start_of_line();
  void delete_chars(int count);
  void insert_blanks(int count);
  void erase_chars(int count);
  void trim_lines();
  void apply_sgr(const std::string& params);
  void handle_csi(char cmd, const std::string& params);
  void skip_escape(const char*& p, const char* end);
  void ensure_current_width();
  void reset_sgr();
  // preserve_cols: do not trim trailing spaces before this column (active cursor).
  TerminalStyledRow spans_from_cells(const std::vector<ScreenCell>& cells,
                                     int preserve_cols = 0) const;
  std::vector<TerminalStyledRow> build_visible_rows() const;
  void rebuild_cache() const;

  int rows_ = 24;
  int cols_ = 80;
  int cursor_col_ = 0;
  bool bold_ = false;
  ftxui::Color fg_;
  ftxui::Color bg_;
  bool bg_default_ = true;
  std::vector<TerminalStyledRow> lines_;
  std::vector<ScreenCell> current_cells_;
  // Cambia cuando el historial se reescribe (reset/resize) y las copias ya no
  // valen. Único entre instancias para que un cursor no case con otra pantalla.
  std::uint64_t epoch_ = 0;
  // Líneas descartadas por delante desde el último cambio de epoch_.
  std::uint64_t dropped_lines_ = 0;
  mutable std::string cached_text_;
  mutable bool cache_valid_ = false;
};

}  // namespace tuide
