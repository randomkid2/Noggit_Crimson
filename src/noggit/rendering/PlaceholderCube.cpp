// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/PlaceholderCube.hpp>

#include <opengl/context.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace
{
  // The shaders are string literals here rather than files registered in resources.qrc, for one
  // reason: this primitive has to work when the client data it stands in for is broken, and
  // keeping its two shaders inside the translation unit that uses them means there is no second
  // place for them to go missing from. Between them they are shorter than the comment explaining
  // them; a .glsl pair plus two qrc aliases would be more moving parts, not fewer.
  constexpr char const* VERTEX_SOURCE = R"(#version 330 core

in vec3 position;

uniform mat4 model_view_projection;
uniform vec3 centre;
uniform float half_extent;

out vec3 local_pos;

void main()
{
  local_pos = position * half_extent;
  gl_Position = model_view_projection * vec4(centre + local_pos, 1.0);
}
)";

  // GLSL mod() is x - y * floor(x / y), so it stays non-negative for negative x, which is what
  // makes the parity of three floor()s work across the cube's centre instead of mirroring there.
  constexpr char const* FRAGMENT_SOURCE = R"(#version 330 core

in vec3 local_pos;

uniform vec3 mark_color;
uniform vec3 ink_color;
uniform float cell_size;
uniform float flat_fill;

out vec4 out_color;

void main()
{
  float parity = mod( floor(local_pos.x / cell_size)
                    + floor(local_pos.y / cell_size)
                    + floor(local_pos.z / cell_size)
                    , 2.0);

  vec3 checker = mix(mark_color, ink_color, parity);

  out_color = vec4(mix(checker, mark_color, flat_fill), 1.0);
}
)";

  // sRGB components of the design tokens, exact /255 conversions.
  //
  // The dark cell is INK #100E0B, the same value the interface uses as its deepest surface, so a
  // placeholder does not introduce a colour the rest of the program does not already contain.
  const glm::vec3 INK_COLOR (16.0f / 255.0f, 14.0f / 255.0f, 11.0f / 255.0f);

  // Pure magenta, not a token: this is the colour the whole modding ecosystem already reads as
  // "asset missing", including the shanecube.blp fallback a missing TEXTURE already gets
  // (TextureManager.cpp:520). A mapper should not have to learn a new convention here.
  const glm::vec3 MODEL_COLOR (1.0f, 0.0f, 1.0f);

  // WARN #E2803C and INFO #6FAEDC from Noggit::Ui::Design.
  const glm::vec3 WORLD_MODEL_COLOR (226.0f / 255.0f, 128.0f / 255.0f, 60.0f / 255.0f);
  const glm::vec3 WORLD_MODEL_DOODAD_COLOR (111.0f / 255.0f, 174.0f / 255.0f, 220.0f / 255.0f);

  // ACCENT #DFA52E, the one colour in the design system that means "the thing you are acting on".
  const glm::vec3 SELECTED_COLOR (223.0f / 255.0f, 165.0f / 255.0f, 46.0f / 255.0f);

  // Contrast of each mark against the dark cell, WCAG 2.1 sRGB relative luminance,
  // (Lmax + 0.05) / (Lmin + 0.05), computed for these exact values: magenta 6.145, WARN 6.779,
  // INFO 8.043. That is not a WCAG conformance claim -- this is a 3D viewport, not body text --
  // it is the measurement that says the checker is still readable as a checker when the cube is
  // small on screen, which a pair of similar values would not be.

  // The three kinds differ by PATTERN as well as by hue, deliberately: the design system's own
  // rule is that a warm accent and a warm warning must never be told apart by colour alone, and
  // at fifty yards a small cube is a colour and nothing else. Cells per cube edge:
  //
  //   M2          8 cells over a 4 yd cube   -> 0.5 yd cells, a fine grid
  //   WMO         2 cells over a 16 yd cube  -> 8 yd cells, four quadrants per face
  //   WMO doodad 16 cells over a 2 yd cube   -> 0.125 yd cells, reads as a dither
  constexpr float MODEL_CELLS_PER_EDGE = 8.0f;
  constexpr float WORLD_MODEL_CELLS_PER_EDGE = 2.0f;
  constexpr float WORLD_MODEL_DOODAD_CELLS_PER_EDGE = 16.0f;

  glm::vec3 markColorFor(Noggit::MissingPlacementKind kind)
  {
    switch (kind)
    {
      case Noggit::MissingPlacementKind::WorldModel:
        return WORLD_MODEL_COLOR;
      case Noggit::MissingPlacementKind::WorldModelDoodad:
        return WORLD_MODEL_DOODAD_COLOR;
      case Noggit::MissingPlacementKind::Model:
      default:
        return MODEL_COLOR;
    }
  }

  float cellsPerEdgeFor(Noggit::MissingPlacementKind kind)
  {
    switch (kind)
    {
      case Noggit::MissingPlacementKind::WorldModel:
        return WORLD_MODEL_CELLS_PER_EDGE;
      case Noggit::MissingPlacementKind::WorldModelDoodad:
        return WORLD_MODEL_DOODAD_CELLS_PER_EDGE;
      case Noggit::MissingPlacementKind::Model:
      default:
        return MODEL_CELLS_PER_EDGE;
    }
  }
}

