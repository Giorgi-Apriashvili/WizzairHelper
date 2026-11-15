#include "grid_top_bar.hpp"

#include "grid_colors.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace wizz::app::grid_bar {

QHBoxLayout* make_row()
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(0);
    return row;
}

void add(QHBoxLayout* row, QWidget* widget, QMargins margin, int stretch)
{
    auto* cell = new QVBoxLayout;
    cell->setContentsMargins(margin);
    cell->setSpacing(0);
    cell->addWidget(widget);
    cell->addStretch(1);  // top-aligned at its own height, like a FlowLayoutPanel row
    row->addLayout(cell, stretch);
}

QLabel* make_summary_label(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(false);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, grid_colors::gray_text());  // ForeColor GrayText
    label->setPalette(palette);
    return label;
}

QString text(std::string_view utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

} // namespace wizz::app::grid_bar
