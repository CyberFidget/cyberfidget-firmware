// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_rig.h
//
/**
 * Integer 2D skeletal animation and fixed-size verlet ragdoll runtime.
 *
 * Authored rigs stay as PROGMEM-friendly POD tables. Forward kinematics,
 * pose playback, and blending use the same integer operations as the website
 * pose solver. The deliberately separate ragdoll path uses float physics.
 */

#ifndef CF_GFX_RIG_H
#define CF_GFX_RIG_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

#include "cf_gfx_mesh.h"

namespace cf { namespace gfx {

/**
 * CROSS-LANGUAGE PARITY CONTRACT
 *
 * A C++ port of the website pose solver must reproduce these operations
 * exactly, in this order. The golden fixtures are the enforcing artifact;
 * any disagreement is a stop-the-line bug, not a tolerance.
 *
 * 1. kSinQ14 is the same 256-entry signed Q14 table the mesh runtime pins:
 *    entry i is 16384 * sin(i * PI / 128) rounded half away from zero.
 *    sinQ14(a) indexes a & 255; cosQ14(a) indexes (a + 64) & 255.
 * 2. Authored degrees convert to binary angles ONCE, on the way in, as
 *    round(degrees * 256 / 360) with halves away from zero, then wrapped
 *    modulo 256. Bone rest values are degrees too, not turns. No other place
 *    in the solver converts an angle.
 * 3. Blending happens in BINARY-ANGLE space, after conversion, never in
 *    degrees. The blend amount is an integer 0..255 and the divisor is 255,
 *    not 256, so amount=255 lands exactly on the second pose. Interpolation
 *    rounds half away from zero.
 * 4. The blended path is the SHORT way around: delta = ((to - from + 128)
 *    & 255) - 128. An exactly-opposite pair (delta == -128) therefore takes
 *    the negative direction deterministically rather than tie-breaking.
 * 5. Rotation of an offset uses the mesh runtime's convention exactly:
 *    x' = (x*cos - y*sin) >> 14, y' = (x*sin + y*cos) >> 14, where >> is an
 *    arithmetic right shift with NO rounding addend. It floors negative
 *    values and matches JavaScript's >>. Do not add a rounding term.
 * 6. Bones resolve parent-first. A bone's world angle is
 *    (parent angle + rest + pose offset) & 255. Its start point is the
 *    parent's start plus the bone's attach offset rotated by the PARENT's
 *    angle; its end point is the start plus (len, 0) rotated by its OWN
 *    angle. The root bone starts at (0, rootDy).
 * 7. Every accumulated coordinate is signed 32-bit. Inputs stay small enough
 *    that this never overflows for authored rigs, but a wider port can
 *    disagree on out-of-range data rather than failing loudly.
 * 8. Physics (verlet ragdoll) is deliberately NOT part of this contract.
 *    Pose/FK agreement is a byte claim; ragdoll agreement is a feel judgment.
 *
 * Bones must be authored parent-first: every non-root parent index is less
 * than its bone's index. RigPose::boneDegrees carries no length metadata, so
 * every authored pose array must contain exactly Rig::boneCount entries. The
 * golden-fixture sync script enforces both conventions. Ragdoll physics has a
 * separate kMaxRigParticles budget; ragdoll() returns false for larger rigs
 * even though their poses can still use up to kMaxRigBones bones.
 *
 * FK coordinates are integer screen pixels (+x right, +y down). RigActor's
 * position is plain int16_t screen pixels, like MeshActor. A caller holding
 * Q8.8 coordinates must convert them before calling setPos().
 */

inline constexpr uint8_t kMaxRigBones = 16;
inline constexpr uint8_t kMaxRigParticles = 11;
inline constexpr uint8_t kMaxRigConstraints = 11;

enum RigRenderMode : uint8_t {
    RIG_RENDER_BAKED16 = 0,
    RIG_RENDER_LINE = 1,
    RIG_RENDER_FLAT = 2,
};

struct RigBone {
    const char* name;
    int8_t parent;
    int16_t length;
    int16_t atX;
    int16_t atY;
    int16_t restDegrees;
};

struct RigPart {
    uint8_t bone;
    RigRenderMode render;
    const unsigned char* const* baked16Cels;
    uint8_t width;
    uint8_t height;
    int8_t pivotX;
    int8_t pivotY;
};

struct RigPose {
    const char* name;
    const int16_t* boneDegrees;
    int16_t rootDy;
};

struct RigAnimation {
    const char* name;
    const RigPose* const* keys;
    const uint16_t* durationsMs;
    uint8_t keyCount;
    bool loop;
};

struct Rig {
    const char* name;
    const RigBone* bones;
    uint8_t boneCount;
    const RigPart* parts;
    uint8_t partCount;
    const RigPose* poses;
    uint8_t poseCount;
    const RigAnimation* animations;
    uint8_t animationCount;
};

static_assert(std::is_trivial<Rig>::value &&
                  std::is_standard_layout<Rig>::value,
              "Rig must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<RigBone>::value &&
                  std::is_standard_layout<RigBone>::value,
              "RigBone must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<RigPart>::value &&
                  std::is_standard_layout<RigPart>::value,
              "RigPart must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<RigPose>::value &&
                  std::is_standard_layout<RigPose>::value,
              "RigPose must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<RigAnimation>::value &&
                  std::is_standard_layout<RigAnimation>::value,
              "RigAnimation must stay POD (PROGMEM-friendly)");

struct RigBoneTransform {
    int32_t startX;
    int32_t startY;
    int32_t endX;
    int32_t endY;
    uint8_t angle;
};

struct RigParticle {
    float x;
    float y;
};

inline int32_t rigRoundRatioHalfAwayFromZero(int64_t numerator,
                                              int64_t denominator) {
    int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    const int64_t magnitude = remainder < 0 ? -remainder : remainder;
    if (magnitude * 2 >= denominator) quotient += numerator < 0 ? -1 : 1;
    return static_cast<int32_t>(quotient);
}

inline uint8_t degreesToBinaryAngle(int32_t degrees) {
    const int32_t rounded =
        rigRoundRatioHalfAwayFromZero(static_cast<int64_t>(degrees) * 256, 360);
    const int32_t wrapped = ((rounded % 256) + 256) % 256;
    return static_cast<uint8_t>(wrapped);
}

inline int32_t interpolateRigInteger(int32_t from, int32_t to,
                                    uint8_t amount) {
    return from + rigRoundRatioHalfAwayFromZero(
                      static_cast<int64_t>(to - from) * amount, 255);
}

inline uint8_t blendBinaryAngle(uint8_t from, uint8_t to, uint8_t amount) {
    const int16_t shortDelta =
        static_cast<int16_t>(((static_cast<int16_t>(to) -
                               static_cast<int16_t>(from) + 128) &
                              0xFF) -
                             128);
    return static_cast<uint8_t>(
        static_cast<int16_t>(from) +
        interpolateRigInteger(0, shortDelta, amount));
}

inline void rotateRigOffset(int32_t x, int32_t y, uint8_t angle,
                            int32_t& outX, int32_t& outY) {
    const int32_t cosine = cosQ14(angle);
    const int32_t sine = sinQ14(angle);
    outX = (x * cosine - y * sine) >> 14;
    outY = (x * sine + y * cosine) >> 14;
}

inline int16_t rigPoseDegrees(const RigPose* pose, uint8_t bone) {
    return pose != nullptr && pose->boneDegrees != nullptr
               ? pose->boneDegrees[bone]
               : 0;
}

inline int32_t rigPoseRootDy(const RigPose* pose) {
    return pose != nullptr ? pose->rootDy : 0;
}

inline bool solveRigPose(const Rig& rig, const RigPose* poseA,
                         const RigPose* poseB, uint8_t blendAmount,
                         RigBoneTransform* out, uint8_t outCapacity) {
    if (out == nullptr || rig.bones == nullptr ||
        rig.boneCount > outCapacity || rig.boneCount > kMaxRigBones) {
        return false;
    }

    const int32_t rootDy = poseB == nullptr
                               ? rigPoseRootDy(poseA)
                               : interpolateRigInteger(rigPoseRootDy(poseA),
                                                       rigPoseRootDy(poseB),
                                                       blendAmount);

    for (uint8_t i = 0; i < rig.boneCount; ++i) {
        const RigBone& bone = rig.bones[i];
        int32_t startX = 0;
        int32_t startY = rootDy;
        uint8_t parentAngle = 0;
        if (bone.parent >= 0) {
            const uint8_t parent = static_cast<uint8_t>(bone.parent);
            if (parent >= i) return false;
            parentAngle = out[parent].angle;
            int32_t rotatedX = 0;
            int32_t rotatedY = 0;
            rotateRigOffset(bone.atX, bone.atY, parentAngle,
                            rotatedX, rotatedY);
            startX = static_cast<int32_t>(out[parent].startX + rotatedX);
            startY = static_cast<int32_t>(out[parent].startY + rotatedY);
        }

        const uint8_t from =
            degreesToBinaryAngle(rigPoseDegrees(poseA, i));
        const uint8_t poseOffset =
            poseB == nullptr
                ? from
                : blendBinaryAngle(
                      from, degreesToBinaryAngle(rigPoseDegrees(poseB, i)),
                      blendAmount);
        const uint8_t angle = static_cast<uint8_t>(
            parentAngle + degreesToBinaryAngle(bone.restDegrees) + poseOffset);
        int32_t endDx = 0;
        int32_t endDy = 0;
        rotateRigOffset(bone.length, 0, angle, endDx, endDy);
        out[i] = RigBoneTransform{
            startX, startY,
            static_cast<int32_t>(startX + endDx),
            static_cast<int32_t>(startY + endDy), angle};
    }
    return true;
}

inline bool solveRigPose(const Rig& rig, const RigPose* pose,
                         RigBoneTransform* out, uint8_t outCapacity) {
    return solveRigPose(rig, pose, nullptr, 0, out, outCapacity);
}

inline const RigPose* findRigPose(const Rig& rig, const char* name) {
    if (name == nullptr || rig.poses == nullptr) return nullptr;
    for (uint8_t i = 0; i < rig.poseCount; ++i) {
        if (rig.poses[i].name != nullptr &&
            strcmp(rig.poses[i].name, name) == 0) {
            return &rig.poses[i];
        }
    }
    return nullptr;
}

inline const RigAnimation* findRigAnimation(const Rig& rig,
                                            const char* name) {
    if (name == nullptr || rig.animations == nullptr) return nullptr;
    for (uint8_t i = 0; i < rig.animationCount; ++i) {
        if (rig.animations[i].name != nullptr &&
            strcmp(rig.animations[i].name, name) == 0) {
            return &rig.animations[i];
        }
    }
    return nullptr;
}

inline uint8_t smoothstepRigAmount(uint8_t amount) {
    const int64_t a = amount;
    return static_cast<uint8_t>(rigRoundRatioHalfAwayFromZero(
        a * a * (765 - 2 * a), 255LL * 255LL));
}

class RigActor {
public:
    enum State : uint8_t {
        STATE_POSE,
        STATE_ANIMATION,
        STATE_RAGDOLL,
        STATE_STANDING,
    };

