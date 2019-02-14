#include "rtctime.h"

#define I2C_DELAY (12) // 12ms is i2c delay when saving time to RTC chip

// Local logging tag
static const char TAG[] = "main";

TaskHandle_t ClockTask;
hw_timer_t *clockCycle = NULL;
bool volatile TimePulseTick = false;

// helper function to setup a pulse for time synchronisation
int timepulse_init(uint32_t pulse_period_ms) {

// use time pulse from GPS as time base with fixed 1Hz frequency
#if defined GPS_INT && defined GPS_CLK

  // setup external interupt for active low RTC INT pin
  pinMode(GPS_INT, INPUT_PULLDOWN);
  // setup external rtc 1Hz clock as pulse per second clock
  ESP_LOGI(TAG, "Time base: GPS timepulse");
  switch (GPS_CLK) {
  case 1000:
    break; // default GPS timepulse 1000ms
  default:
    goto pulse_period_error;
  }
  return 1; // success

// use pulse from on board RTC chip as time base with fixed frequency
#elif defined RTC_INT && defined RTC_CLK

  // setup external interupt for active low RTC INT pin
  pinMode(RTC_INT, INPUT_PULLUP);
  // setup external rtc 1Hz clock as pulse per second clock
  ESP_LOGI(TAG, "Time base: external RTC timepulse");
  if (I2C_MUTEX_LOCK()) {
    switch (RTC_CLK) {
    case 1000: // 1000ms
      Rtc.SetSquareWavePinClockFrequency(DS3231SquareWaveClock_1Hz);
      break;
    case 1: // 1ms
      Rtc.SetSquareWavePinClockFrequency(DS3231SquareWaveClock_1kHz);
      break;
    default:
      I2C_MUTEX_UNLOCK();
      goto pulse_period_error;
    }
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeClock);
    I2C_MUTEX_UNLOCK();
  } else {
    ESP_LOGE(TAG, "I2c bus busy - RTC initialization error");
    return 0; // failure
  }
  return 1; // success

#else
  // use ESP32 hardware timer as time base with adjustable frequency
  if (pulse_period_ms) {
    ESP_LOGI(TAG, "Time base: ESP32 hardware timer");
    clockCycle =
        timerBegin(1, 8000, true); // set 80 MHz prescaler to 1/10000 sec
    timerAttachInterrupt(clockCycle, &CLOCKIRQ, true);
    timerAlarmWrite(clockCycle, 10 * pulse_period_ms, true); // ms
  } else
    goto pulse_period_error;
  return 1; // success

#endif

pulse_period_error:
  ESP_LOGE(TAG, "Unknown timepulse period value");
  return 0; // failure
}

void timepulse_start(void) {
#ifdef GPS_INT // start external clock gps pps line
  attachInterrupt(digitalPinToInterrupt(GPS_INT), CLOCKIRQ, RISING);
#elif defined RTC_INT // start external clock rtc
  attachInterrupt(digitalPinToInterrupt(RTC_INT), CLOCKIRQ, FALLING);
#else                 // start internal clock esp32 hardware timer
  timerAlarmEnable(clockCycle);
#endif
}

// helper function to sync time_t of top of next second
void sync_clock(void) {
// do we have a second time pulse? Then wait for next pulse
#if defined(RTC_INT) || defined(GPS_INT)
  // sync on top of next second by timepulse
  if (xSemaphoreTake(TimePulse, pdMS_TO_TICKS(1000)) == pdTRUE) {
    ESP_LOGI(TAG, "clock synced by timepulse");
    return;
  } else
    ESP_LOGW(TAG, "Missing timepulse, thus clock can't be synced by second");
#endif
  // no external timepulse, thus we must use less precise internal system clock
  while (millis() % 1000)
    ; // wait for milli seconds to be zero before setting new time
  ESP_LOGI(TAG, "clock synced by systime");
  return;
}

// interrupt service routine triggered by either rtc pps or esp32 hardware
// timer
void IRAM_ATTR CLOCKIRQ() {
  xTaskNotifyFromISR(ClockTask, xTaskGetTickCountFromISR(), eSetBits, NULL);
#if defined(GPS_INT) || defined(RTC_INT)
  xSemaphoreGiveFromISR(TimePulse, pdFALSE);
  TimePulseTick = !TimePulseTick; // flip ticker
#endif
  portYIELD_FROM_ISR();
}

#ifdef HAS_RTC // we have hardware RTC

RtcDS3231<TwoWire> Rtc(Wire); // RTC hardware i2c interface

// initialize RTC
int rtc_init(void) {

  // return = 0 -> error / return = 1 -> success

  // block i2c bus access
  if (I2C_MUTEX_LOCK()) {

    Wire.begin(HAS_RTC);
    Rtc.Begin();

    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);

    if (!Rtc.IsDateTimeValid()) {
      ESP_LOGW(TAG,
               "RTC has no valid RTC date/time, setting to compilation date");
      Rtc.SetDateTime(compiled);
    }

    if (!Rtc.GetIsRunning()) {
      ESP_LOGI(TAG, "RTC not running, starting now");
      Rtc.SetIsRunning(true);
    }

    RtcDateTime now = Rtc.GetDateTime();

    if (now < compiled) {
      ESP_LOGI(TAG, "RTC date/time is older than compilation date, updating");
      Rtc.SetDateTime(compiled);
    }

    // configure RTC chip
    Rtc.Enable32kHzPin(false);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);

  } else {
    ESP_LOGE(TAG, "I2c bus busy - RTC initialization error");
    goto error;
  }

  I2C_MUTEX_UNLOCK(); // release i2c bus access
  ESP_LOGI(TAG, "RTC initialized");
  return 1;

error:
  I2C_MUTEX_UNLOCK(); // release i2c bus access
  return 0;

} // rtc_init()

int set_rtctime(time_t t) { // t is seconds epoch time starting 1.1.1970
  if (I2C_MUTEX_LOCK()) {
    sync_clock(); // wait for top of second
    Rtc.SetDateTime(RtcDateTime(t));
    I2C_MUTEX_UNLOCK(); // release i2c bus access
    ESP_LOGI(TAG, "RTC calibrated");
    return 1; // success
  }
  return 0; // failure
} // set_rtctime()

int set_rtctime(uint32_t t) { // t is epoch seconds starting 1.1.1970
  return set_rtctime(static_cast<time_t>(t));
  // set_rtctime()
}

time_t get_rtctime(void) {
  // never call now() in this function, this would cause a recursion!
  time_t t = 0;
  // block i2c bus access
  if (I2C_MUTEX_LOCK()) {
    if (Rtc.IsDateTimeValid()) {
      RtcDateTime tt = Rtc.GetDateTime();
      t = tt.Epoch32Time();
    } else {
      ESP_LOGW(TAG, "RTC has no confident time");
    }
    I2C_MUTEX_UNLOCK(); // release i2c bus access
  }
  return t;
} // get_rtctime()

float get_rtctemp(void) {
  // block i2c bus access
  if (I2C_MUTEX_LOCK()) {
    RtcTemperature temp = Rtc.GetTemperature();
    I2C_MUTEX_UNLOCK(); // release i2c bus access
    return temp.AsFloatDegC();
  } // while
  return 0;
} // get_rtctemp()

#endif // HAS_RTC