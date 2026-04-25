// test_session_fingerprint.cpp
// Tests for StableSessionFingerprint() - extracts stable identifier from Windows audio session strings.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// Basic functionality
// ---------------------------------------------------------------------------

TEST(StableSessionFingerprint, EmptyInputReturnsEmpty) {
    EXPECT_EQ(StableSessionFingerprint(L""), L"");
}

TEST(StableSessionFingerprint, ExtractsGuidAndFlagsFromTypicalSessionId) {
    // Typical format:
    //   {0.0.0.00000000}.{DEV-GUID}|\Device\HarddiskVolume2\...\app-1.2.3\Discord.exe%b{SESSION-GUID}%bFLAGS
    std::wstring sid =
        L"{0.0.0.00000000}.{a1b2c3d4-e5f6-7890-abcd-ef1234567890}"
        L"|\\Device\\HarddiskVolume2\\Users\\user\\AppData\\Local\\Discord\\app-1.2.3\\Discord.exe"
        L"%b{11111111-2222-3333-4444-555555555555}%b1";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"{11111111-2222-3333-4444-555555555555}%b1");
}

TEST(StableSessionFingerprint, IgnoresAppVersionSegment) {
    // Different app versions should produce the same fingerprint when GUID + flags match.
    std::wstring sidV1 =
        L"{0.0.0.00000000}.{dev}"
        L"|\\Device\\HarddiskVolume2\\Users\\user\\AppData\\Local\\Discord\\app-1.0.0\\Discord.exe"
        L"%b{AAAA-BBBB-CCCC-DDDD}%b2";
    std::wstring sidV2 =
        L"{0.0.0.00000000}.{dev}"
        L"|\\Device\\HarddiskVolume2\\Users\\user\\AppData\\Local\\Discord\\app-9.9.9\\Discord.exe"
        L"%b{AAAA-BBBB-CCCC-DDDD}%b2";
    EXPECT_EQ(StableSessionFingerprint(sidV1), StableSessionFingerprint(sidV2));
}

TEST(StableSessionFingerprint, DifferentFlagsProduceDifferentFingerprints) {
    std::wstring sidA = L"prefix%b{GUID}%b1";
    std::wstring sidB = L"prefix%b{GUID}%b2";
    EXPECT_NE(StableSessionFingerprint(sidA), StableSessionFingerprint(sidB));
}

TEST(StableSessionFingerprint, DifferentGuidsProduceDifferentFingerprints) {
    std::wstring sidA = L"prefix%b{AAAA}%b1";
    std::wstring sidB = L"prefix%b{BBBB}%b1";
    EXPECT_NE(StableSessionFingerprint(sidA), StableSessionFingerprint(sidB));
}

// ---------------------------------------------------------------------------
// Edge cases - fallback paths
// ---------------------------------------------------------------------------

TEST(StableSessionFingerprint, SinglePercentBOnly) {
    // Only one %b marker - no previous %b to walk back to.
    std::wstring sid = L"some/path/Discord.exe%b1";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"1");
}

TEST(StableSessionFingerprint, NoPercentB_FallbackToPipe) {
    // No %b markers at all - should fall back to content after last '|'.
    std::wstring sid = L"{0.0.0.00000000}.{dev}|\\Device\\HarddiskVolume2\\Discord.exe";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"\\Device\\HarddiskVolume2\\Discord.exe");
}

TEST(StableSessionFingerprint, NoPercentB_NoPipe_ReturnsFullString) {
    // Neither %b nor '|' present - entire string is the fingerprint.
    std::wstring sid = L"plain-text-session-id";
    EXPECT_EQ(StableSessionFingerprint(sid), L"plain-text-session-id");
}

TEST(StableSessionFingerprint, PercentBAtStart) {
    // %b at position 0 - edge case for rfind with offset 0.
    std::wstring sid = L"%b{GUID}%b1";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"{GUID}%b1");
}

TEST(StableSessionFingerprint, ConsecutivePercentB) {
    // Multiple consecutive %b markers.
    std::wstring sid = L"path%b%b{GUID}%b3";
    std::wstring fp = StableSessionFingerprint(sid);
    // lastPct finds the last %b (before '3'), prev finds the one before that (before '{GUID}')
    EXPECT_EQ(fp, L"{GUID}%b3");
}

TEST(StableSessionFingerprint, RealWorldStreamSession) {
    // Simulated real-world session identifier for a Discord stream audio session.
    std::wstring sid =
        L"{0.0.0.00000000}.{0dbecbbb-feec-477e-8642-b209e47c7a81}"
        L"|\\Device\\HarddiskVolume5\\Users\\ryanm\\AppData\\Local\\Discord\\app-246.0\\Discord.exe"
        L"%b{d34cb318-f2a1-4e9c-b7f5-8a9c1d2e3f4a}%b131074";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"{d34cb318-f2a1-4e9c-b7f5-8a9c1d2e3f4a}%b131074");
}

TEST(StableSessionFingerprint, RealWorldVoiceSession) {
    // Simulated real-world session identifier for a Discord voice chat session.
    std::wstring sid =
        L"{0.0.0.00000000}.{0dbecbbb-feec-477e-8642-b209e47c7a81}"
        L"|\\Device\\HarddiskVolume5\\Users\\ryanm\\AppData\\Local\\Discord\\app-246.0\\Discord.exe"
        L"%b{a1b2c3d4-e5f6-7890-abcd-ef1234567890}%b131074";
    std::wstring fp = StableSessionFingerprint(sid);
    EXPECT_EQ(fp, L"{a1b2c3d4-e5f6-7890-abcd-ef1234567890}%b131074");
}

// ---------------------------------------------------------------------------
// Stability across app version changes
// ---------------------------------------------------------------------------

TEST(StableSessionFingerprint, StableAcrossVersionUpdates) {
    // Verify that the fingerprint is identical regardless of the app version path.
    auto makeSid = [](const std::wstring& version) -> std::wstring {
        return L"{0.0.0.00000000}.{dev}"
               L"|\\Device\\HarddiskVolume2\\Users\\user\\AppData\\Local\\Discord\\"
               + version + L"\\Discord.exe"
               L"%b{STABLE-GUID}%b1";
    };

    std::wstring fp1 = StableSessionFingerprint(makeSid(L"app-100.0"));
    std::wstring fp2 = StableSessionFingerprint(makeSid(L"app-200.5"));
    std::wstring fp3 = StableSessionFingerprint(makeSid(L"app-999.99"));

    EXPECT_EQ(fp1, fp2);
    EXPECT_EQ(fp2, fp3);
}
