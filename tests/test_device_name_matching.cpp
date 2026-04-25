// test_device_name_matching.cpp
// Tests for NameMatchesDefaultMute() - case-insensitive device name matching for defaults.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// SteelSeries Sonar matches
// ---------------------------------------------------------------------------

TEST(NameMatchesDefaultMute, ExactSonarMicrophone) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"SteelSeries Sonar - Microphone"));
}

TEST(NameMatchesDefaultMute, SonarMicrophoneLowercase) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"steelseries sonar - microphone"));
}

TEST(NameMatchesDefaultMute, SonarMicrophoneMixedCase) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"STEELSERIES SONAR - MICROPHONE"));
}

TEST(NameMatchesDefaultMute, SonarMicrophoneWithExtraText) {
    // Device names often have extra context like "(2- SteelSeries Sonar - Microphone)".
    EXPECT_TRUE(NameMatchesDefaultMute(L"(2- SteelSeries Sonar - Microphone)"));
}

TEST(NameMatchesDefaultMute, SonarMicrophoneInMiddleOfName) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"Output: SteelSeries Sonar - Microphone [Primary]"));
}

// ---------------------------------------------------------------------------
// Arctis Nova matches
// ---------------------------------------------------------------------------

TEST(NameMatchesDefaultMute, ExactArctisNova) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"Arctis Nova"));
}

TEST(NameMatchesDefaultMute, ArctisNovaElite) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"Headphones (2- Arctis Nova Elite)"));
}

TEST(NameMatchesDefaultMute, ArctisNovaProWireless) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"Arctis Nova Pro Wireless"));
}

TEST(NameMatchesDefaultMute, ArctisNovaLowercase) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"arctis nova headphones"));
}

TEST(NameMatchesDefaultMute, ArctisNovaUppercase) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"ARCTIS NOVA PRO"));
}

TEST(NameMatchesDefaultMute, ArctisNovaWithPrefix) {
    EXPECT_TRUE(NameMatchesDefaultMute(L"CABLE-Output (Arctis Nova)"));
}

// ---------------------------------------------------------------------------
// Non-matching devices
// ---------------------------------------------------------------------------

TEST(NameMatchesDefaultMute, RealtekSpeakers_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"Speakers (Realtek Audio)"));
}

TEST(NameMatchesDefaultMute, HeadsetGeneric_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"Headset (Logitech G Pro)"));
}

TEST(NameMatchesDefaultMute, VB_Cable_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"CABLE-Output (VB-Audio Virtual Cable)"));
}

TEST(NameMatchesDefaultMute, Voicemeeter_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"Voicemeeter Output"));
}

TEST(NameMatchesDefaultMute, EmptyString_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L""));
}

TEST(NameMatchesDefaultMute, JustArctis_DoesNotMatch) {
    // "Arctis" alone should not match - need "arctis nova".
    EXPECT_FALSE(NameMatchesDefaultMute(L"SteelSeries Arctis 7"));
}

TEST(NameMatchesDefaultMute, JustNova_DoesNotMatch) {
    // "Nova" alone should not match.
    EXPECT_FALSE(NameMatchesDefaultMute(L"Nova Audio System"));
}

TEST(NameMatchesDefaultMute, JustSonar_DoesNotMatch) {
    // "Sonar" alone should not match - need full "steelseries sonar - microphone".
    EXPECT_FALSE(NameMatchesDefaultMute(L"Sonar Audio Output"));
}

TEST(NameMatchesDefaultMute, SteelSeriesButNotSonarOrNova) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"SteelSeries Headset"));
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(NameMatchesDefaultMute, WhitespaceOnly_DoesNotMatch) {
    EXPECT_FALSE(NameMatchesDefaultMute(L"   "));
}

TEST(NameMatchesDefaultMute, SpecialCharactersInName) {
    // Device names can contain special characters.
    EXPECT_TRUE(NameMatchesDefaultMute(L"[Arctis Nova Pro] + Wireless"));
}