    RigActor()
        : rig_(nullptr), x_(0), y_(0), state_(STATE_POSE),
          pose_(nullptr), animation_(nullptr), animationStartMs_(0),
          lastNowMs_(0), transformsValid_(false), historyValid_(false),
          particleCount_(0), constraintCount_(0), settledTicks_(0),
          pinIndex_(-1), standStartMs_(0) {
        memset(transforms_, 0, sizeof(transforms_));
        memset(jointsPrevious_, 0, sizeof(jointsPrevious_));
        memset(jointsCurrent_, 0, sizeof(jointsCurrent_));
        memset(particles_, 0, sizeof(particles_));
        memset(previous_, 0, sizeof(previous_));
        memset(constraints_, 0, sizeof(constraints_));
        memset(standFrom_, 0, sizeof(standFrom_));
        memset(standTarget_, 0, sizeof(standTarget_));
    }

    void setRig(const Rig* rig) {
        rig_ = rig;
        pose_ = nullptr;
        animation_ = nullptr;
        state_ = STATE_POSE;
        transformsValid_ = false;
        historyValid_ = false;
        particleCount_ = 0;
        constraintCount_ = 0;
        settledTicks_ = 0;
        pinIndex_ = -1;
        solveAndTrack(nullptr, nullptr, 0);
    }