namespace Noggit::Rendering::Primitives
{
  PlaceholderCube& PlaceholderCube::getInstance(Noggit::NoggitRenderContext context)
  {
    // try_emplace rather than WireBox's `instances[context] = instance;`. That form needs the
    // value to be default-constructible AND copy-assignable, which is why WireBox carries an
    // operator= that ignores its argument and returns *this -- a copy assignment that does not
    // copy. try_emplace default-constructs in place and needs neither.
    static std::unordered_map<Noggit::NoggitRenderContext, PlaceholderCube> instances;

    return instances.try_emplace(context).first->second;
  }

  void PlaceholderCube::draw(glm::mat4x4 const& model_view_projection
                            , std::vector<PlaceholderCubeInstance> const& instances
                            )
  {
    if (instances.empty())
    {
      return;
    }

    if (!_buffers_are_setup)
    {
      setupBuffers();
    }

    OpenGL::Scoped::use_program shader {*_program.get()};

    // Face culling off for the duration. The cube is a diagnostic that a mapper will fly into
    // and through while working out which placement is broken, and a back-face-culled box goes
    // invisible from the inside -- which is the one moment they most need to see it. Six extra
    // triangles per placeholder is not a cost worth reasoning about.
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;

    shader.uniform("model_view_projection", model_view_projection);
    shader.uniform("ink_color", INK_COLOR);

    OpenGL::Scoped::vao_binder const vao_lock (_vao[0]);

    for (PlaceholderCubeInstance const& instance : instances)
    {
      float const half (MissingPlacementGeometry::halfExtentFor(instance.kind));

      shader.uniform("centre", instance.centre);
      shader.uniform("half_extent", half);
      shader.uniform("cell_size", half * 2.0f / cellsPerEdgeFor(instance.kind));
      shader.uniform("mark_color", instance.selected ? SELECTED_COLOR : markColorFor(instance.kind));
      shader.uniform("flat_fill", instance.selected ? 1.0f : 0.0f);

      gl.drawElements(GL_TRIANGLES, _indices_vbo, 36, GL_UNSIGNED_SHORT, nullptr);
    }
  }

  void PlaceholderCube::setupBuffers()
  {
    _vao.upload();
    _buffers.upload();

    _program.reset
      (new OpenGL::program
        ( { {GL_VERTEX_SHADER, VERTEX_SOURCE}
          , {GL_FRAGMENT_SHADER, FRAGMENT_SOURCE}
          }
        ));

    // A unit cube at +/-1. The half-extent is applied in the vertex shader so that one buffer
    // serves all three sizes and nothing has to be re-uploaded when a kind changes.
    std::vector<glm::vec3> const vertices
      { {-1.0f, -1.0f, -1.0f}
      , { 1.0f, -1.0f, -1.0f}
      , { 1.0f,  1.0f, -1.0f}
      , {-1.0f,  1.0f, -1.0f}
      , {-1.0f, -1.0f,  1.0f}
      , { 1.0f, -1.0f,  1.0f}
      , { 1.0f,  1.0f,  1.0f}
      , {-1.0f,  1.0f,  1.0f}
      };

    // Twelve triangles. Winding is not load-bearing because draw() turns culling off.
    std::vector<std::uint16_t> const indices
      { 0, 1, 2,  2, 3, 0     // -z
      , 4, 5, 6,  6, 7, 4     // +z
      , 0, 4, 7,  7, 3, 0     // -x
      , 1, 5, 6,  6, 2, 1     // +x
      , 0, 1, 5,  5, 4, 0     // -y
      , 3, 2, 6,  6, 7, 3     // +y
      };

    gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertices_vbo, vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t> (_indices_vbo, indices, GL_STATIC_DRAW);

    OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);

    OpenGL::Scoped::use_program shader (*_program.get());

    {
      OpenGL::Scoped::vao_binder const vao_lock (_vao[0]);

      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
      shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

      indices_binder.bind();
    }

    _buffers_are_setup = true;
  }

  void PlaceholderCube::unload()
  {
    _vao.unload();
    _buffers.unload();
    _program.reset();

    _buffers_are_setup = false;
  }
}
