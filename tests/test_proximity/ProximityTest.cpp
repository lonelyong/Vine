#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include <vine/geometry/Sphere.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/State.hpp>
#include <vine/robotics/proximity/CollisionContact.hpp>
#include <vine/robotics/proximity/CollisionDetector.hpp>
#include <vine/robotics/proximity/CollisionGeometry.hpp>
#include <vine/robotics/proximity/CollisionGeometryManager.hpp>
#include <vine/robotics/proximity/CollisionMatrix.hpp>
#include <vine/robotics/proximity/CollisionObject.hpp>
#include <vine/robotics/proximity/CollisionPair.hpp>
#include <vine/robotics/proximity/CollisionRequest.hpp>
#include <vine/robotics/proximity/CollisionResult.hpp>

namespace
{

using namespace vn::robotics::proximity;

/// Mock geometry backend; the real one is FCL-backed and skipped here.
class MockGeometry : public CollisionGeometry
{
  public:
    bool buildFromShape(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) override
    {
        valid_ = shape != nullptr && shape->isValid();
        return valid_;
    }

    bool isValid() const override
    {
        return valid_;
    }

  private:
    bool valid_{ false };
};

/// Mock collision object; a real backend overrides computeWorldTransform().
class MockCollisionObject : public CollisionObject
{
  public:
    explicit MockCollisionObject(const vn::intrusive_ptr<CollisionGeometry>& geometry)
      : CollisionObject(geometry)
    {
    }

    bool isValid() const override
    {
        return geometry() != nullptr && geometry()->isValid();
    }
};

/// Mock detector; a real backend implements the do* hooks with FCL.
class MockDetector : public CollisionDetector
{
  protected:
    bool doAddCollisionObject(CollisionObject*) override
    {
        return true;
    }

    bool doRemoveCollisionObject(CollisionObject*) override
    {
        return true;
    }

    bool doRebuild() override
    {
        return true;
    }

    void doEndUpdate() override
    {
    }

    CollisionResult doCheckCollision(const CollisionRequest&) const override
    {
        return {};
    }

    CollisionResult doCheckCollision(const CollisionRequest&, const vn::robotics::kinematics::State&) const override
    {
        return {};
    }

    CollisionResult doCheckCollision(const CollisionDetector&, const CollisionRequest&,
                                     const vn::robotics::kinematics::State&) const override
    {
        return {};
    }
};

/// Mock geometry manager; the real one is FCL-backed and skipped here.
class MockGeometryManager : public CollisionGeometryManager
{
  public:
    vn::intrusive_ptr<CollisionGeometry>
        createCollisionGeometry(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) const override
    {
        vn::intrusive_ptr<CollisionGeometry> geometry(new MockGeometry());
        geometry->buildFromShape(shape);
        return geometry;
    }

    vn::intrusive_ptr<CollisionObject>
        createCollisionObject(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) const override
    {
        const auto geometry = get(shape);
        if (geometry == nullptr) {
            return {};
        }
        return vn::intrusive_ptr<CollisionObject>(new MockCollisionObject(geometry));
    }
};

/// Returns a fixed frame with a pure translation.
vn::math::Isometry3d translated(const vn::math::Vec3d& offset)
{
    vn::math::Isometry3d tf;
    tf.postTranslate(offset);
    return tf;
}

} // namespace

TEST(CollisionPairTest, OrderIndependentEqualityAndHash)
{
    vn::robotics::kinematics::Frame a;
    a.setName(vn::String(u8"a"));
    vn::robotics::kinematics::Frame b;
    b.setName(vn::String(u8"b"));

    const CollisionPair p1{ &a, &b };
    const CollisionPair p2{ &b, &a };

    EXPECT_TRUE(p1 == p2);
    EXPECT_FALSE(p1 != p2);
    EXPECT_EQ(CollisionPairHasher{}(p1), CollisionPairHasher{}(p2));

    const CollisionPair p3{ &a, &a };
    EXPECT_FALSE(p1 == p3);
}

