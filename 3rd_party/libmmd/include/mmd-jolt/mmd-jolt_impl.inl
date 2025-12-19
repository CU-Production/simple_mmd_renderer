
/**
             Copyright itsuhane@gmail.com, 2012.
  Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)
**/

// ============================================================================
// BoneMotionState implementation
// ============================================================================

inline JoltPhysicsReactor::BoneMotionState::BoneMotionState(
    Poser& p, const Model::RigidBody& body, const JPH::RMat44& bt, JPH::BodyID bid)
    : poser(&p),
      bone_index(body.GetAssociatedBoneIndex()),
      passive(body.GetType() == Model::RigidBody::RIGID_TYPE_KINEMATIC),
      strict(body.GetType() == Model::RigidBody::RIGID_TYPE_PHYSICS_STRICT),
      ghost(body.GetType() == Model::RigidBody::RIGID_TYPE_PHYSICS_GHOST),
      target(JoltPhysicsReactor::GetPoserBoneImage(p, body.GetAssociatedBoneIndex())),
      body_transform(bt),
      body_transform_inv(bt.Inversed()),
      body_id(bid)
{
}

inline JPH::RMat44 JoltPhysicsReactor::BoneMotionState::GetWorldTransform() const {
    JPH::RMat44 bone_matrix = JoltPhysicsReactor::Matrix4fToRMat44(target.skinning_matrix_);
    return bone_matrix * body_transform;
}

inline void JoltPhysicsReactor::BoneMotionState::Synchronize(JPH::PhysicsSystem* physics_system) {
    if (!(passive || ghost)) {
        JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();
        JPH::RMat44 world_transform = body_interface.GetWorldTransform(body_id);
        JPH::RMat44 bone_transform = world_transform * body_transform_inv;
        JoltPhysicsReactor::RMat44ToMatrix4f(bone_transform, target.skinning_matrix_);
    }
}

inline void JoltPhysicsReactor::BoneMotionState::Fix() {
    if (strict) {
        Matrix4f parent_local_matrix;
        target.local_matrix_ = target.global_offset_matrix_inv_ * target.skinning_matrix_;
        if (target.has_parent_) {
            parent_local_matrix = JoltPhysicsReactor::GetPoserBoneImage(*poser, target.parent_).local_matrix_;
            target.local_matrix_ = target.local_matrix_ * parent_local_matrix.Inverse();
        }
        target.local_matrix_.r.v[3].downgrade.vector3d = target.total_translation_ + target.local_offset_;
        if (target.has_parent_) {
            target.local_matrix_ = target.local_matrix_ * parent_local_matrix;
        }
        target.skinning_matrix_ = target.global_offset_matrix_ * target.local_matrix_;
    }
}

inline void JoltPhysicsReactor::BoneMotionState::Reset(JPH::PhysicsSystem* physics_system) {
    JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();
    JPH::RMat44 new_transform = GetWorldTransform();
    body_interface.SetPositionAndRotation(
        body_id,
        new_transform.GetTranslation(),
        new_transform.GetQuaternion(),
        JPH::EActivation::Activate
    );
    body_interface.SetLinearVelocity(body_id, JPH::Vec3::sZero());
    body_interface.SetAngularVelocity(body_id, JPH::Vec3::sZero());
}

// ============================================================================
// JoltPhysicsReactor implementation
// ============================================================================

inline JPH::RMat44 JoltPhysicsReactor::Matrix4fToRMat44(const Matrix4f& m) {
    // MMD uses column-major OpenGL style matrices
    // Jolt uses column vectors, so direct copy works
    return JPH::RMat44(
        JPH::Vec4(m.r.v[0].v[0], m.r.v[0].v[1], m.r.v[0].v[2], m.r.v[0].v[3]),
        JPH::Vec4(m.r.v[1].v[0], m.r.v[1].v[1], m.r.v[1].v[2], m.r.v[1].v[3]),
        JPH::Vec4(m.r.v[2].v[0], m.r.v[2].v[1], m.r.v[2].v[2], m.r.v[2].v[3]),
        JPH::Vec3(m.r.v[3].v[0], m.r.v[3].v[1], m.r.v[3].v[2])
    );
}

