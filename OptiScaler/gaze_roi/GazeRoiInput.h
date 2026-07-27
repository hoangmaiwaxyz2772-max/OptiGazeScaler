#pragma once

struct GazeRoiInputSample
{
    float x = 0.5f;
    float y = 0.5f;
    bool recentered = false;
};

namespace GazeRoiInput
{
GazeRoiInputSample Sample();
void Stop();
}
