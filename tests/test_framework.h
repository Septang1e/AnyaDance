#pragma once

#include "core/math3d.h"

// Tiny assertion harness shared by every test translation unit. Failures are
// counted in one place (Failures()) so main() can report a single total. The
// EXPECT_* macros capture the source expression and location for the message.
namespace anyadance::testing {

// Total assertion failures recorded so far across the whole test binary.
int Failures();

void Expect(bool condition, const char* expr, const char* file, int line);
void ExpectNear(float lhs, float rhs, float eps, const char* expr, const char* file, int line);
void ExpectVec3Near(Vec3 value, Vec3 expected, const char* expr, const char* file, int line);
void ExpectSameRotation(Quat lhs, Quat rhs, const char* expr, const char* file, int line);

// Normalize to the hemisphere with w >= 0 so two quaternions that represent the
// same rotation compare equal componentwise.
Quat Canonical(Quat q);

} // namespace anyadance::testing

#define EXPECT_TRUE(expr) ::anyadance::testing::Expect((expr), #expr, __FILE__, __LINE__)
#define EXPECT_FALSE(expr) ::anyadance::testing::Expect(!(expr), "!(" #expr ")", __FILE__, __LINE__)
#define EXPECT_NEAR(lhs, rhs, eps) \
    ::anyadance::testing::ExpectNear((lhs), (rhs), (eps), #lhs " ~= " #rhs, __FILE__, __LINE__)
#define EXPECT_VEC3_NEAR(value, expected) \
    ::anyadance::testing::ExpectVec3Near((value), (expected), #value " ~= " #expected, __FILE__, __LINE__)
#define EXPECT_SAME_ROTATION(lhs, rhs) \
    ::anyadance::testing::ExpectSameRotation((lhs), (rhs), #lhs " ~= " #rhs, __FILE__, __LINE__)
