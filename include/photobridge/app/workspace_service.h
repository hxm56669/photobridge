#pragma once

#include "photobridge/common/status.h"

namespace photobridge {

struct WorkspaceLayout;

class WorkspaceService {
public:
    Status Initialize(const WorkspaceLayout& layout) const;
};

}  // namespace photobridge