TEST(CollisionMatrixTest, RegisterAndMinDistance)
{
    CollisionMatrix                   matrix;
    vn::robotics::kinematics::Frame a;
    a.setName(vn::String(u8"a"));
    vn::robotics::kinematics::Frame b;
    b.setName(vn::String(u8"b"));
    vn::robotics::kinematics::Frame c;
    c.setName(vn::String(u8"c"));

    matrix.registerObject(&a);
    matrix.registerObject(&b);

    EXPECT_TRUE(matrix.containsObject(&a));
    EXPECT_TRUE(matrix.containsObject(&b));
    EXPECT_FALSE(matrix.containsObject(&c));

    matrix.setMinDistance(&a, &b, 5.0);
    EXPECT_DOUBLE_EQ(matrix.minDistance(&a, &b), 5.0);
    EXPECT_DOUBLE_EQ(matrix.minDistance(&b, &a), 5.0);

    // Sentinel values.
    EXPECT_DOUBLE_EQ(matrix.minDistance(&a, &a), -1.0);
    EXPECT_DOUBLE_EQ(matrix.minDistance(&a, &c), -2.0);

    // Registered owners sorted by name.
    const auto objects = matrix.registeredObjects();
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_EQ(objects[0], &a);
    EXPECT_EQ(objects[1], &b);

    EXPECT_THROW(matrix.registerObject(&a), std::logic_error);
    EXPECT_THROW(matrix.setMinDistance(&a, &c, 1.0), std::logic_error);
    EXPECT_THROW(matrix.setMinDistance(&a, &a, 1.0), std::logic_error);
}

TEST(CollisionMatrixTest, IgnoreAndShouldCheck)
{
    CollisionMatrix                   matrix;
    vn::robotics::kinematics::Frame a;
    a.setName(vn::String(u8"a"));
    vn::robotics::kinematics::Frame b;
    b.setName(vn::String(u8"b"));
    vn::robotics::kinematics::Frame c;
    c.setName(vn::String(u8"c"));

    matrix.registerObject(&a);
    matrix.registerObject(&b);

    EXPECT_TRUE(matrix.shouldCheckCollision(&a, &b));

    matrix.setIgnored(&a, &b, true);
    EXPECT_TRUE(matrix.isIgnored(&a, &b));
    EXPECT_FALSE(matrix.shouldCheckCollision(&a, &b));
    // Unregistered owners are not ignored, so the pair is still checked.
    EXPECT_TRUE(matrix.shouldCheckCollision(&b, &c));

    matrix.setIgnored(&a, &b, false);
    EXPECT_FALSE(matrix.isIgnored(&a, &b));
    EXPECT_TRUE(matrix.shouldCheckCollision(&a, &b));

    // Same owner / unregistered owner.
    EXPECT_TRUE(matrix.isIgnored(&a, &a));
    EXPECT_FALSE(matrix.isIgnored(&a, &c));
    EXPECT_FALSE(matrix.shouldCheckCollision(&a, &a));

    matrix.ignoreAgainstAll(&a);
    EXPECT_TRUE(matrix.isIgnored(&a, &b));
    EXPECT_FALSE(matrix.shouldCheckCollision(&a, &b));
    EXPECT_TRUE(matrix.shouldCheckCollision(&b, &a) == false);
}

TEST(CollisionMatrixTest, CopyOptionsAndUnregister)
{
    CollisionMatrix                   source;
    CollisionMatrix                   target;
    vn::robotics::kinematics::Frame a;
    a.setName(vn::String(u8"a"));
    vn::robotics::kinematics::Frame b;
    b.setName(vn::String(u8"b"));

    source.registerObject(&a);
    source.registerObject(&b);
    target.registerObject(&a);
    target.registerObject(&b);

    source.setMinDistance(&a, &b, 7.5);
    source.setIgnored(&a, &b, true);

    target.copyOptionsFrom(source);
    const auto options = target.collisionOptions(&a, &b);
    ASSERT_TRUE(options.has_value());
    EXPECT_DOUBLE_EQ(options->min_dist, 7.5);
    EXPECT_TRUE(options->ignored);

    target.unregisterObject(&a);
    EXPECT_FALSE(target.containsObject(&a));
    EXPECT_DOUBLE_EQ(target.minDistance(&a, &b), -2.0);
    EXPECT_FALSE(target.collisionOptions(&a, &b).has_value());
}

TEST(CollisionRequestTest, Defaults)
{
    CollisionRequest request;
    EXPECT_TRUE(request.compute_contact_details);
    EXPECT_FALSE(request.stop_at_first_contact);
    EXPECT_EQ(request.max_contacts_per_pair, 1u);
    EXPECT_FALSE(request.enable_parallel);
    EXPECT_EQ(request.collision_matrix, nullptr);
}

