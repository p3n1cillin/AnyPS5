#include "prx/libSceFont/include/FontTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

extern "C" {
int APS5_VABI sceFontMemoryInit(FontMemory*, void*, std::uint32_t, const FontMemoryInterface*, void*, FontMemoryDestroyFunction, void*);
int APS5_VABI sceFontCreateLibrary(const FontMemory*, const void*, FontLibrary*);
int APS5_VABI sceFontDestroyLibrary(FontLibrary*);
int APS5_VABI sceFontSupportExternalFonts(FontLibrary, std::uint32_t, std::uint32_t);
int APS5_VABI sceFontOpenFontMemory(FontLibrary, const void*, std::uint32_t, const FontOpenDetail*, FontHandle*);
int APS5_VABI sceFontCloseFont(FontHandle);
int APS5_VABI sceFontTextSourceInit(FontTextSource*, const void*, std::uint32_t, FontTextParseFunction, void*);
int APS5_VABI sceFontTextSourceSetDefaultFont(FontTextSource*, FontHandle);
int APS5_VABI sceFontCreateString(const FontMemory*, FontTextSource*, const FontCreateStringDetail*, FontString*);
int APS5_VABI sceFontDestroyString(FontString*);
FontTextCharacter* APS5_VABI sceFontStringRefersTextCharacters(FontString, std::uint32_t*);
FontTextCodes* APS5_VABI sceFontCharactersRefersTextCodes(FontTextCharacter*, FontTextCharacter*, FontTextCodes*);
FontTextCodes* APS5_VABI sceFontTextCodesStepNext(FontTextCodes*);
FontTextCodes* APS5_VABI sceFontTextCodesStepBack(FontTextCodes*);
const void* APS5_VABI sceFontSelectLibraryFt(int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Font glyph and text check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static void* APS5_VABI Allocate(void*, std::uint32_t size) {
    return std::malloc(size);
}
static void APS5_VABI Release(void*, void* pointer) {
    std::free(pointer);
}

static void Put16(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

static void Put32(std::vector<unsigned char>& out, std::uint32_t value) {
    Put16(out, value >> 16);
    Put16(out, value & 0xFFFFu);
}

static void PutAll16(std::vector<unsigned char>& out, std::initializer_list<int> values) {
    for (const int value : values) Put16(out, static_cast<std::uint32_t>(static_cast<std::uint16_t>(value)));
}

static std::vector<unsigned char> SquareGlyphFont(std::uint32_t codepoint, int width) {
    std::vector<unsigned char> head;
    Put32(head, 0x00010000u);
    Put32(head, 0x00010000u);
    Put32(head, 0);
    Put32(head, 0x5F0F3CF5u);
    PutAll16(head, {0x000B, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, width, 800, 0, 8, 2, 0, 0});
    std::vector<unsigned char> hhea;
    Put32(hhea, 0x00010000u);
    PutAll16(hhea, {800, -200, 0, width, 0, 0, width, 1, 0, 0, 0, 0, 0, 0, 0, 2});
    std::vector<unsigned char> maxp;
    Put32(maxp, 0x00010000u);
    PutAll16(maxp, {2, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0});
    std::vector<unsigned char> hmtx;
    PutAll16(hmtx, {width, 0, width, 100});
    std::vector<unsigned char> glyf;
    PutAll16(glyf, {1, 100, 0, width - 100, 700, 3, 0});
    glyf.insert(glyf.end(), {1, 1, 1, 1});
    PutAll16(glyf, {100, 0, width - 200, 0, 0, 700, 0, -700});
    std::vector<unsigned char> loca;
    PutAll16(loca, {0, 0, static_cast<int>(glyf.size() / 2)});
    std::vector<unsigned char> cmap;
    PutAll16(cmap, {0, 1, 3, 1});
    Put32(cmap, 12);
    PutAll16(cmap, {4, 32, 0, 4, 4, 1, 0, static_cast<int>(codepoint), 0xFFFF, 0, static_cast<int>(codepoint), 0xFFFF, 1 - static_cast<int>(codepoint), 1, 0, 0});
    const std::vector<std::pair<const char*, const std::vector<unsigned char>*>> tables = {
        {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea}, {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}};
    std::vector<unsigned char> font;
    Put32(font, 0x00010000u);
    PutAll16(font, {static_cast<int>(tables.size()), 64, 2, static_cast<int>(tables.size()) * 16 - 64});
    std::uint32_t offset = 12 + static_cast<std::uint32_t>(tables.size()) * 16;
    for (const auto& [tag, data] : tables) {
        font.insert(font.end(), tag, tag + 4);
        Put32(font, 0);
        Put32(font, offset);
        Put32(font, static_cast<std::uint32_t>(data->size()));
        offset += (static_cast<std::uint32_t>(data->size()) + 3u) & ~3u;
    }
    for (const auto& [tag, data] : tables) {
        font.insert(font.end(), data->begin(), data->end());
        font.resize((font.size() + 3u) & ~std::size_t{3});
    }
    return font;
}

static std::int32_t APS5_VABI ParseCodes(FontTextSource* source, void** order, FontTextParseResult* result) {
    const auto* current = static_cast<const std::uint32_t*>(source->current);
    if (current == source->end) {
        *order = const_cast<std::uint32_t*>(current);
        result->Terminate.terminateCode = 0;
        return 0;
    }
    *order = const_cast<std::uint32_t*>(current);
    result->FontCode.font = nullptr;
    result->FontCode.code = *current;
    source->current = current + 1;
    return 1;
}

static void CheckTextCodes(const FontMemory& memory, FontHandle font) {
    const std::uint32_t text[] = {'A', 'B', 'C', 0x0E01, 'D', 0x0301};
    FontTextSource source{};
    Require(sceFontTextSourceInit(&source, text, sizeof(text), ParseCodes, nullptr) == SCE_FONT_OK);
    Require(sceFontTextSourceSetDefaultFont(&source, font) == SCE_FONT_OK);
    FontString string = nullptr;
    Require(sceFontCreateString(&memory, &source, nullptr, &string) == SCE_FONT_OK);
    std::uint32_t count = 0;
    FontTextCharacter* characters = sceFontStringRefersTextCharacters(string, &count);
    Require(characters && count == 6);

    FontTextCodes codes{};
    FontTextCodes* step = sceFontCharactersRefersTextCodes(characters, nullptr, &codes);
    Require(step == &codes && step->code == 'A' && step->order == &text[0]);
    Require(sceFontTextCodesStepNext(step) == &codes && codes.code == 'B' && codes.order == &text[1]);
    Require(sceFontTextCodesStepNext(step) == &codes && codes.code == 'C' && codes.order == &text[2]);
    Require(sceFontTextCodesStepBack(step) == &codes && codes.code == 'B' && codes.order == &text[1]);
    Require(sceFontTextCodesStepBack(step) == &codes && codes.code == 'A');
    Require(sceFontTextCodesStepBack(step) == nullptr && codes.code == 'A');
    step = sceFontCharactersRefersTextCodes(characters + 1, characters + 3, &codes);
    Require(step == &codes && codes.code == 'B');
    Require(sceFontTextCodesStepNext(step) == &codes && codes.code == 'C');
    Require(sceFontTextCodesStepNext(step) == nullptr && codes.code == 'C');
    step = sceFontCharactersRefersTextCodes(characters + 4, nullptr, &codes);
    Require(sceFontTextCodesStepNext(step) == &codes && codes.code == 0x0301);
    Require(sceFontTextCodesStepNext(step) == nullptr);
    Require(sceFontCharactersRefersTextCodes(characters + 2, characters + 2, &codes) == nullptr);
    Require(sceFontCharactersRefersTextCodes(nullptr, nullptr, &codes) == nullptr);
    Require(sceFontCharactersRefersTextCodes(characters, nullptr, nullptr) == nullptr);
    Require(sceFontTextCodesStepNext(nullptr) == nullptr && sceFontTextCodesStepBack(nullptr) == nullptr);

    Require(sceFontDestroyString(&string) == SCE_FONT_OK);
}

int main() {
    const FontMemoryInterface iface{Allocate, Release, nullptr, nullptr, nullptr, nullptr};
    FontMemory memory{};
    Require(sceFontMemoryInit(&memory, nullptr, 0, &iface, nullptr, nullptr, nullptr) == SCE_FONT_OK);
    FontLibrary library = nullptr;
    Require(sceFontCreateLibrary(&memory, sceFontSelectLibraryFt(0), &library) == SCE_FONT_OK);
    Require(sceFontSupportExternalFonts(library, 4, 0x52) == SCE_FONT_OK);
    const std::vector<unsigned char> fontData = SquareGlyphFont('A', 700);
    FontHandle font = nullptr;
    Require(sceFontOpenFontMemory(library, fontData.data(), static_cast<std::uint32_t>(fontData.size()), nullptr, &font) == SCE_FONT_OK);

    CheckTextCodes(memory, font);

    Require(sceFontCloseFont(font) == SCE_FONT_OK);
    Require(sceFontDestroyLibrary(&library) == SCE_FONT_OK);
}
