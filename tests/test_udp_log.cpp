#include "test_framework.h"
#include "tests.h"

#include "core/udp_log.h"

namespace anyadance::tests {

void TestLog() {
    UdpLog log;
    log.Add("Reset to T-Pose", "Sent", "payload1");
    EXPECT_TRUE(log.Entries().size() == 1);
    EXPECT_TRUE(log.Entries().back().payload == "payload1");
    EXPECT_TRUE(!log.Entries().back().timeText.empty());

    // Rapid manipulations with the same reason coalesce into the last entry
    // instead of flooding the log, so two quick updates add only one row.
    log.AddManipulation("Sent", "payload2");
    log.AddManipulation("Sent", "payload3");
    EXPECT_TRUE(log.Entries().size() == 2);
    EXPECT_TRUE(log.Entries().back().payload == "payload3");

    log.Add("Socket error", "Failed", "payload4", "err");
    EXPECT_TRUE(log.Entries().back().detail == "err");

    // The ring buffer caps at 1000 entries, dropping the oldest.
    for (int i = 0; i < 1100; ++i) {
        log.Add("Keyboard/button state", "Sent", "x");
    }
    EXPECT_TRUE(log.Entries().size() == 1000);

    // Clear empties the buffer.
    log.Clear();
    EXPECT_TRUE(log.Entries().empty());
}

} // namespace anyadance::tests