    const Rig* rig() const { return rig_; }

    void setPos(int16_t x, int16_t y) {
        x_ = x;
        y_ = y;
        if (state_ == STATE_POSE || state_ == STATE_ANIMATION) {
            refreshJointHistory();
        }
    }

    int16_t x() const { return x_; }
    int16_t y() const { return y_; }
    State state() const { return state_; }
    bool isRagdoll() const {
        return state_ == STATE_RAGDOLL || state_ == STATE_STANDING;
    }

    bool play(const char* name) {
        if (rig_ == nullptr) return false;
        const RigAnimation* animation = findRigAnimation(*rig_, name);
        if (animation != nullptr) return play(animation);
        const RigPose* pose = findRigPose(*rig_, name);
        if (pose == nullptr) return false;
        pose_ = pose;
        animation_ = nullptr;
        state_ = STATE_POSE;
        solveAndTrack(pose_, nullptr, 0);
        return true;
    }

    bool play(const RigAnimation* animation) {
        if (rig_ == nullptr || animation == nullptr ||
            animation->keys == nullptr || animation->keyCount == 0) {
            return false;
        }
        animation_ = animation;
        pose_ = nullptr;
        animationStartMs_ = lastNowMs_;
        state_ = STATE_ANIMATION;
        updateAnimation(lastNowMs_);
        return true;
    }

