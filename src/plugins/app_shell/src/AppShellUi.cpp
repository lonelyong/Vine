#include "AppShellUi.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <QTimer>

#include <vine/Colorf.hpp>
#include <vine/geometry/Array.hpp>
#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/ShaderPreset.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/Icon.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>

V_APPFW_NS_BEGIN

namespace
{

/**
 * @brief Adds a large Ribbon button executing the given command by name.
 *
 * @param group Target group.
 * @param text Button label.
 * @param icon Icon resource path.
 * @param command Registered command name.
 * @return The created button (owned by the group).
 */
gui::RibbonButton* addCommandButton(gui::RibbonGroup* group, const String& text, const String& icon, const String& command)
{
    auto* button = new gui::RibbonButton();
    button->setText(text);
    button->setIcon(gui::Icon(icon));
    button->setButtonSize(gui::RibbonItemSize::Large);
    button->setCommand(command);
    group->addButton(button);
    return button;
}

/**
 * @brief Builds a flat-shaded axis-aligned box node centred at a position.
 *
 * The box is authored as raw position / normal / index arrays (six faces,
 * twelve triangles, per-face vertex normals) filled into the geometry's
 * channels, so the vsg backend renders it directly.
 *
 * @param root    Root group receiving the node.
 * @param diffuse Flat diffuse colour of the box.
 * @param name    Node and geometry name.
 * @param centre  World-space centre of the box.
 * @param half    Half extents along X / Y / Z.
 * @return The created node (kept alive by the root group).
 */
vine::intrusive_ptr<vine::graphics::MatrixTransform>
addBox(vine::graphics::Group* root, const vine::Colorf& diffuse, const vine::String& name, const vine::math::Vec3d& centre, const vine::math::Vec3d& half)
{
    using vine::math::Vec3f;

    vine::geometry::Vec3fArray  positions;
    vine::geometry::Vec3fArray  normals;
    vine::geometry::UInt32Array indices;

    // Build each face with a consistent outward (counter-clockwise, seen from
    // the outside) winding. For a face normal n, pick in-plane unit vectors
    // u and v with u x v = n; then the four corners ordered as
    //   -u-v, u-v, u+v, -u+v
    // are guaranteed to wind CCW around the outward normal, so the winding is
    // consistent across all faces by construction.
    const int face_table[6][3] = {
        // { normal axis, u axis, v axis } with implicit positive unit axes.
        { 0, 1, 2 }, // +X: u=Y, v=Z  -> YxZ = +X
        { 1, 2, 0 }, // +Y: u=Z, v=X  -> ZxX = +Y
        { 2, 0, 1 }, // +Z: u=X, v=Y  -> XxY = +Z
        { 0, 2, 1 }, // -X: u=Z, v=Y  -> ZxY = -X
        { 1, 0, 2 }, // -Y: u=X, v=Z  -> XxZ = -Y
        { 2, 1, 0 }, // -Z: u=Y, v=X  -> YxX = -Z
    };

    auto axisVector = [](int axis) { return Vec3f(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f); };

    for (int f = 0; f < 6; ++f) {
        const int  n_axis = face_table[f][0];
        const int  u_axis = face_table[f][1];
        const int  v_axis = face_table[f][2];
        const bool neg    = f >= 3; // the last three rows are the -axis faces

        const Vec3f normal = neg ? -axisVector(n_axis) : axisVector(n_axis);
        const Vec3f u      = axisVector(u_axis);
        const Vec3f v      = axisVector(v_axis);

        const std::uint32_t base  = static_cast<std::uint32_t>(positions.size());
        const float         hs[3] = { static_cast<float>(half.x), static_cast<float>(half.y), static_cast<float>(half.z) };
        const float         cu[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        const float         cv[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
        for (int k = 0; k < 4; ++k) {
            const Vec3f corner = normal * hs[n_axis] + u * (cu[k] * hs[u_axis]) + v * (cv[k] * hs[v_axis]);
            positions.push_back(corner);
            normals.push_back(normal);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    auto geometry = vine::make_intrusive<vine::graphics::Geometry>();
    geometry->setName(name);
    geometry->setPositions(positions);
    geometry->setNormals(normals);
    geometry->setIndices(indices);

    auto material = vine::make_intrusive<vine::graphics::Material>();
    material->setDiffuse(diffuse);
    // Matte plastic: a low grey specular keeps lit surfaces looking solid.
    // The Material default specular is pure white, which makes the deferred
    // specular G-buffer attachment read all-white and paints broad white
    // highlights that read as glass / see-through on opaque boxes.
    material->setSpecular(vine::Colorf(0.12f, 0.12f, 0.12f, 1.0f));
    material->setShininess(64.0f);
    geometry->setMaterial(material);

    // Place the box with a MatrixTransform: the transform node carries the
    // position and owns the leaf geometry.
    auto node = vine::make_intrusive<vine::graphics::MatrixTransform>();
    node->setName(name);
    node->setMatrix(vine::math::translate(centre));
    node->addChild(geometry);

    root->addChild(node);
    return node;
}

/**
 * @brief Builds the five-pointed-star point-sprite program.
 *
 * One vertex input (position); the per-point colour is derived inside the
 * vertex stage from the point's own position (HSV->RGB), and the fragment
 * stage keeps the five outward spikes of a point sprite (gl_PointCoord,
 * XGraph PointCloud.fs.glsl style). A POINT_LIST pipeline must write
 * gl_PointSize or it trips VUID-VkGraphicsPipelineCreateInfo-topology-08773.
 *
 * @return Configured star program.
 */
vine::intrusive_ptr<vine::graphics::ShaderProgram> makeStarPointProgram()
{
    using vine::intrusive_ptr;
    using vine::graphics::ShaderProgram;
    using vine::graphics::ShaderStage;
    using vine::graphics::ShaderStageType;

    auto program = make_intrusive<ShaderProgram>();
    program->setName(u8"star_sprite");
    ShaderStage vs;
    vs.type   = ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"layout(location = 0) out vec4 vColor;\n"
                // Small HSV->RGB helper so each point can carry its own
                // rainbow colour without a colour attribute.
                u8"vec3 hsv2rgb(vec3 c)\n"
                u8"{\n"
                u8"    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);\n"
                u8"    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
                u8"}\n"
                u8"void main()\n"
                u8"{\n"
                u8"    gl_Position = pc.projection * pc.modelView * vec4(vsg_Vertex, 1.0);\n"
                // A POINT_LIST pipeline must write gl_PointSize or it trips
                // VUID-VkGraphicsPipelineCreateInfo-topology-08773. A large
                // size leaves room for the procedural star shape in the FS.
                u8"    gl_PointSize = 40.0;\n"
                // Per-point colour: hue wraps around the point's angle,
                // then shifts with height so stacked points differ.
                u8"    float hue = 0.5 + 0.5 * atan(vsg_Vertex.z, vsg_Vertex.x) / 3.14159265;\n"
                u8"    hue = fract(hue + vsg_Vertex.y * 0.35);\n"
                u8"    vColor = vec4(hsv2rgb(vec3(hue, 0.85, 0.95)), 1.0);\n"
                u8"}\n";
    program->addStage(vs);
    ShaderStage fs;
    fs.type   = ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec4 vColor;\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main()\n"
                u8"{\n"
                // Centred point coordinates; keep the five outward spikes
                // (radius follows sin(5 * angle), see XGraph
                // PointCloud.fs.glsl), discard the rest of the square.
                u8"    vec2 p = gl_PointCoord * 2.0 - vec2(1.0);\n"
                u8"    if (dot(p, p) > sin(atan(p.y, p.x) * 5.0)) discard;\n"
                u8"    outColor = vColor;\n"
                u8"}\n";
    program->addStage(fs);
    return program;
}

/**
 * @brief Builds the FORWARD feature-showcase scene (single-pass showcase).
 *
 * Shown when the demo pipeline preset is Forward (VINE_PIPELINE=forward). The
 * DEFAULT demo uses Deferred with the opaque addDeferredDemoCubes scene,
 * because the forward-only slices below would be overridden by the G-buffer
 * geometry program. Exercises, in one window view, every forward graphics
 * slice:
 *  - ground + lit box: MatrixTransform + Material (default Phong path)
 *  - StateNode{ PolygonMode::Line } box         -> per-subtree polygon state
 *  - StateNode{ CullMode::Back } box            -> per-subtree culling state
 *  - StateNode{ Topology::Points } + cyan user program point cloud
 *  - StateNode{ Topology::Points } + star-sprite user program: rainbow
 *    five-pointed stars (colour derived per point in the vertex stage)
 *  - Geometry::setProgram magenta box (runtime-compiled GLSL, official vsg
 *    "pc" projection/modelView contract)
 *  - nested MatrixTransform (world-matrix chain)
 *  - StateNode blend + translucent material box
 *
 * @param scene Scene to receive the content.
 */
void addDemoCubes(vine::graphics::Scene* scene)
{
    using vine::intrusive_ptr;
    using vine::graphics::BlendState;
    using vine::graphics::Geometry;
    using vine::graphics::MatrixTransform;
    using vine::graphics::ShaderProgram;
    using vine::graphics::ShaderStage;
    using vine::graphics::ShaderStageType;
    using vine::graphics::StateNode;
    using vine::math::Vec3d;
    using vine::math::Vec3f;

    // Single root: one identity Group owns every demo subtree; the scene
    // renders exactly this root. The world is Z-up (robotics convention), so
    // the ground is a slab in the XY plane (top at z = 0) and boxes stand on
    // +Z.
    auto root = make_intrusive<vine::graphics::Group>();
    scene->setRoot(root);

    // Ground (wide, slightly below origin) + one plain lit box: the default
    // material / Phong path.
    addBox(root.get(), vine::Colorf(0.45f, 0.47f, 0.52f, 1.0f), u8"ground", Vec3d(0.0, 0.0, -0.05), Vec3d(3.0, 3.0, 0.05));
    addBox(root.get(), vine::Colorf(0.30f, 0.62f, 0.36f, 1.0f), u8"lit_box", Vec3d(0.0, 0.0, 0.4), Vec3d(0.5, 0.5, 0.4));

    // --- StateNode{ PolygonMode::Line }: wireframe box ----------------------
    // Each box is attached under ITS state node only (single parent).
    {
        auto state = make_intrusive<StateNode>();
        state->setPolygonMode(vine::graphics::PolygonMode::Line);
        addBox(state.get(), vine::Colorf(1.0f, 0.68f, 0.12f, 1.0f), u8"wire_box", Vec3d(-2.3, 0.6, 0.35), Vec3d(0.35, 0.35, 0.35));
        root->addChild(state);
    }

    // --- StateNode{ CullMode::Back }: single-sided box ----------------------
    {
        auto state = make_intrusive<StateNode>();
        state->setCullMode(vine::graphics::CullMode::Back);
        addBox(state.get(), vine::Colorf(0.20f, 0.75f, 0.85f, 1.0f), u8"culled_box", Vec3d(-1.6, -0.8, 0.4), Vec3d(0.4, 0.4, 0.4));
        root->addChild(state);
    }

    // --- Custom magenta program on a box (Geometry::setProgram) -------------
    {
        auto program = make_intrusive<ShaderProgram>();
        program->setName(u8"demo_magenta");
        ShaderStage vs;
        vs.type   = ShaderStageType::Vertex;
        vs.source = u8"#version 450\n"
                    u8"layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
                    u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                    u8"void main(){ gl_Position = pc.projection * pc.modelView * vec4(vsg_Vertex, 1.0); }\n";
        program->addStage(vs);
        ShaderStage fs;
        fs.type   = ShaderStageType::Fragment;
        fs.source = u8"#version 450\n"
                    u8"layout(location = 0) out vec4 outColor;\n"
                    u8"void main(){ outColor = vec4(0.9, 0.1, 0.85, 1.0); }\n";
        program->addStage(fs);

        auto  box      = addBox(root.get(), vine::Colorf(1.0f, 1.0f, 1.0f, 1.0f), u8"custom_program", Vec3d(0.0, 0.0, 0.85), Vec3d(0.42, 0.42, 0.45));
        auto* geometry = dynamic_cast<Geometry*>(box->children().front().get());
        if (geometry != nullptr) {
            geometry->setProgram(program);
        }
    }

    // --- Point cloud: StateNode{ Topology::Points } + cyan program ----------
    {
        auto program = make_intrusive<ShaderProgram>();
        program->setName(u8"demo_cyan_points");
        ShaderStage vs;
        vs.type   = ShaderStageType::Vertex;
        vs.source = u8"#version 450\n"
                    u8"layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;\n"
                    u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                    // A POINT_LIST pipeline must write gl_PointSize or it trips
                    // VUID-VkGraphicsPipelineCreateInfo-topology-08773.
                    u8"void main(){ gl_Position = pc.projection * pc.modelView * vec4(vsg_Vertex, 1.0); gl_PointSize = 3.0; }\n";
        program->addStage(vs);
        ShaderStage fs;
        fs.type   = ShaderStageType::Fragment;
        fs.source = u8"#version 450\n"
                    u8"layout(location = 0) out vec4 outColor;\n"
                    u8"void main(){ outColor = vec4(0.1, 0.9, 0.95, 1.0); }\n";
        program->addStage(fs);

        auto cloud = make_intrusive<Geometry>();
        cloud->setName(u8"point_cloud");
        vine::geometry::Vec3fArray points;
        // Four stacked rings in the Z-up world: horizontal rings at rising
        // heights, centred on the footprint (-2.3, -1.2).
        for (int ring = 0; ring < 4; ++ring) {
            const float r   = 0.35f + 0.12f * static_cast<float>(ring);
            const float z_h = 0.75f + (-0.25f + 0.17f * static_cast<float>(ring));
            for (int k = 0; k < 36; ++k) {
                const float a = 6.2831853f * static_cast<float>(k) / 36.0f;
                points.emplace_back(-2.3f + r * std::cos(a), -1.2f + r * std::sin(a), z_h);
            }
        }
        cloud->setPositions(points);
        cloud->setProgram(program);

        auto state = make_intrusive<StateNode>();
        state->setTopology(vine::graphics::Topology::Points);
        state->addChild(cloud);
        root->addChild(state);
    }

    // --- Point cloud #2: StateNode{ Topology::Points } + star-sprite program --
    // A second point-list demo: each point is drawn as a large five-pointed
    // star sprite (procedural fragment shape via gl_PointCoord, XGraph
    // PointCloud.fs.glsl style) whose colour is derived per point inside the
    // vertex stage from the point's own position (no colour attribute needed).
    {
        auto program = makeStarPointProgram();

        auto stars = make_intrusive<Geometry>();
        stars->setName(u8"star_cloud");
        vine::geometry::Vec3fArray points;
        // Three stacked rings (a "some point geometry" showcase) with radius
        // and height growing per ring, centred on footprint (2.05, 0.1).
        for (int ring = 0; ring < 3; ++ring) {
            const float r   = 0.30f + 0.16f * static_cast<float>(ring);
            const float z_h = 0.95f + (-0.55f + 0.55f * static_cast<float>(ring));
            for (int k = 0; k < 12; ++k) {
                const float a = 6.2831853f * static_cast<float>(k) / 12.0f + static_cast<float>(ring);
                points.emplace_back(2.05f + r * std::cos(a), 0.1f + r * std::sin(a), z_h);
            }
        }
        stars->setPositions(points);
        stars->setProgram(program);

        auto state = make_intrusive<StateNode>();
        state->setTopology(vine::graphics::Topology::Points);
        state->addChild(stars);
        root->addChild(state);
    }

    // --- Nested MatrixTransform: world-matrix chain --------------------------
    {
        auto outer = make_intrusive<MatrixTransform>();
        outer->setName(u8"nested_outer");
        outer->setMatrix(vine::math::translate(Vec3d(1.5, 0.9, 0.0)));
        // Inner box built directly under the outer transform (single parent);
        // its own matrix raises it along +Z.
        addBox(outer.get(), vine::Colorf(0.95f, 0.5f, 0.1f, 1.0f), u8"nested_inner", Vec3d(0.0, 0.0, 0.32), Vec3d(0.28, 0.28, 0.28));
        root->addChild(outer);
    }

    // --- StateNode blend + translucent box -----------------------------------
    {
        auto       state = make_intrusive<StateNode>();
        BlendState blend;
        blend.enabled = true;
        blend.src     = vine::graphics::BlendFactor::SrcAlpha;
        blend.dst     = vine::graphics::BlendFactor::OneMinusSrcAlpha;
        state->setBlend(blend);

        auto  box      = addBox(state.get(), vine::Colorf(1.0f, 0.25f, 0.25f, 1.0f), u8"translucent", Vec3d(0.9, -1.1, 0.7), Vec3d(0.55, 0.55, 0.55));
        auto* geometry = dynamic_cast<Geometry*>(box->children().front().get());
        if (geometry != nullptr) {
            geometry->setOpacity(0.5f);
        }
        root->addChild(state);
    }
}

} // namespace

/**
 * @brief Lights the demo scene (scene-level v4a rig) and frames it.
 *
 * Runs unconditionally so the default demo (ground + box stack) is lit by an
 * ambient fill plus a camera-side key sun and a soft back fill, viewed from
 * an elevated 3/4 angle. The off-screen/PiP validation renders the same
 * engine scene and camera, so the PiP matches the main view.
 *
 * Shadow mapping is deferred until the custom-shader / multi-pass slice is
 * mature: the demo sun never sets castShadow() yet, so no half-built shadow
 * path runs.
 *
 * @param render_control Render view whose engine receives the lights.
 */
void addDemoLighting(gui::RenderControl* render_control)
{
    auto scene = render_control->view()->scene();
    if (scene == nullptr) {
        return;
    }
    auto ambient = vine::graphics::Light::createAmbient();
    ambient->setName(u8"scene_ambient");
    ambient->setIntensity(0.35f);
    scene->addLight(ambient);

    // Key sun from the camera side (front-right-top of the default elevated
    // 3/4 view): it lights the box faces the camera actually sees. The world
    // is Z-up (robotics convention), so "above" is +Z and the sun travels
    // downward (negative z). Lights travel toward the given direction and
    // both the deferred and forward shaders shade with the opposite ray, so
    // the negative x / y / z here puts the source on the camera's side.
    auto sun = vine::graphics::Light::createDirectional(vine::math::Vec3d(-0.35, -0.35, -0.75));
    sun->setName(u8"scene_sun");
    sun->setIntensity(1.0f);
    scene->addLight(sun);

    // Soft cool fill from the opposite (back) side so orbiting to the far
    // side does not drop those faces to ambient-only either.
    auto fill = vine::graphics::Light::createDirectional(vine::math::Vec3d(0.5, 0.5, -0.4));
    fill->setName(u8"scene_fill");
    fill->setIntensity(0.5f);
    scene->addLight(fill);

    // Dev switch: VINE_VSG_EXTRA_SUNS adds two more directional lights so the
    // scene exercises the deferred light pass' full three-light capacity.
    if (std::getenv("VINE_VSG_EXTRA_SUNS") != nullptr) {
        auto sun2 = vine::graphics::Light::createDirectional(vine::math::Vec3d(-0.9, -0.3, -0.5));
        sun2->setName(u8"scene_sun2");
        sun2->setColor(vine::Colorf(0.6f, 0.8f, 1.0f, 1.0f));
        sun2->setIntensity(0.7f);
        scene->addLight(sun2);

        auto sun3 = vine::graphics::Light::createDirectional(vine::math::Vec3d(-0.2, 0.95, -0.3));
        sun3->setName(u8"scene_sun3");
        sun3->setColor(vine::Colorf(1.0f, 0.55f, 0.35f, 1.0f));
        sun3->setIntensity(0.5f);
        scene->addLight(sun3);
    }

    // Raise the camera to an elevated 3/4 view of the stack (Z-up: the up
    // vector is +Z). Runs before RenderControl::init() creates the orbit
    // manipulator, so the manipulator home syncs to this vantage.
    auto* camera = render_control->view()->camera();
    if (camera != nullptr) {
        camera->setViewMatrixAsLookAt(vine::math::Vec3d(6.5, 6.5, 5.0),  // eye: front-right, elevated
                                      vine::math::Vec3d(0.0, 0.0, 0.6),  // target: mid-stack
                                      vine::math::Vec3d(0.0, 0.0, 1.0)); // up: +Z
    }
}

/**
 * @brief TEMP GPU-validation hook for the v3 render-to-texture chain.
 *
 * When the environment variable VINE_VSG_OFFSCREEN is set, registers:
 *   1. an order < 0 pass that renders the engine scene into an off-screen
 *      RGBA8 + depth RenderTarget each frame (publishing it as "SceneColor"),
 *      exercising the vsg render-to-texture path, and
 *   2. an order > 0 ScreenPass that samples "SceneColor" and composites it
 *      into a picture-in-picture sub-viewport of the window, so the off-screen
 *      result is actually visible (render-to-texture -> sample -> present).
 *
 * Run it to check for Vulkan validation errors and the log lines
 * "[VsgRenderer] EXPERIMENTAL off-screen target ... attached" and
 * "[VsgRenderer] EXPERIMENTAL screen PiP ... attached".
 *
 * @param render_control Render view whose engine receives the passes.
 */
void addOffscreenValidationPass(gui::RenderControl* render_control)
{
    const char* enabled = std::getenv("VINE_VSG_OFFSCREEN");
    if (enabled == nullptr || enabled[0] == '\0') {
        return;
    }
    auto* engine = render_control->engine();
    if (engine == nullptr) {
        return;
    }

    // Recipe #1 (RenderPipelineBuilder): compose the "off-screen RT -> PiP
    // ScreenPass" wiring instead of hand-building the two passes.
    const double dpr    = render_control->devicePixelRatio();
    const int    margin = static_cast<int>(10.0 * dpr);
    const int    pip_w  = static_cast<int>(320.0 * dpr);
    const int    pip_h  = static_cast<int>(180.0 * dpr);

    // Bottom-right anchoring against the (current or default) surface size.
    const auto anchorRect = [render_control, margin](int w, int h, int& out_x, int& out_y) {
        auto* engine_ptr = render_control->engine();
        int   sw         = (engine_ptr != nullptr) ? engine_ptr->frameContext().surface_width : 0;
        int   sh         = (engine_ptr != nullptr) ? engine_ptr->frameContext().surface_height : 0;
        if (sw <= 0 || sh <= 0) {
            // Surface not realized yet: anchor against the default viewport so
            // the PiP never briefly covers the whole surface.
            sw = 1280;
            sh = 720;
        }
        if (w > sw / 2) {
            w = sw / 2;
            h = static_cast<int>(w * 9 / 16);
        }
        if (h > sh / 2) {
            h = sh / 2;
            w = static_cast<int>(h * 16 / 9);
        }
        out_x = sw - w - margin;
        out_y = sh - h - margin;
    };
    int px = 0, py = 0;
    anchorRect(pip_w, pip_h, px, py);

    vine::graphics::RenderPipelineBuilder builder(engine);
    builder.setCamera(render_control->view()->camera());
    builder.setContent(render_control->view()->scene());
    auto* screen = builder.addOffscreenToScreen(u8"SceneColor",
                                                640,
                                                360,
                                                vine::graphics::RenderTarget::ColorFormat::RGBA8,
                                                vine::graphics::RenderTarget::DepthFormat::D24,
                                                px,
                                                py,
                                                pip_w,
                                                pip_h);
    if (screen == nullptr) {
        return;
    }

    // Re-anchor once the backend surface is realized and sized.
    QTimer::singleShot(600, [render_control, screen, pip_w, pip_h, margin] {
        auto* engine_ptr = render_control->engine();
        int   sw         = (engine_ptr != nullptr) ? engine_ptr->frameContext().surface_width : 0;
        int   sh         = (engine_ptr != nullptr) ? engine_ptr->frameContext().surface_height : 0;
        if (sw <= 0 || sh <= 0) {
            sw = 1280;
            sh = 720;
        }
        int w = pip_w;
        int h = pip_h;
        if (w > sw / 2) {
            w = sw / 2;
            h = static_cast<int>(w * 9 / 16);
        }
        if (h > sh / 2) {
            h = sh / 2;
            w = static_cast<int>(h * 16 / 9);
        }
        screen->setViewport(sw - w - margin, sh - h - margin, w, h);
    });
}

void buildAppShellRibbon(gui::MainWindow* wnd)
{
    auto* bar = wnd->ribbonBar();

    auto* plugin_tab = new gui::RibbonTab();
    plugin_tab->setTitle(u8"插件");
    bar->addTab(plugin_tab);
    auto* plugin_group = new gui::RibbonGroup();
    plugin_group->setTitle(u8"插件管理");
    plugin_tab->addGroup(plugin_group);

    auto* help_tab = new gui::RibbonTab();
    help_tab->setTitle(u8"帮助");
    bar->addTab(help_tab);
    auto* help_group = new gui::RibbonGroup();
    help_group->setTitle(u8"帮助");
    help_tab->addGroup(help_group);

    addCommandButton(plugin_group, u8"插件信息", u8":/icons/show_plugins.svg", u8"show_plugins");
    addCommandButton(plugin_group, u8"渲染后端", u8":/icons/show_plugins.svg", u8"show_render_backends");
    addCommandButton(plugin_group, u8"配置管理", u8":/icons/show_config.svg", u8"show_config");
    addCommandButton(help_group, u8"命令管理器", u8":/icons/show_commands.svg", u8"show_commands");
    addCommandButton(help_group, u8"帮助", u8":/icons/show_help.svg", u8"show_help");
    addCommandButton(help_group, u8"关于", u8":/icons/about.svg", u8"about");
}

/**
 * @brief Adds a second content slot on the main camera (env VINE_VSG_SLOT_DEMO).
 *
 * Demonstrates same-camera stacking: a pass that shares the MAIN camera but
 * runs at a higher order (20, after the order-0 window pass) draws its own
 * retained content into the same window buffer (no clear -> on-top, depth-off
 * style), so the extra boxes stay screen-aligned with the main view while the
 * camera orbits. The backend keys a camera's content slots by the pass order,
 * so the higher-order pass is its own slot stacked on top of the main one.
 *
 * @param render_control Render view whose engine receives the slot pass.
 *
 * Backend-mechanism validator (same-camera content-slot stacking), retained
 * for backend regression; it intentionally drives the engine directly, not a
 * builder preset.
 */
void addSlotOverlayDemo(gui::RenderControl* render_control)
{
    if (std::getenv("VINE_VSG_SLOT_DEMO") == nullptr) {
        return;
    }
    auto* engine = render_control->engine();
    if (engine == nullptr || render_control->view()->camera() == nullptr) {
        return;
    }

    // Register after RenderControl::init() has provisioned the default window
    // pass (order 0), so the extra slot stacks on top of the real main view.
    QTimer::singleShot(300, [render_control] {
        auto* engine = render_control->engine();
        if (engine == nullptr || render_control->view()->camera() == nullptr) {
            return;
        }
        auto overlay      = vine::make_intrusive<vine::graphics::Scene>();
        auto overlay_root = vine::make_intrusive<vine::graphics::Group>();
        overlay->setRoot(overlay_root);
        const auto add_overlay_box = [overlay_root](const vine::Colorf& color, const vine::math::Vec3d& centre, double half) {
            auto box = addBox(overlay_root.get(), color, u8"slot_overlay", centre, vine::math::Vec3d(half, half, half));
            // Top (on-top) layers are lit by a pure ambient light: a WHITE
            // ambient material makes ambientColor == diffuse == the box color.
            if (auto* geometry = dynamic_cast<vine::graphics::Geometry*>(box->children().front().get())) {
                if (auto* material = geometry->material(); material != nullptr) {
                    material->setAmbient(vine::Colorf(1.0f, 1.0f, 1.0f, 1.0f));
                    material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
                }
            }
        };
        // Sparse, vivid boxes offset from the main cubes so the overlay is
        // clearly visible on top while tracking the main camera.
        add_overlay_box(vine::Colorf(1.0f, 0.30f, 0.10f, 1.0f), vine::math::Vec3d(-2.9, -2.4, 0.8), 0.35);
        add_overlay_box(vine::Colorf(1.0f, 0.95f, 0.10f, 1.0f), vine::math::Vec3d(2.6, 1.9, 1.2), 0.28);

        auto pass = vine::make_intrusive<vine::graphics::RenderPass>();
        pass->setName(u8"slot_overlay");
        pass->setCamera(render_control->view()->camera());
        pass->setClearEnabled(false); // overlay: no clear + no depth -> on-top
        pass->setOcclusionEnabled(false);
        engine->addPass(pass, overlay, 20); // its own (master camera, order 20) slot
    });
}

/**
 * @brief Bakes ONE off-screen target with two content slots (env
 * VINE_VSG_OFFSCREEN_MULTISLOT).
 *
 * Demonstrates C6.4: the same RenderTarget is rendered by two passes that
 * share the master camera but run at distinct orders (-2 and -1), so each is
 * its own content slot (camera, order) under the target's render graph, drawn
 * in ascending order into the same buffer. The first pass clears and draws
 * the main scene (depth-on); the second draws a few extra boxes from a
 * DIFFERENT scene as an on-top (depth-off) slot, so they composite over the
 * first bake. A ScreenPass then shows the baked texture in a picture-in-
 * picture sub-viewport so the result is visible.
 *
 * @param render_control Render view whose engine receives the passes.
 *
 * Backend-mechanism validator (one target, multiple content slots), retained
 * for backend regression; it intentionally drives the engine directly, not a
 * builder preset.
 */
void addOffscreenMultiSlotDemo(gui::RenderControl* render_control)
{
    if (std::getenv("VINE_VSG_OFFSCREEN_MULTISLOT") == nullptr) {
        return;
    }
    auto* engine = render_control->engine();
    if (engine == nullptr || render_control->view()->camera() == nullptr) {
        return;
    }

    // Shared off-screen target: two content slots bake into it.
    auto target = vine::make_intrusive<vine::graphics::RenderTarget>();
    target->setSize(640, 360);
    target->attachColor(vine::graphics::RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(vine::graphics::RenderTarget::DepthFormat::D24);

    // Slot 0: the main engine scene, cleared (depth-on).
    auto pass_main = vine::make_intrusive<vine::graphics::RenderPass>();
    pass_main->setName(u8"multislot_main");
    pass_main->setCamera(render_control->view()->camera());
    pass_main->setRenderTarget(target);
    pass_main->setOutputName(u8"MultiColor");
    engine->addPass(pass_main, render_control->view()->scene(), -2); // content = view scene

    // Slot 1: an extra scene drawn on top (no clear -> on-top, depth-off).
    auto overlay      = vine::make_intrusive<vine::graphics::Scene>();
    auto overlay_root = vine::make_intrusive<vine::graphics::Group>();
    overlay->setRoot(overlay_root);
    const auto add_overlay_box = [overlay_root](const vine::Colorf& color, const vine::math::Vec3d& centre, const vine::math::Vec3d& half) {
        auto box = addBox(overlay_root.get(), color, u8"mslot_overlay", centre, half);
        // On-top (depth-off) slots are lit by a pure ambient light: a WHITE
        // ambient material makes ambientColor == diffuse == the box colour.
        if (auto* geometry = dynamic_cast<vine::graphics::Geometry*>(box->children().front().get())) {
            if (auto* material = geometry->material(); material != nullptr) {
                material->setAmbient(vine::Colorf(1.0f, 1.0f, 1.0f, 1.0f));
                material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
            }
        }
    };
    add_overlay_box(vine::Colorf(1.0f, 0.30f, 0.10f, 1.0f), vine::math::Vec3d(-2.4, -1.8, 0.9), vine::math::Vec3d(0.55, 0.55, 0.55));
    add_overlay_box(vine::Colorf(0.30f, 0.90f, 0.30f, 1.0f), vine::math::Vec3d(2.2, 1.6, 1.3), vine::math::Vec3d(0.45, 0.45, 0.45));

    auto pass_top = vine::make_intrusive<vine::graphics::RenderPass>();
    pass_top->setName(u8"multislot_top");
    pass_top->setCamera(render_control->view()->camera());
    pass_top->setRenderTarget(target);
    pass_top->setOutputName(u8"MultiColor"); // publishes the same baked target
    pass_top->setClearEnabled(false);        // no clear + no depth -> on-top slot
    pass_top->setOcclusionEnabled(false);
    engine->addPass(pass_top, overlay, -1); // slot = (master, -1)

    // PiP screen pass sampling the baked texture into the window.
    const double dpr   = render_control->devicePixelRatio();
    const int    pip_w = static_cast<int>(300.0 * dpr);
    const int    pip_h = static_cast<int>(169.0 * dpr);
    int          sx    = 0;
    int          sy    = 0;
    {
        int sw = engine->frameContext().surface_width;
        int sh = engine->frameContext().surface_height;
        if (sw <= 0 || sh <= 0) {
            sw = 1280;
            sh = 720;
        }
        const int margin = 8;
        sx               = sw - pip_w - margin;
        sy               = sh - pip_h - margin;
    }
    auto screen = vine::make_intrusive<vine::graphics::ScreenPass>();
    screen->setName(u8"multislot_pip");
    screen->addInputName(u8"MultiColor");
    // The two bake passes above accumulate into one target, so neither of them owns the hand-off
    // (a promise is a claim about WHOSE content a consumer gets): the consumer reads the whole baked
    // target, and the engine answers it from "a pass draws into it", not from a promise.
    screen->addInputTarget(target);
    screen->setViewport(sx, sy, pip_w, pip_h);
    engine->addPass(screen, 100);
}

// The deferred / G-buffer previews reuse the RenderPipelineBuilder Deferred
// preset's built-in shaders and canonical G-buffer target (single source).
vine::intrusive_ptr<vine::graphics::RenderTarget> makeGbufferTarget();

/**
 * @brief Previews the Deferred G-buffer's colour attachments (env
 * VINE_VSG_GBUFFER).
 *
 * Bakes the engine scene into the canonical G-buffer (RenderPipelineBuilder's
 * default geometry program + target) and shows each colour attachment as a
 * small picture-in-picture: 0 = albedo, 1 = view normal (+ shininess in
 * alpha), 2 = specular, 3 = view position. Lets the MRT writes be validated
 * independently of the lighting pass, while the main window keeps its forward
 * view.
 *
 * @param render_control Render view whose engine receives the passes.
 */
void addGbufferDemo(gui::RenderControl* render_control)
{
    if (std::getenv("VINE_VSG_GBUFFER") == nullptr) {
        return;
    }
    auto* engine = render_control->engine();
    if (engine == nullptr || render_control->view()->camera() == nullptr) {
        return;
    }

    // Canonical G-buffer: engine scene through the default multi-output
    // program into the shared MRT target (single source with the Deferred
    // preset), published as "GBuffer".
    auto target = makeGbufferTarget();
    auto gbuf   = vine::make_intrusive<vine::graphics::RenderPass>();
    gbuf->setName(u8"gbuffer_pass");
    gbuf->setCamera(render_control->view()->camera());
    gbuf->setRenderTarget(target);
    gbuf->setProgramOverride(vine::graphics::RenderPipelineBuilder::defaultGbufferGeometryProgram());
    gbuf->setOutputName(u8"GBuffer");
    gbuf->setOutputTarget(target);   // the G-buffer's only writer owns the hand-off (design §14)
    engine->addPass(gbuf, render_control->view()->scene(), -3);

    // Preview each colour attachment of the same published target: each preview declares the SAME wire
    // at the grain it really is — one image, not "a name plus an attachment index". The identity is
    // the address (target + attachment), so the producer's whole-target promise above answers it.
    static const vine::String preview_labels[] = { u8"GBuffer.albedo", u8"GBuffer.normal",
                                                   u8"GBuffer.specular", u8"GBuffer.position" };
    const double dpr   = render_control->devicePixelRatio();
    const int    pip_w = static_cast<int>(108.0 * dpr);
    const int    pip_h = static_cast<int>(61.0 * dpr);
    for (int attachment = 0; attachment < 4; ++attachment) {
        auto screen = vine::make_intrusive<vine::graphics::ScreenPass>();
        screen->setName(u8"gbuffer_preview");
        screen->addInputName(u8"GBuffer");
        // No setSourceAttachment: the declared image already says which attachment this preview
        // samples (the name above is the sugar, the image is the wire).
        auto image = vine::make_intrusive<vine::graphics::ImageRef>(preview_labels[attachment]);
        image->bind(target, attachment);
        screen->addInput(image);
        const int x = 8 + attachment * (pip_w + 8);
        screen->setViewport(x, 8, pip_w, pip_h);
        engine->addPass(screen, 120 + attachment);
    }
}

/**
 * @brief Renders the engine scene into a G-buffer and lights it in a
 * fullscreen deferred pass (env VINE_VSG_DEFERRED).
 *
 * S2a vertical slice: the G-buffer geometry pass (three colour attachments:
 * albedo / view normal / view position) runs off-screen; a fullscreen
 * deferred-lighting ScreenPass (a fragment program sampling every G-buffer
 * attachment) re-lights it in one draw and shows the result in a preview
 * sub-viewport. The main window keeps its forward-lit view, so the two can be
 * compared side by side (A/B). The lights are the content scene's own
 * (ambient + directional), pushed to the lighting shader in view space.
 *
 * @param render_control Render view whose engine receives the passes.
 */
void addDeferredDemo(gui::RenderControl* render_control)
{
    if (std::getenv("VINE_VSG_DEFERRED") == nullptr) {
        return;
    }
    auto* engine = render_control->engine();
    if (engine == nullptr || render_control->view()->camera() == nullptr) {
        return;
    }

    // Shared G-buffer: engine scene through the multi-output program.
    auto target = makeGbufferTarget();
    auto gbuf   = vine::make_intrusive<vine::graphics::RenderPass>();
    gbuf->setName(u8"deferred_gbuffer");
    gbuf->setCamera(render_control->view()->camera());
    gbuf->setRenderTarget(target);
    gbuf->setProgramOverride(vine::graphics::RenderPipelineBuilder::defaultGbufferGeometryProgram());
    gbuf->setOutputName(u8"GBuffer");
    gbuf->setOutputTarget(target);   // the G-buffer's only writer owns the hand-off (design §14)
    engine->addPass(gbuf, render_control->view()->scene(), -3);

    // Deferred-lighting preview: a fullscreen program sampling the G-buffer.
    const double dpr   = render_control->devicePixelRatio();
    const int    pw    = static_cast<int>(340.0 * dpr);
    const int    ph    = static_cast<int>(191.0 * dpr);
    auto         light = vine::make_intrusive<vine::graphics::ScreenPass>();
    light->setName(u8"deferred_light");
    light->addInputName(u8"GBuffer");
    light->addInputTarget(target);   // a fullscreen program reads the whole source target
    light->setCamera(render_control->view()->camera());
    light->setProgram(vine::graphics::RenderPipelineBuilder::defaultDeferredLightProgram());
    light->setViewport(8, 8, pw, ph);
    // The content scene must be bound to THIS pass too: ScreenPass forwards a scene's lights to the
    // fullscreen program, and a program pass with no scene is handed the backend's default ambient
    // only (no directional term) - a flat image contradicting this function's own documentation.
    engine->addPass(light, render_control->view()->scene(), 130);
}

/**
 * @brief Builds a shared G-buffer MRT target (albedo / normal / spec / pos).
 *
 * Four colour attachments (albedo RGBA8, view-space normal RGBA16F with alpha
 * = shininess / 256, specular colour RGBA8, view-space position RGBA16F) plus
 * a depth attachment. Position is stored so the deferred lighting pass is
 * exact under the backend's depth convention.
 *
 * @return The target with four colour attachments + depth.
 */
vine::intrusive_ptr<vine::graphics::RenderTarget> makeGbufferTarget()
{
    // The A/B preview shares the Deferred preset's canonical G-buffer layout
    // (single source in RenderPipelineBuilder).
    return vine::graphics::RenderPipelineBuilder::defaultGbufferTarget(640, 360);
}

/**
 * @brief Builds the forward-only overlay scene of the DEFAULT (Deferred) demo.
 *
 * The Deferred G-buffer pass only bakes opaque content through one geometry
 * program, so forward-only elements — the rainbow five-pointed star point
 * cloud and the alpha-blended translucent box — cannot live in the opaque
 * scene. This scene is handed to the pipeline builder as transparent content
 * (RenderPipelineBuilder::setTransparentContent) and is composited depth-on
 * over the deferred-lit result: the translucent box is positioned overlapping
 * the opaque stack, so it is genuinely occluded / blended against the opaque
 * geometry instead of floating on top. Gated to the Deferred default demo;
 * the Forward preset already renders the same elements inside its own scene
 * (addDemoCubes).
 *
 * @return The overlay scene (owned by the caller / the engine).
 */
vine::intrusive_ptr<vine::graphics::Scene> makeForwardOverlayScene()
{
    using vine::intrusive_ptr;
    using vine::graphics::BlendState;
    using vine::graphics::Geometry;
    using vine::graphics::Group;
    using vine::graphics::MatrixTransform;
    using vine::graphics::Scene;
    using vine::graphics::StateNode;
    using vine::graphics::Topology;
    using vine::math::Vec3d;

    auto overlay_root = make_intrusive<Group>();

    // Translucent box: forward alpha blend, placed overlapping the opaque
    // stack so the depth-composited result shows real occlusion. Z-up world:
    // the box stands above the ground plane footprint (1.0, -0.9).
    {
        auto       state = make_intrusive<StateNode>();
        BlendState blend;
        blend.enabled = true;
        blend.src     = vine::graphics::BlendFactor::SrcAlpha;
        blend.dst     = vine::graphics::BlendFactor::OneMinusSrcAlpha;
        state->setBlend(blend);

        // addBox attaches the node under the given group: attach under the
        // blend StateNode only, so the box is NOT also drawn unblended as a
        // direct child of the overlay root.
        auto box = addBox(state.get(), vine::Colorf(1.0f, 0.30f, 0.25f, 1.0f), u8"translucent_overlay", Vec3d(1.0, -0.9, 1.05), Vec3d(0.55, 0.55, 0.55));
        if (auto* geometry = dynamic_cast<Geometry*>(box->children().front().get())) {
            // Per-geometry opacity drives the forward alpha blend.
            geometry->setOpacity(0.5f);
        }
        overlay_root->addChild(state);
    }

    // Rainbow five-pointed star point cloud (custom point-sprite program).
    {
        auto program = makeStarPointProgram();
        auto stars   = make_intrusive<Geometry>();
        stars->setName(u8"star_cloud_overlay");
        vine::geometry::Vec3fArray points;
        // Three stacked rings (radius and height growing per ring) in the
        // Z-up world: the rings are horizontal (XY) at rising heights above
        // the opaque stack's right side (clear airspace).
        for (int ring = 0; ring < 3; ++ring) {
            const float r = 0.30f + 0.16f * static_cast<float>(ring);
            const float h = 1.35f + (-0.55f + 0.55f * static_cast<float>(ring));
            for (int k = 0; k < 12; ++k) {
                const float a = 6.2831853f * static_cast<float>(k) / 12.0f + static_cast<float>(ring);
                points.emplace_back(2.0f + r * std::cos(a), 0.1f + r * std::sin(a), h);
            }
        }
        stars->setPositions(points);
        stars->setProgram(program);

        auto state = make_intrusive<StateNode>();
        state->setTopology(Topology::Points);
        state->addChild(stars);
        overlay_root->addChild(state);
    }

    auto overlay = make_intrusive<Scene>();
    overlay->setRoot(overlay_root);

    // The overlay is drawn as main content (depth-on, receives content lights)
    // inside the composite, so give it the same light rig as the opaque scene
    // so the translucent box shows consistent shading (Z-up directions).
    auto ambient = vine::graphics::Light::createAmbient();
    ambient->setName(u8"overlay_ambient");
    ambient->setIntensity(0.35f);
    overlay->addLight(ambient);
    // Same key + fill rig as the opaque scene (see addDemoLighting) so the
    // translucent box / star cloud shade consistently with the lit result.
    auto sun = vine::graphics::Light::createDirectional(vine::math::Vec3d(-0.35, -0.35, -0.75));
    sun->setName(u8"overlay_sun");
    sun->setIntensity(1.0f);
    overlay->addLight(sun);
    auto fill = vine::graphics::Light::createDirectional(vine::math::Vec3d(0.5, 0.5, -0.4));
    fill->setName(u8"overlay_fill");
    fill->setIntensity(0.5f);
    overlay->addLight(fill);

    return overlay;
}

/**
 * @brief Returns whether the demo should run the Deferred pipeline.
 *
 * The DEFAULT demo (no env) is now Deferred. VINE_PIPELINE=forward /
 * forward_shadowed selects the forward preset (which shows the forward
 * feature-showcase scene), deferred / deferred_shadowed forces Deferred, and
 * the legacy VINE_VSG_DEFERRED_FULL alias is subsumed by the deferred default.
 */
bool demoUsesDeferred()
{
    if (const char* mode = std::getenv("VINE_PIPELINE"); mode != nullptr) {
        return std::strcmp(mode, "deferred") == 0 || std::strcmp(mode, "deferred_shadowed") == 0;
    }
    return true; // default demo = Deferred
}

/**
 * @brief Builds the DEFAULT (Deferred) demo scene: opaque material boxes only.
 *
 * The default demo pipeline is Deferred, whose G-buffer geometry pass writes
 * albedo / normal / specular / view position through one geometry program.
 * Only plain lit, OPAQUE material boxes belong here: translucent blending,
 * polygon-line wireframes and custom per-drawable programs are forward-only
 * features that a G-buffer pass would override or mis-light (they stay
 * reachable under VINE_PIPELINE=forward via addDemoCubes).
 *
 * @param scene Scene to receive the content.
 */
void addDeferredDemoCubes(vine::graphics::Scene* scene)
{
    using vine::math::Vec3d;

    auto root = vine::make_intrusive<vine::graphics::Group>();
    scene->setRoot(root);

    // Z-up world (robotics convention: X forward, Z up): the ground is a slab
    // lying in the XY plane (top at z = 0) and every box stands along +Z.
    addBox(root.get(), vine::Colorf(0.45f, 0.47f, 0.52f, 1.0f), u8"ground", Vec3d(0.0, 0.0, -0.05), Vec3d(3.0, 3.0, 0.05));
    addBox(root.get(), vine::Colorf(0.30f, 0.62f, 0.36f, 1.0f), u8"box_green", Vec3d(0.0, 0.0, 0.4), Vec3d(0.5, 0.5, 0.4));
    addBox(root.get(), vine::Colorf(0.90f, 0.30f, 0.25f, 1.0f), u8"box_red", Vec3d(1.4, 0.5, 0.35), Vec3d(0.35, 0.35, 0.35));
    addBox(root.get(), vine::Colorf(0.20f, 0.55f, 0.90f, 1.0f), u8"box_blue", Vec3d(-1.4, -0.5, 0.35), Vec3d(0.35, 0.35, 0.35));
    addBox(root.get(), vine::Colorf(0.95f, 0.72f, 0.15f, 1.0f), u8"box_gold", Vec3d(-1.2, 1.3, 0.3), Vec3d(0.3, 0.3, 0.3));
    // A tall pillar and a low slab for varied depth/position content.
    addBox(root.get(), vine::Colorf(0.55f, 0.30f, 0.85f, 1.0f), u8"box_purple", Vec3d(1.1, -1.1, 0.95), Vec3d(0.3, 0.3, 0.95));
    addBox(root.get(), vine::Colorf(0.15f, 0.75f, 0.65f, 1.0f), u8"box_teal", Vec3d(-1.6, 0.9, 0.2), Vec3d(0.5, 0.3, 0.2));
}

/**
 * @brief Assembles the main-window pipeline from the shared presets, plus an
 * axis-gizmo HUD overlay (env VINE_PIPELINE).
 *
 * Values: forward | deferred | forward_shadowed | deferred_shadowed. The
 * shadowed variants are placeholders (the shadow slice is not implemented, so
 * they assemble the same pipeline as their unshadowed counterpart). With no
 * env var the DEFAULT is now the DEFERRED preset (the default demo also draws
 * the G-buffer's four colour attachments as small top-left previews); set
 * VINE_PIPELINE=forward to switch to the forward feature-showcase scene. The
 * legacy VINE_VSG_DEFERRED_FULL alias is subsumed by the deferred default.
 *
 * An axis gizmo mirroring the view camera is always added (configurable via
 * PipelineOptions::gizmo) and - together with any Deferred G-buffer - is kept
 * in step with the window by a single surface-layout step. Because the main
 * window pass carries the view camera, RenderControl does not add a second
 * forward pass.
 *
 * @param render_control Render view whose engine receives the passes.
 */
void addDemoPipeline(gui::RenderControl* render_control, vine::intrusive_ptr<vine::graphics::Scene> transparent)
{
    auto* engine = render_control->engine();
    auto* view   = render_control->view();
    if (engine == nullptr || view == nullptr || view->camera() == nullptr) {
        return;
    }

    using vine::graphics::PipelinePreset;
    // Default demo preset = Deferred (see demoUsesDeferred); VINE_PIPELINE
    // keeps forward available for the forward feature-showcase scene.
    PipelinePreset preset = demoUsesDeferred() ? PipelinePreset::Deferred : PipelinePreset::Forward;
    if (const char* override_mode = std::getenv("VINE_PIPELINE"); override_mode != nullptr && std::strcmp(override_mode, "forward_shadowed") == 0) {
        preset = PipelinePreset::ForwardShadowed;
    }

    vine::graphics::RenderPipelineBuilder builder(engine);
    builder.setCamera(view->camera());
    builder.setContent(view->scene());
    // Forward-only overlay content (Deferred default demo): handed to the
    // builder so it composites the content depth-on over the deferred-lit
    // result instead of floating on top (see setTransparentContent).
    if (transparent != nullptr) {
        builder.setTransparentContent(std::move(transparent));
    }
    vine::graphics::PipelineOptions options;
    // Axis-gizmo HUD overlay: mirrors the view camera in the bottom-left.
    options.gizmo.source_camera = view->camera();
    options.gizmo.pixel_ratio   = render_control->devicePixelRatio();
    // Frame-rate readout HUD overlay (bottom-right), on by default in the
    // demo; set options.fps.enabled = false to turn it off.
    options.fps.enabled         = true;
    options.fps.pixel_ratio     = render_control->devicePixelRatio();
    auto pipeline               = builder.build(preset, options);
    if (pipeline == nullptr) {
        return;
    }

    // Deferred preset: the builder's G-buffer pass published its MRT target
    // as "GBuffer". Draw each colour attachment as a small top-left preview so
    // the default demo shows the G-buffer alongside the lit window result
    // (0 = albedo, 1 = view normal + shininess, 2 = specular, 3 = view pos).
    if (preset == PipelinePreset::Deferred) {
        const double dpr   = render_control->devicePixelRatio();
        const int    pip_w = static_cast<int>(160.0 * dpr);
        const int    pip_h = static_cast<int>(90.0 * dpr);
        // The G-buffer the builder created, reached through the pipeline handle: the previews declare
        // the one image they sample (target + attachment) instead of a name plus an index — the same
        // wire the builder's producer promised as a whole target.
        auto*        gbuffer = pipeline->offscreenTarget();
        for (int attachment = 0; attachment < 4; ++attachment) {
            auto preview = vine::make_intrusive<vine::graphics::ScreenPass>();
            preview->setName(u8"gbuffer_preview");
            preview->addInputName(u8"GBuffer");
            if (gbuffer != nullptr) {
                // The declared image carries the attachment, so the host does not repeat it.
                auto image = vine::make_intrusive<vine::graphics::ImageRef>(u8"GBuffer.preview");
                image->bind(vine::intrusive_ptr<vine::graphics::RenderTarget>(gbuffer), attachment);
                preview->addInput(image);
            }
            const int x = 8 + attachment * (pip_w + 8);
            preview->setViewport(x, 8, pip_w, pip_h);
            engine->addPass(preview, 120 + attachment);
        }
    }

    // One creator-managed layout step keeps the (deferred) G-buffer and the
    // gizmo overlay in step with the window; it owns the pipeline handle.
    // The deferred off-screen targets are sized at DEVICE pixels so the light
    // pass samples them 1:1 with the swapchain (sizing them at the logical
    // surface size made the whole deferred result a 2x-upscaled soft image).
    // The gizmo / fps overlays take the LOGICAL size (they apply their own
    // pixel_ratio internally), so they are re-laid-out at logical afterwards.
    view->addSurfaceLayout([pipeline, render_control](int width, int height) {
        const double d  = render_control->devicePixelRatio();
        const int    dw = static_cast<int>(width * d);
        const int    dh = static_cast<int>(height * d);
        pipeline->resize(dw, dh);
        if (auto* g = pipeline->gizmo(); g != nullptr) {
            g->onSurfaceResized(width, height);
        }
        if (auto* f = pipeline->fpsOverlay(); f != nullptr) {
            f->onSurfaceResized(width, height);
        }
    });
}

AppShellDock buildAppShellDock(gui::MainWindow* wnd)
{
    AppShellDock result;

    auto* manager = wnd->dockPanelManager();

    auto* left_panel = manager->createDockPanel(u8"项目", gui::DockAreas::Left);
    left_panel->setId(u8"dock_project");

    // Render view in the central client area; init() picks the first
    // registered render backend (e.g. "vsg") by default.
    auto* render_control = new gui::RenderControl();
    manager->setCentralWidget(render_control);
    // Default demo = Deferred with an OPAQUE scene; VINE_PIPELINE=forward uses
    // the forward feature-showcase scene (translucent / wireframe / custom
    // point & star programs, which a G-buffer pass would override). Under the
    // Deferred preset the forward-only elements (rainbow star point cloud +
    // translucent box) live in a separate overlay scene that the pipeline
    // builder composites depth-on over the deferred result (see
    // makeForwardOverlayScene / setTransparentContent). The translucent box
    // overlaps the opaque pillar to demonstrate the depth-correct composite.
    vine::intrusive_ptr<vine::graphics::Scene> overlay_scene;
    if (demoUsesDeferred()) {
        addDeferredDemoCubes(render_control->view()->scene().get());
        overlay_scene = makeForwardOverlayScene();
    }
    else {
        addDemoCubes(render_control->view()->scene().get());
    }
    addDemoLighting(render_control);
    addOffscreenValidationPass(render_control);
    // Dev switch: VINE_VSG_SLOT_DEMO stacks a second (camera, content slot)
    // overlay pass on the main camera to validate same-view multi-slot drawing.
    addSlotOverlayDemo(render_control);
    // Dev switch: VINE_VSG_OFFSCREEN_MULTISLOT bakes ONE off-screen target with
    // two content slots (main scene + on-top overlay) shown via PiP (C6.4).
    addOffscreenMultiSlotDemo(render_control);
    // Dev switch: VINE_VSG_GBUFFER previews the Deferred G-buffer's colour
    // attachments (albedo / normal / spec / view pos) via PiP.
    addGbufferDemo(render_control);
    // Dev switch: VINE_VSG_DEFERRED adds a fullscreen deferred-lighting pass
    // that reads the G-buffer and shows the lit result (S2a, A/B preview).
    addDeferredDemo(render_control);
    // Main window pipeline: env VINE_PIPELINE (forward | deferred |
    // forward_shadowed | deferred_shadowed) selects a shared preset; the
    // DEFAULT is Deferred (demoUsesDeferred). An axis-gizmo HUD overlay rides
    // on the built pipeline. Under Deferred the overlay scene is handed to the
    // builder as transparent content, so the builder composites it depth-on
    // over the deferred-lit result.
    addDemoPipeline(render_control, overlay_scene);
    // Dev switch: setting VINE_SHADER_PRESET exercises the FlatShaded preset
    // through the whole engine/backend path (default = StandardPhong).
    if (std::getenv("VINE_SHADER_PRESET") != nullptr) {
        render_control->engine()->setShaderPreset(vine::graphics::ShaderPreset::FlatShaded);
    }
    // Register the 3D view so other plugins (tests/editors) can reach the
    // render engine/scene without depending on app shell internals.
    wnd->setPrimaryRenderControl(render_control);
    result.render_control = render_control;

    // Initialize once the window is shown and the central widget is laid out:
    // the backend attaches to the realized native surface. Deferred so the
    // first layout pass has happened by the time init() runs.
    QTimer::singleShot(100, [render_control] { render_control->init(); });

    auto* right_panel = manager->createDockPanel(u8"属性", gui::DockAreas::Right);
    right_panel->setId(u8"dock_properties");

    auto* console_panel = new gui::ConsolePanel();
    auto* console_dock  = manager->createDockPanel(u8"控制台", console_panel, gui::DockAreas::Bottom);
    console_dock->setId(u8"dock_console");

    if (auto* app = ::vine::obj_cast<gui::GuiApplication>(Application::current())) {
        app->setConsolePanel(console_panel);
    }

    result.console_panel = console_panel;
    return result;
}

V_APPFW_NS_END
