#include <time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_pm.h>

// 設定関係
#pragma region Configrations

// WiFi設定
#define WIFI_SSID "(SSID)"          // 接続するアクセスポイントのSSID（要設定！）
#define WIFI_PASS "(PASSWORD)"      // 接続するアクセスポイントのパスワード（要設定！）

// NTPサーバ設定
#define NTP_HOST1 "(NTP HOST)"      // NTPサーバを設定（要設定！）
#define NTP_HOST2 "ntp.jst.mfeed.ad.jp"
#define NTP_HOST3 "ntp.nict.jp"

// GPIOピン設定
#define GPIO_5V GPIO_NUM_32        // 5Vライン制御
#define GPIO_JJY GPIO_NUM_33      // 電波発信制御
#define GPIO_LED_POW GPIO_NUM_4  // パワーLED
#define GPIO_LED_ERR GPIO_NUM_16  // エラーLED
#define GPIO_SW1 GPIO_NUM_35      // 強制発信スイッチ
#define GPIO_SW2 GPIO_NUM_34      // 未使用（予備）

// 発信スケジュール（開始時間が早い順に時分を設定）
const struct scheInfo {
  int m_hour;
  int m_minute;
} schedules[] = {
  { 3, 57 }, { 7, 57 }, { 11, 57 }, { 15, 57 }, { 19, 57 }, { 23, 57 }
};
#define SCHE_TIMES_MINITES 10  // 1回に発信する時間（分）

// その他
#define TIMEZONE "JST-9"  // タイムゾーン

#pragma endregion

hw_timer_t* g_signalOnTimer = nullptr;              // 発信ONタイマ
hw_timer_t* g_signalOffTimer = nullptr;             // 発信OFFタイマ
volatile SemaphoreHandle_t g_hSemaphore = nullptr;  // 割り込みタイマ通知用のセマフォ
volatile bool contMode = false;                     // 連続動作モード

// スケジュール処理
#pragma region Schedule

#define SCHE_LENGTH sizeof(schedules) / sizeof(scheInfo)

/*
 * 指定した時間がスケジュールに含まれているかどうか取得
 * curTime: 確認する時間
 * resultTime: 含まれている場合は終了時間、含まれていない場合は次の開始時間
 * result: 含まれている場合は、true。それ以外は、false。
 */
bool checkSchedule(const struct tm* curTime, struct tm* resultTime) {
  struct tm tmpTime = *curTime;
  time_t begin, end, cur = mktime(&tmpTime);

  for (int i = 0; i < SCHE_LENGTH; ++i) {
    // 開始時間を現在日で表現
    tmpTime.tm_hour = schedules[i].m_hour;
    tmpTime.tm_min = schedules[i].m_minute;
    tmpTime.tm_sec = 0;
    // 開始時間を経過秒単位に変換
    begin = mktime(&tmpTime);

    if (begin <= cur) {
      // 開始時間が過ぎているスケジュール
      end = begin + SCHE_TIMES_MINITES * 60 + 1;
      // 終了時間を確認
      if (cur < end) {
        // 現在時間がスケジュールに含まれている
        localtime_r(&end, resultTime);
        return true;
      }
    } else {
      // 開始時間が現在時間より先のスケジュール（次のスケジュール）
      if (i == 0) {
        // 先頭のときは前日最後のスケジュールに該当しないか確認
        tmpTime.tm_hour = schedules[SCHE_LENGTH - 1].m_hour;
        tmpTime.tm_min = schedules[SCHE_LENGTH - 1].m_minute;
        tmpTime.tm_sec = 0;
        end = (mktime(&tmpTime) - 86400) + SCHE_TIMES_MINITES * 60 + 1;
        if (cur < end) {
          // 現在時間がスケジュールに含まれている
          localtime_r(&end, resultTime);
          return true;
        }
      }
      // 該当するスケジュールなし
      localtime_r(&begin, resultTime);
      return false;
    }
  }

  // 該当するスケジュールがない場合は、翌日の先頭が次のスケジュール
  tmpTime.tm_hour = schedules[0].m_hour;
  tmpTime.tm_min = schedules[0].m_minute;
  tmpTime.tm_sec = 0;
  begin = mktime(&tmpTime) + 86400;
  localtime_r(&begin, resultTime);

  return false;
}

