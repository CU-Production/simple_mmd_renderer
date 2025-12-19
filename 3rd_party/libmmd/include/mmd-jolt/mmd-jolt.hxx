
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

// Standard library includes
#include <cmath>
#include <algorithm>

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
#include <Jolt/Physics/Collision/ObjectLayerPairFilterMask.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>

namespace mmd {

// MMD uses 16 collision groups (0-15) with a 16-bit collision mask
// We configure Jolt with 32-bit ObjectLayer (OBJECT_LAYER_BITS=32)
// This gives us 16 bits for group and 16 bits for mask - perfect for MMD!

// Broadphase layer definitions
namespace JoltBroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);  // Static objects (ground)
    static constexpr JPH::BroadPhaseLayer MOVING(1);       // Dynamic/kinematic MMD bodies
    static constexpr uint32_t NUM_LAYERS = 2;
};

// Helper to create MMD collision ObjectLayer from group index and mask
// group: 0-15 (MMD collision group index)
// mask: 16-bit mask specifying which groups this body collides with
inline JPH::ObjectLayer MakeMMDObjectLayer(uint32_t group, uint32_t mask) {
    // Convert group index (0-15) to group bit (1 << group)
    uint32_t group_bit = (1u << group) & JPH::ObjectLayerPairFilterMask::cMask;
    uint32_t mask_bits = mask & JPH::ObjectLayerPairFilterMask::cMask;
    return JPH::ObjectLayerPairFilterMask::sGetObjectLayer(group_bit, mask_bits);
}

// Static ground layer - collides with all moving objects
inline JPH::ObjectLayer GetGroundObjectLayer() {
    // Ground has no group bit but collides with all groups (mask = all 1s)
    return JPH::ObjectLayerPairFilterMask::sGetObjectLayer(0, JPH::ObjectLayerPairFilterMask::cMask);
}

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
    std::unique_ptr<JPH::BroadPhaseLayerInterfaceMask> broad_phase_layer_interface_;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterMask> object_vs_broadphase_layer_filter_;
    std::unique_ptr<JPH::ObjectLayerPairFilterMask> object_layer_pair_filter_;
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
