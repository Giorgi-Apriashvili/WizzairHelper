#include "font_warmup.hpp"

#include "qt_strings.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QRect>

#include <algorithm>
#include <utility>

namespace wizz::app {

std::vector<QFont> FontWarmup::default_fonts()
{
    // The UI fonts plus the monospace and semibold faces the busy panel and the grids use.
    // The busy panel's wait note, where the "⏳" appears, is the semibold face at the UI
    // size (bold_form_font); a status line too long for the UI font is drawn at 9 pt
    // (hint_font). Each is a font engine of its own, and so is each text rendering (D-58:
    // the hinting preference and the weight axis are part of the engine's key): ui_style's
    // fonts carry the one the app draws with.
    return {style::form_font(), style::bold_form_font(), style::data_font(), style::mono_font(10.0, true),
            style::mono_font(9.5), style::hint_font(), style::caption_font()};
}

QString FontWarmup::default_sample()
{
    // The wait notes (01 §10.5), the punctuation of the status lines and grids, and the
    // letters and digits every status line and grid row is made of, so their outlines
    // are read too.
    return to_qstring(ui::strings::retry_countdown("0.0")) + QLatin1Char(' ') +
           to_qstring(ui::strings::waiting_for_server("0")) +
           QStringLiteral(" → … — – · “” ≤ ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 (),.:;%");
}

QImage FontWarmup::warm(const QFont& font, const QString& sample)
{
    // Laying the text out resolves every glyph, the fallback fonts included; drawing it
    // reads their outlines and colour layers.
    const QFontMetrics metrics{font};
    const QRect bounds = metrics.boundingRect(sample);
    QImage image{std::max(1, bounds.width() + 8), std::max(1, metrics.height() + 8),
                 QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);
    QPainter painter{&image};
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(4 - bounds.x(), 4 + metrics.ascent(), sample);
    return image;
}

FontWarmup::FontWarmup(std::vector<QFont> fonts, QString sample, Step step)
    : thread_([fonts = std::move(fonts), sample = std::move(sample), step = std::move(step)] {
          for (const QFont& font : fonts) {
              if (step) {
                  step(font, sample);
              } else {
                  (void)warm(font, sample);
              }
          }
      })
{
}

FontWarmup::~FontWarmup()
{
    wait();
}

void FontWarmup::wait()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace wizz::app
