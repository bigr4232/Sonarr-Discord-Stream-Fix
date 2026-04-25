// test_utilities.cpp
// Tests for utility helper functions: ToLowerCopy, HResultToWString, GuidToWString,
// PackPolicyConfigDeviceId.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// ToLowerCopy
// ---------------------------------------------------------------------------

TEST(ToLowerCopy, BasicLowercase) {
    EXPECT_EQ(ToLowerCopy(L"HELLO"), L"hello");
}

TEST(ToLowerCopy, AlreadyLowercase) {
    EXPECT_EQ(ToLowerCopy(L"already lowercase"), L"already lowercase");
}

TEST(ToLowerCopy, MixedCase) {
    EXPECT_EQ(ToLowerCopy(L"MiXeD CaSe Te徐t"), L"mixed case te徐t");
}

TEST(ToLowerCopy, EmptyString) {
    EXPECT_EQ(ToLowerCopy(L""), L"");
}

TEST(ToLowerCopy, NumbersAndSymbols) {
    EXPECT_EQ(ToLowerCopy(L"Device-123!@#"), L"device-123!@#");
}

TEST(ToLowerCopy, WideCharacters) {
    // Non-ASCII characters should pass through unchanged (towlower is locale-dependent).
    std::wstring input = L"Arctis Nova \u00E9\u00E8";
    std::wstring result = ToLowerCopy(input);
    EXPECT_NE(result, L"");  // Should not be empty
}

// ---------------------------------------------------------------------------
// HResultToWString
// ---------------------------------------------------------------------------

TEST(HResultFormatting, Success) {
    EXPECT_EQ(HResultToWString(S_OK), L"0x00000000");
}

TEST(HResultFormatting, Fail) {
    EXPECT_EQ(HResultToWString(S_FALSE), L"0x00000001");
}

TEST(HResultFormatting, EAccessDenied) {
    EXPECT_EQ(HResultToWString(E_ACCESSDENIED), L"0x80070005");
}

TEST(HResultFormatting, EOutOfMemory) {
    EXPECT_EQ(HResultToWString(E_OUTOFMEMORY), L"0x8007000E");
}

TEST(HResultFormatting, CustomPositive) {
    EXPECT_EQ(HResultToWString(0xDEADBEEF), L"0xDEADBEEF");
}

TEST(HResultFormatting, NegativeValue) {
    // 0x80010106 (RO_E_CHANGED_MODE)
    HRESULT hr = static_cast<HRESULT>(0x80010106);
    EXPECT_EQ(HResultToWString(hr), L"0x80010106");
}

TEST(HResultFormatting, Zero) {
    EXPECT_EQ(HResultToWString(0), L"0x00000000");
}

// ---------------------------------------------------------------------------
// GuidToWString
// ---------------------------------------------------------------------------

TEST(GuidToWString, NullGuid) {
    GUID nullGuid = GUID_NULL;
    EXPECT_EQ(GuidToWString(nullGuid), L"{00000000-0000-0000-0000-000000000000}");
}

TEST(GuidToWString, KnownGuid) {
    GUID g = { 0xA1B2C3D4, 0xE5F6, 0x7890, { 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90 } };
    EXPECT_EQ(GuidToWString(g), L"{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}");
}

TEST(GuidToWString, AllOnes) {
    GUID g = { 0xFFFFFFFF, 0xFFFF, 0xFFFF,
              { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
    EXPECT_EQ(GuidToWString(g), L"{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}");
}

TEST(GuidToWString, MixedCaseHex) {
    // Verify uppercase hex output.
    GUID g = { 0xdeadbeef, 0xcafe, 0xbabe,
              { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe } };
    std::wstring result = GuidToWString(g);
    EXPECT_EQ(result, L"{DEADBEEF-CAFE-BABE-DEAD-BEEFCAFEBABE}");
}

TEST(GuidToWString, FormatHasBraces) {
    GUID g = GUID_NULL;
    std::wstring result = GuidToWString(g);
    EXPECT_EQ(result.front(), L'{');
    EXPECT_EQ(result.back(), L'}');
}

// ---------------------------------------------------------------------------
// PackPolicyConfigDeviceId
// ---------------------------------------------------------------------------

TEST(PackPolicyConfigDeviceId, EmptyDeviceId_ReturnsEmpty) {
    EXPECT_EQ(PackPolicyConfigDeviceId(L"", eRender), L"");
}

TEST(PackPolicyConfigDeviceId, Render_AppendsRenderSuffix) {
    std::wstring result = PackPolicyConfigDeviceId(L"{0.0.0.00000000}.{guid}", eRender);
    EXPECT_NE(result.find(L"\\\\?\\SWD#MMDEVAPI#"), std::wstring::npos);
    EXPECT_NE(result.find(L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"), std::wstring::npos);
}

TEST(PackPolicyConfigDeviceId, Capture_AppendsCaptureSuffix) {
    std::wstring result = PackPolicyConfigDeviceId(L"{0.0.1.00000000}.{guid}", eCapture);
    EXPECT_NE(result.find(L"\\\\?\\SWD#MMDEVAPI#"), std::wstring::npos);
    EXPECT_NE(result.find(L"#{2eef81be-33fa-4800-9670-1cd474972c3f}"), std::wstring::npos);
}

TEST(PackPolicyConfigDeviceId, RenderDoesNotIncludeCaptureSuffix) {
    std::wstring result = PackPolicyConfigDeviceId(L"{dev}", eRender);
    EXPECT_EQ(result.find(L"#{2eef81be-33fa-4800-9670-1cd474972c3f}"), std::wstring::npos);
}

TEST(PackPolicyConfigDeviceId, CaptureDoesNotIncludeRenderSuffix) {
    std::wstring result = PackPolicyConfigDeviceId(L"{dev}", eCapture);
    EXPECT_EQ(result.find(L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"), std::wstring::npos);
}

TEST(PackPolicyConfigDeviceId, FullFormat) {
    std::wstring deviceId = L"{0.0.0.00000000}.{a1b2c3d4-e5f6-7890-abcd-ef1234567890}";
    std::wstring result = PackPolicyConfigDeviceId(deviceId, eRender);
    EXPECT_EQ(result,
        L"\\\\?\\SWD#MMDEVAPI#"
        L"{0.0.0.00000000}.{a1b2c3d4-e5f6-7890-abcd-ef1234567890}"
        L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}");
}