inline void JoltPhysicsReactor::RMat44ToMatrix4f(const JPH::RMat44& src, Matrix4f& dst) {
    JPH::Vec4 c0 = src.GetColumn4(0);
    JPH::Vec4 c1 = src.GetColumn4(1);
    JPH::Vec4 c2 = src.GetColumn4(2);
    JPH::Vec3 c3 = src.GetTranslation();
    
    dst.r.v[0].v[0] = c0.GetX(); dst.r.v[0].v[1] = c0.GetY(); dst.r.v[0].v[2] = c0.GetZ(); dst.r.v[0].v[3] = c0.GetW();
    dst.r.v[1].v[0] = c1.GetX(); dst.r.v[1].v[1] = c1.GetY(); dst.r.v[1].v[2] = c1.GetZ(); dst.r.v[1].v[3] = c1.GetW();
    dst.r.v[2].v[0] = c2.GetX(); dst.r.v[2].v[1] = c2.GetY(); dst.r.v[2].v[2] = c2.GetZ(); dst.r.v[2].v[3] = c2.GetW();
    dst.r.v[3].v[0] = c3.GetX(); dst.r.v[3].v[1] = c3.GetY(); dst.r.v[3].v[2] = c3.GetZ(); dst.r.v[3].v[3] = 1.0f;
}

inline JoltPhysicsReactor::JoltPhysicsReactor() 
    : has_floor_(true), gravity_strength_(9.8f)
{
    // Initialize Jolt (should be called once per application, but safe to call multiple times)
    JPH::RegisterDefaultAllocator();
    
    // Install trace and assert callbacks (optional, can be customized)
    // JPH::Trace = ...;
    // JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = ...;)
    
    // Create factory
    JPH::Factory::sInstance = new JPH::Factory();
    
    // Register all Jolt physics types
    JPH::RegisterTypes();
    
    // Create allocators
    temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(20 * 1024 * 1024); // 20 MB
    job_system_ = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, 
        JPH::cMaxPhysicsBarriers, 
        std::thread::hardware_concurrency() - 1
    );
    
    // Create layer interfaces for mask-based collision filtering (similar to Bullet)
    // This allows us to use MMD's collision group/mask system
    broad_phase_layer_interface_ = std::make_unique<JPH::BroadPhaseLayerInterfaceMask>(JoltBroadPhaseLayers::NUM_LAYERS);
    // Configure broadphase layers:
    // Layer 0 (NON_MOVING): Static objects only (ground plane with group bit 0)
    broad_phase_layer_interface_->ConfigureLayer(JoltBroadPhaseLayers::NON_MOVING, 0, 0);
    // Layer 1 (MOVING): All moving objects (any group bits set)
    broad_phase_layer_interface_->ConfigureLayer(JoltBroadPhaseLayers::MOVING, 0xFF, 0);
    
    object_vs_broadphase_layer_filter_ = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterMask>(*broad_phase_layer_interface_);
    object_layer_pair_filter_ = std::make_unique<JPH::ObjectLayerPairFilterMask>();
    
    // Create physics system
    const JPH::uint cMaxBodies = 65536;
    const JPH::uint cNumBodyMutexes = 0;  // Auto-detect
    const JPH::uint cMaxBodyPairs = 65536;
    const JPH::uint cMaxContactConstraints = 10240;
    
    physics_system_ = std::make_unique<JPH::PhysicsSystem>();
    physics_system_->Init(
        cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
        *broad_phase_layer_interface_,
        *object_vs_broadphase_layer_filter_,
        *object_layer_pair_filter_
    );
    
    // Configure physics settings for better stability (important for MMD cloth/hair simulation)
    JPH::PhysicsSettings settings = physics_system_->GetPhysicsSettings();
    settings.mNumVelocitySteps = 10;          // Increase solver iterations for stability (default: 10)
    settings.mNumPositionSteps = 2;           // Increase position iterations (default: 2)
    settings.mBaumgarte = 0.1f;               // Reduce Baumgarte stabilization for smoother motion (default: 0.2)
    settings.mSpeculativeContactDistance = 0.01f;  // Reduce speculative contacts
    settings.mPenetrationSlop = 0.01f;        // Reduce penetration allowance
    physics_system_->SetPhysicsSettings(settings);
    
    // Set gravity (scaled by 10 like in Bullet version, since MMD uses 0.1m as unit)
    gravity_direction_ = JPH::Vec3(0.0f, -1.0f, 0.0f);
    physics_system_->SetGravity(gravity_direction_ * gravity_strength_ * 10.0f);
    
    // Create ground plane
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    
    JPH::PlaneShapeSettings ground_shape_settings(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f));
    ground_shape_settings.SetEmbedded();
    
    // Ground uses special layer that collides with all groups
    JPH::BodyCreationSettings ground_settings(
        ground_shape_settings.Create().Get(),
        JPH::RVec3::sZero(),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static,
        GetGroundObjectLayer()  // Use mask-based layer for ground
    );
    ground_settings.mFriction = 0.265f;
    ground_settings.mRestitution = 0.0f;
    
    ground_body_id_ = body_interface.CreateAndAddBody(ground_settings, JPH::EActivation::DontActivate);
}