    bool play(const RigAnimation& animation) { return play(&animation); }

    bool blend(const char* poseA, const char* poseB, uint8_t amount) {
        if (rig_ == nullptr) return false;
        return blend(findRigPose(*rig_, poseA), findRigPose(*rig_, poseB),
                     amount);
    }

    bool blend(const RigPose* poseA, const RigPose* poseB, uint8_t amount) {
        if (rig_ == nullptr || poseA == nullptr || poseB == nullptr) {
            return false;
        }
        animation_ = nullptr;
        pose_ = nullptr;
        state_ = STATE_POSE;
        solveAndTrack(poseA, poseB, smoothstepRigAmount(amount));
        return true;
    }

    bool blend(const RigPose& poseA, const RigPose& poseB, uint8_t amount) {
        return blend(&poseA, &poseB, amount);
    }

    bool ragdoll(float impulseX = 0.0f, float impulseY = 0.0f) {
        if (rig_ == nullptr || !transformsValid_ ||
            rig_->boneCount == 0 || rig_->boneCount > kMaxRigParticles) {
            return false;
        }
        particleCount_ = rig_->boneCount;
        for (uint8_t i = 0; i < particleCount_; ++i) {
            const RigParticle point = jointForBone(i);
            particles_[i] = point;
            float vx = impulseX;
            float vy = impulseY;
            if (historyValid_) {
                vx += (jointsCurrent_[i].x - jointsPrevious_[i].x) * 0.9f;
                vy += (jointsCurrent_[i].y - jointsPrevious_[i].y) * 0.9f;
            }
            previous_[i] = RigParticle{point.x - vx, point.y - vy};
        }
        captureConstraintRestLengths();
        settledTicks_ = 0;
        pinIndex_ = -1;
        state_ = STATE_RAGDOLL;
        return true;
    }

    bool pin(const char* bone, float x, float y) {
        const int8_t index = boneIndex(bone);
        if (index < 0) return false;
        return pin(static_cast<uint8_t>(index), x, y);
    }

    bool pin(uint8_t bone, float x, float y) {
        if (state_ != STATE_RAGDOLL || bone >= particleCount_) return false;
        pinIndex_ = static_cast<int8_t>(bone);
        pinX_ = x;
        pinY_ = y;
        return true;
    }

