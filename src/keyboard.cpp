#include "keyboard.h"
#include "display.h"
#include "theme.h"

// Three layouts: lower, upper, symbols. Each row is a string of keys; special
// keys are handled separately (shift, backspace, space, OK, cancel).
static const char *ROWS_LOWER[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *ROWS_UPPER[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char *ROWS_SYM[3] = {"1234567890", "@#$_-+()/", ".,:;!?'\""};

enum Layout { LOWER, UPPER, SYM };

static bool s_active = false;
static bool s_committed = false;
static bool s_mask = false;     // field is a password (dots by default)
static bool s_reveal = false;   // user tapped "show"
static Layout s_layout = LOWER;
static size_t s_maxLen = 0;
static char s_buf[80];
static char s_title[40];

// Geometry
#define KB_TOP 92          // keyboard area starts here
#define ROW_H 40
#define KEY_GAP 4
#define FIELD_Y 44

static const char *const *curRows() {
  if (s_layout == UPPER) return ROWS_UPPER;
  if (s_layout == SYM) return ROWS_SYM;
  return ROWS_LOWER;
}

void keyboard_open(const char *title, const char *initial, size_t maxLen,
                   bool mask) {
  s_active = true;
  s_committed = false;
  s_layout = LOWER;
  s_mask = mask;
  s_reveal = false;
  s_maxLen = min(maxLen, sizeof(s_buf) - 1);
  strncpy(s_buf, initial ? initial : "", s_maxLen);
  s_buf[s_maxLen] = '\0';
  strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
  s_title[sizeof(s_title) - 1] = '\0';
}

bool keyboard_active() { return s_active; }
bool keyboard_committed() { return s_committed; }
const char *keyboard_value() { return s_buf; }

// Compute the x/width for key index k in a row of n keys.
static void keyCell(int n, int k, int16_t &x, int16_t &w) {
  const int16_t W = gfx->width();
  const int16_t margin = 6;
  int16_t avail = W - 2 * margin;
  w = (avail - (n - 1) * KEY_GAP) / n;
  x = margin + k * (w + KEY_GAP);
}

static int16_t rowY(int r) { return KB_TOP + r * (ROW_H + KEY_GAP); }

void keyboard_draw() {
  const int16_t W = gfx->width();
  gfx->fillScreen(COL_BG);

  // Title + text field. Password fields reserve room for a show/hide button.
  drawCenteredText(s_title, W / 2, 18, 2, COL_MUTED);
  const int16_t showBtnW = 62;
  int16_t fieldW = s_mask ? (W - 20 - showBtnW - 6) : (W - 20);
  gfx->fillRoundRect(10, FIELD_Y - 4, fieldW, 30, 6, COL_TILE);

  char shown[80];
  if (s_mask && !s_reveal) {
    size_t n = strlen(s_buf);
    for (size_t i = 0; i < n && i < sizeof(shown) - 1; i++) shown[i] = '*';
    shown[min(n, sizeof(shown) - 1)] = '\0';
  } else {
    strncpy(shown, s_buf, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
  }
  drawText(strlen(shown) ? shown : "", 18, FIELD_Y + 4, 2, COL_TEXT);

  if (s_mask) {  // show/hide toggle to the right of the field
    int16_t bx = 10 + fieldW + 6;
    gfx->fillRoundRect(bx, FIELD_Y - 4, showBtnW, 30, 6, COL_TILE_DN);
    drawCenteredText(s_reveal ? "hide" : "show", bx + showBtnW / 2, FIELD_Y + 11,
                     1, COL_TEXT);
  }

  // Character rows.
  const char *const *rows = curRows();
  for (int r = 0; r < 3; r++) {
    int n = strlen(rows[r]);
    int16_t y = rowY(r);
    for (int k = 0; k < n; k++) {
      int16_t x, w;
      keyCell(n, k, x, w);
      gfx->fillRoundRect(x, y, w, ROW_H, 6, COL_TILE);
      char c[2] = {rows[r][k], '\0'};
      drawCenteredText(c, x + w / 2, y + ROW_H / 2, 2, COL_TEXT);
    }
  }

  // Bottom function row: [Shift/123] [space] [<] [Cancel] [OK]
  int16_t y = rowY(3);
  const int16_t margin = 6;
  int16_t fw = (W - 2 * margin - 4 * KEY_GAP) / 5;
  auto fkey = [&](int idx, const char *lbl, uint16_t bg, uint16_t fg) {
    int16_t x = margin + idx * (fw + KEY_GAP);
    int16_t w = (idx == 1) ? (fw * 2 + KEY_GAP) : fw;  // space is double-wide
    gfx->fillRoundRect(x, y, w, ROW_H, 6, bg);
    drawCenteredText(lbl, x + w / 2, y + ROW_H / 2, 2, fg);
  };
  fkey(0, s_layout == SYM ? "abc" : "^123", COL_TILE_DN, COL_TEXT);
  fkey(1, "space", COL_TILE, COL_TEXT);  // occupies slots 1-2
  fkey(3, "<", COL_TILE_DN, COL_TEXT);
  // OK + Cancel share the last slot stacked horizontally is awkward; use slot 4
  // split: draw Cancel above-handled as wide OK; instead put Cancel at far right
  // via a separate small bar.
  int16_t x4 = margin + 4 * (fw + KEY_GAP);
  gfx->fillRoundRect(x4, y, fw, ROW_H, 6, COL_ON);
  drawCenteredText("OK", x4 + fw / 2, y + ROW_H / 2, 2, COL_BG);

  // Cancel: a slim button on the title row, top-right.
  gfx->fillRoundRect(W - 78, 10, 68, 26, 6, COL_WARN);
  drawCenteredText("Cancel", W - 78 + 34, 23, 1, COL_TEXT);

  gfx->flush();
}

bool keyboard_handle_touch(int16_t x, int16_t y) {
  const int16_t W = gfx->width();

  // Cancel (title-row button).
  if (x >= W - 78 && x <= W - 10 && y >= 10 && y <= 36) {
    s_active = false;
    s_committed = false;
    return true;
  }

  // Show/hide toggle (password fields only), beside the text field.
  if (s_mask) {
    const int16_t showBtnW = 62;
    int16_t bx = 10 + (W - 20 - showBtnW - 6) + 6;
    if (x >= bx && x <= bx + showBtnW && y >= FIELD_Y - 4 && y <= FIELD_Y + 26) {
      s_reveal = !s_reveal;
      keyboard_draw();
      return false;
    }
  }

  // Character rows.
  const char *const *rows = curRows();
  for (int r = 0; r < 3; r++) {
    int16_t ry = rowY(r);
    if (y < ry || y >= ry + ROW_H) continue;
    int n = strlen(rows[r]);
    for (int k = 0; k < n; k++) {
      int16_t kx, kw;
      keyCell(n, k, kx, kw);
      if (x >= kx && x < kx + kw) {
        size_t len = strlen(s_buf);
        if (len < s_maxLen) {
          s_buf[len] = rows[r][k];
          s_buf[len + 1] = '\0';
          keyboard_draw();
        }
        return false;
      }
    }
    return false;  // tapped a gap in this row
  }

  // Function row.
  int16_t fy = rowY(3);
  if (y >= fy && y < fy + ROW_H) {
    const int16_t margin = 6;
    int16_t fw = (W - 2 * margin - 4 * KEY_GAP) / 5;
    int16_t x0 = margin;
    int16_t x1 = margin + (fw + KEY_GAP);              // space start
    int16_t x3 = margin + 3 * (fw + KEY_GAP);          // backspace
    int16_t x4 = margin + 4 * (fw + KEY_GAP);          // OK

    if (x >= x0 && x < x0 + fw) {  // shift / layout
      s_layout = (s_layout == LOWER) ? UPPER
                 : (s_layout == UPPER) ? SYM
                                       : LOWER;
      keyboard_draw();
      return false;
    }
    if (x >= x1 && x < x1 + 2 * fw + KEY_GAP) {  // space
      size_t len = strlen(s_buf);
      if (len < s_maxLen) {
        s_buf[len] = ' ';
        s_buf[len + 1] = '\0';
        keyboard_draw();
      }
      return false;
    }
    if (x >= x3 && x < x3 + fw) {  // backspace
      size_t len = strlen(s_buf);
      if (len > 0) {
        s_buf[len - 1] = '\0';
        keyboard_draw();
      }
      return false;
    }
    if (x >= x4 && x < x4 + fw) {  // OK
      s_active = false;
      s_committed = true;
      return true;
    }
  }
  return false;
}