#pragma endregion

// JJY処理
#pragma region JJY

#define JJY_MARKER 0x02
#define JJY_ON 0x05
#define JJY_OFF 0x08
#define JJY_IS(c) c ? JJY_ON : JJY_OFF
#define JJY_GET_ONTIME(v) v * 100 * 1000

/*
 最大2桁の10進数をBCDに変換
 c: 変換する10進数の値
 */
unsigned char bin2bcd2(unsigned int c) {
  // 3シフト目までは必ず条件に一致しないので飛ばす
  c = c << 3;
  for (int i = 3; i < 8; ++i) {
    if ((c & 0xF000) > 0x4000)
      c = c + 0x3000;
    if ((c & 0xF00) > 0x400)
      c = c + 0x300;
    c = c << 1;
  }
  return c >> 8;
}

/*
 最大3桁の10進数をBCDに変換
  c: 変換する10進数の値
 */
unsigned short bin2bcd3(unsigned int c) {
  // 3シフト目までは必ず条件に一致しないので飛ばす
  c = c << 3;
  for (int i = 3; i < 12; ++i) {
    if ((c & 0xF00000) > 0x400000)
      c = c + 0x300000;
    if ((c & 0xF0000) > 0x40000)
      c = c + 0x30000;
    if ((c & 0xF000) > 0x4000)
      c = c + 0x3000;
    c = c << 1;
  }
  return c >> 12;
}

/*
 8bit分の偶数パリティを計算
  c: 計算対象の値
 */
inline unsigned char calcParity(unsigned char c) {
  // NOTE: https://qiita.com/shiozaki/items/e803483cbd8b3c6ab28d
  c ^= c >> 4;
  c ^= c >> 2;
  c ^= c >> 1;
  return c & 0x01;
}

/*
 指定した日時のJJYフォーマットのタイムコードを生成
 time: タイムコードを生成する対象の時間
 pTimecode: 生成したタイムコードを格納する60バイトの配列
 */