    void unpin() { pinIndex_ = -1; }

    void standUp() {
        if (state_ != STATE_RAGDOLL || particleCount_ == 0) return;
        startStandUp(lastNowMs_);
    }

    void update(uint32_t nowMs) {
        lastNowMs_ = nowMs;
        if (rig_ == nullptr) return;
        switch (state_) {
            case STATE_ANIMATION:
                updateAnimation(nowMs);
                break;
            case STATE_RAGDOLL:
                if (stepRagdoll() && pinIndex_ < 0) startStandUp(nowMs);
                break;
            case STATE_STANDING:
                updateStandUp(nowMs);
                break;
            case STATE_POSE:
                break;
        }
    }

    void draw(DisplayProxy& display) const {
        if (rig_ == nullptr || !transformsValid_) return;
        if (rig_->parts != nullptr && rig_->partCount != 0) {
            for (uint8_t i = 0; i < rig_->partCount; ++i) {
                drawPart(display, rig_->parts[i]);
            }
        } else {
            for (uint8_t i = 0; i < rig_->boneCount; ++i) {
                const RigPart fallback =
                    {i, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0};
                drawPart(display, fallback);
            }
        }
    }

    uint8_t boneCount() const {
        return rig_ != nullptr ? rig_->boneCount : 0;
    }

    const RigBoneTransform& boneTransform(uint8_t index) const {
        return transforms_[index];
    }

    uint8_t particleCount() const { return particleCount_; }

    RigParticle particle(uint8_t index) const {
        return index < particleCount_ ? particles_[index]
                                      : RigParticle{0.0f, 0.0f};
    }

    float maxHardConstraintError() const {
        float maximum = 0.0f;
        for (uint8_t i = 0; i < constraintCount_; ++i) {
            if (constraints_[i].stiffness < 1.0f) continue;
            const RigParticle& a = particles_[constraints_[i].a];
            const RigParticle& b = particles_[constraints_[i].b];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float error =
                fabsf(sqrtf(dx * dx + dy * dy) - constraints_[i].rest);
            if (error > maximum) maximum = error;
        }
        return maximum;
    }

private:
    struct Constraint {
        uint8_t a;
        uint8_t b;
        float stiffness;
        float rest;
    };

    static constexpr float kGravity = 0.35f;
    static constexpr float kDamping = 0.995f;
    static constexpr float kFriction = 0.72f;
    static constexpr float kFloorY = 58.0f;
    static constexpr uint8_t kRelaxationIterations = 3;

    int8_t boneIndex(const char* name) const {
        if (rig_ == nullptr || name == nullptr) return -1;
        for (uint8_t i = 0; i < rig_->boneCount; ++i) {
            if (rig_->bones[i].name != nullptr &&
                strcmp(rig_->bones[i].name, name) == 0) {
                return static_cast<int8_t>(i);
            }
        }
        return -1;
    }

    bool solveAndTrack(const RigPose* a, const RigPose* b, uint8_t amount) {
        if (rig_ == nullptr) return false;
        transformsValid_ =
            solveRigPose(*rig_, a, b, amount, transforms_, kMaxRigBones);
        if (transformsValid_) refreshJointHistory();
        return transformsValid_;
    }

    RigParticle jointForBone(uint8_t index) const {
        const RigBoneTransform& transform = transforms_[index];
        if (rig_->bones[index].parent < 0) {
            return RigParticle{static_cast<float>(x_ + transform.startX),
                               static_cast<float>(y_ + transform.startY)};
        }
        return RigParticle{static_cast<float>(x_ + transform.endX),
                           static_cast<float>(y_ + transform.endY)};
    }

    void refreshJointHistory() {
        if (rig_ == nullptr || !transformsValid_ ||
            rig_->boneCount > kMaxRigParticles) {
            return;
        }
        for (uint8_t i = 0; i < rig_->boneCount; ++i) {
            const RigParticle point = jointForBone(i);
            if (historyValid_) jointsPrevious_[i] = jointsCurrent_[i];
            else jointsPrevious_[i] = point;
            jointsCurrent_[i] = point;
        }
        historyValid_ = true;
    }

