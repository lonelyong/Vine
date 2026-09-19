/**
 * @brief Shadow phases: they hand the renderer to a RenderEngine and assert the resulting picture.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace vine::graphics;

namespace selftest
{

/** @brief Builds a horizontal quad in the XZ plane whose normal faces up. */
GeometryPtr makeGroundQuad(float half)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float corners[6][2] = { { -half, -half }, { half, -half }, { half, half },
                                 { -half, -half }, { half, half },  { -half, half } };
    for (const auto& corner : corners) {
        positions.emplace_back(corner[0], 0.0f, corner[1]);
    }
    geom->setPositions(vine::graphics::packAttribute(positions));
    vine::geometry::Vec3fArray normals;
    for (int i = 0; i < 6; ++i) {
        normals.emplace_back(0.0f, 1.0f, 0.0f);
    }
    geom->setNormals(vine::graphics::packAttribute(normals));
    return geom;
}

/**
 * @brief Asserts the shadow the DEFERRED path builds actually darkens the ground.
 *
 * Every phase above drives the renderer directly; this one drives the ENGINE, because what is
 * under test is the builder's RECIPE for a shadow (a castShadow sun in the content scene -> a
 * depth-only pass at PipelineStage::Depth, its view-projection stated on the target, the lighting
 * pass declaring that target as an input) and the engine's plumbing of it (resolvePassInputs ->
 * setPassInputs -> the backend's bindings). A phase that built those passes by hand would prove
 * the backend can bind a map; it would not notice the recipe building the wrong one, aiming the
 * light camera the wrong way, or declaring the input on the wrong pass.
 *
 * The engine brings the renderer up itself (RenderEngine::initialize forwards the default content
 * program and initializes the backend), so this phase runs AFTER the harness-driven teardown at the
 * end of main(): one session, one owner, no doubt about which slot ledger is live.
 *
 * The picture is read back from the pipeline's COMPOSITE target. The deferred path bakes the lit
 * image off-screen only when it has forward content to composite, so the phase hands it an EMPTY
 * transparent scene: the forward pass then draws nothing and the composite holds exactly the lit
 * opaque image. The window cannot be used — readColorBuffer refuses a null target.
 *
 * The two pixels are ground points at the SAME world z (0.4) and mirrored x, so they project onto
 * one row: the sun is slanted along +x, which makes x = +0.3 the wall's shadow and x = -1.0 the
 * same surface in the sun. Sharing a row means the measurement cannot be an artefact of two rows
 * (the sun's light is the only difference between the two pixels), and their columns are the
 * projection of those points through this phase's camera, computed once below rather than searched
 * for at run time — a search for the darkest pixel finds one wherever the shadow landed.
 *
 * The picture is three levels: the background (the lighting program's own 0.06 grey), the ground in
 * the shadow (the ambient fill only - no ambient light is announced, so the backend's 0.15 fill
 * times the 0.8 albedo, ~31 per channel) and the ground in the sun (~196). That is what makes the
 * measurement unambiguous: "went dark" cannot be confused with "was never drawn", and each of the
 * four bugs this phase found showed up as one of those levels being wrong.
 *
 * Mutations checked, each on its own build: the sun's castShadow off (the shadow is gone: the
 * shadowed pixel reads the lit value); the map's v axis not flipped and its depth not inverted (one
 * at a time: the lit pixel reads the shadowed value, or the sun vanishes entirely); and the shadow
 * pass' camera borrowed instead of owned (the map comes back empty - the pass draws through freed
 * memory).
 *
 * @param backend Backend under test (the engine initializes it: it must be down when called).
 * @param frames  Frames to drive.
 * @return true when the shadowed ground is measurably darker than the lit ground.
 */
/**
 * @brief The scene both shadow phases measure, and where they read it.
 *
 * ONE scene for both paths (a deferred and a forward pipeline measure the same picture, so a
 * difference between them is a difference of PATH), and one place the sample pixels are derived:
 * the two ground points are at the same world z with mirrored x, so they project onto ONE row (the
 * camera has no roll) and the sun is the only difference between them.
 */
struct ShadowPixelScene
{
    vine::intrusive_ptr<Scene> content;              ///< Ground + the wall standing on it + the sun.
    vine::intrusive_ptr<Scene> forward_placeholder;  ///< Empty forward scene (see runDeferredShadowPixelPhase).
    CameraPtr                  camera;               ///< Camera looking down at the ground from the front.
    LightPtr                   sun;                  ///< The slanted, shadow-casting directional light.
    int                        shadow_col = 343;     ///< Pixel of (0.3, 0, 0.4) - in the wall's shadow.
    int                        lit_col    = 240;     ///< Pixel of (-1.0, 0, 0.4) - the same ground, in the sun.
    int                        row        = 196;     ///< The row both points project onto.
};

