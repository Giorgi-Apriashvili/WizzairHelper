// The application icon (D-50), checked where Windows and Qt take it from:
//   - the window icon Application sets from the PNGs embedded as a Qt resource,
//   - the committed WizzairHelper.ico (src/app/resources/icon), and
//   - the icon group of the real WizzairHelper.exe, which the resource script compiles
//     from that .ico.
// Every image must be the committed PNG drawn for its size, never a resampled one.
#include "app_info.hpp"

#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QScopeGuard>
#include <QSize>
#include <QString>
#include <QWidget>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

using wizz::app::kAppIconSizes;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

[[nodiscard]] std::filesystem::path path_from_utf8(const char* utf8)
{
    return std::filesystem::path{QString::fromUtf8(utf8).toStdWString()};
}

[[nodiscard]] std::filesystem::path icon_dir()
{
    return path_from_utf8(WIZZ_TEST_ICON_DIR);
}

[[nodiscard]] std::filesystem::path committed_png_path(int size)
{
    return icon_dir() / (L"WizzairHelper-" + std::to_wstring(size) + L".png");
}

[[nodiscard]] Bytes read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] QImage committed_png(int size)
{
    return QImage(QString::fromStdWString(committed_png_path(size).wstring()), "PNG");
}

[[nodiscard]] std::set<int> all_sizes()
{
    return {kAppIconSizes.begin(), kAppIconSizes.end()};
}

[[nodiscard]] bool same_pixels(const QImage& actual, const QImage& expected)
{
    return !actual.isNull() && !expected.isNull() &&
           actual.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
               expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

[[nodiscard]] int opaque_pixel_count(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += qAlpha(image.pixel(x, y)) == 255 ? 1 : 0;
        }
    }
    return count;
}

// Little-endian fields of the icon file and resource formats.
[[nodiscard]] unsigned read_u16(const Bytes& bytes, std::size_t offset)
{
    return static_cast<unsigned>(bytes.at(offset)) | (static_cast<unsigned>(bytes.at(offset + 1)) << 8);
}

[[nodiscard]] std::uint32_t read_u32(const Bytes& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(read_u16(bytes, offset)) |
           (static_cast<std::uint32_t>(read_u16(bytes, offset + 2)) << 16);
}

/// The width or height byte of an icon directory entry: 0 stands for 256.
[[nodiscard]] int icon_side(std::uint8_t value)
{
    return value == 0 ? 256 : value;
}

[[nodiscard]] bool starts_with_png_signature(const std::uint8_t* data, std::size_t size)
{
    return size >= std::size(kPngSignature) && std::equal(std::begin(kPngSignature), std::end(kPngSignature), data);
}

TEST(AppIcon, Window_icon_offers_16_24_32_48_and_256_px)
{
    const QIcon icon = QApplication::windowIcon();
    ASSERT_FALSE(icon.isNull()) << "the application sets no window icon";
    std::set<int> available;
    for (const QSize& size : icon.availableSizes()) {
        EXPECT_EQ(size.width(), size.height());
        available.insert(size.width());
    }
    EXPECT_EQ(available, all_sizes());

    const QWidget window;  // a top-level window without an icon of its own shows the application's
    for (const int size : {16, 24, 32, 48, 256}) {
        SCOPED_TRACE(size);
        const QSize wanted(size, size);
        EXPECT_TRUE(icon.availableSizes().contains(wanted));
        const QImage image = icon.pixmap(wanted, 1.0).toImage();
        ASSERT_EQ(image.size(), wanted);
        // The art covers 31-40 % of every size with fully opaque pixels.
        EXPECT_GT(opaque_pixel_count(image), size * size / 5) << "the pixmap is (nearly) empty";
        EXPECT_TRUE(same_pixels(image, committed_png(size))) << "not the embedded PNG drawn for this size";
        EXPECT_TRUE(same_pixels(window.windowIcon().pixmap(wanted, 1.0).toImage(), committed_png(size)));
    }
}

