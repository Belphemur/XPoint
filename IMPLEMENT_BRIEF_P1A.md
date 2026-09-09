GOAL: Phase 1a — font loading infrastructure, no reader changes yet.

SCOPE: new src/BookFontLoader.{h,cpp}:
- Scan /fonts/*.ttf|.otf on SD; build the family manifest.
- Two-tier allocation: PSRAM tier (S3) vs C3 DRAM tier. kMaxDramFontBytes
  is derived from ESP.getFreeHeap()/getMaxAllocHeap() measured after all
  arenas are allocated (keep the 32KB/16KB heap-gate floors) — NOT a
  hardcoded 256KB (§3.3). Oversized files: LOG_ERR("BFNT", "Font %s too
  large for DRAM tier (%u > %u)") and stay listed but greyed out.
- FontChain assembly (≤8 faces, styleCoverage()).
- fontFingerprint() = FNV-1a over the LOADED font bytes ⊕ styleCoverage
  — content-based, never path/mtime (§3.4).
- Builtin fallback: singleton FontChain over BitmapBookFont (4 style
  instances = 16KB static BSS — accounted in C3 budget).
- sfnt VALIDATION BOUNDARY before TtfFont::init: table-directory bounds +
  numTables sanity; on failure LOG_ERR + skip the face. TtfFont::init only
  checks len<12 (TtfFont.cpp:36). Test corpus includes 2 deliberately
  malformed fonts.
- Face bytes: loaded through the framebuffer loan (loadFaceBytes), NOT kept
  resident; stb needs bytes addressable only during init().

WATCH: single shared glyph arena backs all chain faces — TtfFont::flushGlyphs
  rewinds to the face's init mark (TtfFont.cpp:133) and can invalidate
  later faces' cached glyphs. Verify alternating styles do not storm
  cross-face invalidation; per-face arenas is the accepted fix if it does.

GATE: pio run; on-device log listing discovered families with fingerprints;
  malformed fonts skipped with LOG_ERR, no crash.

BUILD NOTES:
- Kanban materializes YOUR worktree at .worktrees/<task-id> on the branch
  above before you start. Do NOT pre-create branches/worktrees manually
  (causes branch-already-checked-out conflicts).
- After claiming: git submodule update --init freeink-sdk, then wire the
  shared build cache per the crosspoint-reader-dev skill (platformio.local.ini
  -> .pio-build-cache).
- Build gate: pio run -e default unless stated otherwise. Format:
  ./bin/clang-format-fix -g. Obey AGENTS.md: makeUniqueNoThrow (never bare
  new), no std::string in hot paths, tr() for UI strings, HalStorage only
  (never SdFat direct).

REVIEW GATE — multiple rounds, mandatory before merge:
1. Push and open a PR to Belphemur/XPoint (base develop). Attach the PR URL
   to this card (kanban_attach_url).
2. Review loop until CLEAN: python3 ~/.hermes/skills/github/answer-code-review/scripts/reply_review.py threads --pr <N> --open --full
   lists open GitHub Copilot AND CodeRabbit threads. Answer EVERY thread
   with file:line evidence (reply_review.py answer / bulk), applying
   required code/doc fixes. reply_review.py check --pr <N> must report
   ALL resolved.
3. After your LAST push, wait for the fresh Copilot + CodeRabbit passes
   triggered by the push and clear any new threads — you need at least
   one full re-review round with zero code changes after it.
4. CI green: gh pr checks <N> — all required checks pass.
5. Only then merge via the pr-merge-gate skill. Never merge with open
   threads or failing CI.

OPENCODE (optional help): write the brief to a file, then:
opencode run --dir <worktree> --agent build --auto -m opencode-go/glm-5.3-flash
--title "<short-slug>" "Acknowledge: the task brief is on stdin. Read it in
full, then proceed." < /tmp/brief.md > /tmp/oc_<slug>.log 2>&1.
kimi-k3 is reserved for heavy design review (audit task only). Never commit
investigation artifacts.

AUTHORITATIVE DESIGN: DESIGN_NATIVE_TTF_SUPPORT.md on branch
  design/native-ttf-support (worktree crosspoint-x-reader-design-ttf).
Cite file:line from it.