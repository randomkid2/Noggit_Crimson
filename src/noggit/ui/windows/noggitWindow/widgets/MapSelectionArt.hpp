// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WINDOWS_NOGGITWINDOW_WIDGETS_MAPSELECTIONART_HPP
#define NOGGIT_UI_WINDOWS_NOGGITWINDOW_WIDGETS_MAPSELECTIONART_HPP

#include <QColor>
#include <QPixmap>

class QIcon;

// The marks the map-selection window draws with a QPainter, collected in one place so the
// window's own files stay layout code.
//
// WHY THESE FIVE ARE PAINTED AND NOT TAKEN FROM THE ICON SET. The set is real and is preferred
// wherever it has the mark: the tab glyphs, the gear and the info mark below all come from
// FontAwesome's chain, whose first tier is <exe dir>/themes/<theme>/icons/<name>.png and needs
// no font at all. The set was inventoried before any of this was written, and it ships
// map.png, bookmark.png, cog.png, info.png, plus.png, caretright.png, chevrondown.png and
// chevronup.png -- and NO magnifier, NO map pin and NO chevron pointing RIGHT.
//
// The three that are missing could only come from the Font Awesome glyph font, which this fork
// deliberately does not redistribute -- its licence forbids committing it to a public
// repository -- so on a clean install they would resolve to nothing at all. A search field whose
// magnifier is absent looks like a broken field, and an empty state whose only illustration
// fails to draw looks like a failed load. Painting them is the difference between a mark that is
// always there and a mark that is there on the author's machine.
//
// The two that are NOT missing are here for a different and narrower reason, stated at each.
//
// Every function takes its pen colour, which is the other half of the argument: both icon
// engines resolve their pen from one of exactly two theme-owned slots -- an unchecked icon is
// text.dim and a checked one is the accent -- so neither can put text.hi on a crimson-tinted
// button or a hairline on a card. These can.
//
// Every function takes the device pixel ratio and returns a pixmap already stamped with it, so
// the caller draws at logical size and the mark is rendered at the display's real resolution.
// Getting that wrong is exactly the 4x pixel deficit that made this window's icons look soft.
namespace Noggit::Ui::Widget::MapSelectionArt
{
  //! The magnifier beside the SEARCH heading. Not in the shipped icon set.
  QPixmap magnifierGlyph (int extent, QColor const& ink, qreal ratio);

  //! The outlined pin that fills the right pane's empty state. Not in the shipped icon set.
  //! Drawn as a real teardrop -- a circle with the two tangents from the point below it, so the
  //! silhouette has no corner where the head meets the spike.
  QPixmap mapPinGlyph (int extent, QColor const& ink, qreal ratio);

  //! The chevron at the right edge of a map card. The set has chevrondown and chevronup but no
  //! chevronright, and caretright is a filled triangle, which is a different mark: a caret means
  //! "expand me", a chevron means "there is more this way".
  QPixmap chevronGlyph (int extent, QColor const& ink, qreal ratio);

  //! The plus on the add-map button. plus.png IS in the set, and this is here anyway because the
  //! button's fill is brand.fill #2E1516 and its label is text.hi #F3F0E9 at 14.940:1 -- a
  //! text.dim plus beside a text.hi label would be a two-tone control. The icon engine cannot be
  //! told to use text.hi; this can.
  QPixmap plusGlyph (int extent, QColor const& ink, qreal ratio);

  //! The circular emblem at the left of a map card: a filled disc, a hairline ring, and the
  //! expansion crest inset inside it.
  //!
  //! A STYLE SHEET CANNOT DO THIS. border-radius rounds a widget's BACKGROUND box, and a QLabel's
  //! pixmap is drawn over that box unclipped, so a rounded QLabel showing a square picture is
  //! still a square picture. The disc has to be baked, and baking it is also what gives the
  //! crests a common ground: the nine expansion PNGs are 225x225 with transparent corners, so
  //! without a disc behind them they float on whatever the card fill happens to be and the row
  //! has no consistent left column.
  QPixmap emblem ( QIcon const& crest
                 , int extent
                 , QColor const& fill
                 , QColor const& ring
                 , qreal ratio
                 );
}

#endif // NOGGIT_UI_WINDOWS_NOGGITWINDOW_WIDGETS_MAPSELECTIONART_HPP
