// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The cube drawn where a model should be and is not.
//
// WHY NOT REUSE FakeGeometry. Model.h:131-137 already holds a box built from an M2's own
// bounding box, and it was the obvious candidate. It does not fit, for three reasons that are
// structural rather than stylistic:
//
//   1. It is built at Model.cpp:118, inside the successful-load path, from
//      Model::bounding_box_min/max. A model that threw never reaches that line, and those two
//      vectors are never written for it -- the constructor's memset is commented out at
//      Model.cpp:19-24 and GLM_FORCE_CTOR_INIT is not defined in this tree -- so there is
//      nothing to build a box from.
//   2. It is drawn through ModelRender, which returns at the top of both draw() overloads when
//      loading_failed() is set, and whose render passes need geometry, skins and a texture unit
//      that a failed model does not have.
//   3. It answers a different question. FakeGeometry means "this model loaded and has no visible
//      passes" -- a particle emitter -- and is drawn red at WorldRender.cpp so the mapper can see
//      an emitter. Overloading that same red box with "this file does not exist" would make the
//      two indistinguishable, and they need opposite fixes.
//
// So this is a separate, deliberately tiny primitive: one static unit cube, drawn in WORLD space
// around a centre, with a procedural checker. World space, not the instance transform, because
// ModelInstance::recalcExtents returns before updateTransformMatrix() for a failed model and
// _transform_mat is therefore the uninitialised default from SceneObject.hpp:94. The rotation of
// a model that does not exist means nothing anyway.
//
// The checker is generated in the fragment shader rather than sampled from
// textures/shanecube.blp. The placeholder is needed precisely when client data is broken or
// absent, so it must not itself depend on client data being loadable.

#ifndef NOGGIT_RENDERING_PLACEHOLDERCUBE_HPP
#define NOGGIT_RENDERING_PLACEHOLDERCUBE_HPP

#include <noggit/ContextObject.hpp>
#include <noggit/MissingPlacementLog.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <vector>

namespace Noggit::Rendering::Primitives
{
  struct PlaceholderCubeInstance
  {
    // World-space centre. For an ADT placement this is SceneObject::pos; for a WMO's internal
    // doodad it is wmo_doodad_instance::world_pos, which is valid after a failure because
    // update_transform_matrix_wmo gates only on finishedLoading().
    glm::vec3 centre = {0.0f, 0.0f, 0.0f};

    MissingPlacementKind kind = MissingPlacementKind::Model;

    // Drawn flat in the accent colour instead of checkered. The usual selection box comes from
    // WMOInstance::draw and ModelInstance::draw_box, and both of those return early for a failed
    // asset, so without this the mapper can now select a placeholder and get no feedback that
    // anything happened.
    bool selected = false;
  };

  class PlaceholderCube
  {
  public:
    PlaceholderCube() = default;

    PlaceholderCube(PlaceholderCube const&) = delete;
    PlaceholderCube(PlaceholderCube&&) = delete;
    PlaceholderCube& operator= (PlaceholderCube const&) = delete;
    PlaceholderCube& operator= (PlaceholderCube&&) = delete;

    // One instance per render context, the same shape WireBox::getInstance uses, because GL
    // objects belong to the context that created them and Noggit runs more than one (the asset
    // browser and the preview renderer each have their own).
    static PlaceholderCube& getInstance(Noggit::NoggitRenderContext context);

    // One program bind and one VAO bind for the whole batch; the per-cube cost is four uniform
    // writes and a 36-index drawElements. Not instanced on purpose: the number of placeholders
    // is the number of BROKEN placements, which on a healthy map is zero and on a broken one is
    // the thing the mapper is about to go and fix. Optimising that count is optimising for
    // failure.
    void draw(glm::mat4x4 const& model_view_projection
             , std::vector<PlaceholderCubeInstance> const& instances
             );

    void unload();

  private:
    void setupBuffers();

    bool _buffers_are_setup = false;

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vao;
    OpenGL::Scoped::deferred_upload_buffers<2> _buffers;
    GLuint const& _vertices_vbo = _buffers[0];
    GLuint const& _indices_vbo = _buffers[1];
    std::unique_ptr<OpenGL::program> _program;
  };
}

#endif // NOGGIT_RENDERING_PLACEHOLDERCUBE_HPP
