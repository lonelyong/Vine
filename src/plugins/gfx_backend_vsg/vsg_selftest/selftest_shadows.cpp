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

}  // namespace selftest