    void updateAnimation(uint32_t nowMs) {
        if (animation_ == nullptr || animation_->keyCount == 0) return;
        uint32_t total = 0;
        for (uint8_t i = 0; i < animation_->keyCount; ++i) {
            total += animation_->durationsMs != nullptr
                         ? animation_->durationsMs[i]
                         : 0;
        }
        if (total == 0) {
            solveAndTrack(animation_->keys[0], nullptr, 0);
            return;
        }
        uint32_t elapsed = nowMs - animationStartMs_;
        if (animation_->loop) elapsed %= total;
        else if (elapsed >= total) elapsed = total - 1;

        uint8_t key = 0;
        uint16_t duration = animation_->durationsMs[0];
        while (key + 1 < animation_->keyCount && elapsed >= duration) {
            elapsed -= duration;
            ++key;
            duration = animation_->durationsMs[key];
        }
        const uint8_t next =
            key + 1 < animation_->keyCount
                ? static_cast<uint8_t>(key + 1)
                : static_cast<uint8_t>(animation_->loop ? 0 : key);
        const uint8_t amount =
            duration == 0
                ? 0
                : static_cast<uint8_t>((elapsed * 255UL) / duration);
        solveAndTrack(animation_->keys[key], animation_->keys[next],
                      smoothstepRigAmount(amount));
    }

    void captureConstraintRestLengths() {
        constraintCount_ = 0;
        for (uint8_t i = 0; i < particleCount_; ++i) {
            const int8_t parent = rig_->bones[i].parent;
            if (parent < 0 || constraintCount_ >= kMaxRigConstraints) continue;
            addConstraint(static_cast<uint8_t>(parent), i, 1.0f);
        }
        const int8_t head = boneIndex("head");
        if (head >= 0 && particleCount_ > 0 &&
            constraintCount_ < kMaxRigConstraints) {
            addConstraint(0, static_cast<uint8_t>(head), 0.35f);
        }
    }

    void addConstraint(uint8_t a, uint8_t b, float stiffness) {
        const float dx = particles_[a].x - particles_[b].x;
        const float dy = particles_[a].y - particles_[b].y;
        constraints_[constraintCount_++] =
            Constraint{a, b, stiffness, sqrtf(dx * dx + dy * dy)};
    }

    bool stepRagdoll() {
        float speed = 0.0f;
        for (uint8_t i = 0; i < particleCount_; ++i) {
            RigParticle& point = particles_[i];
            RigParticle& previous = previous_[i];
            const float vx = (point.x - previous.x) * kDamping;
            const float vy = (point.y - previous.y) * kDamping;
            speed += fabsf(vx) + fabsf(vy);
            previous = point;
            point.x += vx;
            point.y += vy + kGravity;
        }

        for (uint8_t iteration = 0;
             iteration < kRelaxationIterations; ++iteration) {
            if (pinIndex_ >= 0) {
                particles_[static_cast<uint8_t>(pinIndex_)] =
                    RigParticle{pinX_, pinY_};
            }
            for (uint8_t i = 0; i < constraintCount_; ++i) {
                Constraint& constraint = constraints_[i];
                RigParticle& a = particles_[constraint.a];
                RigParticle& b = particles_[constraint.b];
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                float distance = sqrtf(dx * dx + dy * dy);
                if (distance == 0.0f) distance = 0.0001f;
                const float diff =
                    (distance - constraint.rest) / distance *
                    0.5f * constraint.stiffness;
                a.x += dx * diff;
                a.y += dy * diff;
                b.x -= dx * diff;
                b.y -= dy * diff;
            }
            for (uint8_t i = 0; i < particleCount_; ++i) {
                RigParticle& point = particles_[i];
                if (point.y > kFloorY) {
                    point.y = kFloorY;
                    previous_[i].x =
                        point.x - (point.x - previous_[i].x) * kFriction;
                }
                if (point.x < 2.0f) point.x = 2.0f;
                if (point.x > 125.0f) point.x = 125.0f;
                if (point.y < 2.0f) point.y = 2.0f;
            }
        }
        settledTicks_ = speed < 0.9f
                            ? (settledTicks_ < 255
                                   ? static_cast<uint8_t>(settledTicks_ + 1)
                                   : settledTicks_)
                            : 0;
        return settledTicks_ > 70;
    }

