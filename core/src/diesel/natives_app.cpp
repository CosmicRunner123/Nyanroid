// SQApp: the scripts' handle on the app (muramasa_en.exe 0x4efc90 makes it; a task object). Time,
// input, the cursor and window, fonts, sound volumes and the system settings. Addresses are
// Muramasa's; signatures as its SqPlus dispatchers check them.
//
// Input reaches the scripts through the task that made the object (CTask: mouse, touch, keyboard and
// gamepad at app+0x38 / +0x3c / +0x4c / +0x40); this PC has a mouse and keyboard, no touch screen and
// no gamepad, as Nyanroid's front ends present them.

#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>

#include "natives.hpp"

namespace nyan::diesel {

namespace {

struct AppObject final : NativeObject {
    std::string name;  // the constructor's argument ("App")
};

NativeObject* make(HSQUIRRELVM v) {
    auto* o = new AppObject;
    o->name = argString(v, 2);
    return o;
}

SQInteger pushInt(HSQUIRRELVM v, SQInteger i) {
    sq_pushinteger(v, i);
    return 1;
}
SQInteger pushFloat(HSQUIRRELVM v, SQFloat f) {
    sq_pushfloat(v, f);
    return 1;
}
SQInteger pushString(HSQUIRRELVM v, const std::string& s) {
    sq_pushstring(v, s.c_str(), static_cast<SQInteger>(s.size()));
    return 1;
}

// --- Time -----------------------------------------------------------------------------------------

// getFrameSpeed(): float (0x4f06b0, app+0x100): the last frame's length in 60 Hz frames.
SQInteger getFrameSpeed(HSQUIRRELVM v) { return pushFloat(v, host(v).clock.frameSpeed); }
// getMilliTime(): int (0x4f06c0): the core's time stamp of this frame (core+0x24).
SQInteger getMilliTime(HSQUIRRELVM v) { return pushInt(v, static_cast<SQInteger>(host(v).clock.milliTime)); }
// getSystemTime(): int (0x4f06d0): the window's clock read now (QueryPerformanceCounter in ms); the
// frame's time here, which keeps runs reproducible.
SQInteger getSystemTime(HSQUIRRELVM v) { return pushInt(v, static_cast<SQInteger>(host(v).clock.milliTime)); }
// setMaxFrameTime(ms: float) (0x4f3170): raises this frame's tween step limit (app+0x108); a negative
// value stops tweens for the frame; once negative it stays so until the next frame.
SQInteger setMaxFrameTime(HSQUIRRELVM v) {
    auto& c = host(v).clock;
    float t = argFloat(v, 2);
    if (c.maxFrameTime >= 0.0f) {
        if (t < 0.0f) {
            c.maxFrameTime = t;
        } else if (c.maxFrameTime <= t) {
            c.maxFrameTime = t;
        }
    }
    return 0;
}
// setFrameRate(fps: int): int (0x4f3150 -> core 0x10035b50): the rate the core paces frames to.
SQInteger setFrameRate(HSQUIRRELVM v) {
    auto& h = host(v);
    h.rateTime = h.clock.milliTime;
    h.rateFrame = h.frame;
    h.frameRate = static_cast<int32_t>(argInt(v, 2));
    return pushInt(v, 0);
}
SQInteger getFrameRate(HSQUIRRELVM v) { return pushInt(v, host(v).frameRate); }

// --- Keyboard -------------------------------------------------------------------------------------

// isKeyDown(key, interval): int (0x4f07b0 -> CTask 0x485e00 with mode 0): the key is down, at most
// once per `interval` ms while held.
SQInteger isKeyDown(HSQUIRRELVM v) {
    auto& h = host(v);
    bool down = h.input.repeat.isKeyDown(h.input.keyboard, static_cast<int>(argInt(v, 2)), h.clock.milliTime,
                                         static_cast<uint32_t>(argInt(v, 3)));
    return pushInt(v, down ? 1 : 0);
}
// isKeyUp(key): int (0x4f07d0 -> CTask 0x485e70 -> CKeyboard 0x10043c60).
SQInteger isKeyUp(HSQUIRRELVM v) {
    return pushInt(v, host(v).input.keyboard.isUp(static_cast<int>(argInt(v, 2))) ? 1 : 0);
}
// getKeyState(key): int (0x4f07e0 -> CKeyboard 0x10043d40): 0, 1 pressed, 2 held, 3 released.
SQInteger getKeyState(HSQUIRRELVM v) { return pushInt(v, host(v).input.keyboard.state(static_cast<int>(argInt(v, 2)))); }

// --- Mouse ----------------------------------------------------------------------------------------

// getCursorPosition() (0x4f06e0): the pointer in screen pixels to SQMATH_RET_1 / _2.
SQInteger getCursorPosition(HSQUIRRELVM v) {
    auto& h = host(v);
    h.setResult(static_cast<float>(h.input.mouse.x()), static_cast<float>(h.input.mouse.y()));
    return 0;
}
// setCursorPosition(x, y) (0x4f0780 -> CMouse 0x100112f0): moves the pointer there.
SQInteger setCursorPosition(HSQUIRRELVM v) {
    host(v).input.mouse.moveTo(static_cast<int>(argInt(v, 2)), static_cast<int>(argInt(v, 3)));
    return 0;
}
SQInteger getMouseButtonStateL(HSQUIRRELVM v) { return pushInt(v, host(v).input.mouse.buttonState(Mouse::Left)); }
SQInteger getMouseButtonStateR(HSQUIRRELVM v) { return pushInt(v, host(v).input.mouse.buttonState(Mouse::Right)); }
SQInteger getMouseWheelState(HSQUIRRELVM v) { return pushInt(v, host(v).input.mouse.wheelState()); }
// getMouseMove() (0x4f08b0): this frame's pointer movement to SQMATH_RET_1 / _2.
SQInteger getMouseMove(HSQUIRRELVM v) {
    auto& h = host(v);
    h.setResult(static_cast<float>(h.input.mouse.moveX()), static_cast<float>(h.input.mouse.moveY()));
    return 0;
}
SQInteger clearMouseInput(HSQUIRRELVM v) {
    host(v).input.mouse.clear();
    return 0;
}

// --- Touch and gamepad: none on this PC -------------------------------------------------------------

SQInteger returnZero(HSQUIRRELVM v) { return pushInt(v, 0); }
SQInteger returnZeroFloat(HSQUIRRELVM v) { return pushFloat(v, 0.0f); }
SQInteger returnNothing(HSQUIRRELVM) { return 0; }
// cvttss2si: the float truncated, 0x80000000 for NaN or out of range.
int32_t truncateFloat(float f) {
    if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) return INT32_MIN;
    return static_cast<int32_t>(f);
}
// Vector results of absent devices: (0, 0).
SQInteger zeroVector(HSQUIRRELVM v) {
    host(v).setResult(0.0f, 0.0f);
    return 0;
}

// --- Cursor and window ----------------------------------------------------------------------------

// showCursor(show) (0x4f28d0): ShowCursor until the count is on the asked side.
SQInteger showCursor(HSQUIRRELVM v) {
    host(v).cursorVisible = argInt(v, 2) != 0;
    return 0;
}
// setCursor(shape) / getCursor() (0x4f2910 / 0x4f2930, app+0xcc): 1 arrow, 7 hand (0x4017c0).
SQInteger setCursor(HSQUIRRELVM v) {
    host(v).cursor = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger getCursor(HSQUIRRELVM v) { return pushInt(v, host(v).cursor); }
// setWindowTitle(title): int (0x42ef00): the title, read as UTF-8; 0 on success.
SQInteger setWindowTitle(HSQUIRRELVM v) {
    host(v).windowTitle = argString(v, 2);
    return pushInt(v, 0);
}
SQInteger isFullScreen(HSQUIRRELVM v) { return pushInt(v, host(v).fullScreen ? 1 : 0); }
// setFullScreen(on): int (0x4f3110): a settings change the frame loop applies.
SQInteger setFullScreen(HSQUIRRELVM v) {
    auto& c = host(v).config;
    int32_t on = static_cast<int32_t>(argInt(v, 2));
    if (c.fullScreen != on) {
        c.fullScreen = on;
        c.dirty = 1;
    }
    return pushInt(v, 0);
}
// getPlatformName(): string (0x4f32b0 -> CWindow 0x100387f0).
SQInteger getPlatformName(HSQUIRRELVM v) { return pushString(v, "Windows"); }
// getProductName(): string (0x4f32d0): the app object's name (ScriptHost's constructor).
SQInteger getProductName(HSQUIRRELVM v) { return pushString(v, host(v).appName); }
// fadeoutScreen(ms) / fadeoutColor(ms, r, g, b) (0x4f2940 / 0x4f2970 -> 0x42ec00): the app's screen
// fade shows the last frame shown (asked for here, copied in by the next draw: CApp +0xf8), or a
// colour filled in at once (bytes: (char)(int)(c * 255)), and fades from 1 to 0 over ms, linearly;
// its scale (1, 1) and normal blend are set again.
void startScreenFade(ScriptHost::Impl& h, int32_t ms) {
    auto& f = h.screenFade;
    f.fade.moveTo({1.0f, 0.0f, 0.0f}, 0, 0);
    f.fade.moveTo({0.0f, 0.0f, 0.0f}, ms, 0);
}
SQInteger fadeoutScreen(HSQUIRRELVM v) {
    auto& h = host(v);
    h.screenFade.captureRequested = true;
    startScreenFade(h, argInt(v, 2));
    return 0;
}
SQInteger fadeoutColor(HSQUIRRELVM v) {
    auto& h = host(v);
    auto& f = h.screenFade;
    f.colour = true;
    for (int i = 0; i < 3; ++i) {
        auto byte = static_cast<uint8_t>(static_cast<int32_t>(argFloat(v, 3 + i) * 255.0f));
        f.rgb[static_cast<size_t>(i)] = static_cast<float>(byte) / 255.0f;
    }
    startScreenFade(h, argInt(v, 2));
    return 0;
}
// exit() (0x4f3320; totono_en.exe 0x4c41e0, phantom.exe 0x140110710): drops the window's close
// callback (its vtable +0xa4 with null) and posts it WM_CLOSE (the core's ICore +0xc, Mware
// 0x100359f0: PostMessageA(hwnd, 0x10, 0, 0)). The frame goes on; the window closes at the next
// pump, and no frame follows (ScriptHost::frame).
SQInteger exitApp(HSQUIRRELVM v) {
    auto& h = host(v);
    if (h.hasCloseCallback) sq_release(v, &h.closeCallback);
    sq_resetobject(&h.closeCallback);
    h.hasCloseCallback = false;
    h.quit = true;
    return 0;
}
// setCloseCallback(fn | null) (0x4f35b0): what the window's close button calls instead of quitting.
SQInteger setCloseCallback(HSQUIRRELVM v) {
    auto& h = host(v);
    if (h.hasCloseCallback) sq_release(v, &h.closeCallback);
    sq_resetobject(&h.closeCallback);
    h.hasCloseCallback = false;
    if (argCount(v) >= 1 && sq_gettype(v, 2) != OT_NULL) {
        sq_getstackobj(v, 2, &h.closeCallback);
        sq_addref(v, &h.closeCallback);
        h.hasCloseCallback = true;
    }
    return 0;
}
// createDirectory(path): int (0x4f31c0 -> Mware 0x10008930): 0 if made or already there, else -1.
SQInteger createDirectory(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string path = argString(v, 2);
    if (path.empty()) return pushInt(v, -1);
    return pushInt(v, h.saves->makeDirectory(h.writablePath(path)) ? 0 : -1);
}
// getFileTime(path): table (0x4f31e0 -> Mware 0x10009250): is_exist and the local time last written.
SQInteger getFileTime(HSQUIRRELVM v) {
    auto& h = host(v);
    auto t = h.saves->modifiedTime(h.writablePath(argString(v, 2)));
    std::tm tm{};
    if (t) {
        std::time_t tt = static_cast<std::time_t>(*t);
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
    }
    sq_newtable(v);
    auto put = [&](const char* k, SQInteger value) {
        sq_pushstring(v, k, -1);
        sq_pushinteger(v, value);
        sq_newslot(v, -3, SQFalse);
    };
    put("is_exist", t ? 1 : 0);
    put("year", t ? tm.tm_year + 1900 : 0);
    put("month", t ? tm.tm_mon + 1 : 0);
    put("day", t ? tm.tm_mday : 0);
    put("weekday", t ? tm.tm_wday : 0);
    put("hour", t ? tm.tm_hour : 0);
    put("min", t ? tm.tm_min : 0);
    put("sec", t ? tm.tm_sec : 0);
    return 1;
}

// --- Fonts ----------------------------------------------------------------------------------------

// loadFont(name, file): int (0x4f21a0 -> font pool 0x44f520): registers a font file under a name.
SQInteger loadFont(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string name = argString(v, 2), file = argString(v, 3);
    bool ok = h.readAnyFile(file).has_value();
    if (!ok) h.note(h.missing, file);
    ScriptHost::Impl::Font f;
    f.file = file;
    h.fonts[name] = f;
    h.fontInstances.erase(name);
    return pushInt(v, ok ? 1 : 0);
}
// loadSystemFont: a Windows font by name. Muramasa's takes a table (0x4f22c0 -> 0x44f630: name,
// system_font_name, size (24), weight (400), charset, vertical, noAA and bias) and answers an int;
// Totono's takes them in order (0x4c3490: name, system_font_name, size, weight, charset, vertical,
// noAA) and answers a bool.
SQInteger loadSystemFont(HSQUIRRELVM v) {
    auto& h = host(v);
    ScriptHost::Impl::Font f;
    std::string name;
    if (argCount(v) >= 2 && sq_gettype(v, 2) == OT_STRING) {
        name = argString(v, 2);
        f.systemName = argString(v, 3);
        f.size = static_cast<int32_t>(argInt(v, 4, f.size));
        f.weight = static_cast<int32_t>(argInt(v, 5, f.weight));
        f.charset = static_cast<int32_t>(argInt(v, 6, f.charset));
        f.vertical = static_cast<int32_t>(argInt(v, 7, f.vertical));
        f.noAA = static_cast<int32_t>(argInt(v, 8, f.noAA));
        h.fonts[name] = f;
        h.fontInstances.erase(name);
        sq_pushbool(v, SQTrue);
        return 1;
    }
    if (argCount(v) >= 1 && sq_gettype(v, 2) == OT_TABLE) {
        auto field = [&](const char* key) {
            sq_pushstring(v, key, -1);
            if (SQ_FAILED(sq_get(v, 2))) return false;
            return true;
        };
        auto intField = [&](const char* key, int32_t& out) {
            if (!field(key)) return;
            SQInteger i = 0;
            if (SQ_SUCCEEDED(sq_getinteger(v, -1, &i))) out = static_cast<int32_t>(i);
            sq_pop(v, 1);
        };
        auto stringField = [&](const char* key, std::string& out) {
            if (!field(key)) return;
            const SQChar* s = nullptr;
            if (sq_gettype(v, -1) == OT_STRING && SQ_SUCCEEDED(sq_getstring(v, -1, &s))) out = s;
            sq_pop(v, 1);
        };
        stringField("name", name);
        stringField("system_font_name", f.systemName);
        intField("size", f.size);
        intField("weight", f.weight);
        intField("charset", f.charset);
        intField("vertical", f.vertical);
        intField("noAA", f.noAA);
        if (field("bias")) {
            SQFloat b = 0;
            if (SQ_SUCCEEDED(sq_getfloat(v, -1, &b))) f.bias = b;
            sq_pop(v, 1);
        }
    }
    h.fonts[name] = f;
    h.fontInstances.erase(name);
    return pushInt(v, 1);
}
// clearFontCache(name) (0x4f2750) / unloadFont(name) (0x4f2800).
SQInteger clearFontCache(HSQUIRRELVM) { return 0; }
SQInteger unloadFont(HSQUIRRELVM v) {
    host(v).fonts.erase(argString(v, 2));
    host(v).fontInstances.erase(argString(v, 2));
    return 0;
}

// --- Textures, particles, tasks, sounds -----------------------------------------------------------

// loadTexture(file): int (0x4f1000 -> CApp 0x484f00): a name not in the app's texture map is loaded
// (the graphics' vtable +100) and listed, even when nothing loads (0x484e10 keeps a null texture):
// 1; a name already listed: 0. Nothing draws from the map; it keeps textures loaded.
SQInteger loadTexture(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string file = argString(v, 2);
    bool fresh = h.appTextures.insert(file).second;
    int w = 0, ht = 0;
    if (fresh) h.textureSize(file, w, ht);  // the load (a missing file is noted)
    return pushInt(v, fresh ? 1 : 0);
}
// unloadTexture(file) (0x4f20d0): the name leaves the map (its texture released).
SQInteger unloadTexture(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string file = argString(v, 2);
    h.appTextures.erase(file);
    h.textTextures.erase(file);
    return 0;
}

// loadTextTexture(font, setting, name): int (muramasa_en.exe 0x4f10a0, phantom.exe 0x14010d540:
// the same keys, defaults and quirks). The text is the setting's "text" when the name holds
// "text://", else the file of that name (its bytes up to the first NUL; 0 if it does not open);
// an empty text answers 0. The older text code lays it out and draws it into a texture
// (renderTextTexture), which goes into the app's texture map under the name unless the name is
// there already (0x484e10; the new texture is dropped); 1 either way.
//
// The setting (read only from a table; numbers as SqPlus converts them, floats truncated):
// text_size (32), ruby_size (12), weight (400), alpha_bias (0..1 x 255, the fonts' bias),
// typing_area {x, y, w, h} (0, 0, 512, 512), then x and y, which the exe stores crosswise (x into
// the area's y, y into its x), width / height (the texture, 0: the laid-out size), row_space,
// pitch_full, pitch_half, ruby_offset, ruby_margine_rate, shadow_power (1: the shadow's alpha, 0..1),
// shadow_offset, shadow_color {r, g, b} (absent: black; each missing component 0) and font_color
// (absent: white; each missing component 1), each component x 255 truncated to a byte,
// shadow_type (0 none; 15 an outline of radius shadow_offset, 3 at most; else bits 1 / 2 / 4 / 8
// move the shadow left / right / up / down by shadow_offset), charset (10), typing_type
// (0 character by character, 1 word wrap, 2 vertical) and text.
SQInteger loadTextTexture(HSQUIRRELVM v) {
    auto& h = host(v);
    const std::string fontName = argString(v, 2), name = argString(v, 4);
    TextTextureSettings s;
    std::string text;
    if (argCount(v) >= 2 && sq_gettype(v, 3) == OT_TABLE) {
        // SquirrelObject's ToInteger / ToFloat (0x42c350 / 0x42c370): numbers converted, else 0.
        auto toInt = [&](SQInteger idx) -> int32_t {
            SQObjectType t = sq_gettype(v, idx);
            if (t == OT_FLOAT) {
                SQFloat f = 0;
                sq_getfloat(v, idx, &f);
                return truncateFloat(static_cast<float>(f));
            }
            if (t == OT_INTEGER) {
                SQInteger i = 0;
                sq_getinteger(v, idx, &i);
                return static_cast<int32_t>(i);
            }
            return 0;
        };
        auto toFloat = [&](SQInteger idx) -> float {
            SQObjectType t = sq_gettype(v, idx);
            if (t == OT_FLOAT) {
                SQFloat f = 0;
                sq_getfloat(v, idx, &f);
                return static_cast<float>(f);
            }
            if (t == OT_INTEGER) {
                SQInteger i = 0;
                sq_getinteger(v, idx, &i);
                return static_cast<float>(static_cast<int32_t>(i));
            }
            return 0.0f;
        };
        // The value of a key of the table at `table` pushed (true), or nothing (false).
        auto get = [&](SQInteger table, const char* key) {
            sq_pushstring(v, key, -1);
            if (SQ_FAILED(sq_get(v, table < 0 ? table - 1 : table))) return false;
            return true;
        };
        auto intKey = [&](const char* key, int& out) {
            if (!get(3, key)) return;
            out = toInt(-1);
            sq_pop(v, 1);
        };
        // A colour: absent, `absent`; a table's r, g and b (each `missing` unless there), or
        // `missing` for all three when it is not a table; each x 255 truncated to a byte.
        auto colourKey = [&](const char* key, uint32_t absent, float missing) -> uint32_t {
            if (!get(3, key)) return absent;
            float rgb[3] = {missing, missing, missing};
            if (sq_gettype(v, -1) == OT_TABLE) {
                const char* names[3] = {"r", "g", "b"};
                for (int i = 0; i < 3; ++i) {
                    if (!get(-1, names[i])) continue;
                    rgb[i] = toFloat(-1);
                    sq_pop(v, 1);
                }
            }
            sq_pop(v, 1);
            uint32_t c = 0;
            for (int i = 0; i < 3; ++i) c |= static_cast<uint32_t>(static_cast<uint8_t>(truncateFloat(rgb[i] * 255.0f))) << (8 * i);
            return c;
        };
        intKey("ruby_size", s.rubySize);
        intKey("text_size", s.textSize);
        intKey("weight", s.weight);
        if (get(3, "alpha_bias")) {
            float f = toFloat(-1);
            sq_pop(v, 1);
            f = 0.0f > f ? 0.0f : (f < 1.0f ? f : 1.0f);
            s.alphaBias = truncateFloat(f * 255.0f);
        }
        if (get(3, "typing_area")) {
            if (sq_gettype(v, -1) == OT_TABLE) {
                const char* names[4] = {"x", "y", "w", "h"};
                for (int i = 0; i < 4; ++i) {
                    if (!get(-1, names[i])) continue;
                    s.area[static_cast<size_t>(i)] = toInt(-1);
                    sq_pop(v, 1);
                }
            }
            sq_pop(v, 1);
        }
        intKey("x", s.area[1]);
        intKey("y", s.area[0]);
        intKey("width", s.width);
        intKey("height", s.height);
        intKey("row_space", s.rowSpace);
        intKey("pitch_full", s.pitchFull);
        intKey("pitch_half", s.pitchHalf);
        intKey("ruby_offset", s.rubyOffset);
        if (get(3, "ruby_margine_rate")) {
            s.rubyMargineRate = toFloat(-1);
            sq_pop(v, 1);
        }
        float power = 1.0f;
        if (get(3, "shadow_power")) {
            power = toFloat(-1);
            sq_pop(v, 1);
        }
        int offset = 0;
        intKey("shadow_offset", offset);
        s.shadowColor = colourKey("shadow_color", 0, 0.0f);
        s.fontColor = colourKey("font_color", 0xffffffffu, 1.0f) & 0xffffffu;
        intKey("shadow_type", s.shadowType);
        int charset = 10;  // an index into Mware's charset table (CreateFontW's; not needed to find the face)
        intKey("charset", charset);
        intKey("typing_type", s.typingType);
        if (get(3, "text")) {
            if (sq_gettype(v, -1) == OT_STRING) {
                const SQChar* t = nullptr;
                sq_getstring(v, -1, &t);
                text = t ? t : "";
            } else if (h.log) {
                h.log("loadTextTexture: a text that is not a string (the exe reads through a null pointer)");
            }
            sq_pop(v, 1);
        }
        // The shadow's alpha (1 at most, 0 at least) and its offset by type.
        const float a = !(power < 1.0f) ? 1.0f : power;
        s.shadowAlpha = static_cast<uint32_t>(static_cast<uint8_t>(truncateFloat((a > 0.0f ? a : 0.0f) * 255.0f))) << 24;
        if (s.shadowType == 15) {
            s.shadowX = offset;
        } else if (s.shadowType != 0) {
            s.shadowX = ((s.shadowType & 1) ? -offset : 0) + ((s.shadowType & 2) ? offset : 0);
            s.shadowY = ((s.shadowType & 4) ? -offset : 0) + ((s.shadowType & 8) ? offset : 0);
        }
    }
    if (name.find("text://") == std::string::npos) {
        auto data = h.readAnyFile(name);
        if (!data) {
            h.note(h.missing, name);
            return pushInt(v, 0);
        }
        text.assign(reinterpret_cast<const char*>(data->data()), data->size());
        text.resize(std::strlen(text.c_str()));
    }
    if (text.empty()) return pushInt(v, 0);
    const bool vertical = s.typingType == 2;
    auto fonts = [&](int size) { return h.typingFont(fontName, size, s.weight, vertical); };
    Image img = renderTextTexture(text, s, fonts, h.lineBreaks, h.log);
    if (h.appTextures.insert(name).second) h.textTextures[name] = std::make_shared<const Image>(std::move(img));
    return pushInt(v, 1);
}
// getParticleParameter(name) (0x4f2b90): media/particle/<name>.particle's record as SQParticle's
// getParameterTable gives a system's (the loader 0x475be0, then 0x4efb10): a missing file's is all
// zeros with no textures.
SQInteger getParticleParameter(HSQUIRRELVM v) {
    pushParticleParameter(v, argString(v, 2));
    return 1;
}
// loadParticle(name) / unloadParticle(name) (both 0x4dcba0: the exe's `return -1`).
SQInteger particleFileStub(HSQUIRRELVM v) { return pushInt(v, -1); }
// getParticleNameList(name) (0x4f29d0) / getParticleFileList() (0x4f2ae0): the exe gathers the
// particle loader's names (0x476200 / 0x476380) into a list, makes an array of size 0 (0x43d580)
// and sets each name at its index (0x42b8f0, a raw set: out of range, refused), so both answer an
// empty array.
SQInteger emptyArray(HSQUIRRELVM v) {
    sq_newarray(v, 0);
    return 1;
}
// callTask(name): int (0x4f2c30): the name goes to CApp +0x78 for the frame's task switch
// (0x42e400 -> 0x42e260); 0.
SQInteger callTask(HSQUIRRELVM v) {
    host(v).calledTask = argString(v, 2);
    return pushInt(v, 0);
}
// showMessageBox(caption, text, type): int (0x4f3360 -> CApp 0x42f610): MessageBoxW(window, text,
// caption, type), both strings from UTF-8, then the window activated again; the button's id. The
// frame loop waits inside the call, so the script is suspended there until the player answers
// (ScriptHost::messageBox / answerMessageBox; the frame goes on after). From a script thread, or
// inside another native's call, the VM cannot suspend there: the call answers at once with the
// box's first button (IDOK for MB_OK, all the scripts ask for) and the frame stops after the
// script, before the draw, until the player answers.
int32_t firstButton(int32_t type) {
    switch (type & 0xf) {
    case 2: return 3;   // MB_ABORTRETRYIGNORE: IDABORT
    case 3:             // MB_YESNOCANCEL
    case 4: return 6;   // MB_YESNO: IDYES
    case 5: return 4;   // MB_RETRYCANCEL: IDRETRY
    case 6: return 2;   // MB_CANCELTRYCONTINUE: IDCANCEL
    default: return 1;  // MB_OK, MB_OKCANCEL: IDOK
    }
}
SQInteger showMessageBox(HSQUIRRELVM v) {
    auto& h = host(v);
    auto& m = h.messageBox;
    std::string caption = argString(v, 2), text = argString(v, 3);
    int32_t type = static_cast<int32_t>(argInt(v, 4));
    if (m.pending || m.answered) return pushInt(v, firstButton(type));  // (a second box in one frame)
    m.pending = true;
    ++m.serial;
    m.caption = caption;
    m.text = text;
    m.type = type;
    if (h.log) h.log("message box: " + caption + ": " + text);
    if (v == h.vm) {
        SQRESULT r = sq_suspendvm(v);
        if (r == -666) {  // SQ_SUSPEND_FLAG (Squirrel's sqvm.h)
            m.resume = true;
            return r;
        }
        sq_reseterror(v);
    }
    m.resume = false;
    return pushInt(v, firstButton(type));
}
// mountFile(file, key): int (0x4f2cf0): the key's hex digits (0-9 and capital A-F, two to a byte;
// any other character: -1 at once; an odd count: no key) and the file go to the file manager's
// mount (Archives::mount): 0 or -1. unmountFile(file) (0x4f2e70) and isMounted(file) (0x4f2f20):
// the file manager's.
SQInteger mountFile(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string file = argString(v, 2), hex = argString(v, 3);
    std::vector<uint8_t> key;
    if (!hex.empty() && hex.size() % 2 == 0) {
        for (size_t i = 0; i < hex.size(); ++i) {
            char c = hex[i];
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return pushInt(v, -1);
            if (i % 2 == 0) key.push_back(static_cast<uint8_t>(digit << 4));
            else key.back() = static_cast<uint8_t>(key.back() + digit);
        }
    }
    return pushInt(v, h.game.archives->mount(file, key.empty() ? nullptr : key.data(), key.size()));
}
SQInteger unmountFile(HSQUIRRELVM v) { return pushInt(v, host(v).game.archives->unmount(argString(v, 2))); }
SQInteger isMounted(HSQUIRRELVM v) { return pushInt(v, host(v).game.archives->isMounted(argString(v, 2)) ? 1 : 0); }
// injectSoundFile(file) / ejectSoundFile(file): int (0x4f2fd0 / 0x4f3070 -> the sound object's
// vtable +0x78 / +0x7c, both Mware's CSound 0x10040c90: -2, not supported).
SQInteger soundFileStub(HSQUIRRELVM v) { return pushInt(v, -2); }

}  // namespace

// The font a name stands for, as Mware would make it (0x1004ff00): the face Windows finds under the
// font's system name among the game's font files and the host's fonts (a font loaded by file: that
// file's first face), at the font's size and weight. A face not found falls back to the host's first
// font (Windows would substitute one too).
std::shared_ptr<FontFace> ScriptHost::Impl::findFace(const std::string& family) {
    for (const auto& [n, f] : fonts) {
        if (f.file.empty() || !loadedFontFiles.insert(f.file).second) continue;
        if (auto data = readAnyFile(f.file)) {
            for (auto& face : FontFace::load(std::make_shared<const Bytes>(std::move(*data)))) gameFaces.push_back(face);
        } else {
            note(missing, f.file);
        }
    }
    if (family.empty()) return nullptr;
    for (const auto& f : gameFaces) {
        if (f->answersTo(family)) return f;
    }
    for (const auto& f : systemFaces) {
        if (f->answersTo(family)) return f;
    }
    return nullptr;
}

std::shared_ptr<FontInstance> ScriptHost::Impl::typingFont(const std::string& family, int size, int weight, bool vertical) {
    std::string key = family + '\x1f' + std::to_string(size) + '\x1f' + std::to_string(weight) + (vertical ? "\x1fv" : "");
    if (auto it = typingFonts.find(key); it != typingFonts.end()) return it->second;
    std::shared_ptr<FontFace> face = findFace(family);
    if (!face) {
        face = !systemFaces.empty() ? systemFaces.front() : !gameFaces.empty() ? gameFaces.front() : nullptr;
        if (log) log("loadTextTexture: no face \"" + family + "\"; using a substitute");
    }
    auto inst = std::make_shared<FontInstance>(face, size, weight, false, vertical);
    typingFonts[key] = inst;
    return inst;
}

std::shared_ptr<FontInstance> ScriptHost::Impl::fontInstance(const std::string& name) {
    if (auto it = fontInstances.find(name); it != fontInstances.end()) return it->second;
    findFace("");  // the game's font files in
    Font spec;
    if (auto it = fonts.find(name); it != fonts.end()) spec = it->second;
    std::shared_ptr<FontFace> face;
    if (!spec.systemName.empty()) face = findFace(spec.systemName);
    if (!face && !spec.file.empty()) {
        if (auto data = readAnyFile(spec.file)) {
            auto faces = FontFace::load(std::make_shared<const Bytes>(std::move(*data)));
            if (!faces.empty()) face = faces.front();
        }
    }
    if (!face) {
        face = !systemFaces.empty() ? systemFaces.front() : !gameFaces.empty() ? gameFaces.front() : nullptr;
        if (log) log("font " + name + ": no face \"" + spec.systemName + "\"; using a substitute");
    }
    // Muramasa's and Phantom's fonts are Mware's (their glyphs drawn by its blit with the bias the
    // native made of the setting: 0..1 x 255); You and Me and Her's are the exe's own.
    int bias = -1;
    if (!olderText) {
        const float b = 0.0f > spec.bias ? 0.0f : (spec.bias < 1.0f ? spec.bias : 1.0f);
        bias = truncateFloat(b * 255.0f);
    }
    auto inst = std::make_shared<FontInstance>(face, spec.size, spec.weight, spec.noAA != 0, false, bias);
    fontInstances[name] = inst;
    return inst;
}

void ScriptHost::setScreenReader(ScreenReader reader) { impl_->screenReader = std::move(reader); }
void ScriptHost::setTargetReader(ScreenReader reader) { impl_->targetReader = std::move(reader); }

void ScriptHost::addSystemFont(std::shared_ptr<const Bytes> data) {
    for (auto& face : FontFace::load(std::move(data))) impl_->systemFaces.push_back(face);
}

namespace {

// --- Sound volumes --------------------------------------------------------------------------------

// defineSoundGroup(name) (0x4f3380 -> 0x4ec530): a group at volume 1, if new.
SQInteger defineSoundGroup(HSQUIRRELVM v) {
    host(v).groupVolumes.try_emplace(argString(v, 2));
    return 0;
}
// setSoundTypeVolume(type, volume, ms: float, curve) (0x4f3420): tweens the type's volume (within
// [0, 1]) from where it is; a time of 0 applies at once (the time is unsigned there, as in the tween).
SQInteger setSoundTypeVolume(HSQUIRRELVM v) {
    auto& h = host(v);
    float volume = argFloat(v, 3);
    volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    int32_t ms = static_cast<int32_t>(argFloat(v, 4));
    auto& vol = h.typeVolumes[static_cast<int32_t>(argInt(v, 2))];
    vol.action.set(vol.action.now, volume, ms, static_cast<int32_t>(argInt(v, 5)));
    if (ms == 0) vol.dirty = 1;
    return 0;
}
// setSoundGroupVolume(name, volume, ms: float, curve) (0x4f34d0): the same for a defined group.
SQInteger setSoundGroupVolume(HSQUIRRELVM v) {
    auto& h = host(v);
    auto it = h.groupVolumes.find(argString(v, 2));
    if (it == h.groupVolumes.end()) return 0;
    float volume = argFloat(v, 3);
    volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    int32_t ms = static_cast<int32_t>(argFloat(v, 4));
    it->second.action.set(it->second.action.now, volume, ms, static_cast<int32_t>(argInt(v, 5)));
    it->second.dirty = 1;
    return 0;
}

// --- System settings ------------------------------------------------------------------------------

// saveSystemConfig(): int (0x42f320): system.dat in the save root.
SQInteger saveSystemConfig(HSQUIRRELVM v) {
    auto& h = host(v);
    Bytes data = h.config.serialize();
    bool ok = h.saves->write(h.writablePath(h.systemConfigPath()), data);
    return pushInt(v, ok ? 0 : -1);
}
// loadSystemConfig(): int (0x42f490): reads system.dat and applies it.
SQInteger loadSystemConfig(HSQUIRRELVM v) {
    auto& h = host(v);
    auto data = h.saves->read(h.writablePath(h.systemConfigPath()));
    h.config.deserialize(data ? ByteSpan(*data) : ByteSpan());
    h.applyConfig();
    return pushInt(v, 0);
}
// applySystemConfig() (0x4f36d0): the frame loop applies the settings before the next frame.
SQInteger applySystemConfig(HSQUIRRELVM v) {
    host(v).config.dirty = 1;
    return 0;
}
SQInteger applyPresetConfig(HSQUIRRELVM v) {
    auto& c = host(v).config;
    c.applyPreset(static_cast<int>(argInt(v, 2)));
    c.dirty = 1;
    return 0;
}
SQInteger isExistUserSetting(HSQUIRRELVM v) { return pushInt(v, host(v).config.userSetting); }
SQInteger setConfigScreenSize(HSQUIRRELVM v) {
    auto& c = host(v).config;
    c.screenWidth = static_cast<int32_t>(argInt(v, 2));
    c.screenHeight = static_cast<int32_t>(argInt(v, 3));
    return 0;
}
SQInteger getConfigScreenSize(HSQUIRRELVM v) {
    auto& h = host(v);
    h.setResult(static_cast<float>(h.config.screenWidth), static_cast<float>(h.config.screenHeight));
    return 0;
}
// getConfig_UseSelfShadow (0x4f39e0): shadows and self shadows both on.
SQInteger getConfigUseSelfShadow(HSQUIRRELVM v) {
    auto& c = host(v).config;
    return pushInt(v, c.useShadow && c.useSelfShadow ? 1 : 0);
}
// setConfig_AntiAliasType (0x4f3860): 0..16; setConfig_PointLightNum (0x4f3a20): at most 16.
SQInteger setConfigAntiAliasType(HSQUIRRELVM v) {
    SQInteger t = argInt(v, 2);
    if (static_cast<uint32_t>(t) < 0x11) host(v).config.antiAliasType = static_cast<int32_t>(t);
    return 0;
}
SQInteger setConfigPointLightNum(HSQUIRRELVM v) {
    uint32_t n = static_cast<uint32_t>(argInt(v, 2));
    host(v).config.pointLightNum = static_cast<int32_t>(n > 16 ? 16 : n);
    return 0;
}

// The plain settings: setConfig_X(int) stores, getConfig_X() reads.
#define CONFIG_FIELD(Name, field)                                                    \
    SQInteger setConfig##Name(HSQUIRRELVM v) {                                         \
        host(v).config.field = static_cast<int32_t>(argInt(v, 2));                      \
        return 0;                                                                       \
    }                                                                                   \
    SQInteger getConfig##Name(HSQUIRRELVM v) { return pushInt(v, host(v).config.field); }
CONFIG_FIELD(FullScreen, fullScreen)
CONFIG_FIELD(FixedAspect, fixedAspect)
CONFIG_FIELD(FrameRate, frameRate)
CONFIG_FIELD(SceneDegreeSizeRatio, sceneDegreeSizeRatio)
CONFIG_FIELD(UseSoftFocus, useSoftFocus)
CONFIG_FIELD(UseHDR, useHDR)
CONFIG_FIELD(UseDOF, useDOF)
CONFIG_FIELD(UseToneMapping, useToneMapping)
CONFIG_FIELD(ShadowQuality, shadowQuality)
CONFIG_FIELD(UseShadow, useShadow)
#undef CONFIG_FIELD
SQInteger setConfigUseSelfShadow(HSQUIRRELVM v) {
    host(v).config.useSelfShadow = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger getConfigAntiAliasType(HSQUIRRELVM v) { return pushInt(v, host(v).config.antiAliasType); }
SQInteger getConfigPointLightNum(HSQUIRRELVM v) { return pushInt(v, host(v).config.pointLightNum); }

// --- The exes' own natives (registered by a callback WinMain sets at app+0xc4) -------------------

// IsEcoMode(): int (muramasa_en.exe 0x401940, totono_en.exe 0x4019c0): the launcher's second option
// as WinMain read it after the app started.
SQInteger isEcoMode(HSQUIRRELVM v) { return pushInt(v, host(v).ecoMode); }
// RebootRequest() (totono_en.exe 0x4019d0): ShellExecuteW(null, "open", "kmsdll.exe", "\"<the exe>\"
// 3000 --nodlg"), which starts the game again without its launcher once the exit() that follows has
// closed it (the Diesel session's closeForReboot). Its result (opened or not) no script reads.
SQInteger rebootRequest(HSQUIRRELVM v) {
    host(v).reboot = true;
    return 0;
}
// SetClearTrue(cleared: int) (totono_en.exe 0x4019b0): the launcher's third option, which picks
// WINDOW_NAME_2 for the window at the next start.
SQInteger setClearTrue(HSQUIRRELVM v) {
    auto& opts = host(v).config.launcherOptions;
    if (opts.size() >= 3) opts[2] = static_cast<int32_t>(argInt(v, 2));
    return 0;
}

// --- Physics worlds -------------------------------------------------------------------------------

// createPhysicsWorld(id): int (0x4f32f0 -> 0x42f1a0; totono_en.exe 0x430130, phantom.exe
// 0x1400396d0): an id already in the app's world map (+0x124) answers -1; a new one asks Mware's
// core for a world (ICore vtable +0x94; Totono's +0x90), and only a world it gets is set up (its
// vtable +0x58 with 0.8) and listed; 0 either way. Every game's Mware.dll answers that call with no
// world (its shared `return 0`: Mware.dll 0x100046e0, Totono's 0x100046f0, Phantom's 0x180004480),
// so the map stays empty: always 0.
SQInteger createPhysicsWorld(HSQUIRRELVM v) { return pushInt(v, 0); }
// deletePhysicsWorld(id): int (0x4f3310 -> 0x42f240): a listed world released and unlisted (0),
// else -1; with no world ever listed, always -1.
SQInteger deletePhysicsWorld(HSQUIRRELVM v) { return pushInt(v, -1); }

// SQPhysics (muramasa_en.exe 0x542150 ff., totono_en.exe 0x514dd0 ff., phantom.exe 0x14017f630
// ff.): a task object holding the app's world of an id (+0xb8; Totono +0xbc, Phantom +0xe0), which
// every method forwards to and which none of the three games can make (createPhysicsWorld): so
// init(id) finds none and answers 0 (an int: 0x4042a0 pushes it), getGravity sets SQMATH_RET to
// (0, 0, 0, 0), isActive answers 0, rayTest / sweepTest answer null (with a world, a hit's table of
// name, normal, position (and distance): a world's bodies are SQRigidBody's, a 3D class), and
// setGravity / setActive do nothing.
struct PhysicsObject final : NativeObject {
    std::string name;
};
NativeObject* makePhysics(HSQUIRRELVM v) {
    auto* o = new PhysicsObject;
    o->name = argString(v, 2);
    return o;
}
SQInteger physicsInit(HSQUIRRELVM v) { return pushInt(v, 0); }
SQInteger physicsNothing(HSQUIRRELVM) { return 0; }
SQInteger physicsGetGravity(HSQUIRRELVM v) {
    host(v).setResult(0.0f, 0.0f, 0.0f, 0.0f);
    return 0;
}
SQInteger physicsIsActive(HSQUIRRELVM v) { return pushInt(v, 0); }
SQInteger physicsNoHit(HSQUIRRELVM v) {
    sq_pushnull(v);
    return 1;
}

// SQTrophy (phantom.exe only: 0x14000a450 ff.; the Steam achievements). `init` starts a worker
// thread (0x14000a590) and answers 0; `unlockTrophy(id)` queues the id for it and answers 0; the
// worker takes an id only once the game's Steam stats object exists (0x1402d01b0, made when Steam
// is running), then sets ACHIEVEMENT_<id> (1 .. 34; the last of 2 .. 34 also sets 1). Nyanroid has
// no Steam, as a PC without it running: the ids wait in the queue. `update` is the object's vtable
// +0x70, the exe's `return 0`; `isDoing` answers +0x128, which only the object's copy writes (0);
// `updateInitialize` acts only while it is set (0). Every answer is an int (0x14000b8e0).
struct TrophyObject final : NativeObject {
    std::string name;
    std::vector<int32_t> queue;  // +0x118: ids waiting for Steam
};
NativeObject* makeTrophy(HSQUIRRELVM v) {
    auto* o = new TrophyObject;
    o->name = argString(v, 2);
    return o;
}
SQInteger trophyZero(HSQUIRRELVM v) { return pushInt(v, 0); }
SQInteger trophyUnlock(HSQUIRRELVM v) {
    if (auto* o = self<TrophyObject>(v)) o->queue.push_back(static_cast<int32_t>(argInt(v, 2)));
    return pushInt(v, 0);
}

}  // namespace

ClassNatives physicsNatives() {
    return {{{"init", physicsInit},
             {"setGravity", physicsNothing},
             {"getGravity", physicsGetGravity},
             {"setActive", physicsNothing},
             {"isActive", physicsIsActive},
             {"rayTest", physicsNoHit},
             {"sweepTest", physicsNoHit}},
            makePhysics};
}

ClassNatives trophyNatives() {
    return {{{"init", trophyZero},
             {"update", trophyZero},
             {"unlockTrophy", trophyUnlock},
             {"isDoing", trophyZero},
             {"updateInitialize", trophyZero}},
            makeTrophy};
}

std::vector<NativeImpl> exeNatives() {
    std::vector<NativeImpl> out = {{"IsEcoMode", isEcoMode}, {"RebootRequest", rebootRequest}, {"SetClearTrue", setClearTrue}};
    for (const auto& n : n1Globals()) out.push_back(n);  // Saya_en.exe's (0x51a891, 0x51a8ab)
    return out;
}

void ScriptHost::Impl::applyConfig() {
    // 0x444ef0: the frame rate, then the window (size, full screen, aspect) and anti-aliasing; the
    // window here is the front end's, so only its full-screen state is kept.
    config.dirty = 0;
    rateTime = clock.milliTime;
    rateFrame = frame;
    frameRate = config.frameRate;
    fullScreen = config.fullScreen != 0;
}

ClassNatives appNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {
        {"getFrameSpeed", getFrameSpeed},
        {"getMilliTime", getMilliTime},
        {"getSystemTime", getSystemTime},
        {"setMaxFrameTime", setMaxFrameTime},
        {"setFrameRate", setFrameRate},
        {"getFrameRate", getFrameRate},
        {"isKeyDown", isKeyDown},
        {"isKeyUp", isKeyUp},
        {"getKeyState", getKeyState},
        {"getCursorPosition", getCursorPosition},
        {"setCursorPosition", setCursorPosition},
        {"getMouseButtonStateL", getMouseButtonStateL},
        {"getMouseButtonStateR", getMouseButtonStateR},
        {"getMouseWheelState", getMouseWheelState},
        {"getMouseMove", getMouseMove},
        {"clearMouseInput", clearMouseInput},
        {"getTouchContactNum", returnZero},
        {"getTouchState", returnZero},
        {"isTouchMoving", returnZero},
        {"getTouchTime", returnZero},
        {"getTouchMove", zeroVector},
        {"getTouchPosition", zeroVector},
        {"getTouchVelocity", zeroVector},
        {"clearTouchInput", returnNothing},
        {"getGamepadButtonState", returnZero},
        {"isButtonDown", returnZero},
        {"getGamepadTriggerValue", returnZeroFloat},
        {"getGamepadThumbValue", zeroVector},
        {"setGamepadVibration", returnNothing},
        {"showCursor", showCursor},
        {"setCursor", setCursor},
        {"getCursor", getCursor},
        {"setWindowTitle", setWindowTitle},
        {"isFullScreen", isFullScreen},
        {"setFullScreen", setFullScreen},
        {"getPlatformName", getPlatformName},
        {"getProductName", getProductName},
        {"exit", exitApp},
        {"fadeoutScreen", fadeoutScreen},
        {"fadeoutColor", fadeoutColor},
        {"setCloseCallback", setCloseCallback},
        {"createDirectory", createDirectory},
        {"getFileTime", getFileTime},
        {"loadFont", loadFont},
        {"loadSystemFont", loadSystemFont},
        {"clearFontCache", clearFontCache},
        {"unloadFont", unloadFont},
        {"loadTexture", loadTexture},
        {"unloadTexture", unloadTexture},
        {"loadTextTexture", loadTextTexture},
        {"createPhysicsWorld", createPhysicsWorld},
        {"deletePhysicsWorld", deletePhysicsWorld},
        {"loadParticle", particleFileStub},
        {"unloadParticle", particleFileStub},
        {"getParticleNameList", emptyArray},
        {"getParticleFileList", emptyArray},
        {"getParticleParameter", getParticleParameter},
        {"setParticleDebugDrawEnable", returnNothing},
        {"callTask", callTask},
        {"mountFile", mountFile},
        {"unmountFile", unmountFile},
        {"isMounted", isMounted},
        {"injectSoundFile", soundFileStub},
        {"showMessageBox", showMessageBox},
        {"ejectSoundFile", soundFileStub},
        {"defineSoundGroup", defineSoundGroup},
        {"setSoundTypeVolume", setSoundTypeVolume},
        {"setSoundGroupVolume", setSoundGroupVolume},
        {"saveSystemConfig", saveSystemConfig},
        {"loadSystemConfig", loadSystemConfig},
        {"applySystemConfig", applySystemConfig},
        {"applyPresetConfig", applyPresetConfig},
        {"isExistUserSetting", isExistUserSetting},
        {"setConfig_ScreenSize", setConfigScreenSize},
        {"getConfig_ScreenSize", getConfigScreenSize},
        {"setConfig_FullScreen", setConfigFullScreen},
        {"getConfig_FullScreen", getConfigFullScreen},
        {"setConfig_FixedAspect", setConfigFixedAspect},
        {"getConfig_FixedAspect", getConfigFixedAspect},
        {"setConfig_FrameRate", setConfigFrameRate},
        {"getConfig_FrameRate", getConfigFrameRate},
        {"setConfig_AntiAliasType", setConfigAntiAliasType},
        {"getConfig_AntiAliasType", getConfigAntiAliasType},
        {"setConfig_SceneDegreeSizeRatio", setConfigSceneDegreeSizeRatio},
        {"getConfig_SceneDegreeSizeRatio", getConfigSceneDegreeSizeRatio},
        {"setConfig_UseSoftFocus", setConfigUseSoftFocus},
        {"getConfig_UseSoftFocus", getConfigUseSoftFocus},
        {"setConfig_UseHDR", setConfigUseHDR},
        {"getConfig_UseHDR", getConfigUseHDR},
        {"setConfig_UseDOF", setConfigUseDOF},
        {"getConfig_UseDOF", getConfigUseDOF},
        {"setConfig_UseToneMapping", setConfigUseToneMapping},
        {"getConfig_UseToneMapping", getConfigUseToneMapping},
        {"setConfig_ShadowQuality", setConfigShadowQuality},
        {"getConfig_ShadowQuality", getConfigShadowQuality},
        {"setConfig_UseShadow", setConfigUseShadow},
        {"getConfig_UseShadow", getConfigUseShadow},
        {"setConfig_UseSelfShadow", setConfigUseSelfShadow},
        {"getConfig_UseSelfShadow", getConfigUseSelfShadow},
        {"setConfig_PointLightNum", setConfigPointLightNum},
        {"getConfig_PointLightNum", getConfigPointLightNum},
    };
    return c;
}

}  // namespace nyan::diesel
