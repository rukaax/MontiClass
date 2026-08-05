/**
 * @file control.cpp
 * @brief EC11 正交解码和按键消抖。
 *
 * EC11 的 A/B（本项目为 CLK/DT）是相位相差 90° 的方波。仅观察 CLK
 * 上升沿无法分辨触点反弹，容易把一次旋转识别成多步；因此这里记录完整
 * 的 00 -> 01 -> 11 -> 10 -> 00 状态序列，四个有效相位才提交一格。
 */
#include "control.h"

namespace {
// 同一触点在 150 us 内的毛刺直接丢弃。完整卡点还会有更长的独立保护，
// 两层过滤可大幅降低廉价 EC11 的误触发。
constexpr uint32_t kEdgeDebounceUs = 150;
constexpr uint32_t kDetentGuardUs = 4000;
constexpr uint32_t kButtonDebounceMs = 35;
constexpr uint32_t kLongPressMs = 500;

// 中断与主循环共享的数据。所有读取统计值的地方都要进入临界区。
volatile int16_t encoderDelta = 0;       // 已完成、尚未交给 UI 的卡点数
volatile int8_t quarterSteps = 0;        // 当前卡点已走过的 1/4 相位数
volatile uint8_t lastEncoderState = 0;   // bit1=CLK，bit0=DT
volatile uint32_t lastEdgeUs = 0;
volatile uint32_t lastDetentUs = 0;
volatile uint32_t validTransitions = 0;
volatile uint32_t invalidTransitions = 0;
volatile uint32_t filteredEdges = 0;
volatile int32_t totalDetents = 0;

// rawButtonDown 是未经消抖的引脚电平，stableButtonDown 才能驱动 UI。
bool rawButtonDown = false;
bool stableButtonDown = false;
uint32_t rawButtonChangedMs = 0;
uint32_t buttonPressedMs = 0;
bool shortPress = false;
bool longPress = false;
bool longPressFired = false;

// 用 (旧状态 << 2) | 新状态查表。表中 1/-1 代表一个四分之一相位；
// 对角跳变（例如 00 直接到 11）在正常旋转中不可能发生，记为 0。
// 符号特意与旧版“CLK 上升沿时 DT=HIGH 为正方向”的手感保持一致。
constexpr int8_t kQuadratureTable[16] = {
    0,  1, -1,  0,
   -1,  0,  0,  1,
    1,  0,  0, -1,
    0, -1,  1,  0,
};

uint8_t IRAM_ATTR readEncoderState() {
  return static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) << 1) |
                              digitalRead(Pins::kEncoderDt));
}

void IRAM_ATTR onEncoderEdge() {
  // ISR 只做状态采集和计数，绝不能 Serial.print 或刷新 OLED。
  const uint32_t now = micros();
  if (now - lastEdgeUs < kEdgeDebounceUs) {
    ++filteredEdges;
    return;
  }
  lastEdgeUs = now;

  const uint8_t newState = readEncoderState();
  const uint8_t previousState = lastEncoderState;
  const int8_t movement = kQuadratureTable[(previousState << 2) | newState];
  lastEncoderState = newState;

  if (movement == 0) {
    // 相同状态是无意义重复边沿；不同状态但 movement=0 为异常跳变。
    if (newState != previousState) ++invalidTransitions;
    return;
  }

  ++validTransitions;
  quarterSteps += movement;

  // 只有累计一整圈（4 个有效相位）才把结果交给应用层。
  // kDetentGuardUs 额外抑制“反弹绕完整圈”的罕见坏情况。
  if (quarterSteps >= 4 || quarterSteps <= -4) {
    if (now - lastDetentUs >= kDetentGuardUs) {
      const int8_t direction = quarterSteps > 0 ? 1 : -1;
      encoderDelta += direction;
      totalDetents += direction;
      lastDetentUs = now;
    } else {
      ++filteredEdges;
    }
    // 无论提交或被保护窗过滤，都从下一个完整卡点重新累计。
    quarterSteps = 0;
  }
}
}  // namespace

void controlInit() {
  // CLK/DT 触点按下时接地，故使用内部上拉；SW 按下时输出高电平，故下拉。
  pinMode(Pins::kEncoderClk, INPUT_PULLUP);
  pinMode(Pins::kEncoderDt, INPUT_PULLUP);
  pinMode(Pins::kEncoderSw, INPUT_PULLDOWN);

  // 在开启中断前读取初始相位，防止上电第一个边沿使用了错误的旧状态。
  lastEncoderState = readEncoderState();
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderClk), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderDt), onEncoderEdge, CHANGE);

}

void controlUpdate() {
  // 事件只存活一个主循环，调用者无需再手动清除。
  shortPress = false;
  longPress = false;

  const uint32_t now = millis();
  const bool nowRawDown = digitalRead(Pins::kEncoderSw) == HIGH;
  if (nowRawDown != rawButtonDown) {
    rawButtonDown = nowRawDown;
    rawButtonChangedMs = now;
  }

  // 电平稳定超过消抖时间后才认可为真正的按下/松开。
  if (rawButtonDown != stableButtonDown && now - rawButtonChangedMs >= kButtonDebounceMs) {
    stableButtonDown = rawButtonDown;
    if (stableButtonDown) {
      buttonPressedMs = now;
      longPressFired = false;
    } else if (!longPressFired) {
      // 松开前没有触发长按，才把它识别为一次短按。
      shortPress = true;
    }
  }

  // 按住达到阈值就立即触发，只触发一次；松手不会再补发短按或长按。
  if (stableButtonDown && !longPressFired && now - buttonPressedMs >= kLongPressMs) {
    longPress = true;
    longPressFired = true;
  }
}

int controlReadEncoderDelta() {
  // 取值和清零必须是一个原子操作，避免 ISR 恰好插入而丢失一步。
  noInterrupts();
  const int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();
  return delta;
}

bool controlWasShortPress() { return shortPress; }
bool controlWasLongPress() { return longPress; }