    void startStandUp(uint32_t nowMs) {
        float hipX = particles_[0].x;
        if (hipX < 28.0f) hipX = 28.0f;
        if (hipX > 100.0f) hipX = 100.0f;
        x_ = static_cast<int16_t>(hipX);
        y_ = 44;
        for (uint8_t i = 0; i < particleCount_; ++i) {
            standFrom_[i] = particles_[i];
        }
        RigBoneTransform targetTransforms[kMaxRigBones] = {};
        solveRigPose(*rig_, static_cast<const RigPose*>(nullptr),
                     targetTransforms, kMaxRigBones);
        for (uint8_t i = 0; i < particleCount_; ++i) {
            const RigBoneTransform& transform = targetTransforms[i];
            if (rig_->bones[i].parent < 0) {
                standTarget_[i] =
                    RigParticle{static_cast<float>(x_ + transform.startX),
                                static_cast<float>(y_ + transform.startY)};
            } else {
                standTarget_[i] =
                    RigParticle{static_cast<float>(x_ + transform.endX),
                                static_cast<float>(y_ + transform.endY)};
            }
        }
        pinIndex_ = -1;
        standStartMs_ = nowMs;
        state_ = STATE_STANDING;
    }

    void updateStandUp(uint32_t nowMs) {
        const uint32_t elapsed = nowMs - standStartMs_;
        if (elapsed >= 450) {
            for (uint8_t i = 0; i < particleCount_; ++i) {
                particles_[i] = standTarget_[i];
            }
            state_ = animation_ != nullptr ? STATE_ANIMATION : STATE_POSE;
            if (state_ == STATE_ANIMATION) animationStartMs_ = nowMs;
            solveAndTrack(nullptr, nullptr, 0);
            return;
        }
        const float t = static_cast<float>(elapsed) / 450.0f;
        const float amount = t * t * (3.0f - 2.0f * t);
        for (uint8_t i = 0; i < particleCount_; ++i) {
            particles_[i].x =
                standFrom_[i].x +
                (standTarget_[i].x - standFrom_[i].x) * amount;
            particles_[i].y =
                standFrom_[i].y +
                (standTarget_[i].y - standFrom_[i].y) * amount;
        }
    }

    void segmentForBone(uint8_t bone, float& sx, float& sy,
                        float& ex, float& ey, uint8_t& angle) const {
        if (state_ == STATE_RAGDOLL || state_ == STATE_STANDING) {
            const int8_t parent = rig_->bones[bone].parent;
            if (parent < 0) {
                sx = particles_[bone].x;
                sy = particles_[bone].y;
                angle = transforms_[bone].angle;
                ex = sx + static_cast<float>(
                              (rig_->bones[bone].length * cosQ14(angle)) >>
                              14);
                ey = sy + static_cast<float>(
                              (rig_->bones[bone].length * sinQ14(angle)) >>
                              14);
            } else {
                sx = particles_[static_cast<uint8_t>(parent)].x;
                sy = particles_[static_cast<uint8_t>(parent)].y;
                ex = particles_[bone].x;
                ey = particles_[bone].y;
                angle = angleFromVector(ex - sx, ey - sy);
            }
            return;
        }
        const RigBoneTransform& transform = transforms_[bone];
        sx = static_cast<float>(x_ + transform.startX);
        sy = static_cast<float>(y_ + transform.startY);
        ex = static_cast<float>(x_ + transform.endX);
        ey = static_cast<float>(y_ + transform.endY);
        angle = transform.angle;
    }

