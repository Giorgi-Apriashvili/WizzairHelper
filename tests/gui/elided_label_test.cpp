// ElidedLabel: the BusyPanel status line (01 §10.1, WinForms AutoEllipsis): one line
// of plain text cut with "…" to the width it is given, the full text kept.

#include "elided_label.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QHelpEvent>

#include <gtest/gtest.h>

namespace {

using wizz::app::ElidedLabel;

TEST(ElidedLabel, Keeps_the_whole_text_and_cuts_only_the_painted_one)
{
    ElidedLabel label;
    const QString text = QStringLiteral("Fetching fares:  KSC → London (LON)   20 Nov – 5 Dec    (5 of 7)");
    label.setText(text);
    label.resize(label.fontMetrics().horizontalAdvance(text) + 10, 20);
    EXPECT_FALSE(label.is_elided());
    EXPECT_EQ(label.elided_text(), text);
    EXPECT_TRUE(label.tooltip_text().isEmpty());

    label.resize(label.fontMetrics().horizontalAdvance(text) / 2, 20);
    EXPECT_TRUE(label.is_elided());
    EXPECT_EQ(label.text(), text);
    EXPECT_TRUE(label.elided_text().endsWith(QChar(0x2026)));
    EXPECT_LE(label.fontMetrics().horizontalAdvance(label.elided_text()), label.width());
    EXPECT_EQ(label.tooltip_text(), text);  // AutoEllipsis shows the full text on hover
}

TEST(ElidedLabel, A_compact_font_takes_a_text_too_long_for_the_label_font_before_it_is_cut)
{
    ElidedLabel label;
    QFont regular = label.font();
    regular.setPointSizeF(10.0);
    label.setFont(regular);
    QFont compact = regular;
    compact.setPointSizeF(9.0);
    const QString text = QStringLiteral("Use a smaller radius, fewer dates, or a specific destination.");
    const int full = QFontMetrics(regular).horizontalAdvance(text);
    const int small = QFontMetrics(compact).horizontalAdvance(text);
    ASSERT_LT(small, full);
    label.setText(text);
    label.resize((full + small) / 2, 20);
    EXPECT_TRUE(label.is_elided()) << "no compact font: cut";
    EXPECT_EQ(label.painted_font(), regular);

    label.set_compact_font(compact);
    EXPECT_FALSE(label.is_elided());
    EXPECT_EQ(label.elided_text(), text);
    EXPECT_EQ(label.painted_font(), compact);

    label.resize(full + 4, 20);
    EXPECT_EQ(label.painted_font(), regular) << "the label's own font while the text fits";

    label.resize(small / 2, 20);
    EXPECT_TRUE(label.is_elided());
    EXPECT_LE(QFontMetrics(compact).horizontalAdvance(label.elided_text()), label.width());
    EXPECT_EQ(label.tooltip_text(), text);

    label.set_compact_font(std::nullopt);
    EXPECT_EQ(label.painted_font(), regular);
}

TEST(ElidedLabel, A_text_that_fits_has_no_hover_tooltip)
{
    ElidedLabel label;
    label.setText(QStringLiteral("Places found."));
    label.resize(400, 20);
    QHelpEvent help(QEvent::ToolTip, QPoint(5, 5), QPoint(5, 5));
    QCoreApplication::sendEvent(&label, &help);
    EXPECT_FALSE(help.isAccepted());
}

TEST(ElidedLabel, Is_plain_text_without_mnemonics)
{
    ElidedLabel label;
    label.setText(QStringLiteral("<b>Fares & taxes</b>"));
    label.resize(400, 20);
    EXPECT_EQ(label.elided_text(), QStringLiteral("<b>Fares & taxes</b>"));
}

TEST(ElidedLabel, Never_asks_for_the_width_of_its_text)
{
    ElidedLabel label;
    label.setText(QString(500, QChar(u'W')));
    EXPECT_EQ(label.minimumSizeHint().width(), 0);
    EXPECT_EQ(label.sizePolicy().horizontalPolicy(), QSizePolicy::Ignored);
    EXPECT_EQ(label.alignment(), Qt::AlignLeft | Qt::AlignVCenter);  // MiddleLeft
}

} // namespace