/**
 * @brief Builds the scene both shadow phases measure (see ShadowPixelScene).
 *
 * The ground is bright with a BLACK specular: the shadow scales the light's diffuse term, and a
 * highlight would put a shadow-independent term into the pixels the phases measure. The wall stands
 * ON the ground (its bottom edge is the ground line) so its shadow starts at the wall and falls onto
 * ground the camera can see, and the sun travels along (0.6, -1, 0.4) - down, and toward +x / +z -
 * so that shadow falls on the +x side, where the sampled pixel is.
 *
 * @return The scene, its camera and its sun.
 */
ShadowPixelScene makeShadowPixelScene()
{
    ShadowPixelScene scene;
    scene.content = vine::intrusive_ptr<Scene>(new Scene());
    auto root     = vine::intrusive_ptr<Group>(new Group());

    auto ground_material = MaterialPtr(new Material());
    ground_material->setDiffuse(vine::Colorf(0.8f, 0.8f, 0.8f, 1.0f));
    ground_material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    auto ground = makeGroundQuad(3.0f);
    ground->setMaterial(ground_material);
    ground->setName(u8"shadow-ground");
    root->addChild(ground);

    auto wall_material = MaterialPtr(new Material());
    wall_material->setDiffuse(vine::Colorf(0.9f, 0.9f, 0.9f, 1.0f));
    auto wall = makeVisibleQuad(1.0f, 0.0f, 1.0f);
    wall->setMaterial(wall_material);
    wall->setName(u8"shadow-caster");
    auto wall_place = vine::intrusive_ptr<MatrixTransform>(new MatrixTransform());
    wall_place->setMatrix(vine::math::translate(vine::math::Vec3d(0.0, 1.0, 0.0)));
    wall_place->addChild(wall);
    root->addChild(wall_place);

    // A TEXTURED patch on the ground, well left of the two sampled ground points. It is what lets these
    // phases see whether the path under test SAMPLES the material's texture at all: the forward stage
    // always has, the deferred G-buffer stage gained it when it learned to bind the material's map (behind
    // VINE_DIFFUSE_MAP, so untextured content pays nothing). A stage that quietly stopped binding the
    // sampler would leave the patch WHITE - the material's colour - and every other assertion here would
    // still hold, because nothing else in the scene carries a texture.
    //
    // The material is white with a BLACK specular, so the map is the only colour on the patch and no
    // highlight can wash the hue out; the map is the two-tone one (red half / blue half) with UVs spanning
    // it, so both hues have to appear.
    auto patch_material = MaterialPtr(new Material());
    patch_material->setDiffuse(vine::Colorf(1.0f, 1.0f, 1.0f, 1.0f));
    patch_material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    patch_material->setTexture(makeTwoToneTexture());
    auto patch = makeGroundQuad(0.45f);
    {
        // makeGroundQuad's corner order, matching makeTexturedQuad's UV walk (v = 0 at the far edge).
        const float uvs[6][2] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f },
                                  { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
        vine::geometry::Vec2fArray texcoords;
        for (const auto& uv : uvs) {
            texcoords.emplace_back(uv[0], uv[1]);
        }
        patch->setTexcoords2(vine::graphics::packAttribute(texcoords));
    }
    patch->setMaterial(patch_material);
    patch->setName(u8"shadow-textured");
    auto patch_place = vine::intrusive_ptr<MatrixTransform>(new MatrixTransform());
    // Just above the ground: coplanar geometry would z-fight in the shadow map and the G-buffer alike.
    patch_place->setMatrix(vine::math::translate(vine::math::Vec3d(-2.4, 0.01, 0.3)));
    patch_place->addChild(patch);
    root->addChild(patch_place);

    scene.content->setRoot(root);

    scene.sun = LightPtr(Light::createDirectional(vine::math::Vec3d(0.6, -1.0, 0.4)));
    scene.sun->setName(u8"shadow-sun");
    scene.sun->setCastShadow(true);
    scene.content->addLight(scene.sun);

    // A camera looking down at the ground from the front: the self-test's flat camera sees the world
    // y = 0 plane edge-on, i.e. as a line of pixels.
    scene.camera = CameraPtr(new Camera());
    scene.camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 3.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                        vine::math::Vec3d(0.0, 1.0, 0.0));
    scene.camera->setProjectionMatrixAsPerspective(45.0, 640.0 / 360.0, 0.1, 1000.0);

    scene.forward_placeholder = vine::intrusive_ptr<Scene>(new Scene());
    return scene;
}

