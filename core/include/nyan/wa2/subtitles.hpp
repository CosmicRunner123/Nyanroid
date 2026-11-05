#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

struct SpriteObject;

// The Todokanai patch's subtitles for spoken lines that have no message text (its d3d9.dll: file
// reader 0x100044b0, trigger 0x10002b20, message hook 0x10002520, per-EndScene step 0x10002c40). A
// block starts when a sound effect or voice with its key plays in its scene and shows its lines by
// the clock; it ends with its last line or when the story reaches its end message.
class Subtitles {
public:
    struct Line {
        uint64_t start = 0, end = 0;  // ms from the trigger; end = start + the file's duration
        std::string text;             // `^` breaks the line
    };
    struct Block {
        int key = 0, scene = 0, endScene = 0, endMsg = 0;
        std::vector<Line> lines;
    };

    // `todokanai/subtitles`: `{`, then "scene key endScene endMsg", then "start duration text" lines
    // until `}` (0x100044b0, a text-mode ifstream: CRLF reads as LF).
    void load(ByteSpan file);
    const std::vector<Block>& blocks() const { return blocks_; }

    // A sound with this key played (type 1 voice: the number with the VA offset; type 2 SE: its
    // number). `viScene` is the VI scene (0x4be4cc), `curMsg` the last message marked read (0xa391e8).
    void trigger(int key, int type, int viScene, int curMsg, uint64_t nowMs);
    // A message line was marked read (0x405180).
    void messageMarked(int viScene, int curMsg);
    // One step of the overlay (once per EndScene there; once per logic frame here): the line to show
    // now, if any, and the move to the next line.
    void step(uint64_t nowMs);
    // The text drawn this frame ("" when none).
    const std::string& text() const { return shown_; }
    bool playing() const { return playing_; }

    // The overlay's text draw (0x1000c540): glyphs of `font.png` (in image slot `fontSlot`) by the
    // DLL's table, rows of 35 px from y = 50, each row centred for 20 px per byte; advance w - 2.
    static void draw(const std::string& text, int fontSlot, const std::function<void(const SpriteObject&)>& emit);

private:
    void endIfReached(int viScene, int curMsg);

    std::vector<Block> blocks_;
    bool playing_ = false;
    int block_ = -1;
    size_t line_ = 0;
    uint64_t start_ = 0;
    std::string shown_;
};

}  // namespace nyan::wa2
