#pragma once
// A titled card of the grouped input area (prototype "layout" direction):
//
//   ┌──────────────────────────────────────────────────┐
//   │ (icon) CAPTION   optional hint in muted text     │
//   │ body rows …                                      │
//   └──────────────────────────────────────────────────┘
//
// A rounded surface with a 1 px border on the window background; the caption is
// small, semibold, upper case and tracked, the icon an accent-coloured glyph of
// the icon font (Segoe Fluent Icons, Segoe MDL2 Assets on Windows 10).

#include <QColor>
#include <QMargins>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace wizz::app {

/// One icon-font glyph, by default in the accent colour, optionally mirrored (the return plane).
class GlyphLabel final : public QWidget {
    Q_OBJECT

public:
    GlyphLabel(char16_t glyph, bool mirrored, QWidget* parent = nullptr, QColor colour = {},
               qreal point_size = 11.0);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    char16_t glyph_;
    bool mirrored_;
    QColor colour_;
    qreal point_size_;
};

class SectionCard final : public QWidget {
    Q_OBJECT

public:
    /// Contents margins of the card.
    static constexpr QMargins kPadding{14, 8, 14, 10};

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