/**
 * @brief Asserts the shadow picture a shadow phase produced (see ShadowPixelScene).
 *
 * The picture has three levels on purpose: the background (the lighting program's own 0.06 grey,
 * ~15), the ground in the shadow (the ambient fill only - no ambient light is announced, so the
 * backend's 0.15 fill times the 0.8 albedo, ~31 per channel) and the ground in the sun (~196). That
 * is what makes the measurement unambiguous: "went dark" cannot be confused with "was never drawn".
 *
 * It also asserts the TEXTURED patch (see makeShadowPixelScene): both halves of its two-tone map have
 * to appear, which is the only thing here that can tell a path that samples the material's texture from
 * one that does not - a path that skipped the sampler would draw the patch in its white material colour
 * and pass every other check in this function.
 *
 * @param image What to read (a 640x360 picture).
 * @param scene Scene whose sample columns the assertions use.
 * @param what  Which picture this is, for the failure messages.
 * @return true when the ground in the shadow is measurably darker than the same ground in the sun.
 */
bool assertShadowPicture(const PixelImage& image, const ShadowPixelScene& scene, const char* what)
{
    if (image.width != 640 || image.height != 360) {
        std::fprintf(stderr, "[selftest] FAIL: %s is %dx%d, not the 640x360 the sample pixels are for\n", what,
                     image.width, image.height);
        return false;
    }
    bool ok = true;
    const int shadow_sum = image.at(scene.shadow_col, scene.row, 0) + image.at(scene.shadow_col, scene.row, 1) +
                           image.at(scene.shadow_col, scene.row, 2);
    const int lit_sum = image.at(scene.lit_col, scene.row, 0) + image.at(scene.lit_col, scene.row, 1) +
                        image.at(scene.lit_col, scene.row, 2);
    const int shadow_r = image.at(scene.shadow_col, scene.row, 0);
    const int lit_r    = image.at(scene.lit_col, scene.row, 0);

    // The lit ground must be plainly lit: without this half, a picture that drew nothing (or drew the
    // background everywhere) would "pass" the darker-in-shadow comparison below with two dark pixels.
    if (lit_sum < 300) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %s: the ground beside the wall read (%d,%d,%d) at column %d - the sun did not "
                     "reach the ground, so the shadow comparison would be between two unlit pixels\n",
                     what, image.at(scene.lit_col, scene.row, 0), image.at(scene.lit_col, scene.row, 1),
                     image.at(scene.lit_col, scene.row, 2), scene.lit_col);
        ok = false;
    }
    // ...and the shadowed ground must show the AMBIENT term only, not the background (the shading writes
    // a 0.06 grey where nothing was drawn) and not the lit surface: 0.8 albedo * the 0.15 ambient fill
    // the backend keeps when no ambient light is announced is ~31.
    else if (shadow_r < 20 || shadow_r > 45) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %s: the ground in the shadow read (%d,%d,%d) at column %d - expected the "
                     "ambient-only surface (~31 per channel: 0.8 albedo * the 0.15 ambient fill), not the background "
                     "(15) and not a lit surface (%d)\n",
                     what, shadow_r, image.at(scene.shadow_col, scene.row, 1), image.at(scene.shadow_col, scene.row, 2),
                     scene.shadow_col, lit_r);
        ok = false;
    }
    else if (shadow_sum + 200 > lit_sum) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %s: the ground in the shadow read %d against %d in the sun (columns %d/%d, row "
                     "%d) - the castShadow sun did not darken the ground it is blocked from\n",
                     what, shadow_sum, lit_sum, scene.shadow_col, scene.lit_col, scene.row);
        ok = false;
    }

    // The textured patch's two halves: red and blue can ONLY come from the map (everything else in this
    // scene is grey or white), so finding one of each is the assertion that the path under test sampled
    // the material's texture. A path that did not would draw the patch in the material's white and fail
    // here - which is the whole reason the patch is in the scene.
    bool red_seen  = false;
    bool blue_seen = false;
    for (int y = 0; y < image.height && !(red_seen && blue_seen); ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int r = image.at(x, y, 0);
            const int g = image.at(x, y, 1);
            const int b = image.at(x, y, 2);
            red_seen  = red_seen || (r > g + 40 && r > b + 40);
            blue_seen = blue_seen || (b > g + 40 && b > r + 40);
        }
    }
    if (!red_seen || !blue_seen) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %s: the textured patch showed no %s half of its two-tone map (red %s, blue %s) - "
                     "the path under test did not sample the material's texture\n",
                     what, !red_seen ? "red" : "blue", red_seen ? "found" : "missing", blue_seen ? "found" : "missing");
        ok = false;
    }
    return ok;
}

bool runDeferredShadowPixelPhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames)
{
    const ShadowPixelScene scene = makeShadowPixelScene();

    auto engine = vine::intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    if (!engine->initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the shadow phase's engine could not bring the backend up\n");
        return false;
    }

    PipelineOptions options;
    options.path             = ShadingPath::Deferred;
    options.offscreen_width  = 640;
    options.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setContent(scene.content);
    builder.setCamera(scene.camera.get());
    builder.setTransparentContent(scene.forward_placeholder);
    auto pipeline = builder.build(options);
    if (pipeline == nullptr || pipeline->compositeTarget() == nullptr) {
        std::fprintf(stderr, "[selftest] FAIL: the deferred pipeline did not build (no composite target)\n");
        engine->shutdown();
        return false;
    }

    // Build the fullscreen slot through a DIFFERENT camera, then move to the measured one. The sun is
    // fixed in the world, so the picture at the measured camera must not depend on where the camera
    // stood when the slot was built. What makes that a real test is the shadow block: it steps from
    // the PASS' view space into light clip, so it is a per-frame value that has to be rewritten as
    // the camera moves. A block written only when the slot is built freezes that step at the build
    // camera, and the shadow then slides with the viewpoint (and the ground, its own caster under the
    // thus-wrong lookup, reads self-shadowed across the whole map). No other phase moves the camera
    // after a slot exists, which is why this is the only place that class of bug can show.
    scene.camera->setViewMatrixAsLookAt(vine::math::Vec3d(3.5, 2.2, 4.0), vine::math::Vec3d(0.0, 0.3, 0.0),
                                        vine::math::Vec3d(0.0, 1.0, 0.0));
    engine->frame(1.0 / 60.0);   // the lighting pass' first render: its slot is built through THIS camera
    scene.camera->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 3.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                        vine::math::Vec3d(0.0, 1.0, 0.0));

    for (int i = 0; i < frames; ++i) {
        engine->frame(1.0 / 60.0);
    }

    PixelImage image;
    const bool read_ok = backend->readColorBuffer(pipeline->compositeTarget(), 0, image.pixels);
    if (read_ok) {
        image.width  = pipeline->compositeTarget()->width();
        image.height = pipeline->compositeTarget()->height();
    }
    if (!read_ok) {
        std::fprintf(stderr, "[selftest] FAIL: the deferred shadow phase could not read its composite\n");
        engine->shutdown();
        return false;
    }

    const bool ok = assertShadowPicture(image, scene, "the deferred composite");
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] deferred shadow: the ground in the sun's shadow read (%d,%d,%d) against (%d,%d,%d) "
                     "beside it in the sun (columns %d/%d of row %d), so the built shadow pass reached the lighting\n",
                     image.at(scene.shadow_col, scene.row, 0), image.at(scene.shadow_col, scene.row, 1),
                     image.at(scene.shadow_col, scene.row, 2), image.at(scene.lit_col, scene.row, 0),
                     image.at(scene.lit_col, scene.row, 1), image.at(scene.lit_col, scene.row, 2), scene.shadow_col,
                     scene.lit_col, scene.row);
    }

    pipeline = nullptr;   // unregisters its passes from the engine before the session goes down
    engine->shutdown();
    return ok;
}

bool runForwardShadowPixelPhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames)
{
    const ShadowPixelScene scene = makeShadowPixelScene();

    auto engine = vine::intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    if (!engine->initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the forward shadow phase's engine could not bring the backend up\n");
        return false;
    }

    RenderPipelineBuilder builder(engine.get());
    builder.setContent(scene.content);
    builder.setCamera(scene.camera.get());
    auto pipeline = builder.build(PipelineOptions{});
    if (pipeline == nullptr || pipeline->windowPass() == nullptr) {
        std::fprintf(stderr, "[selftest] FAIL: the forward pipeline did not build (no window pass)\n");
        engine->shutdown();
        return false;
    }

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(640, 360);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    pipeline->windowPass()->setRenderTarget(target);

    for (int i = 0; i < frames; ++i) {
        engine->frame(1.0 / 60.0);
    }

    PixelImage image;
    const bool read_ok = backend->readColorBuffer(target.get(), 0, image.pixels);
    if (read_ok) {
        image.width  = target->width();
        image.height = target->height();
    }
    if (!read_ok) {
        std::fprintf(stderr, "[selftest] FAIL: the forward shadow phase could not read its target\n");
        engine->shutdown();
        return false;
    }

    const bool ok = assertShadowPicture(image, scene, "the retargeted forward pass");
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] forward shadow: the ground in the sun's shadow read (%d,%d,%d) against (%d,%d,%d) "
                     "beside it in the sun (columns %d/%d of row %d), so the content set's shadow binding reached the "
                     "shading\n",
                     image.at(scene.shadow_col, scene.row, 0), image.at(scene.shadow_col, scene.row, 1),
                     image.at(scene.shadow_col, scene.row, 2), image.at(scene.lit_col, scene.row, 0),
                     image.at(scene.lit_col, scene.row, 1), image.at(scene.lit_col, scene.row, 2), scene.shadow_col,
                     scene.lit_col, scene.row);
    }

    pipeline = nullptr;
    engine->shutdown();
    return ok;
}