    static uint8_t angleFromVector(float x, float y) {
        const float turns = atan2f(y, x) * (256.0f / 6.28318530718f);
        int32_t rounded =
            turns < 0.0f ? static_cast<int32_t>(turns - 0.5f)
                         : static_cast<int32_t>(turns + 0.5f);
        return static_cast<uint8_t>(rounded);
    }

    static int16_t drawCoordinate(float value) {
        return static_cast<int16_t>(value < 0.0f ? value - 0.5f
                                                 : value + 0.5f);
    }

    bool isNamed(uint8_t bone, const char* name) const {
        return rig_->bones[bone].name != nullptr &&
               strcmp(rig_->bones[bone].name, name) == 0;
    }

    void drawPart(DisplayProxy& display, const RigPart& part) const {
        if (part.bone >= rig_->boneCount) return;
        float sx = 0.0f;
        float sy = 0.0f;
        float ex = 0.0f;
        float ey = 0.0f;
        uint8_t angle = 0;
        segmentForBone(part.bone, sx, sy, ex, ey, angle);

        if (isNamed(part.bone, "head")) {
            drawHead(display, sx, sy, ex, ey, angle);
            return;
        }

        if (part.render == RIG_RENDER_BAKED16 &&
            part.baked16Cels != nullptr) {
            const unsigned char* cel = part.baked16Cels[angle >> 4];
            if (cel != nullptr) {
                display.drawXbm(drawCoordinate(sx) - part.pivotX,
                                drawCoordinate(sy) - part.pivotY,
                                part.width, part.height, cel);
                return;
            }
        }
        // Flat is intentionally the same primitive line representation.
        // Baked16 also falls back here until cel-emission tooling binds data.
        display.drawLine(drawCoordinate(sx), drawCoordinate(sy),
                         drawCoordinate(ex), drawCoordinate(ey));
        if (isNamed(part.bone, "l_arm") ||
            isNamed(part.bone, "r_arm") ||
            isNamed(part.bone, "l_thigh") ||
            isNamed(part.bone, "r_thigh") ||
            isNamed(part.bone, "l_fore") ||
            isNamed(part.bone, "r_fore")) {
            display.fillCircle(drawCoordinate(ex), drawCoordinate(ey), 1);
        }
    }

    void drawHead(DisplayProxy& display, float sx, float sy,
                  float ex, float ey, uint8_t angle) const {
        const float hx = (sx + ex) * 0.5f;
        const float hy = (sy + ey) * 0.5f - 1.0f;
        display.fillCircle(drawCoordinate(hx), drawCoordinate(hy), 4);
        const float sine = static_cast<float>(sinQ14(angle)) / 16384.0f;
        const float cosine = static_cast<float>(cosQ14(angle)) / 16384.0f;
        display.fillCircle(drawCoordinate(hx - sine * 1.6f - cosine * 0.5f),
                           drawCoordinate(hy + cosine * 1.6f - sine * 0.5f),
                           0);
        display.fillCircle(drawCoordinate(hx + sine * 1.6f - cosine * 0.5f),
                           drawCoordinate(hy - cosine * 1.6f - sine * 0.5f),
                           0);
    }

    const Rig* rig_;
    int16_t x_;
    int16_t y_;
    State state_;
    const RigPose* pose_;
    const RigAnimation* animation_;
    uint32_t animationStartMs_;
    uint32_t lastNowMs_;
    RigBoneTransform transforms_[kMaxRigBones];
    bool transformsValid_;
    RigParticle jointsPrevious_[kMaxRigParticles];
    RigParticle jointsCurrent_[kMaxRigParticles];
    bool historyValid_;
    RigParticle particles_[kMaxRigParticles];
    RigParticle previous_[kMaxRigParticles];
    uint8_t particleCount_;
    Constraint constraints_[kMaxRigConstraints];
    uint8_t constraintCount_;
    uint8_t settledTicks_;
    int8_t pinIndex_;
    float pinX_;
    float pinY_;
    RigParticle standFrom_[kMaxRigParticles];
    RigParticle standTarget_[kMaxRigParticles];
    uint32_t standStartMs_;
};

}}  // namespace cf::gfx

#endif  // CF_GFX_RIG_H
