#include <vine/vsg/CameraBridge.hpp>

#include <vsg/app/ProjectionMatrix.h>
#include <vsg/app/ViewMatrix.h>
#include <vsg/maths/vec3.h>

#include <vine/graphics/Camera.hpp>

V_VSG_NS_BEGIN

::vsg::ref_ptr<::vsg::Camera> CameraBridge::create(vine::raw_ptr<const vine::graphics::Camera> camera)
{
    auto view = ::vsg::LookAt::create();
    auto projection = ::vsg::ref_ptr<::vsg::ProjectionMatrix>();
    auto vsgCamera = ::vsg::Camera::create(projection, view);
    apply(camera, vsgCamera);
    return vsgCamera;
}

void CameraBridge::apply(vine::raw_ptr<const vine::graphics::Camera> camera, ::vsg::ref_ptr<::vsg::Camera> vsgCamera)
{
    if (camera == nullptr || vsgCamera == nullptr) {
        return;
    }

    // View: mirror the Vine look-at parameters into a vsg::LookAt.
    if (auto lookAt = vsgCamera->viewMatrix.cast<::vsg::LookAt>()) {
        const vine::math::Vec3d eye = camera->eye();
        const vine::math::Vec3d center = camera->target();
        const vine::math::Vec3d up = camera->up();
        lookAt->eye = ::vsg::dvec3(eye.x, eye.y, eye.z);
        lookAt->center = ::vsg::dvec3(center.x, center.y, center.z);
        lookAt->up = ::vsg::dvec3(up.x, up.y, up.z);
    }

    // Projection: mirror the Vine projection parameters.
    if (camera->projectionType() == vine::graphics::Camera::ProjectionType::Orthographic) {
        auto ortho = vsgCamera->projectionMatrix.cast<::vsg::Orthographic>();
        if (ortho == nullptr) {
            ortho = ::vsg::Orthographic::create();
            vsgCamera->projectionMatrix = ortho;
        }
        const double half_h = camera->orthographicHeight() * 0.5;
        const double half_w = half_h * camera->aspectRatio();
        ortho->left = -half_w;
        ortho->right = half_w;
        ortho->bottom = -half_h;
        ortho->top = half_h;
        ortho->nearDistance = camera->nearPlane();
        ortho->farDistance = camera->farPlane();
    }
    else {
        auto persp = vsgCamera->projectionMatrix.cast<::vsg::Perspective>();
        if (persp == nullptr) {
            persp = ::vsg::Perspective::create();
            vsgCamera->projectionMatrix = persp;
        }
        persp->fieldOfViewY = camera->fieldOfView();  // degrees, VSG converts internally
        persp->aspectRatio = camera->aspectRatio();
        persp->nearDistance = camera->nearPlane();
        persp->farDistance = camera->farPlane();
    }
}

V_VSG_NS_END
