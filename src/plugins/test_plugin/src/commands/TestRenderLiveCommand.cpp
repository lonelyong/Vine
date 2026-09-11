#include "TestRenderLiveCommand.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cmath>

#include <vine/Colorf.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/geometry/Array.hpp>
#include <vine/math/Transform3.hpp>

V_APPFW_NS_BEGIN

namespace
{

using vine::intrusive_ptr;
using vine::math::Vec3d;
using vine::math::Vec3f;

/**
 * @brief Adds a unit triangle at a world-space position to the scene.
 *
 * Attaches the triangle under the scene's root group, creating an identity
 * root Group on the scene when none is set yet.
 *
 * @param scene   Target scene.
 * @param diffuse Diffuse colour of the triangle.
 * @param name    Name of the node and geometry.
 * @param at      World-space position.
 * @return The created node (kept alive by the scene).
 */
intrusive_ptr<vine::graphics::MatrixTransform> addDemoTriangle(vine::graphics::Scene* scene,
                                                              const vine::Colorf& diffuse,
                                                              const vine::String& name,
                                                              const Vec3d& at)
{
    // One triangle in the XY plane, authored directly as positions.
    const vine::geometry::Vec3fArray positions = { Vec3f(-1.0f, -1.0f, 0.0f),
                                                   Vec3f(1.0f, -1.0f, 0.0f),
                                                   Vec3f(0.0f, 1.0f, 0.0f) };

    auto geometry = make_intrusive<vine::graphics::Geometry>();
    geometry->setName(name);
    geometry->setPositions(positions);

    auto material = make_intrusive<vine::graphics::Material>();
    material->setDiffuse(diffuse);
    geometry->setMaterial(material);

    auto node = intrusive_ptr<vine::graphics::MatrixTransform>(
        new vine::graphics::MatrixTransform());
    node->setName(name);
    node->setMatrix(vine::math::translate(at));
    node->addChild(geometry);

    vine::graphics::Group* root = dynamic_cast<vine::graphics::Group*>(scene->root().get());
    if (root == nullptr) {
        auto group = make_intrusive<vine::graphics::Group>();
        scene->setRoot(group);
        root = group.get();
    }
    root->addChild(node);
    return node;
}

}  // namespace

V_OBJECT_META_IMPL(TestRenderLiveCommand, Command)

vine::async::Task<CommandResult> TestRenderLiveCommand::execute(CommandExecutionContext* context)
{
    auto* gui_app =
        vine::obj_cast<vine::appfw::gui::GuiApplication>(context ? context->application() : nullptr);
    auto* rc = gui_app ? gui_app->mainWindow()->primaryRenderControl() : nullptr;
    if (rc == nullptr) {
        co_return CommandResult(CommandStatus::Failed, String(u8"未找到主渲染视图"));
    }

    // Single running demo per process.
    static QTimer* s_timer = nullptr;
    if (s_timer != nullptr && s_timer->isActive()) {
        co_return CommandResult(CommandStatus::Success, String(u8"演示已在运行"));
    }

    // Keep an owning reference so the content outlives the passes binding it.
    auto scene = rc->view()->scene();
    auto base  = addDemoTriangle(scene.get(), vine::Colorf(0.8f, 0.2f, 0.2f, 1.0f), u8"live_base",
                                 Vec3d(0.0, 0.0, -4.0));
    auto mover = addDemoTriangle(scene.get(), vine::Colorf(0.2f, 0.4f, 0.9f, 1.0f), u8"live_mover",
                                 Vec3d(3.0, 0.0, -6.0));

    // Each demo triangle lives under its own MatrixTransform as one leaf
    // Geometry child; grab the leaf for the visibility blink below.
    auto* mover_geom = dynamic_cast<vine::graphics::Geometry*>(mover->children().front().get());
    auto* base_mat = dynamic_cast<vine::graphics::Geometry*>(base->children().front().get())
                         ->material();

    s_timer = new QTimer(QCoreApplication::instance());
    int tick = 0;
    QObject::connect(s_timer, &QTimer::timeout, [=]() mutable {
        ++tick;
        const double t = tick * 0.05;

        // Node transform updates live (matrix path).
        mover->setMatrix(
            vine::math::translate(Vec3d(3.0 * std::cos(t), 1.2 * std::sin(t), -6.0)));

        // Node-level opacity oscillates (per-vertex alpha path).
        mover->setOpacity(0.2f + 0.8f * static_cast<float>(0.5 + 0.5 * std::sin(t)));

        // Geometry-level visibility blinks (child attach/detach path).
        mover_geom->setVisible((tick / 24) % 2 == 0);

        // Material diffuse cycles through hues (shared Phong uniform path).
        const float c = static_cast<float>(t);
        base_mat->setDiffuse(vine::Colorf(0.5f + 0.5f * std::sin(c),
                                          0.5f + 0.5f * std::sin(c + 2.1f),
                                          0.5f + 0.5f * std::sin(c + 4.2f), 1.0f));

        rc->renderFrame();
    });
    s_timer->start(33);

    co_return CommandResult(CommandStatus::Success, String(u8"实时渲染演示已启动"));
}

V_APPFW_NS_END
