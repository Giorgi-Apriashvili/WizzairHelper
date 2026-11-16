#include "font_warmup.hpp"

#include "qt_strings.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QFontMetrics>

#include <utility>

namespace wizz::app {

std::vector<QFont> FontWarmup::default_fonts()
{
    // The UI fonts plus the monospace and semibold faces the busy panel and the grids use.
    return {style::form_font(),         style::bold_form_font(), style::data_font(), style::mono_font(10.0, true),
            style::mono_font(9.5),        style::semibold_font(10.0), style::hint_font(), style::caption_font()};
}

QString FontWarmup::default_sample()
{
    // The wait notes (01 §10.5) plus the punctuation of the status lines and grids.
    return to_qstring(ui::strings::retry_countdown("0.0")) + QLatin1Char(' ') +
           to_qstring(ui::strings::waiting_for_server("0")) +
           QStringLiteral(" → … — – · “” ≤");
}

FontWarmup::FontWarmup(std::vector<QFont> fonts, QString sample)
    : thread_([fonts = std::move(fonts), sample = std::move(sample)] {
          for (const QFont& font : fonts) {
              // Shaping the text resolves every glyph, the fallback fonts included.
              (void)QFontMetrics{font}.horizontalAdvance(sample);
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
