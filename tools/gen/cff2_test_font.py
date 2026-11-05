#!/usr/bin/env python3
"""Builds the tiny CFF2 variable font tests/core_tests.cpp embeds (testCff2Font): one glyph "A", a
square from (100,0) to (500,400) whose right and top edges move out by 100 at the wght axis' maximum
(one variation region, so each blend carries one delta). Needs fontTools.
    tools/gen/cff2_test_font.py <out.otf>"""
import io, sys
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
fb = FontBuilder(1000, isTTF=False)
fb.setupGlyphOrder([".notdef", "A"])
fb.setupCharacterMap({0x41: "A"})
# A square from (100,0) to (500,400) at the default; at wght max its right and top edges move by +100.
# blend: n defaults, n*k deltas, n.  One region (k = 1).
progA = [100, 0, 'rmoveto',
         400, 100, 1, 'blend', 0, 'rlineto',
         0, 400, 100, 1, 'blend', 'rlineto',
         -400, -100, 1, 'blend', 0, 'rlineto']
cs = {".notdef": T2CharString(program=[]), "A": T2CharString(program=progA)}
fb.setupHorizontalMetrics({".notdef": (500, 0), "A": (600, 100)})
fb.setupHorizontalHeader(ascent=800, descent=-200)
fb.setupNameTable({"familyName": "Nyan Test VF", "styleName": "Regular"})
fb.setupOS2(sTypoAscender=800, usWinAscent=800, usWinDescent=200)
fb.setupPost()
fb.setupFvar(axes=[("wght", 400, 400, 900, "Weight")], instances=[])
fb.setupCFF2(cs, regions=[{"wght": (0, 1.0, 1.0)}])
fb.updateHead(created=0, modified=0)  # the same bytes every time
fb.font.recalcTimestamp = False
buf = io.BytesIO(); fb.save(buf); data = buf.getvalue()
open(sys.argv[1], 'wb').write(data)
print(len(data), 'bytes')
