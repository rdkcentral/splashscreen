/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <essos.h>

#include <stdio.h>
#ifdef FILE
#undef FILE
#endif

#include <jpeglib.h>
#include <png.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <setjmp.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

#include <unistd.h>

namespace
{
constexpr const char* kDefaultDismissFile = "/tmp/.dismissSplash";

volatile std::sig_atomic_t gShouldQuit = 0;

uint64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void onSignal(int)
{
    gShouldQuit = 1;
}

bool fileExists(const char* path)
{
    struct stat st;
    return (path != nullptr) && (lstat(path, &st) == 0);
}

struct Image
{
    int width = 0;
    int height = 0;
    // Always RGBA8 to simplify GL upload.
    std::vector<uint8_t> rgba;
};

struct JpegErrorManager
{
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void jpegErrorExit(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    longjmp(err->jump, 1);
}

std::optional<Image> decodeJpegToRgba(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
    {
        std::printf("decodeJpeg: failed to open '%s' (%s)\n", path.c_str(), std::strerror(errno));
        return std::nullopt;
    }

    jpeg_decompress_struct cinfo{};
    JpegErrorManager jerr{};

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;

    if (setjmp(jerr.jump))
    {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        std::printf("decodeJpeg: decode failed '%s'\n", path.c_str());
        return std::nullopt;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    // Request libjpeg to convert any supported input color space (including
    // CMYK/YCCK) into RGB output.
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int width = static_cast<int>(cinfo.output_width);
    const int height = static_cast<int>(cinfo.output_height);
    const int components = static_cast<int>(cinfo.output_components);

    if (width <= 0 || height <= 0 || components != 3)
    {
        std::printf("decodeJpeg: unsupported output format after conversion: %dx%d comps=%d\n", width, height, components);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return std::nullopt;
    }

    const size_t rowStride = static_cast<size_t>(width * components);

    Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    std::vector<uint8_t> row(rowStride);

    while (cinfo.output_scanline < cinfo.output_height)
    {
        JSAMPROW rowPtr = row.data();
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);

        const int y = static_cast<int>(cinfo.output_scanline - 1);
        uint8_t* dst = out.rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) * 4);

        if (components == 3)
        {
            for (int x = 0; x < width; ++x)
            {
                const uint8_t r = row[static_cast<size_t>(x) * 3 + 0];
                const uint8_t g = row[static_cast<size_t>(x) * 3 + 1];
                const uint8_t b = row[static_cast<size_t>(x) * 3 + 2];
                dst[static_cast<size_t>(x) * 4 + 0] = r;
                dst[static_cast<size_t>(x) * 4 + 1] = g;
                dst[static_cast<size_t>(x) * 4 + 2] = b;
                dst[static_cast<size_t>(x) * 4 + 3] = 0xFF;
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);

    return out;
}

struct PngReadState
{
    std::vector<uint8_t> rgba;
};

void pngReadFn(png_structp pngPtr, png_bytep outBytes, png_size_t byteCount)
{
    FILE* fp = reinterpret_cast<FILE*>(png_get_io_ptr(pngPtr));
    if (std::fread(outBytes, 1, byteCount, fp) != byteCount)
    {
        png_error(pngPtr, "pngReadFn: read error");
    }
}

std::optional<Image> decodePngToRgba(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
    {
        std::printf("decodePng: failed to open '%s' (%s)\n", path.c_str(), std::strerror(errno));
        return std::nullopt;
    }

    uint8_t signature[8]{};
    if (std::fread(signature, 1, sizeof(signature), fp) != sizeof(signature) || png_sig_cmp(signature, 0, sizeof(signature)) != 0)
    {
        std::printf("decodePng: invalid signature '%s'\n", path.c_str());
        std::fclose(fp);
        return std::nullopt;
    }

    png_structp pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!pngPtr)
    {
        std::fclose(fp);
        return std::nullopt;
    }

    png_infop infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr)
    {
        png_destroy_read_struct(&pngPtr, nullptr, nullptr);
        std::fclose(fp);
        return std::nullopt;
    }

    if (setjmp(png_jmpbuf(pngPtr)))
    {
        png_destroy_read_struct(&pngPtr, &infoPtr, nullptr);
        std::fclose(fp);
        std::printf("decodePng: decode failed '%s'\n", path.c_str());
        return std::nullopt;
    }

    png_set_read_fn(pngPtr, fp, pngReadFn);
    png_set_sig_bytes(pngPtr, 8);

    png_read_info(pngPtr, infoPtr);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bitDepth = 0;
    int colorType = 0;
    png_get_IHDR(pngPtr, infoPtr, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr);

    if (bitDepth == 16)
        png_set_strip_16(pngPtr);

    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(pngPtr);

    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(pngPtr);

