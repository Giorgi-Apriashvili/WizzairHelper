#pragma once
// A single-line plain-text label that ends with "…" when the text does not fit
// (WinForms Label.AutoEllipsis, used by the BusyPanel status line, 01 §10.1).
// The full text stays available through text() and, while it is cut, as the
// hover tooltip (as AutoEllipsis does). Unlike QLabel it never asks for the
// width of its text: the layout gives it the space and it elides into that.
// An optional compact font (the busy panel's: 1 pt smaller) takes a text that
// does not fit in the label's own font before anything is cut.

#include <QFont>
#include <QString>
#include <QWidget>

#include <optional>

namespace wizz::app {

class ElidedLabel final : public QWidget {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);

    [[nodiscard]] const QString& text() const noexcept { return text_; }
    /// Plain text; no mnemonics or rich text. Repaints only on a change.
    void setText(const QString& text);

    [[nodiscard]] Qt::Alignment alignment() const noexcept { return alignment_; }
    void setAlignment(Qt::Alignment alignment);

    /// A smaller font for a text that does not fit in the label's font (none by default):
    /// the text is drawn in it when it does not fit otherwise, and cut only when it does
    /// not fit in it either.
    void set_compact_font(const std::optional<QFont>& font);
    [[nodiscard]] const std::optional<QFont>& compact_font() const noexcept { return compact_; }
    /// The font the text is drawn in for the current text and width.
    [[nodiscard]] QFont painted_font() const;

    /// The text as painted into the current contents rectangle.
    [[nodiscard]] QString elided_text() const;
    [[nodiscard]] bool is_elided() const;
    /// What hovering shows: the full text while it is cut, else nothing.
    [[nodiscard]] QString tooltip_text() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QString text_;
    Qt::Alignment alignment_ = Qt::AlignLeft | Qt::AlignVCenter;
    std::optional<QFont> compact_;
};

} // namespace wizz::app
