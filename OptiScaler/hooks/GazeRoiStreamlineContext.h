#pragma once

#include <cstdint>

struct GazeRoiStreamlineRect
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GazeRoiStreamlineEvaluationContext
{
    bool active = false;
    bool recentered = false;
    float gazeX = 0.5f;
    float gazeY = 0.5f;
    GazeRoiStreamlineRect colorOriginal {};
    GazeRoiStreamlineRect colorLocal {};
    GazeRoiStreamlineRect depthOriginal {};
    GazeRoiStreamlineRect depthLocal {};
    GazeRoiStreamlineRect motionVectorsOriginal {};
    GazeRoiStreamlineRect motionVectorsLocal {};
    GazeRoiStreamlineRect outputOriginal {};
    GazeRoiStreamlineRect outputLocal {};
};

namespace GazeRoiStreamlineContext
{
const GazeRoiStreamlineEvaluationContext* Current();
void Set(const GazeRoiStreamlineEvaluationContext& context);
void Clear();
}
