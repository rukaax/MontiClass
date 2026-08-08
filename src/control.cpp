/**
 * @file control.cpp
 * @brief EC11 正交解码和按键消抖。
 *
 * EC11 的 A/B（本项目为 CLK/DT）是相位相差 90° 的方波。仅观察 CLK
 * 上升沿无法分辨触点反弹，容易把一次旋转识别成多步；因此这里记录完整
 * 的 00 -> 01 -> 11 -> 10 -> 00 状态序列，四个有效相位才提交一格。
 */
#include "control.h"          // 本模块自己的函数声明（引脚定义也在里面）

// C++ 的“匿名命名空间”：里面的名字只在本文件可见，作用等于 C 里的 static。
namespace {
// 同一触点在 150 us 内的毛刺直接丢弃。完整卡点还会有更长的独立保护，
// 两层过滤可大幅降低廉价 EC11 的误触发。
constexpr uint32_t kEdgeDebounceUs = 150;    // 边沿消抖：两次边沿至少间隔 150 微秒（constexpr = 编译期常量）
constexpr uint32_t kDetentGuardUs = 4000;    // 卡点保护：两次“提交一格”至少间隔 4000 微秒
constexpr uint32_t kButtonDebounceMs = 35;   // 按键消抖：电平要稳定 35 毫秒才算数
constexpr uint32_t kLongPressMs = 500;       // 长按阈值：按住 500 毫秒判定为长按

// 中断与主循环共享的数据。所有读取统计值的地方都要进入临界区。
// volatile 告诉编译器这些变量可能被中断随时改动，不要做优化缓存。
volatile int16_t encoderDelta = 0;       // 已完成、尚未交给 UI 的卡点数
volatile int8_t quarterSteps = 0;        // 当前卡点已走过的 1/4 相位数
volatile uint8_t lastEncoderState = 0;   // bit1=CLK，bit0=DT
volatile uint32_t lastEdgeUs = 0;        // 上一次有效边沿的时间戳（微秒）
volatile uint32_t lastDetentUs = 0;      // 上一次提交一格的时间戳（微秒）
volatile uint32_t validTransitions = 0;  // 统计：有效相位跳变次数
volatile uint32_t invalidTransitions = 0;// 统计：异常跳变次数
volatile uint32_t filteredEdges = 0;     // 统计：被过滤掉的边沿次数
volatile int32_t totalDetents = 0;       // 统计：累计走过的卡点数（含方向）

// rawButtonDown 是未经消抖的引脚电平，stableButtonDown 才能驱动 UI。
bool rawButtonDown = false;          // 原始电平：当前引脚读到的按下状态
bool stableButtonDown = false;       // 消抖后的稳定按下状态
uint32_t rawButtonChangedMs = 0;     // 原始电平上次变化的时间戳（毫秒）
uint32_t buttonPressedMs = 0;        // 稳定按下开始的时间戳（毫秒）
bool shortPress = false;             // 本轮是否发生了短按
bool longPress = false;              // 本轮是否发生了长按
bool longPressFired = false;         // 本次按下是否已触发过长按（防止重复触发）

// 用 (旧状态 << 2) | 新状态查表。表中 1/-1 代表一个四分之一相位；
// 对角跳变（例如 00 直接到 11）在正常旋转中不可能发生，记为 0。
// 符号特意与旧版“CLK 上升沿时 DT=HIGH 为正方向”的手感保持一致。
constexpr int8_t kQuadratureTable[16] = {
    0,  1, -1,  0,
   -1,  0,  0,  1,
    1,  0,  0, -1,
    0, -1,  1,  0,
};

// 读取当前编码器两根线的电平，组合成一个 2 位状态：bit1=CLK，bit0=DT。
// IRAM_ATTR 让这个函数放进内部 RAM，中断里调用更快更安全。
uint8_t IRAM_ATTR readEncoderState() {
  return static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) << 1) |   // CLK 左移 1 位放到高位
                              digitalRead(Pins::kEncoderDt));            // DT 放到低位；static_cast 是 C++ 的类型转换
}