void createJjyTimeCode(const struct tm* time, unsigned char pTimecode[60]) {
  unsigned int tmp;

  // NOTE: https://www.nict.go.jp/sts/jjy_signal.html
  // NOTE: 毎時15分および45分の呼出符号および停波予告は、一般的な電波時計には無関係と思われるので対応しない

  pTimecode[0] = JJY_MARKER;

  // 分 (1 - 8秒)
  tmp = bin2bcd2(time->tm_min);
  pTimecode[1] = JJY_IS(tmp & 0x40);
  pTimecode[2] = JJY_IS(tmp & 0x20);
  pTimecode[3] = JJY_IS(tmp & 0x10);
  pTimecode[4] = JJY_OFF;
  pTimecode[5] = JJY_IS(tmp & 0x08);
  pTimecode[6] = JJY_IS(tmp & 0x04);
  pTimecode[7] = JJY_IS(tmp & 0x02);
  pTimecode[8] = JJY_IS(tmp & 0x01);
  // パリティ:分 (37秒)
  pTimecode[37] = JJY_IS(calcParity(tmp));

  pTimecode[9] = JJY_MARKER;

  // 時 (10 - 18秒)
  tmp = bin2bcd2(time->tm_hour);
  pTimecode[10] = JJY_OFF;
  pTimecode[11] = JJY_OFF;
  pTimecode[12] = JJY_IS(tmp & 0x20);
  pTimecode[13] = JJY_IS(tmp & 0x10);
  pTimecode[14] = JJY_OFF;
  pTimecode[15] = JJY_IS(tmp & 0x08);
  pTimecode[16] = JJY_IS(tmp & 0x04);
  pTimecode[17] = JJY_IS(tmp & 0x02);
  pTimecode[18] = JJY_IS(tmp & 0x01);
  // パリティ:時 (36秒)
  pTimecode[36] = JJY_IS(calcParity(tmp));

  pTimecode[19] = JJY_MARKER;

  // 0固定 (20, 21秒)
  pTimecode[20] = JJY_OFF;
  pTimecode[21] = JJY_OFF;

  // 通算日 (22 - 33秒)
  tmp = bin2bcd3(time->tm_yday + 1);
  pTimecode[22] = JJY_IS(tmp & 0x200);
  pTimecode[23] = JJY_IS(tmp & 0x100);
  pTimecode[24] = JJY_OFF;
  pTimecode[25] = JJY_IS(tmp & 0x80);
  pTimecode[26] = JJY_IS(tmp & 0x40);
  pTimecode[27] = JJY_IS(tmp & 0x20);
  pTimecode[28] = JJY_IS(tmp & 0x10);
  pTimecode[29] = JJY_MARKER;
  pTimecode[30] = JJY_IS(tmp & 0x8);
  pTimecode[31] = JJY_IS(tmp & 0x4);
  pTimecode[32] = JJY_IS(tmp & 0x2);
  pTimecode[33] = JJY_IS(tmp & 0x1);

  // 0固定 (34, 35秒)
  pTimecode[34] = JJY_OFF;
  pTimecode[35] = JJY_OFF;

  // 予備 (38秒)
  pTimecode[38] = JJY_OFF;
  pTimecode[39] = JJY_MARKER;

  // 予備 (40秒)
  pTimecode[40] = JJY_OFF;

  // 西暦年の下２桁 (41 - 48秒)
  // FIXME: 2100年前後に使う場合は要修正(その時代を生きる人にお任せします。)
  tmp = bin2bcd2(time->tm_year - 100);
  pTimecode[41] = JJY_IS(tmp & 0x80);
  pTimecode[42] = JJY_IS(tmp & 0x40);
  pTimecode[43] = JJY_IS(tmp & 0x20);
  pTimecode[44] = JJY_IS(tmp & 0x10);
  pTimecode[45] = JJY_IS(tmp & 0x8);
  pTimecode[46] = JJY_IS(tmp & 0x4);
  pTimecode[47] = JJY_IS(tmp & 0x2);
  pTimecode[48] = JJY_IS(tmp & 0x1);

  pTimecode[49] = JJY_MARKER;

  // 曜日 (50 - 52秒)
  // NOTE: 1桁はBCDに変換しても変わらないのでそのまま使用
  pTimecode[50] = JJY_IS(time->tm_wday & 0x4);
  pTimecode[51] = JJY_IS(time->tm_wday & 0x2);
  pTimecode[52] = JJY_IS(time->tm_wday & 0x1);

  // うるう秒の予告 (53 - 54秒)
  // NOTE: うるう秒でズレても次の同期で合うので考慮しない
  pTimecode[53] = JJY_OFF;
  pTimecode[54] = JJY_OFF;

  // 0固定 (55 - 58秒)
  pTimecode[55] = JJY_OFF;
  pTimecode[56] = JJY_OFF;
  pTimecode[57] = JJY_OFF;
  pTimecode[58] = JJY_OFF;

  pTimecode[59] = JJY_MARKER;
}

#pragma endregion

// エラー通知処理
#pragma region Notify Error

/*
 * エラー通知
 */
void notifyError() {
  // エラーLED点灯
  digitalWrite(GPIO_LED_ERR, HIGH);
}

/*
 * エラー通知解除
 */
void clearError() {
  // エラーLED消灯
  digitalWrite(GPIO_LED_ERR, LOW);
}

#pragma endregion

// 割り込み処理
#pragma region OnInterrupt

/*
 * 初期化中のパワーLED点滅用タイマ割り込み
 */
void IRAM_ATTR onBlinkPowLedTimer() {
  // 500ms間隔で点滅
  digitalWrite(GPIO_LED_POW, (millis() / 500) & 1 ? HIGH : LOW);
}

