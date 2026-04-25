// test_device_config_defaults.cpp
// Tests for DeviceConfig struct default values and enum behavior.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// Default construction
// ---------------------------------------------------------------------------

TEST(DeviceConfigDefaults, DefaultFilterIsAll) {
    DeviceConfig cfg;
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

TEST(DeviceConfigDefaults, DefaultOrdinalIsMinusOne) {
    DeviceConfig cfg;
    EXPECT_EQ(cfg.ordinal, -1);
}

TEST(DeviceConfigDefaults, DefaultFingerprintIsEmpty) {
    DeviceConfig cfg;
    EXPECT_TRUE(cfg.fingerprint.empty());
}

TEST(DeviceConfigDefaults, DefaultNameIsEmpty) {
    DeviceConfig cfg;
    EXPECT_TRUE(cfg.name.empty());
}

TEST(DeviceConfigDefaults, DefaultOfflineIsFalse) {
    DeviceConfig cfg;
    EXPECT_FALSE(cfg.offline);
}

// ---------------------------------------------------------------------------
// Copy semantics
// ---------------------------------------------------------------------------

TEST(DeviceConfigDefaults, CopyConstructorPreservesAllFields) {
    DeviceConfig original;
    original.name = L"Test Device";
    original.filter = DeviceConfig::Filter::StreamOnly;
    original.fingerprint = L"{GUID}%b1";
    original.ordinal = 5;
    original.offline = true;

    DeviceConfig copy = original;

    EXPECT_EQ(copy.name, original.name);
    EXPECT_EQ(copy.filter, original.filter);
    EXPECT_EQ(copy.fingerprint, original.fingerprint);
    EXPECT_EQ(copy.ordinal, original.ordinal);
    EXPECT_EQ(copy.offline, original.offline);
}

TEST(DeviceConfigDefaults, AssignmentOperatorPreservesAllFields) {
    DeviceConfig source;
    source.name = L"Source";
    source.filter = DeviceConfig::Filter::ByOrdinal;
    source.ordinal = 3;

    DeviceConfig dest;
    dest = source;

    EXPECT_EQ(dest.name, source.name);
    EXPECT_EQ(dest.filter, source.filter);
    EXPECT_EQ(dest.ordinal, source.ordinal);
}

// ---------------------------------------------------------------------------
// Enum values are stable
// ---------------------------------------------------------------------------

TEST(DeviceConfigDefaults, FilterEnumValues) {
    // Verify enum values don't accidentally shift.
    EXPECT_EQ(static_cast<int>(DeviceConfig::Filter::All), 0);
    EXPECT_EQ(static_cast<int>(DeviceConfig::Filter::ByFingerprint), 1);
    EXPECT_EQ(static_cast<int>(DeviceConfig::Filter::ByOrdinal), 2);
    EXPECT_EQ(static_cast<int>(DeviceConfig::Filter::StreamOnly), 3);
}

// ---------------------------------------------------------------------------
// Modification after default construction
// ---------------------------------------------------------------------------

TEST(DeviceConfigDefaults, CanModifyAllFields) {
    DeviceConfig cfg;
    cfg.name = L"Modified";
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"new-fingerprint";
    cfg.ordinal = 10;
    cfg.offline = true;

    EXPECT_EQ(cfg.name, L"Modified");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByFingerprint);
    EXPECT_EQ(cfg.fingerprint, L"new-fingerprint");
    EXPECT_EQ(cfg.ordinal, 10);
    EXPECT_TRUE(cfg.offline);
}