    if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(pngPtr);

    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pngPtr);
    
    // Ensure RGBA.
    if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(pngPtr, 0xFF, PNG_FILLER_AFTER);

    png_read_update_info(pngPtr, infoPtr);

    const png_size_t rowBytes = png_get_rowbytes(pngPtr, infoPtr);
    if (rowBytes != width * 4)
    {
        std::printf("decodePng: unexpected rowBytes=%zu width=%u\n", static_cast<size_t>(rowBytes), width);
        png_destroy_read_struct(&pngPtr, &infoPtr, nullptr);
        std::fclose(fp);
        return std::nullopt;
    }

    Image out;
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);

    std::vector<png_bytep> rows(static_cast<size_t>(out.height));
    for (int y = 0; y < out.height; ++y)
    {
        rows[static_cast<size_t>(y)] = out.rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(out.width) * 4);
    }

    png_read_image(pngPtr, rows.data());
    png_read_end(pngPtr, nullptr);

    png_destroy_read_struct(&pngPtr, &infoPtr, nullptr);
    std::fclose(fp);

    return out;
}

bool endsWithCaseInsensitive(std::string_view s, std::string_view suffix)
{
    if (suffix.size() > s.size())
        return false;

    const auto start = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[start + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b)
            return false;
    }
    return true;
}

enum class ImageType
{
    Png,
    Jpeg,
    Unknown
};

ImageType detectImageTypeByMagic(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
    {
        std::printf("detectImageType: could not open '%s'\n", path.c_str());
        return ImageType::Unknown;
    }

    uint8_t header[8]{};
    const size_t n = std::fread(header, 1, sizeof(header), fp);
    std::fclose(fp);

    if (n >= 8 && std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0)
        return ImageType::Png;

    // JPEG files begin with 0xFF 0xD8 0xFF
    if (n >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF)
        return ImageType::Jpeg;

    return ImageType::Unknown;
}

std::optional<Image> decodeImageToRgba(const std::string& path)
{
    if (endsWithCaseInsensitive(path, ".png"))
        return decodePngToRgba(path);

    if (endsWithCaseInsensitive(path, ".jpg") || endsWithCaseInsensitive(path, ".jpeg"))
        return decodeJpegToRgba(path);

    // If extension is unknown, use magic bytes to avoid noisy probe attempts.
    switch (detectImageTypeByMagic(path))
    {
        case ImageType::Png:
            return decodePngToRgba(path);
        case ImageType::Jpeg:
            return decodeJpegToRgba(path);
        default:
            break;
    }

    return std::nullopt;
}

struct GlProgram
{
    GLuint program = 0;
    GLuint texture = 0;
};

GLuint compileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log)), &len, log);
        std::printf("shader compile failed: %.*s\n", static_cast<int>(len), log);

        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

std::optional<GlProgram> createProgramAndTexture(const Image& image)
{
    static const char* kVert =
        "attribute vec4 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main(){ gl_Position = a_pos; v_uv = a_uv; }\n";

    static const char* kFrag =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main(){ gl_FragColor = texture2D(u_tex, v_uv); }\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs)
    {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return std::nullopt;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);

    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");

    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, static_cast<GLsizei>(sizeof(log)), &len, log);
        std::printf("program link failed: %.*s\n", static_cast<int>(len), log);
        glDeleteProgram(prog);
        return std::nullopt;
    }

    glUseProgram(prog);

    const GLint texLoc = glGetUniformLocation(prog, "u_tex");
    glUniform1i(texLoc, 0);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());

    GlProgram out;
    out.program = prog;
    out.texture = tex;
    return out;
}

void destroyProgramAndTexture(GlProgram& p)
{
    if (p.texture)
        glDeleteTextures(1, &p.texture);
    if (p.program)
        glDeleteProgram(p.program);
    p.texture = 0;
    p.program = 0;
}

struct Options
{
    std::string imagePath;
    std::string dismissFile = kDefaultDismissFile;
};

Image makeSolidColorRgbaImage(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            img.rgba[i + 0] = r;
            img.rgba[i + 1] = g;
            img.rgba[i + 2] = b;
            img.rgba[i + 3] = a;
        }
    }

    return img;
}

void printUsage()
{
    std::printf(
    "Usage: rdke_splash [--image <path.jpg|path.png>] [--dismiss-file <path>]\n"
        "\n"
        "Runs a fullscreen OpenGL ES splash screen until the dismiss file exists.\n"
    "If --image is omitted (or decode fails), a solid-color fallback splash is shown.\n"
        "Defaults: --dismiss-file /tmp/.dismissSplash\n");
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options opt;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--image")
        {
            if (i + 1 >= argc)
                return std::nullopt;
            opt.imagePath = argv[++i];
        }
        else if (a == "--dismiss-file")
        {
            if (i + 1 >= argc)
                return std::nullopt;
            opt.dismissFile = argv[++i];
        }
        else if (a == "-h" || a == "--help")
        {
            return std::nullopt;
        }
        else
        {
            std::printf("Unknown arg: %s\n", argv[i]);
            return std::nullopt;
        }
    }

    return opt;
}

struct DisplayState
{
    int width = 0;
    int height = 0;
};

void onDisplaySize(void* userData, int w, int h)
{
    auto* state = static_cast<DisplayState*>(userData);
    if (state->width != w || state->height != h)
    {
        state->width = w;
        state->height = h;
        std::printf("display size: %dx%d\n", w, h);
    }
}

