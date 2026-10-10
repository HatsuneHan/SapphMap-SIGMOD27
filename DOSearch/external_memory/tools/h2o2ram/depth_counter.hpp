#pragma once

namespace h2o2ram_ns {

class DepthCounter
{
    static inline int depth = 0;

public:
    DepthCounter()
    {
        ++depth;
    }

    ~DepthCounter()
    {
        --depth;
    }

    int get_depth() const
    {
        return depth;
    }
};

} // namespace h2o2ram_ns
