
/**
             Copyright itsuhane@gmail.com, 2012.
  Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)
**/

/**
  This is a reference implementation demonstrating how to bind libmmd
  with Jolt Physics. Its behavior may differ from MikuMikuDance.

  To get more control over physics manipulation, you may need to implement
  your own physics binding.

  Reference:
    MMD Model Physics Setup Wiki: http://www10.atwiki.jp/mmdphysics/
    Jolt Physics: https://github.com/jrouwe/JoltPhysics
**/
#ifndef __MMD_JOLT_HXX_5912EA0C3602E47B50077FCA6298F8AC_INCLUDED__
#define __MMD_JOLT_HXX_5912EA0C3602E47B50077FCA6298F8AC_INCLUDED__

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4100 4189 4514 4571 4710 4819 4820 4996 )
#endif

// Jolt Physics includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>

namespace mmd {

// Layer definitions for Jolt
namespace JoltLayers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
};

namespace JoltBroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_LAYERS = 2;
};

// BroadPhaseLayerInterface implementation
class JoltBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    JoltBroadPhaseLayerInterface()
    {
        mObjectToBroadPhase[JoltLayers::NON_MOVING] = JoltBroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[JoltLayers::MOVING] = JoltBroadPhaseLayers::MOVING;
    }

    virtual JPH::uint GetNumBroadPhaseLayers() const override
    {
        return JoltBroadPhaseLayers::NUM_LAYERS;
    }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        JPH_ASSERT(inLayer < JoltLayers::NUM_LAYERS);
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        switch ((JPH::BroadPhaseLayer::Type)inLayer)
        {
        case (JPH::BroadPhaseLayer::Type)JoltBroadPhaseLayers::NON_MOVING: return "NON_MOVING";
        case (JPH::BroadPhaseLayer::Type)JoltBroadPhaseLayers::MOVING: return "MOVING";
        default: JPH_ASSERT(false); return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[JoltLayers::NUM_LAYERS];
};

// ObjectVsBroadPhaseLayerFilter implementation
class JoltObjectVsBroadPhaseLayerFilter : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case JoltLayers::NON_MOVING:
            return inLayer2 == JoltBroadPhaseLayers::MOVING;
        case JoltLayers::MOVING:
            return true;
        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

// ObjectLayerPairFilter implementation
class JoltObjectLayerPairFilter : public JPH::ObjectLayerPairFilter
{
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
    {
        switch (inObject1)
        {
        case JoltLayers::NON_MOVING:
            return inObject2 == JoltLayers::MOVING;
        case JoltLayers::MOVING:
            return true;
        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

class JoltPhysicsReactor : public PhysicsReactor {
public:
    // Structure to track bone transform state for physics synchronization
    struct BoneMotionState {
        Poser* poser;
        size_t bone_index;
        bool passive;       // kinematics only
        bool strict;        // bones are not allowed to shake its length
        bool ghost;         // bones do not affect bone
        JoltPhysicsReactor::BoneImageReference target;
        JPH::RMat44 body_transform;
        JPH::RMat44 body_transform_inv;
        JPH::BodyID body_id;
        
        BoneMotionState(Poser& p, const Model::RigidBody& body, const JPH::RMat44& bt, JPH::BodyID bid);
        void Synchronize(JPH::PhysicsSystem* physics_system);
        void Fix();
        JPH::RMat44 GetWorldTransform() const;
        void Reset(JPH::PhysicsSystem* physics_system);
    };

    JoltPhysicsReactor();
    virtual ~JoltPhysicsReactor();

    /*virtual*/ void AddPoser(Poser &poser);
    /*virtual*/ void RemovePoser(Poser &poser);
    /*virtual*/ void Reset();
    /*virtual*/ void React(float step);

    /*virtual*/ void SetGravityStrength(float strength);
    /*virtual*/ void SetGravityDirection(const Vector3f &direction);

    /*virtual*/ float GetGravityStrength() const;
    /*virtual*/ Vector3f GetGravityDirection() const;

    /*virtual*/ void SetFloor(bool has_floor);
    /*virtual*/ bool IsHasFloor() const;

private:
    // Helper to convert MMD Matrix4f to Jolt RMat44
    static JPH::RMat44 Matrix4fToRMat44(const Matrix4f& m);
    // Helper to convert Jolt RMat44 to MMD Matrix4f
    static void RMat44ToMatrix4f(const JPH::RMat44& src, Matrix4f& dst);

    // Jolt Physics system components
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
    std::unique_ptr<JoltBroadPhaseLayerInterface> broad_phase_layer_interface_;
    std::unique_ptr<JoltObjectVsBroadPhaseLayerFilter> object_vs_broadphase_layer_filter_;
    std::unique_ptr<JoltObjectLayerPairFilter> object_layer_pair_filter_;
    std::unique_ptr<JPH::PhysicsSystem> physics_system_;

    // Ground plane
    JPH::BodyID ground_body_id_;
    bool has_floor_;

    // Gravity
    JPH::Vec3 gravity_direction_;
    float gravity_strength_;

    // Per-poser data
    std::map<Poser*, std::vector<JPH::Ref<JPH::Shape>>> collision_shapes_;
    std::map<Poser*, std::vector<std::unique_ptr<BoneMotionState>>> motion_states_;
    std::map<Poser*, std::vector<JPH::BodyID>> body_ids_;
    std::map<Poser*, std::vector<JPH::Ref<JPH::Constraint>>> constraints_;
};

#include "mmd-jolt_impl.inl"
} /* End of namespace mmd */

#ifdef _MSC_VER
#pragma warning( pop )
#endif

#endif /* __MMD_JOLT_HXX_5912EA0C3602E47B50077FCA6298F8AC_INCLUDED__ */
