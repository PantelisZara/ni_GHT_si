#pragma once
#include "ni_ght_si/engine.hpp"
#include <QVariantList>

namespace night::ui {
// Preserve every retained sample, including explicit gaps; no interpolated zeros.
QVariantList historySeries(const std::vector<HistoryPoint>& points);
}