/*
 * 電波ONタイマ割り込み
 */
void IRAM_ATTR onSignalOnTimer() {
  // ESP32 3.0.0で ets_delay_us が消えたため一旦削除
  /*
  struct timespec nowTime;
  if (clock_gettime(CLOCK_REALTIME, &nowTime) != -1) {
    // 残り時間を待機
    //ets_delay_us((1000000000 - nowTime.tv_nsec) / 1000);
    // 残り時間を待機（発信までの遅延を考慮して僅かに短め）
    ets_delay_us((999999000 - nowTime.tv_nsec) / 1000);
  } else {
    log_w("error clock_gettime");
  }
  */

  // 出力ON
  digitalWrite(GPIO_JJY, HIGH);
  // 通知
  xSemaphoreGiveFromISR(g_hSemaphore, nullptr);
}

/*
 * 電波OFFタイマ割り込み
 */
void IRAM_ATTR onSignalOffTimer() {
  // 出力OFF
  digitalWrite(GPIO_JJY, LOW);
  // 通知
  xSemaphoreGiveFromISR(g_hSemaphore, nullptr);
}

volatile int sw1LastState = LOW;               // SW1の最後のIO状態
volatile unsigned long sw1LastChangeTime = 0;  // SW1を押した時間

/*
 * SW1割り込み
 */
void IRAM_ATTR onSw1Push() {
  // GPIOの状態を取得
  int newState = digitalRead(GPIO_SW1);
  if (newState != sw1LastState) {
    if (newState == HIGH) {
      // SWが押された
      // 時間を記録
      sw1LastChangeTime = millis();
    } else {
      // SWが離された
      if (millis() - sw1LastChangeTime > 150) {
        // 150ms以上押したときだけ反応
        // 強制発信モード切り替え
        contMode = !contMode;
        if (contMode)
          log_i("Enabled continuous mode");
        else
          log_i("Disabled continuous mode");
      }
    }

    sw1LastState = newState;
  }
}

#pragma endregion

/*
 * 初期化
 */
