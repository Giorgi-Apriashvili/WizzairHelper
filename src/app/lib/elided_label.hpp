#pragma once
// A single-line plain-text label that ends with "…" when the text does not fit
// (WinForms Label.AutoEllipsis, used by the BusyPanel status line, 01 §10.1).
// The full text stays available through text() and, while it is cut, as the
// hover tooltip (as AutoEllipsis does). Unlike QLabel it never asks for the
// width of its text: the layout gives it the space and it elides into that.

#include <QString>
#include <QWidget>

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
};

} // namespace wizz::app
