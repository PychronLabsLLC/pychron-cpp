#pragma once

// A scene written as SVG (data publishing design, section 5.6): the draw list
// (scene_draw.hpp) as one path or text element per item, sized in points,
// fonts named and not embedded. The same scene gives the same bytes on every
// compiler: numbers go through format_coord, attributes are in a fixed order,
// lines end in "\n".

#include <string>

#include "pychron/processing/scene.hpp"
#include "pychron/processing/scene_draw.hpp"
#include "pychron/processing/scene_layout.hpp"

namespace pychron::processing {

std::string draw_list_svg(const DrawList& list);
std::string scene_svg(const Scene& scene, const FigureSize& size = {});

}  // namespace pychron::processing
