#pragma once

#include <vsg/app/RecordTraversal.h>
#include <vsg/core/Exception.h>
#include <vsg/nodes/Group.h>

/**
 * @brief A node that fails the record step of a submission on command.
 *
 * WHY A NODE AND NOT A REAL DEVICE-LOST: the failure a submission can really meet here is an exception - vsg
 * throws `vsg::Exception` when it cannot make the step (a command buffer that cannot be allocated) - and a
 * device-lost cannot be summoned on demand on lavapipe. So this node throws exactly that exception type, at
 * exactly that step, at a frame a case chooses: everything above it (the executor's submit, the session's
 * commit, the mark, the report) is the production code.
 */
class FailingStepNode : public ::vsg::Group
{
  public:
    bool armed{false};  ///< Whether the next record step throws (disarmed: an ordinary empty group).

    void accept(::vsg::RecordTraversal& visitor) const override
    {
        if (armed)
        {
            throw ::vsg::Exception{ "the submission step failed (test)", VK_ERROR_DEVICE_LOST };
        }
        ::vsg::Group::traverse(visitor);
    }
};
