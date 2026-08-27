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
//   * THE BANNER GROUND IS NOW ARTWORK, and the procedural contour field that used to fill it
//     is gone from bannerTexture. The reasoning that put a painter there was sound for a
//     PROCEDURAL field -- it does not tile and stretching it destroys the hairline weight -- and
//     it does not apply to a photographic one. banner_backdrop.jpg is 2880x480, and a banner is
//     a wide short strip, so the picture is COVER-fitted rather than stretched: the aspect ratio
//     is preserved, the overflow is cropped, and the result is stamped with the device pixel
//     ratio so drawPixmap places it at logical coordinates without a second scale. At 1440
//     logical pixels wide on a ratio-2 display the device size is exactly 2880x288, the cover
//     scale is exactly 1.0, and no resampling happens at all.
//
//     What is still painted here is everything the artwork cannot decide: the band gradient
//     underneath it (which is also the fallback when the decode fails), the veil that holds the
//     artwork's embers below the text floors, the scrim under the wordmark, and the rule.
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

  //! The full banner ground: the left-to-right darkening band, the cover-fitted backdrop
  //! artwork, the veil that tames it, the scrim that holds the wordmark legible, and the closing
  //! rule. `logical` is in logical pixels and `ratio` is the device pixel ratio the result is
  //! stamped with, so the caller can draw it at 1:1 with drawPixmap.
  //!
  //! THE VEIL AND THE SCRIM ARE NOT DECORATION, they are what makes the artwork usable. The
  //! backdrop is an ember field and it is not uniformly dark: sampled over the rows a cover crop
  //! of a 144-logical band presents at the width the launcher opens at, it runs #000000 to
  //! #FE777E -- relative luminance 0.00000 to 0.35771 -- and its brightest pixel anywhere is
  //! #FF9F7A at 0.474614, which a narrow window's crop can bring into view. text.hi #F3F0E9 on
  //! that measures 1.759:1 and brand.crimson 2.010:1, so RAW ARTWORK CARRIES NO TEXT AT ALL. Two
  //! layers fix it, and both alphas below were SOLVED for rather than chosen:
  //!
  //!   * THE VEIL is the same band gradient painted back over the artwork at 58% alpha, over the
  //!     whole band. It is a TAMING layer, not a guarantee: it exists so an ember cannot read as
  //!     a light source beside dark chrome. At the widths these two windows actually open at,
  //!     where the crop is vertical only, the brightest surviving pixel measures #744142 and
  //!     text.hi on it is 7.159:1. Squeezed to 800 logical, where the crop turns horizontal and
  //!     more of the picture comes into view, the worst case is #784D3C and text.hi is 6.307:1 --
  //!     under the 7:1 prose floor and over the 4.5 body floor. That is accepted BECAUSE NO TEXT
  //!     IS DRAWN THERE: past scrim_fade the band carries decoration and, on the map-selection
  //!     window, one opaque button that supplies its own fill.
  //!
  //!   * THE SCRIM is `scrim_ink` at 86% alpha, held solid to `scrim_hold` and ramped to nothing
  //!     by `scrim_fade` (both LOGICAL pixels from the left edge). This one IS the guarantee, and
  //!     the binding constraint is not the light half of the wordmark but the crimson half:
  //!     brand.crimson needs its background under L = 0.007996 to hold 4.5:1. Veil and scrim
  //!     compose to 1 - 0.42 x 0.14 = 0.9412 -- and because the scrim is at full strength from
  //!     x = 0 to scrim_hold at EVERY width, that composite is the same wherever the window is
  //!     sized to. Against the brightest pixel the artwork contains at all, #FF9F7A, it lands on
  //!     #19120F: text.hi 16.260:1, brand.crimson 4.600:1. Against the darkest it lands on
  //!     #0C0908: text.hi 17.437:1, brand.crimson 4.932:1. Both floors hold unconditionally.
  //!
  //! Pass scrim_fade <= scrim_hold to omit the scrim entirely.
  QPixmap bannerTexture ( QSize const& logical
                        , qreal ratio
                        , QColor const& band_left
                        , QColor const& band_right
                        , QColor const& scrim_ink
                        , QColor const& rule_ink
                        , qreal scrim_hold
                        , qreal scrim_fade
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