// 中断服务函数：CLK 或 DT 电平一变化就被调用。
void IRAM_ATTR onEncoderEdge() {
  // ISR 只做状态采集和计数，绝不能 Serial.print 或刷新 OLED。
  const uint32_t now = micros();            // 取当前微秒时间
  if (now - lastEdgeUs < kEdgeDebounceUs) { // 距上次边沿太近，判定为毛刺
    ++filteredEdges;                        // 过滤计数 +1
    return;                                 // 直接丢弃
  }
  lastEdgeUs = now;                         // 记录本次边沿时间

  const uint8_t newState = readEncoderState();          // 读当前状态
  const uint8_t previousState = lastEncoderState;       // 取上一次状态
  const int8_t movement = kQuadratureTable[(previousState << 2) | newState];  // 查表得到方向：+1/-1/0
  lastEncoderState = newState;                          // 更新“上一次状态”

  if (movement == 0) {                      // 没有有效移动
    // 相同状态是无意义重复边沿；不同状态但 movement=0 为异常跳变。
    if (newState != previousState) ++invalidTransitions;  // 状态变了却查到 0，记为异常
    return;                                 // 结束本次中断
  }

  ++validTransitions;                       // 有效跳变计数 +1
  quarterSteps += movement;                 // 累加 1/4 相位（带方向）

  // 只有累计一整圈（4 个有效相位）才把结果交给应用层。
  // kDetentGuardUs 额外抑制“反弹绕完整圈”的罕见坏情况。
  if (quarterSteps >= 4 || quarterSteps <= -4) {          // 攒够一整格（正向或反向）
    if (now - lastDetentUs >= kDetentGuardUs) {           // 距上次提交足够久，才认可
      const int8_t direction = quarterSteps > 0 ? 1 : -1; // 三目运算符：正数取 +1，否则 -1
      encoderDelta += direction;                          // 把这一格交给 UI
      totalDetents += direction;                          // 累计统计
      lastDetentUs = now;                                 // 记录提交时间
    } else {
      ++filteredEdges;                                    // 太快，判定为反弹并过滤
    }
    // 无论提交或被保护窗过滤，都从下一个完整卡点重新累计。
    quarterSteps = 0;                                     // 1/4 相位计数清零
  }
}
}  // namespace   // 匿名命名空间结束

// 初始化：配置引脚并挂上中断。
void controlInit() {
  // CLK/DT 触点按下时接地，故使用内部上拉；SW 按下时输出高电平，故下拉。
  pinMode(Pins::kEncoderClk, INPUT_PULLUP);    // CLK 上拉输入
  pinMode(Pins::kEncoderDt, INPUT_PULLUP);     // DT 上拉输入
  pinMode(Pins::kEncoderSw, INPUT_PULLDOWN);   // 按键 SW 下拉输入

  // 在开启中断前读取初始相位，防止上电第一个边沿使用了错误的旧状态。
  lastEncoderState = readEncoderState();       // 预读初始状态
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderClk), onEncoderEdge, CHANGE);  // CLK 电平变化触发中断
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderDt), onEncoderEdge, CHANGE);   // DT 电平变化触发中断

}

// 主循环每轮调用：处理按键消抖，生成短按/长按事件。
void controlUpdate() {
  // 事件只存活一个主循环，调用者无需再手动清除。
  shortPress = false;      // 清掉上一轮的短按标记
  longPress = false;       // 清掉上一轮的长按标记

  const uint32_t now = millis();                              // 当前毫秒时间
  const bool nowRawDown = digitalRead(Pins::kEncoderSw) == HIGH;  // 读按键原始电平（高=按下）
  if (nowRawDown != rawButtonDown) {                          // 原始电平发生变化
    rawButtonDown = nowRawDown;                               // 记录新电平
    rawButtonChangedMs = now;                                 // 记录变化时间，重新开始计消抖
  }

  // 电平稳定超过消抖时间后才认可为真正的按下/松开。
  if (rawButtonDown != stableButtonDown && now - rawButtonChangedMs >= kButtonDebounceMs) {
    stableButtonDown = rawButtonDown;      // 更新稳定状态
    if (stableButtonDown) {                // 变为“按下”
      buttonPressedMs = now;               // 记录按下开始时间
      longPressFired = false;              // 重置长按触发标记
    } else if (!longPressFired) {          // 变为“松开”，且此前没触发过长按
      // 松开前没有触发长按，才把它识别为一次短按。
      shortPress = true;                   // 判定为短按
    }
  }

  // 按住达到阈值就立即触发，只触发一次；松手不会再补发短按或长按。
  if (stableButtonDown && !longPressFired && now - buttonPressedMs >= kLongPressMs) {
    longPress = true;          // 判定为长按
    longPressFired = true;     // 标记已触发，避免重复
  }
}

// 取走并清零 ISR 累计的旋转格数。
int controlReadEncoderDelta() {
  // 取值和清零必须是一个原子操作，避免 ISR 恰好插入而丢失一步。
  noInterrupts();                    // 关中断，进入临界区
  const int delta = encoderDelta;    // 读取累计值
  encoderDelta = 0;                  // 清零
  interrupts();                      // 开中断，离开临界区
  return delta;                      // 返回这段时间的旋转量
}

// 对外查询：本轮是否有短按事件。
bool controlWasShortPress() { return shortPress; }
// 对外查询：本轮是否有长按事件。
bool controlWasLongPress() { return longPress; }
