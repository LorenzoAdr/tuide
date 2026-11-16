// Gramática de MATLAB: que un .m se reconozca, parsee sin errores, dé outline
// (función suelta, classdef con métodos) y que la query de resaltado compile.
// Va aparte de tree_sitter_test porque ese binario ya estaba en rojo por otras causas.

#include <cstring>
#include <iostream>
#include <string>

#include "editor/line_comment.hpp"
#include "lsp/lsp_uri.hpp"
#include "parser/tree_sitter_highlight.hpp"
#include "parser/tree_sitter_language.hpp"
#include "parser/tree_sitter_symbols.hpp"

extern "C" {
#include <tree_sitter/api.h>
}

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::cout << "FAIL: " << what << '\n';
    ++failures;
  }
}

struct Parsed {
  TSParser* parser = nullptr;
  TSTree* tree = nullptr;
  ~Parsed() {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }
};

bool parse(const std::string& source, Parsed* out) {
  out->parser = ts_parser_new();
  if (!ts_parser_set_language(out->parser, tuide::tree_sitter_matlab_language())) {
    return false;
  }
  out->tree = ts_parser_parse_string(out->parser, nullptr, source.data(),
                                     static_cast<uint32_t>(source.size()));
  return out->tree != nullptr;
}

bool has_capture(const tuide::LineHighlights& line, int col, const char* prefix) {
  for (const auto& span : line.spans) {
    if (span.start_col <= col && col < span.end_col &&
        span.capture.compare(0, std::strlen(prefix), prefix) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using tuide::SymbolKind;
  using tuide::TreeSitterLangKind;

  check(tuide::language_id_for_path("src/solve.m") == "matlab", ".m maps to matlab");
  check(tuide::tree_sitter_lang_kind_for_path("src/solve.m") == TreeSitterLangKind::kMatlab,
        ".m picks the matlab grammar");
  check(tuide::tree_sitter_lang_kind_for_alias("matlab") == TreeSitterLangKind::kMatlab,
        "fenced ```matlab alias");
  check(tuide::tree_sitter_lang_kind_for_alias("m") == TreeSitterLangKind::kNone,
        "bare 'm' alias stays unclaimed");

  const std::string functions =
      "function y = solve(A, b)\n"
      "  % resuelve Ax = b\n"
      "  y = A \\ b;\n"
      "  if isempty(y), y = helper(b); end\n"
      "end\n"
      "\n"
      "function z = helper(b)\n"
      "  z = zeros(size(b)) + 1.5e3;\n"
      "  s = 'it''s';\n"
      "end\n";
  {
    Parsed p;
    check(parse(functions, &p), "parse function file");
    const TSNode root = ts_tree_root_node(p.tree);
    check(!ts_node_has_error(root), "function file parses without ERROR nodes");

    const auto symbols = tuide::extract_symbols_from_tree(root, functions, "solve.m");
    bool solve = false;
    bool helper = false;
    for (const auto& sym : symbols) {
      solve = solve || (sym.name.find("solve") != std::string::npos &&
                        sym.kind == SymbolKind::kFunction && sym.line == 1 && sym.depth == 0);
      helper = helper || (sym.name.find("helper") != std::string::npos && sym.line == 7);
    }
    check(solve, "outline has function solve at line 1");
    check(helper, "outline has function helper at line 7");

    // Si la query upstream dejara de compilar contra la gramática, ts_query_new
    // devuelve nullptr y todo saldría en texto plano sin ningún aviso.
    const auto lines = tuide::highlights_for_document(root, functions, TreeSitterLangKind::kMatlab);
    check(lines.size() >= 9, "highlight covers the document");
    if (lines.size() >= 9) {
      check(has_capture(lines[0], 0, "keyword"), "'function' is a keyword");
      check(has_capture(lines[0], 13, "function"), "function name is captured");
      check(has_capture(lines[1], 4, "comment"), "% comment is captured");
      check(has_capture(lines[7], 26, "constant.numeric"), "number literal is captured");
      check(has_capture(lines[8], 8, "string"), "single-quoted string is captured");
    }
  }

  const std::string classdef =
      "classdef Point < handle\n"
      "  properties\n"
      "    x = 0\n"
      "  end\n"
      "  methods\n"
      "    function obj = Point(x)\n"
      "      obj.x = x;\n"
      "    end\n"
      "    function r = norm(obj)\n"
      "      r = abs(obj.x);\n"
      "    end\n"
      "  end\n"
      "end\n";
  {
    Parsed p;
    check(parse(classdef, &p), "parse classdef");
    const TSNode root = ts_tree_root_node(p.tree);
    check(!ts_node_has_error(root), "classdef parses without ERROR nodes");
    const auto symbols = tuide::extract_symbols_from_tree(root, classdef, "Point.m");
    bool cls = false;
    bool method = false;
    for (const auto& sym : symbols) {
      cls = cls || (sym.name.find("Point") != std::string::npos &&
                    sym.kind == SymbolKind::kClass && sym.depth == 0);
      method = method || (sym.name.find("norm") != std::string::npos && sym.depth == 1 &&
                          sym.line == 9);
    }
    check(cls, "outline has class Point");
    check(method, "method norm is nested under the class");
  }

  if (failures != 0) {
    return 1;
  }
  std::cout << "tree_sitter_matlab_test OK\n";
  return 0;
}