void onEssosTerminated(void*)
{
    gShouldQuit = 1;
}

EssTerminateListener kTermListener = []() {
    EssTerminateListener l{};
    l.terminated = onEssosTerminated;
    return l;
}();

EssSettingsListener kSettingsListener = []() {
    EssSettingsListener l{};
    l.displaySize = onDisplaySize;
    return l;
}();

} // namespace

int main(int argc, char** argv)
{
    const uint64_t startMs = nowMs();

    const auto opt = parseArgs(argc, argv);
    if (!opt)
    {
        printUsage();
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    EssCtx* ctx = EssContextCreate();
    if (!ctx)
    {
        std::printf("EssContextCreate failed\n");
        return 3;
    }

    bool ok = true;
    DisplayState display;

    if (!EssContextSetTerminateListener(ctx, nullptr, &kTermListener))
        ok = false;
    if (!EssContextSetSettingsListener(ctx, &display, &kSettingsListener))
        ok = false;

    if (ok && !EssContextInit(ctx))
    {
        std::printf("EssContextInit failed\n");
        ok = false;
    }

    if (ok && !EssContextGetDisplaySize(ctx, &display.width, &display.height))
    {
        std::printf("EssContextGetDisplaySize failed\n");
        ok = false;
    }

    if (ok && !EssContextSetInitialWindowSize(ctx, display.width, display.height))
    {
        std::printf("EssContextSetInitialWindowSize failed\n");
        ok = false;
    }

    if (ok && !EssContextStart(ctx))
    {
        std::printf("EssContextStart failed\n");
        ok = false;
    }

    std::optional<Image> image;
    const uint64_t decodeStartMs = nowMs();
    if (!opt->imagePath.empty())
    {
        image = decodeImageToRgba(opt->imagePath);
        if (!image)
        {
            std::printf("Failed to decode image: %s (using fallback)\n", opt->imagePath.c_str());
        }
    }

    if (!image)
    {
        // Match legacy behavior: show a solid red splash if no image.
        // A 1x1 solid texture renders identically on the full-screen quad while
        // avoiding a large fallback allocation and texture upload.
        image = makeSolidColorRgbaImage(1, 1, 0xFF, 0x00, 0x00, 0xFF);
    }
    const uint64_t decodeMs = nowMs() - decodeStartMs;

    GlProgram gl{};
    const uint64_t glStartMs = nowMs();
    if (ok)
    {
        auto prog = createProgramAndTexture(*image);
        if (!prog)
        {
            ok = false;
        }
        else
        {
            gl = *prog;
        }
    }
    const uint64_t glSetupMs = nowMs() - glStartMs;

    if (!ok)
    {
        const char* detail = EssContextGetLastErrorDetail(ctx);
        std::printf("Startup failed. Essos detail: %s\n", (detail ? detail : "(none)"));
        destroyProgramAndTexture(gl);
        EssContextDestroy(ctx);
        return 4;
    }

    static const GLfloat kUvs[4][2] = {
        {0.f, 1.f},
        {1.f, 1.f},
        {0.f, 0.f},
        {1.f, 0.f},
    };

    glUseProgram(gl.program);
    glDisable(GL_BLEND);

    // Use black bars for any letterbox/pillarbox regions.
    glClearColor(0.f, 0.f, 0.f, 1.f);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, kUvs);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    bool firstFrame = true;
    while (!gShouldQuit)
    {
        if (fileExists(opt->dismissFile.c_str()))
            break;

        // Preserve aspect ratio by fitting the image into the display with
        // letterboxing/pillarboxing.
        GLfloat scaleX = 1.0f;
        GLfloat scaleY = 1.0f;
        if (display.width > 0 && display.height > 0 && image->width > 0 && image->height > 0)
        {
            const double displayAspect = static_cast<double>(display.width) / static_cast<double>(display.height);
            const double imageAspect = static_cast<double>(image->width) / static_cast<double>(image->height);
            if (imageAspect > displayAspect)
            {
                scaleY = static_cast<GLfloat>(displayAspect / imageAspect);
            }
            else
            {
                scaleX = static_cast<GLfloat>(imageAspect / displayAspect);
            }
        }

        const GLfloat verts[4][2] = {
            {-scaleX, -scaleY},
            {scaleX, -scaleY},
            {-scaleX, scaleY},
            {scaleX, scaleY},
        };

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);

        glViewport(0, 0, display.width, display.height);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (firstFrame)
        {
            firstFrame = false;
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "RDKE_Splash first_frame_ms=%llu decode_ms=%llu gl_setup_ms=%llu img=%dx%d",
                          static_cast<unsigned long long>(nowMs() - startMs),
                          static_cast<unsigned long long>(decodeMs),
                          static_cast<unsigned long long>(glSetupMs),
                          image->width, image->height);
            std::printf("%s\n", msg);
        }

        EssContextUpdateDisplay(ctx);
        EssContextRunEventLoopOnce(ctx);

        // Reduce CPU/GPU usage while the splash is static.
        usleep(50 * 1000);
    }

    destroyProgramAndTexture(gl);
    EssContextDestroy(ctx);
    return 0;
}