/** @brief Builds the five visible faces of an axis-aligned box (no bottom) as ONE drawable. */
GeometryPtr makeBoxFaces(float half_x, float half_z, float bottom_y, float top_y)
{
    auto                       geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    vine::geometry::Vec3fArray normals;
    // One quad = two triangles, appended in the order given. Culling is off by default (StateNode),
    // so the winding is not load-bearing: the depth test sorts the far faces of the closed body out.
    const auto add_quad = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy,
                              float cz, float dx, float dy, float dz, float nx, float ny, float nz) {
        const float px[6] = { ax, bx, cx, ax, cx, dx };
        const float py[6] = { ay, by, cy, ay, cy, dy };
        const float pz[6] = { az, bz, cz, az, cz, dz };
        for (int i = 0; i < 6; ++i) {
            positions.emplace_back(px[i], py[i], pz[i]);
            normals.emplace_back(nx, ny, nz);
        }
    };
    const float hx = half_x;
    const float hz = half_z;
    // Top: the face this phase measures, facing the sun and the camera alike.
    add_quad(-hx, top_y, -hz, hx, top_y, -hz, hx, top_y, hz, -hx, top_y, hz, 0.0f, 1.0f, 0.0f);
    // The four sides, so the body occludes its own far faces and casts a shadow with a defined edge.
    add_quad(hx, bottom_y, -hz, hx, bottom_y, hz, hx, top_y, hz, hx, top_y, -hz, 1.0f, 0.0f, 0.0f);
    add_quad(-hx, bottom_y, hz, -hx, bottom_y, -hz, -hx, top_y, -hz, -hx, top_y, hz, -1.0f, 0.0f, 0.0f);
    add_quad(-hx, bottom_y, hz, hx, bottom_y, hz, hx, top_y, hz, -hx, top_y, hz, 0.0f, 0.0f, 1.0f);
    add_quad(hx, bottom_y, -hz, -hx, bottom_y, -hz, -hx, top_y, -hz, hx, top_y, -hz, 0.0f, 0.0f, -1.0f);
    geom->setPositions(vine::graphics::packAttribute(positions));
    geom->setNormals(vine::graphics::packAttribute(normals));
    return geom;
}

/** @brief The pixel a world point projects to, in a read-back image (y downwards, like the buffer). */
PixelImage::Point pixelOfWorld(const Camera& camera, const vine::math::Vec3d& world, int width, int height)
{
    const vine::math::Mat4d clip = camera.projectionMatrix() * camera.viewMatrix();
    const double              w    = clip(3, 0) * world.x + clip(3, 1) * world.y + clip(3, 2) * world.z + clip(3, 3);
    const double              x    = clip(0, 0) * world.x + clip(0, 1) * world.y + clip(0, 2) * world.z + clip(0, 3);
    const double              y    = clip(1, 0) * world.x + clip(1, 1) * world.y + clip(1, 2) * world.z + clip(1, 3);
    const double              ndc_x = (w != 0.0) ? x / w : 0.0;
    const double              ndc_y = (w != 0.0) ? y / w : 0.0;
    PixelImage::Point         point;
    point.x = static_cast<int>((ndc_x * 0.5 + 0.5) * static_cast<double>(width));
    point.y = static_cast<int>((0.5 - ndc_y * 0.5) * static_cast<double>(height));
    point.x = std::max(0, std::min(width - 1, point.x));
    point.y = std::max(0, std::min(height - 1, point.y));
    return point;
}

/** @brief Summed colour of a pixel, the one number these phases compare. */
int pixelSum(const PixelImage& image, PixelImage::Point p)
{
    return image.at(p, 0) + image.at(p, 1) + image.at(p, 2);
}

/**
 * @brief Asserts a shadow toggle cannot darken a face the sun reaches: a lit TOP face stays lit.
 *
 * The two phases above measure the GROUND, and that is what let this class of defect through: "the ground
 * got darker" is satisfied by a shadow that is wrong in every other way. A map now states WHOSE it is
 * (RenderTarget::setShadowOf, a fact about the map, not a concept on this generic pass) and the light
 * states whether it casts,
 * so nothing here has to be inferred - but the CONSEQUENCES still need pinning, and they are the ones the
 * two defects behind this phase had: one light's map must scale ONE light's term (the term sits inside the
 * per-light loop, and scaling the rest extinguished the demo's fill light wherever the sun was blocked),
 * and the map must be the one its producer published (a G-buffer that merely had a sampleable depth was
 * bound as the sun's map in a whole deferred branch, which shaded a "shadow" that was a function of world
 * position and moved with the viewpoint).
 *
 * The ground alone cannot pin that down (a wrong map can darken "some ground" by accident), so this
 * phase measures the one thing a wrong map cannot get right: the box's TOP face, which the sun
 * reaches and which nothing else in this scene can occlude. Rendering the same scene with
 * castShadow off and on has to leave every probe on that face unchanged, while the ground behind the
 * box has to be plainly darker - so the phase fails if a lit face is darkened AND fails if the
 * shadow term stopped reaching the ground at all.
 *
 * Both deferred branches are run, because they differ in where the lit image ends up (with an empty
 * transparent scene the lighting pass bakes into a composite; without transparent content it presents
 * through its window pass) and the second is where the inference that bound the G-buffer survived
 * longest. Three camera vantages are used, the last one CLOSE, because the demo's own numbers put one
 * shadow-map texel at ~8.7 mm of world: an artifact a few texels wide is sub-pixel from far away and
 * plainly visible at the scale a host zooms to.
 *
 * @param backend The renderer to drive (a real device, lavapipe in CI).
 * @param frames  Frames to render per read-back, so every pass has been through its first-frame
 *                pipeline build.
 * @return true when a lit top face survives the shadow term and the ground shadow still lands.
 */
