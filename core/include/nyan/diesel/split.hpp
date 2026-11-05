#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

// A sprite split into pieces that fly apart (SQSprite.splitFace / pushSplitAction / beginSplitAction;
// CSprite +0xbe0 .. +0xc5c, muramasa_en.exe 0x45a3e0, 0x454550, 0x45a490, 0x45b120, 0x453960).
// splitFace cuts the picture into a grid of jagged cells (random
// cut positions and corner offsets unless told not to distort), joins some neighbours into groups
// that move as one (the link rate), and each pushed action gives every group a motion (velocity,
// acceleration, angular velocity and acceleration, a delay) from where its centre lies. A tween runs
// along the actions' total time; each frame every piece is placed where its group's motion has taken
// it.
struct SplitFace {
    using V2 = std::array<float, 2>;
    using V3 = std::array<float, 3>;

    // One cell (0xe8 bytes).
    struct Piece {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  // +0x0: the cell in the sprite's pixels (y down)
        V3 offset{};                     // +0x10: where its motion has taken it (pixels, y down)
        V3 angle{};                      // +0x1c: its turn (degrees)
        std::array<V2, 4> jag{};         // +0x68: the corners' offsets (pixels; TL, TR, BL, BR)
        V2 center{};                     // +0x88: its group's centre, from the sprite's centre
        V2 own{};                        // +0x90: its own centre, from the sprite's centre
        int32_t parent = -1;             // +0x9c: the piece it is joined to (-1: a group's first)
        int32_t count = 1;               // +0xa0: the pieces in its group
        int32_t group = -1;              // +0xa4
    };
    // An action's motion term (24 dwords; the table reader 0x50b4e0): its size from the group
    // centre's distance along `dir` (front) and across it (side), between dist_min and dist_max
    // (plus a random part), on the outward direction; random offsets; a per-axis mask.
    struct Term {
        int32_t on = 0;
        V2 edge{1.0f, 1.0f};             // dist_edge: front, side
        float distMin = 0.0f, distMax = 0.0f, distRandMin = 0.0f, distRandMax = 0.0f;
        V3 randMin{}, randMax{};
        V3 mask{1.0f, 1.0f, 1.0f};
        int32_t inv = 0, aspect = 0;
        V3 center{};
        V3 dir{1.0f, 0.0f, 0.0f};        // normalised
    };
    // The delay term (18 dwords; 0x50b9e0): as a motion term, one value.
    struct DelayTerm {
        int32_t on = 0;
        V2 edge{1.0f, 1.0f};
        float distMin = 0.0f, distMax = 0.0f, distRandMin = 0.0f, distRandMax = 0.0f;
        float randMin = 0.0f, randMax = 0.0f, mask = 1.0f;
        int32_t inv = 0, aspect = 0;
        V3 center{};
        V3 dir{1.0f, 0.0f, 0.0f};
    };
    // pushSplitAction's table (0x50ae90, 0x1d8 bytes).
    struct ActionSpec {
        float time = 0.0f;               // seconds (the table's ms x 0.001)
        int32_t seed = 0;
        Term vel, acc, angVel, angAcc;
        DelayTerm delay;
        float regist = 0.0f, angRegist = 0.0f;
    };
    // A group's motion in one action (0x54 bytes).
    struct State {
        V3 pos{}, vel{}, acc{}, angle{}, angVel{}, angAcc{};
        float delay = 0.0f, regist = 0.0f, angRegist = 0.0f;
    };
    struct Action {
        float time = 0.0f;
        std::vector<State> states;       // by group
    };

    int32_t rows = 0, cols = 0;          // +0xbe0 / +0xbe4
    float linkRate = 0.0f;               // +0xbe8
    int32_t seed = 0, noDistortion = 0;  // +0xbec / +0xbf0
    float overflow = 0.0f;               // +0xbf4: ms run on past the tween (keepGoing)
    int32_t keepGoing = 0;               // +0xbf8
    std::vector<Piece> pieces;           // +0xbfc
    std::vector<int32_t> groups;         // +0xc3c: each group's first piece
    std::vector<Action> actions;         // +0xc48
    Tween t{1, 0.0f};                    // +0xc10 (now +0xc24): ms along the actions
    int32_t dirty = 0;                   // +0xc58: the pieces are to be placed again

    // splitFace(rows, cols, linkRate, seed, noDistortion) (0x45a3e0): the cells again when anything
    // changed (0x454550, on a `width` x `height` picture).
    void split(int32_t rows, int32_t cols, float linkRate, int32_t seed, int32_t noDistortion, int width, int height);
    void clearActions();                                         // 0x50fd30
    void push(const ActionSpec& a, int width, int height);       // 0x45a490
    // beginSplitAction(start, end, time, curve, keepGoing) (0x45b120): the tween from start to end
    // (fractions of the actions' total) over `timeMs`.
    void begin(float start, float end, int32_t timeMs, int32_t curve, int32_t keepGoing);
    void setTimer(float ms);                                     // 0x45c550
    float timer() const { return t.timer + overflow; }           // getSplitActionTimer (0x5110e0)
    // The update's part (0x453280): the tween; after it, with keepGoing, time runs on at its speed.
    void step(const FrameClock& clock, float timeScale);
    // 0x453960: every piece where its group's motion is at the current time.
    void place();
};

}  // namespace nyan::diesel