inline JoltPhysicsReactor::~JoltPhysicsReactor() {
    // Remove all posers first
    while (!body_ids_.empty()) {
        RemovePoser(*(body_ids_.begin()->first));
    }
    
    // Remove ground
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    // body_interface.RemoveBody(ground_body_id_);
    // body_interface.DestroyBody(ground_body_id_);
    
    // Cleanup in reverse order
    physics_system_.reset();
    object_layer_pair_filter_.reset();
    object_vs_broadphase_layer_filter_.reset();
    broad_phase_layer_interface_.reset();
    job_system_.reset();
    temp_allocator_.reset();
    
    // Unregister types and destroy factory
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

inline void JoltPhysicsReactor::SetGravityStrength(float strength) {
    gravity_strength_ = strength;
    physics_system_->SetGravity(gravity_direction_ * gravity_strength_ * 10.0f);
}

inline void JoltPhysicsReactor::SetGravityDirection(const Vector3f& direction) {
    Vector3f d = direction.Normalize();
    gravity_direction_ = JPH::Vec3(d.p.x, d.p.y, d.p.z);
    physics_system_->SetGravity(gravity_direction_ * gravity_strength_ * 10.0f);
}

inline float JoltPhysicsReactor::GetGravityStrength() const {
    return gravity_strength_;
}

inline Vector3f JoltPhysicsReactor::GetGravityDirection() const {
    Vector3f d;
    d.p.x = gravity_direction_.GetX();
    d.p.y = gravity_direction_.GetY();
    d.p.z = gravity_direction_.GetZ();
    return d;
}

inline void JoltPhysicsReactor::SetFloor(bool has_floor) {
    if (has_floor == has_floor_) {
        return;
    }
    has_floor_ = has_floor;
    
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    if (has_floor_) {
        body_interface.AddBody(ground_body_id_, JPH::EActivation::DontActivate);
    } else {
        body_interface.RemoveBody(ground_body_id_);
    }
}

inline bool JoltPhysicsReactor::IsHasFloor() const {
    return has_floor_;
}

inline void JoltPhysicsReactor::AddPoser(Poser& poser) {
    const Model& model = poser.GetModel();
    
    if (body_ids_.count(&poser) > 0) {
        return;
    }
    
    poser.ResetPosing();
    
    std::vector<JPH::Ref<JPH::Shape>>& collision_shapes = collision_shapes_[&poser];
    std::vector<std::unique_ptr<BoneMotionState>>& motion_states = motion_states_[&poser];
    std::vector<JPH::BodyID>& body_ids = body_ids_[&poser];
    std::vector<JPH::Ref<JPH::Constraint>>& constraints = constraints_[&poser];
    
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    
    // Create rigid bodies
    for (size_t i = 0; i < model.GetRigidBodyNum(); ++i) {
        const Model::RigidBody& body = model.GetRigidBody(i);
        const Vector3f& body_dimension = body.GetDimensions();
        
        // Create shape based on type
        JPH::Ref<JPH::Shape> shape;
        
        switch (body.GetShape()) {
        case Model::RigidBody::RIGID_SHAPE_SPHERE:
            shape = new JPH::SphereShape(body_dimension.p.x);
            break;
        case Model::RigidBody::RIGID_SHAPE_BOX:
            shape = new JPH::BoxShape(JPH::Vec3(body_dimension.p.x, body_dimension.p.y, body_dimension.p.z));
            break;
        case Model::RigidBody::RIGID_SHAPE_CAPSULE:
            // Jolt capsule uses half-height of cylinder part, Bullet uses full height
            shape = new JPH::CapsuleShape(body_dimension.p.y * 0.5f, body_dimension.p.x);
            break;
        }
        
        collision_shapes.push_back(shape);
        
        // Calculate body transform
        Matrix4f body_transform_mat = YXZToQuaternion(body.GetRotation()).ToRotateMatrix();
        body_transform_mat.r.v[3].downgrade.vector3d = body.GetPosition();
        JPH::RMat44 body_transform = Matrix4fToRMat44(body_transform_mat);
        
        // Determine motion type
        JPH::EMotionType motion_type;
        if (body.GetType() == Model::RigidBody::RIGID_TYPE_KINEMATIC) {
            motion_type = JPH::EMotionType::Kinematic;
        } else {
            motion_type = JPH::EMotionType::Dynamic;
        }
        
        // Create ObjectLayer from MMD collision group and mask
        // This is the KEY to preventing cloth/hair explosion!
        // Bodies only collide if (group1 & mask2) && (group2 & mask1)
        uint32_t collision_group = body.GetCollisionGroup();
        uint32_t collision_mask = static_cast<uint32_t>(body.GetCollisionMask().to_ulong());
        JPH::ObjectLayer object_layer = MakeMMDObjectLayer(collision_group, collision_mask);
        
        // Get initial position from bone
        BoneImageReference bone_image = GetPoserBoneImage(poser, body.GetAssociatedBoneIndex());
        JPH::RMat44 bone_matrix = Matrix4fToRMat44(bone_image.skinning_matrix_);
        JPH::RMat44 initial_transform = bone_matrix * body_transform;
        
        // Create body
        JPH::BodyCreationSettings body_settings(
            shape,
            initial_transform.GetTranslation(),
            initial_transform.GetQuaternion(),
            motion_type,
            object_layer
        );
        
        // Set physics properties
        if (body.GetType() != Model::RigidBody::RIGID_TYPE_KINEMATIC) {
            body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            body_settings.mMassPropertiesOverride.mMass = body.GetMass();
        }
        
        // Damping: MMD uses values 0-1, Jolt uses similar range
        float linear_damp = body.GetTranslateDamp();
        float angular_damp = body.GetRotateDamp();
        
        // Use original damping values (MMD models are tuned for these)
        body_settings.mLinearDamping = linear_damp;
        body_settings.mAngularDamping = angular_damp;
        
        body_settings.mRestitution = body.GetRestitution();
        body_settings.mFriction = body.GetFriction();
        body_settings.mAllowSleeping = false;  // Keep bodies always active like Bullet version
        
        // Use LinearCast for better collision detection with fast-moving objects
        body_settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        
        JPH::BodyID body_id = body_interface.CreateAndAddBody(body_settings, JPH::EActivation::Activate);
        body_ids.push_back(body_id);
        
        // Create motion state for bone synchronization
        auto motion_state = std::make_unique<BoneMotionState>(poser, body, body_transform, body_id);
        motion_states.push_back(std::move(motion_state));
    }
    
    // Create constraints
    for (size_t i = 0; i < model.GetConstraintNum(); ++i) {
        const Model::Constraint& constraint = model.GetConstraint(i);
        
        JPH::BodyID body_id_1 = body_ids[constraint.GetAssociatedRigidBodyIndex(0)];
        JPH::BodyID body_id_2 = body_ids[constraint.GetAssociatedRigidBodyIndex(1)];
        
        const Vector3f& position_low_limit = constraint.GetPositionLowLimit();
        const Vector3f& position_high_limit = constraint.GetPositionHighLimit();
        const Vector3f& rotation_low_limit = constraint.GetRotationLowLimit();
        const Vector3f& rotation_high_limit = constraint.GetRotationHighLimit();
        
        // Calculate constraint transform
        Matrix4f constraint_transform_mat = YXZToQuaternion(constraint.GetRotation()).ToRotateMatrix();
        constraint_transform_mat.r.v[3].downgrade.vector3d = constraint.GetPosition();
        JPH::RMat44 constraint_transform = Matrix4fToRMat44(constraint_transform_mat);
        
        // Get body transforms
        JPH::RMat44 body1_transform = body_interface.GetWorldTransform(body_id_1);
        JPH::RMat44 body2_transform = body_interface.GetWorldTransform(body_id_2);
        
        // Calculate local constraint transforms
        JPH::RMat44 local_transform_1 = body1_transform.Inversed() * constraint_transform;
        JPH::RMat44 local_transform_2 = body2_transform.Inversed() * constraint_transform;
        
        // Create 6DOF constraint settings
        JPH::SixDOFConstraintSettings constraint_settings;
        constraint_settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        
        // Set constraint frames
        constraint_settings.mPosition1 = local_transform_1.GetTranslation();
        constraint_settings.mAxisX1 = local_transform_1.GetAxisX();
        constraint_settings.mAxisY1 = local_transform_1.GetAxisY();
        
        constraint_settings.mPosition2 = local_transform_2.GetTranslation();
        constraint_settings.mAxisX2 = local_transform_2.GetAxisX();
        constraint_settings.mAxisY2 = local_transform_2.GetAxisY();
        
        // Set translation limits
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::TranslationX, position_low_limit.p.x, position_high_limit.p.x);
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::TranslationY, position_low_limit.p.y, position_high_limit.p.y);
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::TranslationZ, position_low_limit.p.z, position_high_limit.p.z);
        
        // Set rotation limits
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::RotationX, rotation_low_limit.p.x, rotation_high_limit.p.x);
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::RotationY, rotation_low_limit.p.y, rotation_high_limit.p.y);
        constraint_settings.SetLimitedAxis(JPH::SixDOFConstraintSettings::RotationZ, rotation_low_limit.p.z, rotation_high_limit.p.z);
        
        // Set spring settings for soft limits (approximating Bullet's spring behavior)
        // Bullet uses stiffness directly, Jolt uses frequency (Hz) and damping ratio
        // frequency = sqrt(stiffness / mass) / (2 * PI)
        // For simplicity, we use a fixed effective mass of 1.0 for the conversion
        const Vector3f& spring_translate = constraint.GetSpringTranslate();
        const Vector3f& spring_rotate = constraint.GetSpringRotate();
        
        // Jolt spring settings for translation limits
        for (int j = 0; j < 3; ++j) {
            if (spring_translate.v[j] > 0.0f) {
                // Convert Bullet stiffness to Jolt frequency
                // Use lower frequency for more stable, less jittery behavior
                float stiffness = spring_translate.v[j];
                float frequency = std::sqrt(stiffness) / (2.0f * 3.14159f);
                // Clamp frequency to prevent instability
                frequency = std::min(frequency, 10.0f);  // Max 10 Hz
                constraint_settings.mLimitsSpringSettings[j].mFrequency = frequency;
                constraint_settings.mLimitsSpringSettings[j].mDamping = 0.8f;  // Higher damping for stability
            }
        }
        
        // Add friction to all axes for damping
        for (int j = 0; j < 6; ++j) {
            constraint_settings.mMaxFriction[j] = 0.1f;  // Add friction for stability
        }
        
        // Create constraint
        // We need to lock the bodies to create a constraint
        JPH::BodyID body_id_array[2] = {body_id_1, body_id_2};
        JPH::BodyLockMultiWrite lock(physics_system_->GetBodyLockInterface(), body_id_array, 2);
        JPH::Body* jolt_body_1 = lock.GetBody(0);
        JPH::Body* jolt_body_2 = lock.GetBody(1);
        if (jolt_body_1 != nullptr && jolt_body_2 != nullptr) {
            JPH::Ref<JPH::Constraint> jolt_constraint = constraint_settings.Create(*jolt_body_1, *jolt_body_2);
            physics_system_->AddConstraint(jolt_constraint);
            constraints.push_back(jolt_constraint);
        }
    }
}

