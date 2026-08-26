// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_LAUNCHERART_HPP
#define NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_LAUNCHERART_HPP

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>

#include <cstdint>

// QSizeF arrives with <QSize>; QIcon and QPainter are only ever referenced here.
class QIcon;
class QPainter;

// Everything the project-selection window has to draw with a QPainter because a Qt style sheet
// cannot express it. Collected in one place so the launcher's widgets stay layout code.
//
// WHY THIS IS PAINTED RATHER THAN SHIPPED AS PNGs, decided per asset and not once for all:
//
//   * THE BANNER TEXTURE is painted because its WIDTH IS NOT FIXED. It spans the central widget,
//     and the window is resizable, so a bitmap would have to tile or stretch. A contour field is
//     a set of continuous closed curves and does not tile; stretching it stretches the 1px line
//     weight along with everything else and the lines stop being hairlines. A painted field is
//     regenerated at the exact width. It is also correct at any device pixel ratio by
//     construction -- this matters, because QIcon::pixmap takes a LOGICAL size and hands back a
//     ratio-1 bitmap, which is the trap the 28px product mark fell into before it was fixed, and
//     a PNG banner would need an @2x twin plus the loader logic to choose it.
//
//   * THE COLOURS COME FROM THE THEME. Every colour below arrives as an argument; BrandBanner
//     takes its set from qproperty- declarations in theme.qss. Baked into a PNG they could not
//     follow a theme, and this file would then be the one place in the application where the
//     palette is unreachable.
//
//   * THE SEED IS A CONSTANT. The field, the glow placement and each project's artwork are
//     deterministic functions of a fixed seed and of the project path. A banner that differed
//     between two launches would read as a rendering fault rather than as texture.
//
// Nothing here reads the client archives, and nothing here touches a file except the optional
// per-project artwork sidecar, which is allowed to be absent and usually is.
namespace Noggit::Ui::Widget::LauncherArt
{
  // -------------------------------------------------------------- action button tiles --

  //! The rounded square that carries an action button's glyph. 40px so it fits the 42px content
  //! rect a 72px button lays out with (see the arithmetic at QPushButton#launcher-action in
  //! theme.qss) with one pixel either side.
  constexpr int TILE_EXTENT = 40;

  //! Transparent padding baked into the RIGHT of the tile pixmap.
  //!
  //! QCommonStyle::drawControl(CE_PushButtonLabel) lays a button's icon and its text out as one
  //! group separated by a hardcoded 4px, and neither the style sheet nor QPushButton exposes that
  //! number. The only way to widen the gap is to make the icon wider than the drawing inside it,
  //! so the pixmap is TILE_EXTENT + TILE_GAP across and setIconSize() is told the same.
  constexpr int TILE_GAP = 12;

  //! The mark inside the tile. 20 of 40 leaves a 10px surround, which is the same proportion the
  //! 16px glyph has inside the application's 28px tool buttons (16/28 = 0.571, 20/40 = 0.500 --
  //! slightly airier, because this tile is four times the area and reads heavier at equal ratio).
  constexpr int GLYPH_EXTENT = 20;

  // ----------------------------------------------------------------------- the field --

  //! Draws a topographic contour field over `area` (LOGICAL pixels, origin at the painter's
  //! current origin) with a marching-squares pass at nine levels.
  //!
  //! The field is a LATTICE of seeded values smoothstep-interpolated to a grid whose cells are
  //! `step` logical pixels across, so the cost is O(area / step^2) per level and independent of
  //! the device pixel ratio. Line alpha ramps from `alpha_low` at the lowest contour to
  //! `alpha_high` at the highest, which is what makes the field read as terrain rather than as
  //! a moire pattern.
  void paintContourField ( QPainter& painter
                         , QSizeF const& area
                         , std::uint32_t seed
                         , QColor const& ink
                         , qreal alpha_low
                         , qreal alpha_high
                         , int step
                         );

  // ------------------------------------------------------------------------- pixmaps --

  //! The full banner ground: the left-to-right darkening band, the seeded glows, the contour
  //! field faded out towards the dark end, and the closing rule. `logical` is in logical pixels
  //! and `ratio` is the device pixel ratio the result is stamped with, so the caller can draw it
  //! at 1:1 with drawPixmap.
  QPixmap bannerTexture ( QSize const& logical
                        , qreal ratio
                        , QColor const& band_left
                        , QColor const& band_right
                        , QColor const& ink
                        , QColor const& rule_ink
                        );

  //! A circular crop of `icon` with a 1px ring.
  //!
  //! A style sheet cannot do this: border-radius rounds the widget's BACKGROUND box, and a
  //! QLabel's pixmap is drawn over that box unclipped, so a rounded QLabel showing a square
  //! picture is still a square picture. The circle has to be baked.
  QPixmap circularIcon (QIcon const& icon, int extent, QColor const& ring, qreal ratio);

  //! A rounded-square tile with a theme glyph stamped in the middle. `glyph` is a Font Awesome
  //! codepoint; see FontAwesome::ThemeIcons.
  //!
  //! THE GLYPH CANNOT COME FROM FontAwesomeIcon. Both icon engines resolve their pen from one of
  //! exactly two theme-owned slots -- an unchecked icon is text.dim and a checked one is the
  //! accent -- and neither of them is ink, which is what a mark on a saturated fill has to be.
  //! That is the measured reason the primary button has carried no glyph at all until now.
  //! ThemeIcons::pixmap TAKES a pen colour, so the tile can state the contrast it needs.
  //!
  //! Returns a tile with no mark when the active theme ships no artwork under that codepoint's
  //! name and the glyph font is absent, which is a legitimate state and not a failure: the tile
  //! still holds the button's geometry so all three action buttons align.
  QPixmap actionTile (char32_t glyph, QColor const& tile, QColor const& ink, qreal ratio);

  //! The small calendar mark beside a project's date. Painted rather than taken from the icon
  //! set because the shipped theme has no calendar artwork and the Font Awesome font is not
  //! redistributed with this fork, so an icon-set route would resolve to nothing on a clean
  //! install -- and a date row whose glyph is missing looks like a bug rather than like a date.
  QPixmap calendarGlyph (int extent, QColor const& ink, qreal ratio);

  //! A project's card artwork, in three tiers, resolved in this order:
  //!
  //!   1. `<project directory>/project_art.png`, then `.jpg`, then `.jpeg`. A SIDECAR, because
  //!      the project JSON cannot carry one: ApplicationProjectWriter rebuilds the whole
  //!      QJsonObject from NoggitProject's members on every save, so a key that is not also a
  //!      member survives reads and is silently deleted the first time the user pins a map.
  //!      Adding the field properly means the reader, the writer and the struct in one commit,
  //!      and it is not needed to fill the slot.
  //!   2. Nothing else -- there is no second file to look for.
  //!   3. A DETERMINISTIC PROCEDURAL TILE, seeded from the project path, which is what every
  //!      existing project gets because none of them has a sidecar. It is not a placeholder: two
  //!      projects side by side carry visibly different fields, so the tile is an identifier.
  //!
  //! The slot is filled in all three cases on purpose. The card reserves a right-hand column for
  //! it, and a column that is empty for every project is a hole in the card -- and the layout
  //! would reflow the day one project acquired a picture.
  QPixmap projectArtwork ( QString const& project_directory
                         , QSize const& logical
                         , QColor const& fill
                         , QColor const& ink
                         , QColor const& ring
                         , qreal ratio
                         );
}

#endif // NOGGIT_UI_WINDOWS_PROJECTSELECTION_WIDGETS_LAUNCHERART_HPP
