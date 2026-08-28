// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/texturing/TextureLayerPolicy.hpp>

#include <utility>

namespace Noggit
{
  namespace
  {
    // Function-local rather than a namespace-scope global so it is constructed on first use. A
    // namespace-scope std::string here would be one more entry in the static initialisation order
    // of a binary that already builds a Qt application, a texture manager and a listfile at start.
    TextureLayerAdmission& mutableCurrent()
    {
      static TextureLayerAdmission admission;
      return admission;
    }
  }

  TextureLayerAdmission const& TextureLayerAdmission::current()
  {
    return mutableCurrent();
  }

  void TextureLayerAdmission::setCurrent(TextureLayerAdmission admission)
  {
    mutableCurrent() = std::move(admission);
  }

  float LayerAlphaProfile::coverage(std::size_t layer) const
  {
    if (layer >= layers || layer >= MAX_LAYERS)
    {
      return 0.f;
    }

    return static_cast<float>(sum[layer]) / static_cast<float>(MAX_LAYER_SUM);
  }
}