void setup() {
  // デバッグ出力用のシリアル出力を初期化
  Serial.begin(115200);
  log_i("Start setup");

  // GPIO初期化
  log_i("Initialize GPIO");
  pinMode(GPIO_5V, OUTPUT);
  pinMode(GPIO_JJY, OUTPUT);
  pinMode(GPIO_LED_POW, OUTPUT);
  pinMode(GPIO_LED_ERR, OUTPUT);
  pinMode(GPIO_SW1, INPUT);
  pinMode(GPIO_SW2, INPUT);

  // タイマ初期化
  log_i("Initialize Timer");
  auto timer = timerBegin(1000000);
  // パワーLED点滅開始
  timerAttachInterrupt(timer, &onBlinkPowLedTimer);
  timerAlarm(timer, 500000, true, 0);
  // 電波On/Off制御用タイマ初期化
  g_signalOnTimer = timerBegin(1000000);
  timerAttachInterrupt(g_signalOnTimer, &onSignalOnTimer);
  g_signalOffTimer = timerBegin(1000000);
  timerAttachInterrupt(g_signalOffTimer, &onSignalOffTimer);

  // 割り込み通知用セマフォ初期化
  g_hSemaphore = xSemaphoreCreateBinary();
  if (g_hSemaphore == nullptr) {
    notifyError();
    log_e("Failed create semaphore...");
    abort();
    return;
  }

  // Wi-Fi初期化
  log_i("Initialize Wi-Fi");
  log_i("SSID: %s", WIFI_SSID);
  log_d("PASS: %s", WIFI_PASS);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Wi-Fi接続まで待機
  auto tryStart = millis();
  int retryCount = 0;
  for (;;) {
    auto stat = WiFi.status();
    if (stat == WL_CONNECTED) {
      log_i("Connected Wi-Fi! (IP: %s)", WiFi.localIP().toString().c_str());
      break;
    }

    if ((stat != WL_IDLE_STATUS && stat != WL_DISCONNECTED) || millis() - tryStart >= 20000) {
      // 接続失敗もしくは20秒待機しても完了しない場合は再試行
      log_e("Failed connect Wi-Fi...(stat=%i)", stat);

      // 高速で再試行しないよう3秒待機
      delay(3000);

      // 再試行
      if (++retryCount == 3) {
        // ３回ダメならエラーを通知
        notifyError();
      }
      log_i("Retry connect Wi-Fi.");
      WiFi.disconnect();
      WiFi.reconnect();

      // 開始時間更新
      tryStart = millis();
    }

    delay(300);
  }

  // エラー通知クリア
  clearError();

  // NTP初期化
  log_i("Syncing clock to ntp server. [%s/%s/%s]", NTP_HOST1, NTP_HOST2, NTP_HOST3);
  configTzTime(TIMEZONE, NTP_HOST1, NTP_HOST2, NTP_HOST3);

  // 同期完了まで待機
  tryStart = millis();
  retryCount = 0;
  for (;;) {
    auto stat = sntp_get_sync_status();
    if (stat == SNTP_SYNC_STATUS_COMPLETED) {
      // 同期完了
      struct tm nowTime;
      getLocalTime(&nowTime);
      log_i("Completed syncing clock! (%d/%d/%d %d:%d:%d)",
            nowTime.tm_year + 1900, nowTime.tm_mon + 1, nowTime.tm_mday,
            nowTime.tm_hour, nowTime.tm_min, nowTime.tm_sec);
      break;
    }

    if (millis() - tryStart >= 5000) {
      // 5秒待機しても完了しない場合は再試行
      log_e("Failed syncing clock...");

      // 再試行
      if (++retryCount == 3) {
        // ３回ダメならエラーを通知
        notifyError();
      }
      log_i("Retry syncing clock to ntp server.");
      configTzTime(TIMEZONE, NTP_HOST1, NTP_HOST2, NTP_HOST3);

      // 開始時間更新
      tryStart = millis();
    }

    delay(300);
  }

  // エラー通知クリア
  clearError();

  // スイッチの割り込み登録
  log_i("Initialize Switch");
  attachInterrupt(GPIO_SW1, &onSw1Push, CHANGE);

  // 電源管理による時計精度の低下を抑制
  log_i("esp_pm_lock_acquire");
  esp_pm_lock_handle_t hCpuLock;
  esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpu", &hCpuLock);
  esp_pm_lock_acquire(hCpuLock);

  // 起動の切っ掛けを取得
  auto wakeupCause = esp_sleep_get_wakeup_cause();
  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
    // タイマによる起動
    log_d("Wake up: Timer");
  } else if (wakeupCause == ESP_SLEEP_WAKEUP_EXT0) {
    // SWによる起動（強制発信モードで開始）
    contMode = true;
    log_i("Enabled continuous mode");
    log_d("Wake up: Switch");
  } else {
    // 通常起動
    log_d("Wake up: power on");
  }

  // パワーLED点滅を終了して点灯
  timerEnd(timer);
  digitalWrite(GPIO_LED_POW, HIGH);

  log_i("Setup succeeded!");
}

/*
 * メインループ
 */