inline void JoltPhysicsReactor::RemovePoser(Poser& poser) {
    if (body_ids_.count(&poser) == 0) {
        return;
    }
    
    std::vector<JPH::Ref<JPH::Constraint>>& constraints = constraints_[&poser];
    std::vector<JPH::BodyID>& body_ids = body_ids_[&poser];
    
    // Remove constraints
    for (auto& constraint : constraints) {
        physics_system_->RemoveConstraint(constraint);
    }
    
    // Remove bodies
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (auto& body_id : body_ids) {
        body_interface.RemoveBody(body_id);
        body_interface.DestroyBody(body_id);
    }
    
    // Clear maps
    constraints_.erase(&poser);
    motion_states_.erase(&poser);
    body_ids_.erase(&poser);
    collision_shapes_.erase(&poser);
}

inline void JoltPhysicsReactor::Reset() {
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    
    // Reset all motion states
    for (auto& pair : motion_states_) {
        for (auto& motion_state : pair.second) {
            motion_state->Reset(physics_system_.get());
        }
    }
    
    // Reset all body velocities
    for (auto& pair : body_ids_) {
        for (auto& body_id : pair.second) {
            body_interface.SetLinearVelocity(body_id, JPH::Vec3::sZero());
            body_interface.SetAngularVelocity(body_id, JPH::Vec3::sZero());
        }
    }
}

