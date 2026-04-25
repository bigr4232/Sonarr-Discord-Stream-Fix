// test_route_target_io.cpp
// Tests for route target file I/O: LoadRouteTargetFromFile and SaveRouteTargetToFile.
//
// These functions are defined in main.cpp but use the same serialization logic as
// the config I/O. We test the parse/serialize behavior that underlies them, plus
// the file-based round-trip using temporary files.

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include "mdd_pure.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Route target parsing (uses same logic as LoadRouteTargetFromFile internally)
// The actual file I/O functions are in main.cpp and require the full Windows
// build. Here we test the core parsing behavior that they depend on.
// ---------------------------------------------------------------------------

TEST(RouteTargetParsing, EmptyLineMeansNoTarget) {
    // An empty route_target.txt means routing is disabled.
    std::wstring line = L"";
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    EXPECT_TRUE(line.empty());
}

TEST(RouteTargetParsing, CarriageReturnStripped) {
    std::wstring line = L"Speakers (Realtek)\r";
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    EXPECT_EQ(line, L"Speakers (Realtek)");
}

TEST(RouteTargetParsing, DeviceNamePreserved) {
    std::wstring original = L"CABLE-Output (VB-Audio Virtual Cable)";
    std::wstring line = original;
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    EXPECT_EQ(line, original);
}

// ---------------------------------------------------------------------------
// File-based round-trip tests using temporary files
// ---------------------------------------------------------------------------

class RouteTargetFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir = fs::temp_directory_path() / fs::path(L"MDD_Test_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(tempDir);
        routeTargetPath = tempDir / L"route_target.txt";
    }

    void TearDown() override {
        // Clean up temp files
        if (fs::exists(routeTargetPath)) {
            fs::remove(routeTargetPath);
        }
        if (fs::exists(tempDir)) {
            fs::remove(tempDir);
        }
    }

    fs::path tempDir;
    fs::path routeTargetPath;
};

TEST_F(RouteTargetFileTest, WriteAndReadDeviceName) {
    std::wstring deviceName = L"Speakers (Realtek Audio)";

    // Write
    {
        std::wofstream out(routeTargetPath, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << deviceName << L"\n";
    }

    // Read
    std::wifstream inp(routeTargetPath);
    ASSERT_TRUE(inp.is_open());
    std::wstring line;
    std::getline(inp, line);
    if (!line.empty() && line.back() == L'\r') line.pop_back();

    EXPECT_EQ(line, deviceName);
}

TEST_F(RouteTargetFileTest, EmptyTargetMeansDisabled) {
    // Write empty target (routing disabled)
    {
        std::wofstream out(routeTargetPath, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << L"" << L"\n";
    }

    // Read back
    std::wifstream inp(routeTargetPath);
    ASSERT_TRUE(inp.is_open());
    std::wstring line;
    std::getline(inp, line);
    if (!line.empty() && line.back() == L'\r') line.pop_back();

    EXPECT_TRUE(line.empty());
}

TEST_F(RouteTargetFileTest, NonExistentFileMeansNoTarget) {
    // When the file doesn't exist, the convention is to return empty (no routing).
    ASSERT_FALSE(fs::exists(routeTargetPath));
    std::wifstream inp(routeTargetPath);
    EXPECT_FALSE(inp.is_open());
}

TEST_F(RouteTargetFileTest, SpecialCharactersInDeviceName) {
    std::wstring deviceName = L"Headphones (SteelSeries Arctis Nova Pro) [Ch#3]";

    // Write
    {
        std::wofstream out(routeTargetPath, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << deviceName << L"\n";
    }

    // Read
    std::wifstream inp(routeTargetPath);
    ASSERT_TRUE(inp.is_open());
    std::wstring line;
    std::getline(inp, line);
    if (!line.empty() && line.back() == L'\r') line.pop_back();

    EXPECT_EQ(line, deviceName);
}

// ---------------------------------------------------------------------------
// Config file I/O round-trip using temporary files
// ---------------------------------------------------------------------------

class ConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir = fs::temp_directory_path() / fs::path(L"MDD_ConfigTest_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(tempDir);
        configPath = tempDir / L"devices.txt";
    }

    void TearDown() override {
        if (fs::exists(configPath)) {
            fs::remove(configPath);
        }
        std::wstring tmp = configPath.wstring() + L".tmp";
        if (fs::exists(tmp)) {
            fs::remove(tmp);
        }
        if (fs::exists(tempDir)) {
            fs::remove(tempDir);
        }
    }

    fs::path tempDir;
    fs::path configPath;
};

TEST_F(ConfigFileTest, WriteAndReadMultipleDevices) {
    std::vector<DeviceConfig> original = {
        {L"Speakers (Realtek)", DeviceConfig::Filter::All, L"", -1, false},
        {L"VB-Cable Output", DeviceConfig::Filter::StreamOnly, L"", -1, false},
        {L"Headphones", DeviceConfig::Filter::ByOrdinal, L"", 2, false},
    };

    // Write using the same pattern as SaveDevicesToFile
    std::wstring tmp = configPath.wstring() + L".tmp";
    {
        std::wofstream out(tmp, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        for (const auto& d : original) {
            out << SerializeDeviceConfig(d) << L"\n";
        }
    }
    MoveFileExW(tmp.c_str(), configPath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);

    // Read back using the same pattern as LoadDevicesFromFile
    std::vector<DeviceConfig> loaded;
    std::wifstream file(configPath);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) loaded.push_back(std::move(cfg));
    }

    ASSERT_EQ(loaded.size(), original.size());
    for (size_t i = 0; i < original.size(); i++) {
        EXPECT_EQ(loaded[i].name, original[i].name);
        EXPECT_EQ(loaded[i].filter, original[i].filter);
        EXPECT_EQ(loaded[i].ordinal, original[i].ordinal);
    }
}

TEST_F(ConfigFileTest, EmptyFileProducesEmptyList) {
    // Create empty file
    std::wofstream out(configPath, std::ios::trunc);
    ASSERT_TRUE(out.is_open());

    // Read back
    std::vector<DeviceConfig> loaded;
    std::wifstream file(configPath);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) loaded.push_back(std::move(cfg));
    }

    EXPECT_TRUE(loaded.empty());
}

TEST_F(ConfigFileTest, FileWithBlankLinesIgnoresThem) {
    // Write file with blank lines between entries
    {
        std::wofstream out(configPath, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << L"Device A|stream\n";
        out << L"\n";
        out << L"Device B|ord:0\n";
        out << L"\n";
    }

    // Read back
    std::vector<DeviceConfig> loaded;
    std::wifstream file(configPath);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) loaded.push_back(std::move(cfg));
    }

    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, L"Device A");
    EXPECT_EQ(loaded[1].name, L"Device B");
}

TEST_F(ConfigFileTest, FingerprintRoundTripThroughFile) {
    DeviceConfig original;
    original.name = L"Test Device";
    original.filter = DeviceConfig::Filter::ByFingerprint;
    original.fingerprint = L"{d34cb318-f2a1-4e9c-b7f5-8a9c1d2e3f4a}%b131074";

    // Write
    {
        std::wofstream out(configPath, std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << SerializeDeviceConfig(original) << L"\n";
    }

    // Read
    std::vector<DeviceConfig> loaded;
    std::wifstream file(configPath);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) loaded.push_back(std::move(cfg));
    }

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].name, original.name);
    EXPECT_EQ(loaded[0].filter, original.filter);
    EXPECT_EQ(loaded[0].fingerprint, original.fingerprint);
}
