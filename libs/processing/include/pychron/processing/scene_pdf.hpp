#pragma once

// A scene written as a one-page PDF 1.4 (data publishing design, section
// 5.6): the draw list (scene_draw.hpp) as vector paths and text in the
// standard 14 fonts (Helvetica, Helvetica-Bold, Symbol for Greek letters;
// nothing embedded), uncompressed, no /ID, CreationDate the caller's. The
// same scene and info give the same bytes on every compiler. Text outside
// WinAnsiEncoding and the Greek letters is written '?'; anchors are placed
// with Helvetica's widths (scene_layout.hpp text_width), so a Greek or other
// glyph without a width there is off by a little. The UI keeps
// QPdfWriter for its own interactive export.

#include <cstdint>
#include <filesystem>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/processing/scene.hpp"
#include "pychron/processing/scene_draw.hpp"
#include "pychron/processing/scene_layout.hpp"

namespace pychron::processing {

struct PdfInfo {
  std::int64_t created_unix = 0;  // /CreationDate, UTC seconds (a release's created_utc)
  std::string title;              // /Title; empty: none
};

std::string draw_list_pdf(const DrawList& list, const PdfInfo& info);
std::string scene_pdf(const Scene& scene, const PdfInfo& info, const FigureSize& size = {});

// scene_pdf written to `path` and marked a user file; Io when it cannot be.
Result<void> save_scene_pdf(const Scene& scene, const PdfInfo& info, const std::filesystem::path& path,
                            const FigureSize& size = {});

}  // namespace pychron::processing