inline void JoltPhysicsReactor::React(float step) {
    // Update kinematic bodies before physics step
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    
    for (auto& pair : motion_states_) {
        for (auto& motion_state : pair.second) {
            if (motion_state->passive) {
                // Update kinematic body position from bone
                // Use SetPositionAndRotation for immediate update (more stable for animation-driven bones)
                // MoveKinematic can cause jitter due to velocity calculation issues
                JPH::RMat44 new_transform = motion_state->GetWorldTransform();
                body_interface.SetPositionAndRotation(
                    motion_state->body_id,
                    new_transform.GetTranslation(),
                    new_transform.GetQuaternion(),
                    JPH::EActivation::Activate
                );
            }
        }
    }
    
    // Step physics simulation
    // Use sub-stepping for stability: divide the step into smaller increments
    // Too many steps can cause instability, 1-2 is usually enough for 30fps
    const int collision_steps = 4;
    
    // Sub-step the simulation for stability (similar to Bullet's stepSimulation with max substeps)
    physics_system_->Update(step, collision_steps, temp_allocator_.get(), job_system_.get());
    
    // Synchronize bone transforms from physics
    for (auto& pair : motion_states_) {
        for (auto& motion_state : pair.second) {
            motion_state->Synchronize(physics_system_.get());
        }
        for (auto& motion_state : pair.second) {
            motion_state->Fix();
        }
    }
}
