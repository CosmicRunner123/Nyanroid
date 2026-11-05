// The pointing hand the Diesel exes show for cursor shape 7 (their window procedure loads Windows'
// IDC_HAND). Windows' picture is not Nyanroid's to copy, so this is Nyanroid's own drawing of a
// pointing hand in a 32 x 32 frame (Windows' cursor size at 100%), white with a black outline,
// the index fingertip the hotspot. Shapes are capsules and a rounded box; each pixel is sampled
// 4 x 4 times against their signed distance (inside: white; within a pixel outside: the outline).
// Included by diesel_session.cpp only.

namespace hand_cursor {

struct Picture {
    int width = 32, height = 32;
    std::vector<uint32_t> argb;  // 0xAARRGGBB, rows top-down, alpha premultiplied out
    int hotX = 6, hotY = 0;
};

inline float capsule(float x, float y, float ax, float ay, float bx, float by, float r) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    float qx = px - dx * t, qy = py - dy * t;
    return std::sqrt(qx * qx + qy * qy) - r;
}

inline float roundBox(float x, float y, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// The hand: the index finger up, three fingers folded over the palm, the thumb against its side,
// the cuff below; lines between the folded fingers and above the cuff.
inline float shape(float x, float y) {
    float d = capsule(x, y, 6.5f, 3.0f, 6.5f, 12.0f, 2.0f);              // index finger
    d = std::min(d, capsule(x, y, 10.5f, 10.0f, 10.5f, 13.0f, 2.0f));    // middle
    d = std::min(d, capsule(x, y, 14.5f, 11.0f, 14.5f, 14.0f, 2.0f));    // ring
    d = std::min(d, capsule(x, y, 18.0f, 12.5f, 18.0f, 15.0f, 1.8f));    // little finger
    d = std::min(d, roundBox(x, y, 4.5f, 12.0f, 19.8f, 22.5f, 3.0f));    // palm
    d = std::min(d, capsule(x, y, 2.6f, 14.0f, 6.0f, 19.0f, 1.9f));      // thumb
    d = std::min(d, roundBox(x, y, 7.0f, 21.5f, 18.0f, 26.0f, 0.8f));    // cuff
    return d;
}

inline bool crease(float x, float y) {
    auto line = [&](float ax, float ay, float bx, float by) { return capsule(x, y, ax, ay, bx, by, 0.45f) < 0.0f; };
    return line(8.5f, 11.0f, 8.5f, 14.0f) || line(12.5f, 11.5f, 12.5f, 15.0f) || line(16.4f, 12.5f, 16.4f, 15.5f) ||
           line(7.5f, 22.5f, 17.5f, 22.5f);
}

inline Picture draw() {
    Picture p;
    p.argb.assign(static_cast<size_t>(p.width) * p.height, 0);
    constexpr int kSamples = 4;
    for (int py = 0; py < p.height; ++py) {
        for (int px = 0; px < p.width; ++px) {
            int white = 0, black = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    float x = px + (sx + 0.5f) / kSamples, y = py + (sy + 0.5f) / kSamples;
                    float d = shape(x, y);
                    if (d < 0.0f && !crease(x, y)) ++white;
                    else if (d < 1.1f) ++black;
                }
            }
            int covered = white + black;
            if (covered == 0) continue;
            uint32_t a = static_cast<uint32_t>(covered * 255 / (kSamples * kSamples));
            uint32_t v = static_cast<uint32_t>(white * 255 / covered);  // the colour of what covers it
            p.argb[static_cast<size_t>(py) * p.width + px] = a << 24 | v << 16 | v << 8 | v;
        }
    }
    return p;
}

}  // namespace hand_cursor