TEST(CollisionResultTest, Basic)
{
    CollisionResult result;
    EXPECT_FALSE(result.hasCollision());

    vn::robotics::kinematics::Frame a;
    a.setName(vn::String(u8"a"));
    vn::robotics::kinematics::Frame b;
    b.setName(vn::String(u8"b"));

    result.pairs[CollisionPair{ &a, &b }].push_back(CollisionContact{});
    EXPECT_TRUE(result.hasCollision());

    CollisionResult other;
    EXPECT_FALSE(result.hasSameCollisionPairsAs(other));

    other.pairs[CollisionPair{ &b, &a }].push_back(CollisionContact{});
    EXPECT_TRUE(result.hasSameCollisionPairsAs(other));

    std::ostringstream os;
    result.print(os);
    EXPECT_FALSE(os.str().empty());
}

TEST(ProximityTest, CollisionObjectWorldTransform)
{
    // root --(offset)--> joint
    vn::robotics::kinematics::Frame root;
    root.setName(vn::String(u8"root"));
    root.setFixedTransform(translated(vn::math::Vec3d(1.0, 0.0, 0.0)));
    vn::robotics::kinematics::Frame joint;
    joint.setName(vn::String(u8"joint"));
    joint.setFixedTransform(translated(vn::math::Vec3d(0.0, 2.0, 0.0)));
    root.addChild(&joint);

    vn::robotics::kinematics::State state;
    state.setup(&root);

    vn::intrusive_ptr<CollisionGeometry> geometry(new MockGeometry());
    geometry->buildFromShape(vn::intrusive_ptr<const vn::geometry::Shape>(new vn::geometry::Sphere(0.5)));

    vn::intrusive_ptr<CollisionObject> object(new MockCollisionObject(geometry));
    object->setFrame(&joint);
    object->setLocalTransform(translated(vn::math::Vec3d(0.0, 0.0, 3.0)));

    object->computeWorldTransform(state);
    const auto expected = vn::robotics::kinematics::Frame::frameInWorld(&joint, state)
                          * object->localTransform();
    EXPECT_EQ(object->worldTransform().translation.x, expected.translation.x);
    EXPECT_EQ(object->worldTransform().translation.y, expected.translation.y);
    EXPECT_EQ(object->worldTransform().translation.z, expected.translation.z);

    // Without a frame the local pose is the world pose.
    object->setFrame(nullptr);
    object->computeWorldTransform(state);
    EXPECT_EQ(object->worldTransform().translation.x, object->localTransform().translation.x);
}

TEST(ProximityTest, CollisionGeometryManager)
{
    MockGeometryManager manager;
    const auto          shape = vn::intrusive_ptr<const vn::geometry::Shape>(new vn::geometry::Sphere(1.0));

    EXPECT_TRUE(manager.add(shape));
    EXPECT_TRUE(manager.add(shape)); // cached
    EXPECT_NE(manager.get(shape), nullptr);

    const auto missing = vn::intrusive_ptr<const vn::geometry::Shape>(new vn::geometry::Sphere(2.0));
    EXPECT_EQ(manager.get(missing), nullptr);

    EXPECT_TRUE(manager.update(shape));
    const auto object = manager.createCollisionObject(shape);
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(object->isValid());

    EXPECT_TRUE(manager.remove(shape));
    EXPECT_EQ(manager.get(shape), nullptr);
    EXPECT_EQ(manager.createCollisionObject(shape), nullptr);
}

TEST(ProximityTest, DetectorBookkeeping)
{
    MockDetector detector;
    vn::robotics::kinematics::Frame owner;
    owner.setName(vn::String(u8"owner"));
    vn::robotics::kinematics::State state;
    state.setup(&owner);

    vn::intrusive_ptr<CollisionGeometry> geometry(new MockGeometry());
    geometry->buildFromShape(vn::intrusive_ptr<const vn::geometry::Shape>(new vn::geometry::Sphere(0.5)));
    vn::intrusive_ptr<CollisionObject> object(new MockCollisionObject(geometry));
    object->setFrame(&owner);

    detector.beginUpdate();
    detector.addObject(&owner, { object });
    detector.endUpdate();

    detector.updateObjectTransform(&owner, state);
    const CollisionResult result = detector.checkCollision(CollisionRequest{});
    EXPECT_FALSE(result.hasCollision());

    // Duplicate registration is a no-op.
    detector.addObject(&owner, { object });
    detector.removeObject(&owner);
    detector.clear();
}
