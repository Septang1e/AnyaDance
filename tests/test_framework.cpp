#include "test_framework.h"

#include <cmath>
#include <iostream>

namespace anyadance::testing {
namespace {
int g_failures = 0;
}

int Failures() {
    return g_failures;
}

void Expect(bool condition, const char* expr, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << " failed: " << expr << '\n';
        ++g_failures;
    }
}

void ExpectNear(float lhs, float rhs, float eps, const char* expr, const char* file, int line) {
    if (std::fabs(lhs - rhs) > eps) {
        std::cerr << file << ':' << line << " failed: " << expr << " got " << lhs << " expected " << rhs << '\n';
        ++g_failures;
    }
}

void ExpectVec3Near(Vec3 value, Vec3 expected, const char* expr, const char* file, int line) {
    ExpectNear(value.x, expected.x, 0.0001f, expr, file, line);
    ExpectNear(value.y, expected.y, 0.0001f, expr, file, line);
    ExpectNear(value.z, expected.z, 0.0001f, expr, file, line);
}

void ExpectSameRotation(Quat lhs, Quat rhs, const char* expr, const char* file, int line) {
    lhs = Normalized(lhs);
    rhs = Normalized(rhs);
    const float dot = lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
    if (std::fabs(std::fabs(dot) - 1.0f) > 0.0001f) {
        std::cerr << file << ':' << line << " failed: " << expr << " quaternion dot " << dot << '\n';
        ++g_failures;
    }
}

Quat Canonical(Quat q) {
    q = Normalized(q);
    if (q.w < 0.0f) {
        return {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

} // namespace anyadance::testing