bool runShadowedLitFacePhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames, bool standalone)
{
    auto engine = vine::intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    if (!engine->initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the lit-face phase's engine could not bring the backend up\n");
        return false;
    }

    auto content = vine::intrusive_ptr<Scene>(new Scene());
    auto root    = vine::intrusive_ptr<Group>(new Group());

    auto ground_material = MaterialPtr(new Material());
    ground_material->setDiffuse(vine::Colorf(0.8f, 0.8f, 0.8f, 1.0f));
    // A BLACK specular again: the shadow scales the light's diffuse term, so a highlight would put a
    // shadow-independent term into the pixels this phase compares.
    ground_material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    auto ground = makeGroundQuad(4.5f); // 9x9: the demo's footprint, the scale its bias is stated for
    ground->setMaterial(ground_material);
    ground->setName(u8"lit-face-ground");
    root->addChild(ground);

    auto box_material = MaterialPtr(new Material());
    box_material->setDiffuse(vine::Colorf(0.85f, 0.85f, 0.85f, 1.0f));
    box_material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    auto box = makeBoxFaces(0.25f, 0.25f, 0.0f, 0.4f);
    box->setMaterial(box_material);
    box->setName(u8"lit-face-box");
    root->addChild(box);

    // A tall pillar, because a 0.5-unit box casts almost nothing this phase can use as a control: the
    // demo's 0.005 bias is a 0.13-unit erosion along the light at this content size, so a small
    // caster's ground shadow all but disappears. The pillar (2.0 tall) puts its shadow 2.0 * (0.6, 0.4)
    // down-sun of its base - at (-1.4, -1.8), clear of the box, its top face and every probe here - so
    // the phase can tell "the shadow term reached the picture" from "it did nothing".
    auto pillar = makeBoxFaces(0.1f, 0.1f, 0.0f, 2.0f);
    pillar->setMaterial(box_material);
    pillar->setName(u8"lit-face-pillar");
    auto pillar_place = vine::intrusive_ptr<MatrixTransform>(new MatrixTransform());
    pillar_place->setMatrix(vine::math::translate(vine::math::Vec3d(-2.6, 0.0, -2.6)));
    pillar_place->addChild(pillar);
    root->addChild(pillar_place);

    content->setRoot(root);
    // The demo's own sun and shadow numbers (AppShellDemo::addDemoLighting): travelling down and
    // toward +x/+z, 1024 texels over the content, a 0.005 bias. Reusing them is the point - a gate
    // measured at other numbers would not describe the picture the demo draws.
    auto sun = LightPtr(Light::createDirectional(vine::math::Vec3d(0.6, -1.0, 0.4)));
    sun->setName(u8"lit-face-sun");
    sun->setCastShadow(true);
    sun->setShadowResolution(1024);
    sun->setShadowBias(0.005f);

    // A FILL light with no shadow of its own, exactly as the demo has one (AppShellDemo::addDemoLighting):
    // it travels down and toward -x/+z, so it lights the box's +z face - the face every vantage here can
    // see, and the one the SUN cannot reach (its direction has no +z component to speak of). That makes
    // the +z face the phase's second subject: its light comes from a lamp whose own map does not exist,
    // so nothing about the sun's shadow may touch it. A shadow term that scales EVERY light's term (the
    // terms are inserted inside the per-light loop) extinguishes this face wherever the sun is blocked -
    // by the box itself - and the face goes dark in the shape of the sun's shadow.
    auto fill = LightPtr(Light::createDirectional(vine::math::Vec3d(-0.2, -0.5, -1.0)));
    fill->setName(u8"lit-face-fill");
    fill->setIntensity(0.5f);
    fill->setCastShadow(false);
    // ANNOUNCED FIRST, so the sun lands in the block's SECOND directional slot: the shader has to name
    // the light its map belongs to (`params.w`), and a term that names the wrong slot - or scales every
    // light - fails here, while a caster in slot 0 would hide a wrong index. The single-light deferred
    // and forward phases cover slot 0.
    content->addLight(fill);
    content->addLight(sun); // second: the caster has to be NAMED by the block, not assumed to be first

    const int width  = 640;
    const int height = 360;
    // Two vantages onto the same box: the sun never moves, so the sun-lit/shadowed classification of a
    // world point is the scene's, not the camera's. The third stands CLOSE, at the scale a host zooms to:
    // one shadow-map texel spans ~8.7 mm of world (the demo's own number), so an artifact a few texels
    // wide is sub-pixel from far away and plainly visible from here.
    struct Vantage
    {
        vine::math::Vec3d eye;
        vine::math::Vec3d target;
    };
    const Vantage vantages[3] = { { vine::math::Vec3d(0.0, 2.4, 3.4), vine::math::Vec3d(0.0, 0.3, 0.0) },
                                  { vine::math::Vec3d(2.6, 2.1, 2.4), vine::math::Vec3d(0.0, 0.25, 0.0) },
                                  { vine::math::Vec3d(0.5, 1.1, 1.3), vine::math::Vec3d(0.0, 0.35, 0.0) } };
    // The probes. The box's top face is the surface under test; the two ground points are the
    // controls that keep the phase from passing while the shadow term does nothing at all.
    const float  top_uvs[3] = { -0.18f, 0.0f, 0.18f };
    // The ground controls, stated as SETS rather than one computed point: the demo's 0.005 bias is a
    // 0.13-unit erosion along the light at this content size, which eats most of a 0.5-unit box's
    // shadow, so WHERE exactly the surviving sliver falls is the renderer's business - that nothing
    // lands up-sun, and that the shadow does reach the picture, is the phase's.
    const vine::math::Vec3d ground_up_sun[4] = { { -0.6, 0.0, -0.5 },
                                                 { -1.0, 0.0, -0.5 },
                                                 { -0.6, 0.0, -1.0 },
                                                 { -1.0, 0.0, -1.0 } };

    bool ok = true;
    for (int v = 0; v < 3; ++v) {
        auto camera = CameraPtr(new Camera());
        camera->setViewMatrixAsLookAt(vantages[v].eye, vantages[v].target, vine::math::Vec3d(0.0, 1.0, 0.0));
        camera->setProjectionMatrixAsPerspective(45.0, static_cast<double>(width) / static_cast<double>(height), 0.1,
                                                 1000.0);

        RenderPipelineBuilder builder(engine.get());
        builder.setContent(content);
        builder.setCamera(camera.get());
        // An EMPTY transparent scene selects the composite branch (the one every demo view uses, since the
        // demo always has overlay content); no transparent content at all selects the branch whose lighting
        // pass presents through its window pass. Both are asserted: that second branch is where the
        // resolver used to pick the G-buffer (its depth promotion stays on there) instead of the map.
        if (!standalone) {
            builder.setTransparentContent(vine::intrusive_ptr<Scene>(new Scene()));
        }
        PipelineOptions options;
        options.path             = ShadingPath::Deferred;
        options.offscreen_width  = width;
        options.offscreen_height = height;
        auto pipeline            = builder.build(options);
        if (pipeline == nullptr || pipeline->windowPass() == nullptr) {
            std::fprintf(stderr, "[selftest] FAIL: the lit-face phase's deferred pipeline did not build\n");
            pipeline = nullptr;
            engine->shutdown();
            return false;
        }
        // The pipeline is read back through its WINDOW pass, retargeted into a target of this phase's own
        // (the composite branch's window pass blits the composite there): readColorBuffer refuses a null
        // target, and the window cannot be read.
        auto target = RenderTargetPtr(new RenderTarget());
        target->setSize(width, height);
        target->attachColor(RenderTarget::ColorFormat::RGBA8);
        target->attachDepth(RenderTarget::DepthFormat::D24);
        pipeline->windowPass()->setRenderTarget(target);

        // The caller sets the flag: this only renders and reads back, so the two frames differ by the
        // shadow term alone.
        const auto render = [&](PixelImage& image) {
            for (int i = 0; i < frames; ++i) {
                engine->frame(1.0 / 60.0);
            }
            image.pixels.clear();
            const bool read = backend->readColorBuffer(target.get(), 0, image.pixels);
            image.width     = width;
            image.height    = height;
            return read;
        };
        PixelImage shadowed;
        PixelImage plain;
        sun->setCastShadow(true);
        if (!render(shadowed)) {
            std::fprintf(stderr, "[selftest] FAIL: the lit-face phase could not read its shadowed frame\n");
            ok = false;
            break;
        }
        sun->setCastShadow(false); // the same scene, the same camera: only the shadow term changes
        if (!render(plain)) {
            std::fprintf(stderr, "[selftest] FAIL: the lit-face phase could not read its unshadowed frame\n");
            ok = false;
            break;
        }
        sun->setCastShadow(true);

        int       plain_up_sun     = 0;
        std::fprintf(stderr, "[selftest] lit-face vantage %d: top face:", v);
        for (int ix = 0; ix < 3; ++ix) {
            for (int iz = 0; iz < 3; ++iz) {
                const vine::math::Vec3d top_point(top_uvs[ix], 0.4, top_uvs[iz]);
                const PixelImage::Point p       = pixelOfWorld(*camera, top_point, width, height);
                const int               lit_sum = pixelSum(plain, p);
                const int               sh_sum  = pixelSum(shadowed, p);
                std::fprintf(stderr, " [%d,%d] %d->%d", ix, iz, lit_sum, sh_sum);
                // The face is lit and must stay so: the sun reaches it and nothing here occludes it.
                if (lit_sum < 450) {
                    std::fprintf(stderr,
                                 "\n[selftest] FAIL: the box's top face read %d (of 765) with the shadow OFF - the "
                                 "probe is not on a lit surface, so this vantage measures nothing\n",
                                 lit_sum);
                    ok = false;
                } else if (lit_sum - sh_sum > 24) {
                    std::fprintf(stderr,
                                 "\n[selftest] FAIL: the box's top face darkened from %d to %d when the shadow was "
                                 "switched on: a surface the sun reaches, which nothing here can occlude, was "
                                 "shadowed - the consuming pass is reading a map that is not the sun's\n",
                                 lit_sum, sh_sum);
                    ok = false;
                }
            }
        }
        std::fprintf(stderr, "\n");
        // The fill-only face: the sun cannot reach it (so it is in the box's own sun shadow) and the
        // fill has no map of its own, so its brightness must not move when the SUN's shadow is toggled.
        std::fprintf(stderr, "[selftest]   fill-only face:");
        for (int iy = 0; iy < 3; ++iy) {
            for (int ix = 0; ix < 3; ++ix) {
                const vine::math::Vec3d face_point(top_uvs[ix], 0.12 + 0.08 * static_cast<double>(iy), 0.25);
                const PixelImage::Point p       = pixelOfWorld(*camera, face_point, width, height);
                const int               lit_sum = pixelSum(plain, p);
                const int               sh_sum  = pixelSum(shadowed, p);
                std::fprintf(stderr, " [%d,%d] %d->%d", ix, iy, lit_sum, sh_sum);
                if (lit_sum < 200) {
                    std::fprintf(stderr,
                                 "\n[selftest] FAIL: the box's fill-lit face read %d with the shadow OFF - the probe "
                                 "is not on the face the fill light reaches, so this vantage measures nothing\n",
                                 lit_sum);
                    ok = false;
                } else if (lit_sum - sh_sum > 24) {
                    std::fprintf(stderr,
                                 "\n[selftest] FAIL: the box's FILL-lit face darkened from %d to %d when the SUN's "
                                 "shadow was switched on: the map of one light is being applied to another, so a "
                                 "face the sun never reaches goes dark in the sun's shadow\n",
                                 lit_sum, sh_sum);
                    ok = false;
                }
            }
        }
        std::fprintf(stderr, "\n");
        // Control A, geometry-free: the shadow term has to darken the picture somewhere, or this phase
        // would pass on a shadow that does nothing. Counting changed pixels needs no hand-computed
        // shadow position, which is unreliable at this content scale: the demo's 0.005 bias is a
        // 0.13-unit shift along the light here, so most of a small caster's ground shadow is eroded
        // away, and WHERE the surviving part falls is the renderer's business.
        std::size_t darkened = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const PixelImage::Point p{ x, y };
                if (pixelSum(plain, p) > pixelSum(shadowed, p) + 30) {
                    ++darkened;
                }
            }
        }
        if (darkened < 50) {
            std::fprintf(stderr,
                         "[selftest] FAIL: switching the shadow on darkened only %zu pixels - the shadow term did "
                         "not reach the picture, so this phase proves nothing\n",
                         darkened);
            ok = false;
        }
        // Control B: ground the sun reaches, up-sun of the box, must not darken at all.
        for (const auto& point : ground_up_sun) {
            const PixelImage::Point p       = pixelOfWorld(*camera, point, width, height);
            const int               lit_sum = pixelSum(plain, p);
            const int               sh_sum  = pixelSum(shadowed, p);
            plain_up_sun                    = std::max(plain_up_sun, lit_sum);
            if (lit_sum - sh_sum > 24) {
                std::fprintf(stderr,
                             "[selftest] FAIL: ground the sun reaches (%.2f,%.2f) darkened from %d to %d when the "
                             "shadow was switched on - the shadow landed where it cannot belong\n",
                             point.x, point.z, lit_sum, sh_sum);
                ok = false;
            }
        }
        std::fprintf(stderr, "[selftest]   up-sun ground lit %d, darkened pixels %zu\n", plain_up_sun, darkened);
        pipeline = nullptr; // unregisters its passes from the engine before the next vantage
    }

    engine->shutdown();
    return ok;
}

}  // namespace selftest
