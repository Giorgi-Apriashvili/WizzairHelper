#include "grid_top_bar.hpp"

#include "elided_label.hpp"
#include "ui_style.hpp"

#include <QHBoxLayout>
#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace wizz::app::grid_bar {

QHBoxLayout* make_row()
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(kPadding);
    row->setSpacing(0);
    return row;
}

void add(QHBoxLayout* row, QWidget* widget, QMargins margin, int stretch)
{
    auto* cell = new QVBoxLayout;
    cell->setContentsMargins(margin);
    cell->setSpacing(0);
    cell->addStretch(1);
    cell->addWidget(widget);
    cell->addStretch(1);  // vertically centred in the bar
    row->addLayout(cell, stretch);
}

ElidedLabel* make_summary_label(QWidget* parent)
{
    auto* label = new ElidedLabel(parent);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    style::set_text_tone(*label, style::TextTone::muted);
    return label;
}

QString text(std::string_view utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

} // namespace wizz::app::grid_bar
