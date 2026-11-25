#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/physics/rigid/contacts.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Contact detection, cache matching and publication of RigidWorld, kept out of the public header.
class RigidWorldContacts final
{
public:
    /// @brief Generate the tick's contacts into the scratch generation and match them to the published one.
    ///
    /// Reads committed poses only and changes neither bodies, commands nor the published contacts. Returns Success
    /// with the scratch generation ready to publish; CapacityExceeded with the exhausted table and its required
    /// count in the world's contact diagnostics; or the failure of a collision query.
    [[nodiscard]] static Status Detect(RigidWorld& inoutWorld, uint64 tick) noexcept;

    /// Publish the scratch generation Detect filled: called once the tick has succeeded.
    static void Publish(RigidWorld& inoutWorld, uint64 tick) noexcept;

private:
    using ContactTables = RigidWorld::ContactTables;
    using ContactPairRecord = RigidWorld::ContactPairRecord;
    using ContactPatchRecord = RigidWorld::ContactPatchRecord;
    using ContactPointRecord = RigidWorld::ContactPointRecord;
    using ContactSide = RigidWorld::ContactSide;

    /// Everything one side of a pair must agree with, read from the world as it is now.
    [[nodiscard]] static ContactSide describeSide(const RigidWorld& world, collision::ColliderHandle collider) noexcept;
    /// Append one pair's contacts to the scratch generation; the caller has checked that they fit.
    static void appendPair(const RigidWorld& world,
                           collision::ColliderHandle first,
                           collision::ColliderHandle second,
                           const collision::ContactPairContacts& contacts,
                           ContactTables& inoutTables) noexcept;
    /// Count every point of a cached pair as retired for one reason.
    static void retirePair(const ContactTables& previous,
                           const ContactPairRecord& pair,
                           ContactRetirement reason,
                           ContactDiagnostics& inoutDiagnostics) noexcept;
    /// Carry cached state into the current pair's points where features and normals still agree.
    static void matchPair(ContactTables& inoutPrevious,
                          const ContactPairRecord& previousPair,
                          ContactTables& inoutCurrent,
                          const ContactPairRecord& currentPair,
                          ContactDiagnostics& inoutDiagnostics) noexcept;
    /// Match the sorted scratch generation against the sorted published one.
    static void matchCache(ContactTables& inoutPrevious,
                           ContactTables& inoutCurrent,
                           ContactDiagnostics& inoutDiagnostics) noexcept;
};
} // namespace ludus::physics::rigid