TEST(AppIcon, Ico_file_holds_the_ten_committed_pngs)
{
    const Bytes file = read_file(icon_dir() / L"WizzairHelper.ico");
    ASSERT_GE(file.size(), 6U);
    EXPECT_EQ(read_u16(file, 0), 0U);  // reserved
    EXPECT_EQ(read_u16(file, 2), 1U);  // 1 = icon (2 would be a cursor)
    const std::size_t count = read_u16(file, 4);
    ASSERT_EQ(count, kAppIconSizes.size());
    const std::size_t directory_end = 6 + 16 * count;
    ASSERT_GE(file.size(), directory_end);

    std::set<int> sizes;
    std::vector<std::pair<std::size_t, std::size_t>> images;  // [begin, end) of every image
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t entry = 6 + 16 * i;
        const int width = icon_side(file[entry]);
        const int height = icon_side(file[entry + 1]);
        SCOPED_TRACE(width);
        EXPECT_EQ(width, height);
        EXPECT_EQ(file[entry + 2], 0) << "palette colours";
        EXPECT_EQ(file[entry + 3], 0) << "reserved";
        EXPECT_EQ(read_u16(file, entry + 4), 1U) << "planes";
        EXPECT_EQ(read_u16(file, entry + 6), 32U) << "bits per pixel";
        const std::size_t length = read_u32(file, entry + 8);
        const std::size_t offset = read_u32(file, entry + 12);
        ASSERT_GT(length, 0U);
        ASSERT_GE(offset, directory_end);
        ASSERT_LE(offset + length, file.size());
        images.emplace_back(offset, offset + length);
        EXPECT_TRUE(sizes.insert(width).second) << "a size appears twice";

        ASSERT_TRUE(starts_with_png_signature(file.data() + offset, length)) << "not a PNG-compressed entry";
        const QImage image = QImage::fromData(
            QByteArray(reinterpret_cast<const char*>(file.data() + offset), static_cast<qsizetype>(length)), "PNG");
        ASSERT_EQ(image.size(), QSize(width, height));
        EXPECT_TRUE(same_pixels(image, committed_png(width))) << "differs from WizzairHelper-" << width << ".png";
    }
    EXPECT_EQ(sizes, all_sizes());

    std::ranges::sort(images);
    for (std::size_t i = 1; i < images.size(); ++i) {
        EXPECT_LE(images[i - 1].second, images[i].first) << "images overlap";
    }
}

/// The bytes of one resource of `module`, empty when it is missing.
[[nodiscard]] Bytes resource_bytes(HMODULE module, const wchar_t* name, const wchar_t* type)
{
    HRSRC const info = FindResourceW(module, name, type);
    if (info == nullptr) {
        return {};
    }
    HGLOBAL const loaded = LoadResource(module, info);
    const auto* const data = static_cast<const std::uint8_t*>(loaded != nullptr ? LockResource(loaded) : nullptr);
    if (data == nullptr) {
        return {};
    }
    return Bytes(data, data + SizeofResource(module, info));
}

BOOL CALLBACK collect_resource_name(HMODULE /*module*/, const wchar_t* /*type*/, wchar_t* name, LONG_PTR names)
{
    std::wstring text = IS_INTRESOURCE(name) ? L"#" + std::to_wstring(reinterpret_cast<std::uintptr_t>(name)) : name;
    reinterpret_cast<std::vector<std::wstring>*>(names)->push_back(std::move(text));
    return TRUE;
}

TEST(AppIcon, Exe_icon_group_holds_the_ten_sizes)
{
    const std::filesystem::path exe = path_from_utf8(WIZZ_TEST_APP_EXE);
    HMODULE const module =
        LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    ASSERT_NE(module, nullptr) << "cannot open " << exe.string() << " (error " << GetLastError() << ")";
    const auto unload = qScopeGuard([module] { FreeLibrary(module); });

    // Explorer, shortcuts, the taskbar and Apps & Features show the first icon group.
    std::vector<std::wstring> groups;
    EnumResourceNamesW(module, RT_GROUP_ICON, collect_resource_name, reinterpret_cast<LONG_PTR>(&groups));
    ASSERT_EQ(groups, std::vector<std::wstring>{L"IDI_ICON1"}) << "the exe has no (or another) icon group";

    const Bytes group = resource_bytes(module, L"IDI_ICON1", RT_GROUP_ICON);
    ASSERT_GE(group.size(), 6U);
    EXPECT_EQ(read_u16(group, 0), 0U);
    EXPECT_EQ(read_u16(group, 2), 1U);
    const std::size_t count = read_u16(group, 4);
    ASSERT_EQ(count, kAppIconSizes.size());
    ASSERT_EQ(group.size(), 6 + 14 * count);  // GRPICONDIRENTRY: 14 bytes, the image's ID last

    std::set<int> sizes;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t entry = 6 + 14 * i;
        const int width = icon_side(group[entry]);
        SCOPED_TRACE(width);
        EXPECT_EQ(icon_side(group[entry + 1]), width);
        EXPECT_EQ(read_u16(group, entry + 4), 1U) << "planes";
        EXPECT_EQ(read_u16(group, entry + 6), 32U) << "bits per pixel";
        EXPECT_TRUE(sizes.insert(width).second) << "a size appears twice";
        const auto id = static_cast<WORD>(read_u16(group, entry + 12));
        const Bytes image = resource_bytes(module, MAKEINTRESOURCEW(id), RT_ICON);
        EXPECT_EQ(image.size(), read_u32(group, entry + 8));
        EXPECT_TRUE(image == read_file(committed_png_path(width)))
            << "icon image " << id << " is not WizzairHelper-" << width << ".png";
    }
    EXPECT_EQ(sizes, all_sizes());
}

} // namespace