void loop() {
  unsigned char timecode[60];

  struct timespec nowTime;
  if (clock_gettime(CLOCK_REALTIME, &nowTime) == -1) {
    // 時間取得失敗（恐らく発生しない）
    log_e("Failed clock_gettime()");
    notifyError();
    delay(500);
    return;
  }

  // エラー通知クリア
  clearError();

  struct tm nowDateTime;
  localtime_r(&nowTime.tv_sec, &nowDateTime);

  if (nowDateTime.tm_sec == 59) {
    // タイムコード生成後に分を跨がないよう59秒のときは2秒待機
    delay(2000);
    // 最新の時間を再取得
    if (clock_gettime(CLOCK_REALTIME, &nowTime) == -1) {
      // 時間取得失敗（恐らく発生しない）
      log_e("Failed clock_gettime()");
      notifyError();
      return;
    }
  }

  // 次の1秒の時刻を取得
  nowTime.tv_sec += 1;
  localtime_r(&nowTime.tv_sec, &nowDateTime);

  // タイムコードを生成
  createJjyTimeCode(&nowDateTime, timecode);

  // 現在の時間に戻しておく
  nowTime.tv_sec -= 1;

  // 5VラインをON
  digitalWrite(GPIO_5V, HIGH);

  struct tm scheTime;
  for (;;) {
    // ONタイマ開始（次の秒の少し前で割り込み）
    timerRestart(g_signalOnTimer);
    //timerAlarm(g_signalOnTimer, (999500000 - nowTime.tv_nsec) / 1000, false, 0);
    // ESP32 3.0.0で ets_delay_us が消えたため次の秒ピッタリで割り込みに変更
    timerAlarm(g_signalOnTimer, (1000000000 - nowTime.tv_nsec) / 1000, false, 0);

    // 次の秒を取得
    nowTime.tv_sec += 1;
    localtime_r(&nowTime.tv_sec, &nowDateTime);

    // スケジュール確認
    if (!contMode && !checkSchedule(&nowDateTime, &scheTime)) {
      // 次の秒がスケジュールの範囲外
      time_t nextTime = mktime(&scheTime);
      // 次のスケジュールまでの待機時間を計算
      unsigned long long sleepTime = (nextTime - nowTime.tv_sec) * 1000000ull;
      // 5VラインをOFF
      digitalWrite(GPIO_5V, LOW);
      if (sleepTime < 180000000) {
        // 次のスケジュールが3分以内だったらスリープせずにそのまま待機
        log_i("A little early...(delay: %llu us)", sleepTime);
        auto sleepTimeMs = sleepTime / 1000;
        auto waitStart = millis();
        while (!contMode && millis() - waitStart < sleepTimeMs) {
          delay(100);
        }
      } else {
        // NOTE: ディープスリープ後は指定した時間より少し早く起きてしまう特性を考慮して1%長めに設定
        sleepTime *= 1.01;
        log_i("Out of term. Wakeup: %u/%u/%u %u:%u (Sleep: %llu us)",
              scheTime.tm_year + 1900, scheTime.tm_mon + 1, scheTime.tm_mday,
              scheTime.tm_hour, scheTime.tm_min, sleepTime);
        esp_sleep_enable_timer_wakeup(sleepTime);
        esp_sleep_enable_ext0_wakeup(GPIO_SW1, HIGH);
        // ディープスリープへ移行
        esp_deep_sleep_start();
      }
      return;
    }

    // 次の発信時間を取得
    int onTime = JJY_GET_ONTIME(timecode[nowDateTime.tm_sec]);

    // ONタイマ完了まで待機
    xSemaphoreTake(g_hSemaphore, portMAX_DELAY);
    log_d("Signal ON");

    // OFFタイマ開始（発信時間経過で割り込み）
    timerRestart(g_signalOffTimer);
    timerAlarm(g_signalOffTimer, onTime, false, 0);

    if (nowDateTime.tm_sec == 59) {
      // 次が0秒なら新しいタイムコードを生成
      nowTime.tv_sec += 1;
      localtime_r(&nowTime.tv_sec, &nowDateTime);
      createJjyTimeCode(&nowDateTime, timecode);
      nowTime.tv_sec -= 1;
    }

    // OFFタイマ完了まで待機
    xSemaphoreTake(g_hSemaphore, portMAX_DELAY);
    log_d("Signal OFF");

    // 次の秒まで待機する準備
    if (clock_gettime(CLOCK_REALTIME, &nowTime) == -1) {
      // 時間取得失敗した場合は前回の時刻をそのまま使用（恐らく発生しない）
      log_w("Failed clock_gettime()");
    }
  }
}
