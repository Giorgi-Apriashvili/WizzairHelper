#pragma once
// A titled card of the grouped input area (DECISIONS D-53):
//
//   ┌──────────────────────────────────────────────────┐
//   │ (icon) CAPTION   optional hint in muted text     │
//   │ body rows …                                      │
//   └──────────────────────────────────────────────────┘
//
// The caption row is as tall with a hint as without one, so cards side by side
// keep their rows on one line.
// A rounded surface with a 1 px border on the window background; the caption is
// small, semibold, upper case and tracked, the icon an accent-coloured glyph of
// the icon font (Segoe Fluent Icons, Segoe MDL2 Assets on Windows 10). Colours
// are read from the theme when the card paints, and the caption and hint take
// theirs again when the look changes.

#include <QMargins>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace wizz::app {

/// One icon-font glyph in the accent colour (or the muted text colour), optionally
/// mirrored (the return plane).
class GlyphLabel final : public QWidget {
    Q_OBJECT

public:
    enum class Tone { accent, muted };

    GlyphLabel(char16_t glyph, bool mirrored, QWidget* parent = nullptr, Tone tone = Tone::accent,
               qreal point_size = 11.0);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    char16_t glyph_;
    bool mirrored_;
    Tone tone_;
    qreal point_size_;
};

class SectionCard final : public QWidget {
    Q_OBJECT

public:
    /// Contents margins of the card.
    static constexpr QMargins kPadding{14, 6, 14, 8};
    /// Between the caption row and the first body row, and between body rows.
    static constexpr int kHeaderGap = 5;
    static constexpr int kRowGap = 6;

    SectionCard(const QString& title, char16_t glyph, bool mirror_glyph = false, QWidget* parent = nullptr);

    /// The rows below the caption.
    [[nodiscard]] QVBoxLayout* body() const noexcept { return body_; }
    [[nodiscard]] QLabel* title_label() const noexcept { return title_; }
    /// A muted hint after the caption (created on first use).
    QLabel* set_hint(const QString& text);
    [[nodiscard]] QLabel* hint_label() const noexcept { return hint_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QVBoxLayout* body_ = nullptr;
    QHBoxLayout* header_ = nullptr;
};

} // namespace wizz::app
