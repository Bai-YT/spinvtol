// monocopter controller, for teensy 3.5
// Nick Zhang 2019


// Pin connection table:
// most of these can be changed, though there are some restrictions
// LED1,LED2,LED3, LED4(blue): 9,10,11,12
// RSSI level from xbee: 3
// RC channel PWM input: 4,5,6,7,8, connect to Ch1,3,4,6 on the RC receiver
// flap servo output: 29
// throttle  output(reserved, not used): 30

// enable using dual ADXL345 for state estimation, comment out if using boards without them
//#define DUALACC

// 3 red led array
#define PIN_LED 13
#define PIN_LED1 9
#define PIN_LED2 10
#define PIN_LED3 11
// blue
#define PIN_LED4 12

#define PIN_SERVO 29


// action sequence for T1_COMPA
#define NONE 0
#define LED_OFF 1
#define LED_ON 2

// action sequence for T1_COMPB
#define RISING_NEUTRAL 1
#define SERVO_MAX 2
#define FALLING_NEUTRAL 3
#define SERVO_MIN 4

#define MAX_SERVO_PULSEWIDTH 930
#define MIN_SERVO_PULSEWIDTH 560
#define CENTRAL_SERVO_PULSEWIDTH 745

// roll-ch1->ch0, pitch ch2->ch1, rudder ch4->ch2
#define PITCH_FULL_PULL 1930.0
#define PITCH_FULL_PUSH 1108.0
#define ROLL_FULL_LEFT 1108.0
#define ROLL_FULL_RIGHT 1930.0
#define RUDDER_FULL_LEFT 1110.0
#define RUDDER_FULL_RIGHT 1930.0
#define VR_MIN 932.0
#define VR_MAX 2032.0

//  millis() use Timer 0
#include <Wire.h>
#include <SerialCommand.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Filters.h>
#include <MatrixMath.h>
// uses FTM0 timer by default, interferes with PWM function on some pins, see https://www.pjrc.com/teensy/td_pulse.html
#include <TeensyDelay.h>

#define I2Cclock 400000
#define I2Cport Wire
#include <MPU9250.h>
#define MPU9250_ADDRESS MPU9250_ADDRESS_AD0 // AD0 on MPU9250 pulled down to GND

typedef mtx_type matrix;
#define multiply(A,B,m,n,p,C) Matrix.Multiply((mtx_type*)A, (mtx_type*)B, m, n, p, (mtx_type*)C)
#define add(A,B,m,n,C) Matrix.Add((mtx_type*)A, (mtx_type*)B, m, n,  (mtx_type*)C)
#define subtract(A,B,m,n,C) Matrix.Subtract((mtx_type*)A, (mtx_type*)B, m, n,  (mtx_type*)C)
#define transpose(A,m,n,C) Matrix.Transpose((mtx_type*)A, m, n, (mtx_type*)C)
#define invert(A,m) Matrix.Invert((mtx_type*)A,m)
#define mtxprint(A,m,n,N) Matrix.Print((mtx_type*)A, m, n, N)
#define mtxcopy(A,m,n,B) Matrix.Copy((mtx_type*)A, m, n, (mtx_type*)B)
SerialCommand sCmd;
float dot;
FilterOnePole lowfilter(LOWPASS,10);

// MPU9250 init sets Wire I2c freq(adxl345 does not), let it run first
MPU9250 myIMU(MPU9250_ADDRESS, I2Cport, I2Cclock);

#ifdef DUALACC
Adafruit_ADXL345_Unified accel_in = Adafruit_ADXL345_Unified(1);
Adafruit_ADXL345_Unified accel_out = Adafruit_ADXL345_Unified(2);
#endif

const float pi = 3.1415926535898;

// the main loop runs at 50HZ, much slower than needed to actuate accurate command
// Precise timing and actuation of POV LEDs and flap servo is achieved by timer interrupts

// the several functions below enable the execution of a sequence of action in precise, asynchrous manner
// the ISR will be called multiple times with timer interrupt, sleeping in between calls to allow main program
// to continue. the ISR itself should determine
// the proper action to take every time it is called, usually by maintaining a sequence number
// it is also responsible for setting up the interrupt registers to it will be called again at proper time
// to execute the next action sequence. 
// upon execution of the last command in sequence, the ISR should stop the timer/disable the ISR from undesirable
// calls in the future. 

float ffmod(float x, float y){
  return x-y*floor(x/y);
}



float fmap_temp;
float fmap(float x, float in_min, float in_max, float out_min, float out_max) {
  fmap_temp = (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  if (out_max>out_min){
    fmap_temp = (fmap_temp>out_max)?out_max:fmap_temp;
    fmap_temp = (fmap_temp<out_min)?out_min:fmap_temp;
  } else{
    fmap_temp = (fmap_temp<out_max)?out_max:fmap_temp;
    fmap_temp = (fmap_temp>out_min)?out_min:fmap_temp;

  }
  return fmap_temp;
}


uint8_t rc_in_pinno[] = {4, 5, 6, 7, 8}; // CH0,1,2,3,4
volatile int rc_in_val[5] = {0};
volatile unsigned long rc_rising_ts[5] = {0};
bool flag_signal_loss = false;

//note: measured dutycycle for futaba FHSS 13564us, 73Hz
void rc0_in_callback() {
  unsigned long timestamp = micros();
  if (digitalRead(rc_in_pinno[0]) == HIGH) {
    rc_rising_ts[0] = timestamp;
  } else {
    rc_in_val[0] = timestamp - rc_rising_ts[0];
  }
}
void rc1_in_callback() {
  unsigned long timestamp = micros();
  if (digitalRead(rc_in_pinno[1]) == HIGH) {
    rc_rising_ts[1] = timestamp;
  } else {
    rc_in_val[1] = timestamp - rc_rising_ts[1];
  }
}
void rc2_in_callback() {
  unsigned long timestamp = micros();
  if (digitalRead(rc_in_pinno[2]) == HIGH) {
    rc_rising_ts[2] = timestamp;
  } else {
    rc_in_val[2] = timestamp - rc_rising_ts[2];
  }
}
void rc3_in_callback() {
  unsigned long timestamp = micros();
  if (digitalRead(rc_in_pinno[3]) == HIGH) {
    rc_rising_ts[3] = timestamp;
  } else {
    rc_in_val[3] = timestamp - rc_rising_ts[3];
  }
}
void rc4_in_callback() {
  unsigned long timestamp = micros();
  if (digitalRead(rc_in_pinno[4]) == HIGH) {
    rc_rising_ts[4] = timestamp;
  } else {
    rc_in_val[4] = timestamp - rc_rising_ts[4];
  }
}

// timer routines
IntervalTimer blinkerTimer;
volatile byte pending_action_blinker = NONE;
volatile bool flash_in_progress = false;
volatile float on_time_ms = 20;

void blinker() {
    switch (pending_action_blinker){
        case LED_ON:
            digitalWrite(PIN_LED,HIGH);
            digitalWrite(PIN_LED1,LOW);
            digitalWrite(PIN_LED2,LOW);
            digitalWrite(PIN_LED3,LOW);
            pending_action_blinker = LED_OFF;
            blinkerTimer.begin(blinker,on_time_ms*1000);
            break;

        case LED_OFF:
            digitalWrite(PIN_LED,LOW);
            digitalWrite(PIN_LED1,HIGH);
            digitalWrite(PIN_LED2,HIGH);
            digitalWrite(PIN_LED3,HIGH);
            pending_action_blinker  = NONE;
            flash_in_progress = false;
            blinkerTimer.end();
            break;

        case NONE:
        default:
            break;
    }
} 

volatile uint32_t oldres;
volatile int duty;
void setPulseWidth(float us)
{
  if (us<MIN_SERVO_PULSEWIDTH) { us = MIN_SERVO_PULSEWIDTH; }
  if (us>MAX_SERVO_PULSEWIDTH) { us = MAX_SERVO_PULSEWIDTH; }
	duty = us/(1e6/300.0)*65535;
#if TEENSYDUINO >= 137
	noInterrupts();
	oldres = analogWriteResolution(16);
	analogWrite(PIN_SERVO, duty);
  // restore resolution
	analogWriteResolution(oldres);
	interrupts();
#endif
}

// ctrl_phase: phase to START command to max deflection
volatile float ctrl_phase;
// 0:azimuth, rad; 1:omega, rad/s
volatile float state_buffer[2];
// micros()
volatile unsigned long state_buffer_ts;
// us to next control phase, time between each new ISR
volatile float quarter_period;
volatile float current_phase;
volatile int pending_action_cyclic = NONE;
volatile float ctrl_magnitude;
// TeensyDelay uses 16bit FTM timer that only goes to about 69ms, if we need a delay longer
// than that we need to handle this gracefully
volatile uint32_t remaining_delay_us;
void cyclic(){
  if (remaining_delay_us!=0){
    if (remaining_delay_us > 60000){
      TeensyDelay::trigger(60000,0);
      remaining_delay_us -= 60000;
    } else {
      TeensyDelay::trigger(remaining_delay_us);
      remaining_delay_us = 0;
    }
    return;
  }

  switch(pending_action_cyclic){

      case RISING_NEUTRAL:
          //Serial.println("RISING_NEUTRAL");
          //setPulseWidth(CENTRAL_SERVO_PULSEWIDTH);
          setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
          pending_action_cyclic = SERVO_MAX;
          // adjust control phase to sync with estimated state
          // 5e5 = 2(for 2pi) * 1e6 (sec -> us) / 4 (quarter period)
          quarter_period = 5e5*pi/state_buffer[1];
          current_phase = state_buffer[0] + float(micros()-state_buffer_ts)*1e-6*state_buffer[1];
          remaining_delay_us = (ffmod(ctrl_phase-current_phase,2*pi)/state_buffer[1])*1e6;
          //Serial.print("current phase= ");
          //Serial.println(ffmod(current_phase,2*pi)/pi*180);
          //Serial.print("dist 2 ctrl phase= ");
          //Serial.println((ffmod(ctrl_phase-current_phase,2*pi))/pi*180);
          //Serial.print("wakeup after(s) ");
          //Serial.println((ffmod(ctrl_phase-current_phase,2*pi)/state_buffer[1]));
          //Serial.println(ffmod((ctrl_phase-current_phase)/2/pi,360));
          break;
          
      case SERVO_MAX:
          //Serial.println("SERVO_MAX");
          setPulseWidth(fmap(ctrl_magnitude,0.0,1.0,CENTRAL_SERVO_PULSEWIDTH, MAX_SERVO_PULSEWIDTH));
          pending_action_cyclic = FALLING_NEUTRAL;
          remaining_delay_us = quarter_period;
          break;

      case FALLING_NEUTRAL:
          //Serial.println("FALLING_NEUTRAL");
          //setPulseWidth(CENTRAL_SERVO_PULSEWIDTH);
          setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
          pending_action_cyclic = SERVO_MIN;
          remaining_delay_us = quarter_period;
          break;

      case SERVO_MIN:
          //Serial.println("SERVO_MIN");
          setPulseWidth(fmap(ctrl_magnitude,0.0,1.0,CENTRAL_SERVO_PULSEWIDTH, MIN_SERVO_PULSEWIDTH));
          pending_action_cyclic  = RISING_NEUTRAL;
          remaining_delay_us = quarter_period;
          break;

      case NONE:
      default:
          //Serial.println("fallthrough");
          setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
          return;
          break;
  }

  if (remaining_delay_us > 60000){
    TeensyDelay::trigger(60000,0);
    remaining_delay_us -= 60000;
  } else {
    TeensyDelay::trigger(remaining_delay_us);
    remaining_delay_us = 0;
  }
  return;
} 



#ifdef DUALACC
// ADXL 345

void displaySensorDetails(Adafruit_ADXL345_Unified &accel)
{
  sensor_t sensor;
  accel.getSensor(&sensor);
  Serial.println(F("------------------------------------"));
  Serial.print  (F("Sensor:       ")); Serial.println(sensor.name);
  Serial.print  (F("Driver Ver:   ")); Serial.println(sensor.version);
  Serial.print  (F("Unique ID:    ")); Serial.println(sensor.sensor_id);
  Serial.print  (F("Max Value:    ")); Serial.print(sensor.max_value); Serial.println(F(" m/s^2"));
  Serial.print  (F("Min Value:    ")); Serial.print(sensor.min_value); Serial.println(F(" m/s^2"));
  Serial.print  (F("Resolution:   ")); Serial.print(sensor.resolution); Serial.println(F(" m/s^2"));
  Serial.println(F("------------------------------------"));
  Serial.println("");
  delay(500);
}

void displayDataRate(Adafruit_ADXL345_Unified &accel)
{
  Serial.print  (F("Data Rate:    "));

  switch (accel.getDataRate())
  {
    case ADXL345_DATARATE_3200_HZ:
      Serial.print  (F("3200 "));
      break;
    case ADXL345_DATARATE_1600_HZ:
      Serial.print  (F("1600 "));
      break;
    case ADXL345_DATARATE_800_HZ:
      Serial.print  (F("800 "));
      break;
    case ADXL345_DATARATE_400_HZ:
      Serial.print  (F("400 "));
      break;
    case ADXL345_DATARATE_200_HZ:
      Serial.print  (F("200 "));
      break;
    case ADXL345_DATARATE_100_HZ:
      Serial.print  (F("100 "));
      break;
    case ADXL345_DATARATE_50_HZ:
      Serial.print  (F("50 "));
      break;
    case ADXL345_DATARATE_25_HZ:
      Serial.print  (F("25 "));
      break;
    case ADXL345_DATARATE_12_5_HZ:
      Serial.print  (F("12.5 "));
      break;
    case ADXL345_DATARATE_6_25HZ:
      Serial.print  (F("6.25 "));
      break;
    case ADXL345_DATARATE_3_13_HZ:
      Serial.print  (F("3.13 "));
      break;
    case ADXL345_DATARATE_1_56_HZ:
      Serial.print  (F("1.56 "));
      break;
    case ADXL345_DATARATE_0_78_HZ:
      Serial.print  (F("0.78 "));
      break;
    case ADXL345_DATARATE_0_39_HZ:
      Serial.print  (F("0.39 "));
      break;
    case ADXL345_DATARATE_0_20_HZ:
      Serial.print  (F("0.20 "));
      break;
    case ADXL345_DATARATE_0_10_HZ:
      Serial.print  (F("0.10 "));
      break;
    default:
      Serial.print  (F("???? "));
      break;
  }
  Serial.println(F(" Hz"));
}

void displayRange(Adafruit_ADXL345_Unified &accel)
{
  Serial.print  (F("Range:         +/- "));

  switch (accel.getRange())
  {
    case ADXL345_RANGE_16_G:
      Serial.print  (F("16 "));
      break;
    case ADXL345_RANGE_8_G:
      Serial.print  (F("8 "));
      break;
    case ADXL345_RANGE_4_G:
      Serial.print  (F("4 "));
      break;
    case ADXL345_RANGE_2_G:
      Serial.print  (F("2 "));
      break;
    default:
      Serial.print  (F("?? "));
      break;
  }
  Serial.println(F(" g"));
}
#endif


const int xbee_rssi_pin = 3;
volatile unsigned long xbee_rssi_last_rising;
volatile unsigned long xbee_rssi_val;
volatile bool xbee_rssi_newdata = false;
void xbee_rssi_callback() {
  unsigned long timestamp = micros();

  if (digitalRead(xbee_rssi_pin) == HIGH) {
    xbee_rssi_last_rising = timestamp;
  } else {
   xbee_rssi_val = timestamp - xbee_rssi_last_rising;
   xbee_rssi_newdata = true;

  }
}

// --------- Serial command handlers -------
int onoff(char* arg) {
  if (arg != NULL) {
    if (!strcmp(arg, "on")) {
      return 1;
    } else if (!strcmp(arg, "off")) {
      return 0;
    }
  }
  return -1;
}
void led() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    digitalWrite(13, rval);
  }
}

void led1() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    digitalWrite(PIN_LED1, !rval);
  }
}
void led2() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    digitalWrite(PIN_LED2, !rval);
  }
}
void led3() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    digitalWrite(PIN_LED3, !rval);
  }
}

bool mag_verbose = true;
void display_mag() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    mag_verbose = (bool)rval;
  }
}

#ifdef DUALACC
bool acc_verbose = true;
void display_acc() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    acc_verbose = (bool)rval;
  }
}
#endif

bool rc_verbose = true;
void display_rc() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    rc_verbose = (bool)rval;
  }
}
bool sig_verbose = true;
void display_sig() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    sig_verbose = (bool)rval;
  }
}

bool rssi_verbose = true;
void display_rssi() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
    rssi_verbose = (bool)rval;
  }
}

void display_all() {
  int rval = onoff(sCmd.next());
  if (rval != -1) {
#ifdef DUALACC
    rssi_verbose = acc_verbose = mag_verbose = rc_verbose = sig_verbose =  (bool)rval;
#else
    rssi_verbose = mag_verbose = rc_verbose = sig_verbose =  (bool)rval;
#endif
  }
}


bool human_readable_output = false;
void human(){
  human_readable_output = true;
}

void machine(){
  human_readable_output = false;
}

int synchro_pinno = A1;
unsigned long epoch;
// trigger a falling edge on pin, have the other system ready for sync signal
// to receive such signal, connect pin9 of avionics to pin4 on testStand, also connect both GND
void synchronize(){
  digitalWrite(synchro_pinno,LOW);
  epoch = millis();
  Serial.print(F("Syncing ... epoch = "));
  Serial.println(epoch);
  //delay(1);
  digitalWrite(synchro_pinno,HIGH);
}

// ---------------- EKF ----------------
// x = [theta(azimuth), omega] unit: rad, rad/s

// actually matrix here logically means type of the elements in a matrix, an array of elements compose a matrix
matrix x[2][1],F[2][2],FT[2][2],P[2][2],Q[2][2],H[1][2],HT[2][1];
float S;

// unit: (rad/s2)^2, this is a rough estimate, need to see video footage, of actual monocopter
const float acc_variance=10.0;
// experiment shows 60 deg variation for 4sigma(95% ish) -> convert to 1 sigma in rad:pow(0.2618,2)
const float mag_variance= pow(0.2618,2);

void kf_predict(){
  static unsigned long last_update = millis();
  unsigned long ts = millis();
  matrix buff_col[2][1],buff_square[2][2];
  float delta_t = (ts - last_update) / 1000.0;
  last_update = ts;
  // because F.transpose is calculated in place, we need to re-init F each time
  F[0][0] = 1;
  F[1][0] = 0;
  F[1][1] = 1;
  F[0][1] = delta_t;
  // x = Fx + Bu (no control so no Bu here)
  
  //Serial.print(F("entering prediction = "));
  //Serial.print("kf_dt : ");
  //Serial.print(delta_t);

  multiply(F,x,2,2,1,buff_col);
  mtxcopy(buff_col,2,1,x);
  
  //mtxprint(F,2,2,"F");
  //mtxprint(x,2,1,"x");
  //Serial.print("prediction -> ");
  //mtxprint(x,2,1,"x");

  // P = FPF.T + Q
  multiply(F,P,2,2,2,buff_square);
  transpose(F,2,2,FT);
  multiply(buff_square,FT,2,2,2,P);
  Q[0][0] = 0.25*delta_t*delta_t*delta_t*delta_t*acc_variance;
  Q[0][1] = 0.5*delta_t*delta_t*delta_t*acc_variance;
  Q[1][0] = Q[0][1];
  Q[1][1] = delta_t*delta_t*acc_variance;
  add(P,Q,2,2,P);
  //mtxprint(P,2,2,"P");
  return;
}


#ifdef DUALACC
//z:acc observation, z = [delta_acc], unit: m/s2
void kf_update_acc(float z){

  matrix buff_single[1][1],buff_row[1][2],buff_col[2][1],buff_square[2][2],buff_square2[2][2];
  matrix I[2][2];
  I[0][0] = 1;
  I[1][1] = 1;  
  I[0][1] = 0;
  I[1][0] = 0;
  // y = z - H(x), where H is a function for EKF
  
  matrix y[1][1];
  // constant from experiment, corresbond to 11.5 in rev/s representation
  y[0][0] = z - 0.2913*x[1][0]*x[1][0];
  H[0][0] = 0;
  // Jacobian
  H[0][1] = 2*0.2913*x[1][0];

  // S = HPH.T + R
  
  multiply(H,P,1,2,2,buff_row);
  multiply(buff_row,HT,1,2,1,buff_single);
  
  // covariance of innovation, R = var of observation, calculated from gain variation
  S = buff_single[0][0] + pow(1*x[1][0]*x[1][0],2);
  

  // K = P HT S-1 ----------------------
  matrix K[2][1];
  multiply(P,HT,2,2,1,buff_col); 
  //mtxprint(P,2,2,"P");
  //mtxprint(HT,2,1,"HT");
  
  matrix mat_Sinv[1][1];
  mat_Sinv[0][0] = 1.0/S;
  multiply(buff_col,mat_Sinv,2,1,1,K);

  //mtxprint(K,2,1,"K");
  //mtxprint(x,2,1,"x");

  // x = x + Ky
  multiply(K,y,2,1,1,buff_col);
  //mtxprint(buff_col,2,1,"Ky");
  add(x,buff_col,2,1,x);
  //Serial.print("update -> ");
  //mtxprint(x,2,1,"x");

  
  // P = (I-KH)P
  multiply(K,H,2,1,2,buff_square);
  subtract(I,buff_square,2,2,buff_square);
  multiply(buff_square,P,2,2,2,buff_square2);
  mtxcopy(buff_square,2,2,P);
  // y = z - Hx post fit residual, not needed
  return;
}
#endif

// this is called only once per rev, when algorithm picks up a signature
// in mag output that signifies a particular azimuth angle is reached
// though this angle is offset by a function linear to omega (quicker rev-> larger offset)
//z:mag observation, always zero since we update at reference point only
// TODO make variance a function of omega, low omega mean less error
void kf_update_mag(){
  //Serial.print("update ->");
  //mtxprint(x,2,1,"x");
  
  static float z = 0.0;
  z += 2*3.1415926;
  matrix buff_single[1][1],buff_row[1][2],buff_col[2][1],buff_square[2][2],buff_square2[2][2];
  matrix I[2][2];
  I[0][0] = 1;
  I[1][1] = 1;  
  I[0][1] = 0;
  I[1][0] = 0;
  // y = z - H(x), where H is a function for EKF
  

  // const from experiment
  // z = 0 = theta - c*omega + noise (This is when we trigger this update)
  H[0][0] = 1;
  // from statistical experiment summary
  //H[0][1] = -0.0660225;
  //adjust based on performance evaluation
  H[0][1] = -0.10768916;
  //H[0][1] = 0;
  transpose(H,1,2,HT);
  multiply(H,x,1,2,1,buff_single);

  matrix y[1][1];
  y[0][0] = z - buff_single[0][0];
  //Serial.print("y ");
  //Serial.println(y[0][0]);
  // S = HPH.T + R
  
  multiply(H,P,1,2,2,buff_row);
  multiply(buff_row,HT,1,2,1,buff_single);
  
  // covariance of innovation
  S = buff_single[0][0] + mag_variance;
  //Serial.print("S :");
  //Serial.println(S);
  //mtxprint(P,2,2,"P");
  //mtxprint(H,1,2,"H");
  // K = P HT S-1 ----------------------
  matrix K[2][1];
  multiply(P,HT,2,2,1,buff_col); 
  matrix mat_Sinv[1][1];
  mat_Sinv[0][0] = 1.0/S;
  multiply(buff_col,mat_Sinv,2,1,1,K);
  // x = x + Ky
  multiply(K,y,2,1,1,buff_col);
  //mtxprint(buff_col,2,1,"Ky");
  add(x,buff_col,2,1,x);
//  Serial.print("update -> ");
//  Serial.print(" K ");
//  Serial.print(K[0][0]);
//  Serial.print(" ");
//  Serial.print(K[1][0]);
//  Serial.print(" x: ");
//  Serial.print(x[0][0]);
//  Serial.print(" ");
//  Serial.println(x[1][0]);
  
  // P = (I-KH)P
  multiply(K,H,2,1,2,buff_square);
  subtract(I,buff_square,2,2,buff_square2);
  multiply(buff_square2,P,2,2,2,buff_square);
  mtxcopy(buff_square,2,2,P);
  // y = z - Hx post fit residual, not needed
  return;
}


#ifdef DUALACC
inline float acc_norm(sensors_event_t* event){
  return sqrt(event->acceleration.x*event->acceleration.x+event->acceleration.y*event->acceleration.y+event->acceleration.z*event->acceleration.z);
}
#endif



void setup() {
  Serial.begin(115200);
  while (!Serial);

  for (uint8_t i = 0; i < sizeof(rc_in_pinno) / sizeof(uint8_t);  i++) {
    pinMode(rc_in_pinno[i], INPUT);
  }

  attachInterrupt( digitalPinToInterrupt(xbee_rssi_pin), xbee_rssi_callback,CHANGE);
  attachInterrupt( digitalPinToInterrupt(rc_in_pinno[0]), rc0_in_callback, CHANGE);
  attachInterrupt( digitalPinToInterrupt(rc_in_pinno[1]), rc1_in_callback, CHANGE);
  attachInterrupt( digitalPinToInterrupt(rc_in_pinno[2]), rc2_in_callback, CHANGE);
  attachInterrupt( digitalPinToInterrupt(rc_in_pinno[3]), rc3_in_callback, CHANGE);
  attachInterrupt( digitalPinToInterrupt(rc_in_pinno[4]), rc4_in_callback, CHANGE);

  // ---------------- MPU9250 init ---------------
  Wire.begin();
  // Read the WHO_AM_I register, this is a good test of communication
  byte c = myIMU.readByte(MPU9250_ADDRESS, WHO_AM_I_MPU9250);
  Serial.print(F("MPU9250 I AM 0x"));
  Serial.print(c, HEX);
  Serial.print(F(" I should be 0x"));
  Serial.println(0x71, HEX);

  if (c == 0x71) // WHO_AM_I should always be 0x71
  {
    Serial.println(F("MPU9250 is online..."));

    // Start by performing self test and reporting values
    myIMU.MPU9250SelfTest(myIMU.selfTest);
    Serial.print(F("x-axis self test: acceleration trim within : "));
    Serial.print(myIMU.selfTest[0], 1); Serial.println(F("% of factory value"));
    Serial.print(F("y-axis self test: acceleration trim within : "));
    Serial.print(myIMU.selfTest[1], 1); Serial.println(F("% of factory value"));
    Serial.print(F("z-axis self test: acceleration trim within : "));
    Serial.print(myIMU.selfTest[2], 1); Serial.println(F("% of factory value"));
    Serial.print(F("x-axis self test: gyration trim within : "));
    Serial.print(myIMU.selfTest[3], 1); Serial.println(F("% of factory value"));
    Serial.print(F("y-axis self test: gyration trim within : "));
    Serial.print(myIMU.selfTest[4], 1); Serial.println(F("% of factory value"));
    Serial.print(F("z-axis self test: gyration trim within : "));
    Serial.print(myIMU.selfTest[5], 1); Serial.println(F("% of factory value"));

    // Calibrate gyro and accelerometers, load biases in bias registers
    myIMU.calibrateMPU9250(myIMU.gyroBias, myIMU.accelBias);

    myIMU.initMPU9250();
    // Initialize device for active mode read of acclerometer, gyroscope, and
    // temperature
    Serial.println(F("MPU9250 initialized for active data mode...."));

    // Read the WHO_AM_I register of the magnetometer, this is a good test of
    // communication
    byte d = myIMU.readByte(AK8963_ADDRESS, WHO_AM_I_AK8963);
    Serial.print(F("AK8963 "));
    Serial.print(F("I AM 0x"));
    Serial.print(d, HEX);
    Serial.print(F(" I should be 0x"));
    Serial.println(0x48, HEX);

    if (d != 0x48)
    {
      // Communication failed, stop here
      Serial.println(F("Communication failed, abort!"));
      Serial.flush();
      abort();
    }

    // Get magnetometer calibration from AK8963 ROM
    myIMU.initAK8963(myIMU.factoryMagCalibration);
    // Initialize device for active mode read of magnetometer
    Serial.println(F("AK8963 initialized for active data mode...."));

    //  Serial.println("Calibration values: ");
    Serial.print(F("X-Axis factory sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[0], 2);
    Serial.print(F("Y-Axis factory sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[1], 2);
    Serial.print(F("Z-Axis factory sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[2], 2);


    // Get sensor resolutions, only need to do this once
    myIMU.getAres();
    myIMU.getGres();
    myIMU.getMres();

    // The next call delays for 4 seconds, and then records about 15 seconds of
    // data to calculate bias and scale.
    //    myIMU.magCalMPU9250(myIMU.magBias, myIMU.magScale);
    Serial.println(F("AK8963 mag biases (mG)"));
    Serial.println(myIMU.magBias[0]);
    Serial.println(myIMU.magBias[1]);
    Serial.println(myIMU.magBias[2]);

    Serial.println(F("AK8963 mag scale (mG)"));
    Serial.println(myIMU.magScale[0]);
    Serial.println(myIMU.magScale[1]);
    Serial.println(myIMU.magScale[2]);

    Serial.println(F("Magnetometer:"));
    Serial.print(F("X-Axis sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[0], 2);
    Serial.print(F("Y-Axis sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[1], 2);
    Serial.print(F("Z-Axis sensitivity adjustment value "));
    Serial.println(myIMU.factoryMagCalibration[2], 2);
  } else
  {
    Serial.print(F("Could not connect to MPU9250: 0x"));
    Serial.println(c, HEX);

    // Communication failed, stop here
    Serial.println(F("Communication failed, abort!"));
    Serial.flush();
    abort();
  }


#ifdef DUALACC
  // ---------------- ADXL345 init ----------------
  Serial.println(F("Accelerometer Init -- inner (0x53)")); Serial.println("");
  /* Initialise the sensor */
  if (!accel_in.begin())
  {
    /* There was a problem detecting the ADXL345 ... check your connections */
    Serial.println(F("Inner ADXL345 not detected"));
    while (1);
  }

  /* Set the range to whatever is appropriate for your project */
  accel_in.setRange(ADXL345_RANGE_16_G);
  // accel_in.setRange(ADXL345_RANGE_8_G);
  // accel_in.setRange(ADXL345_RANGE_4_G);
  // accel_in.setRange(ADXL345_RANGE_2_G);

  /* Display some basic information on this sensor */
  //displaySensorDetails(accel_in);

  /* Display additional settings (outside the scope of sensor_t) */
  //displayDataRate(accel_in);
  //displayRange(accel_in);
  Serial.println("");
  // ------ outer ADXL345
  Serial.println(F("Accelerometer Init -- outer (0x1D)")); Serial.println("");
  /* Initialise the sensor */
  if (!accel_out.begin(0x1D))
  {
    /* There was a problem detecting the ADXL345 ... check your connections */
    Serial.println(F("Outer ADXL345 not detected"));
    while (1);
  }

  /* Set the range to whatever is appropriate for your project */
  accel_out.setRange(ADXL345_RANGE_16_G);
  // accel_out.setRange(ADXL345_RANGE_8_G);
  // accel_out.setRange(ADXL345_RANGE_4_G);
  // accel_out.setRange(ADXL345_RANGE_2_G);

  /* Display some basic information on this sensor */
  //displaySensorDetails(accel_out);

  /* Display additional settings (outside the scope of sensor_t) */
  //displayDataRate(accel_out);
  //displayRange(accel_out);
  Serial.println("");
  // ----------- end ADXL345 init -----------
#endif

  // --------  Register serial commands ------
  pinMode(13, OUTPUT);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_LED3, OUTPUT);
  pinMode(PIN_LED4, OUTPUT);
  pinMode(PIN_SERVO,OUTPUT);
  digitalWrite(PIN_LED1,HIGH);
  digitalWrite(PIN_LED2,HIGH);
  digitalWrite(PIN_LED3,HIGH);
  digitalWrite(PIN_LED4,HIGH);
  digitalWrite(PIN_SERVO,LOW);

  pinMode(synchro_pinno,OUTPUT);
  digitalWrite(synchro_pinno,HIGH);
 
  sCmd.addCommand("led", led); // test function, takes 1 argument on or off
  sCmd.addCommand("led1", led1); // test function, takes 1 argument on or off
  sCmd.addCommand("led2", led2); // test function, takes 1 argument on or off
  sCmd.addCommand("led3", led3); // test function, takes 1 argument on or off
  sCmd.addCommand("mag", display_mag);
#ifdef DUALACC
  sCmd.addCommand("acc", display_acc);
#endif
  sCmd.addCommand("rc", display_rc);
  sCmd.addCommand("sig", display_sig);
  sCmd.addCommand("rssi", display_rssi);
  sCmd.addCommand("all", display_all);
  sCmd.addCommand("sync",synchronize);
  sCmd.addCommand("human",human);
  sCmd.addCommand("machine",machine);

  analogWriteFrequency(PIN_SERVO, 300);
  pending_action_cyclic = RISING_NEUTRAL;
  TeensyDelay::begin();
  TeensyDelay::addDelayChannel(cyclic, 0);
  TeensyDelay::trigger(10,0);
  
}
// --------------------- LOOP ---------------------
float mag_hist[4];
uint8_t mag_index;
unsigned long led_off_ts;
unsigned long loop_ts;
bool flag_reset_point = false;
bool flag_led_on = false;
bool flag_calibrated = false;
bool flag_running = false;
bool flag_servo_protect = false;
float pitch_normalized;
float roll_normalized;
float rudder_normalized;
// where the monocopter is pointed, this is only a visual reference for pilot
// roll/pitch will be based on this angle
float ref_heading;
unsigned long last_mag_update_ts;
void loop() {
  sCmd.readSerial();
  //block -- serial update
  static int serial_update_freq = 10;
  static unsigned long serial_update_ts = millis();
  if ( (millis() - serial_update_ts) > (unsigned long) (1000 / float(serial_update_freq)) ) {
    serial_update_ts = millis();
    if (rc_verbose && human_readable_output) {
      Serial.print(F("RC in : ch0 : "));
      Serial.print(rc_in_val[0]);
      Serial.print(F(" | ch1 : "));
      Serial.print(rc_in_val[1]);
      Serial.print(F(" | ch2 : "));
      Serial.print(rc_in_val[2]);
      Serial.print(F(" | ch3 : "));
      Serial.print(rc_in_val[3]);
      Serial.print(F(" -> "));
      Serial.println(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
    }
  }
  // block ----

  //block -- magnetometer update
  static int mag_update_freq = 10;
  static unsigned long mag_update_ts = millis();
  // If intPin goes high, all data registers have new data
  // On interrupt, check if data ready interrupt
  if (myIMU.readByte(MPU9250_ADDRESS, INT_STATUS) & 0x01)
  {
    myIMU.readMagData(myIMU.magCount);  // Read the x/y/z adc values
    // Calculate the magnetometer values in milliGauss
    // Include factory calibration per data sheet and user environmental
    // corrections
    // Get actual magnetometer value, this depends on scale being set
    myIMU.mx = (float)myIMU.magCount[0] * myIMU.mRes
               * myIMU.factoryMagCalibration[0] - myIMU.magBias[0];
    myIMU.my = (float)myIMU.magCount[1] * myIMU.mRes
               * myIMU.factoryMagCalibration[1] - myIMU.magBias[1];
    myIMU.mz = (float)myIMU.magCount[2] * myIMU.mRes
               * myIMU.factoryMagCalibration[2] - myIMU.magBias[2];
  }
  
  if ( (millis() - mag_update_ts) > (unsigned long) (1000 / float(mag_update_freq)) ) {

      // Print mag values in degree/sec
      if (mag_verbose && human_readable_output) {
        Serial.print(F("X-mag field: ")); Serial.print(myIMU.mx);
        Serial.print(F(" mG "));
        Serial.print(F("Y-mag field: ")); Serial.print(myIMU.my);
        Serial.print(F(" mG "));
        Serial.print(F("Z-mag field: ")); Serial.print(myIMU.mz);
        Serial.println(F(" mG"));
      } 
  }
  //block ----------
  
 //block -- Xbee rssi update
  static int rssi_update_freq = 10;
  static unsigned long rssi_update_ts = millis();
  if ( (millis() - rssi_update_ts) > (unsigned long) (1000 / float(rssi_update_freq)) ) {
    if (rssi_verbose && human_readable_output){
      if (xbee_rssi_newdata){
        Serial.print(F("RSSI : "));
        Serial.println(xbee_rssi_val);
        xbee_rssi_newdata = false;
      } else{
        Serial.println(F("No RSSI update"));
      }
    }
  }

#ifdef DUALACC
  //block -- Accelerometer update
  static int accel_update_freq = 10;
  static unsigned long accel_update_ts = millis();
  /* Get a new sensor event */
  sensors_event_t event_in;
  accel_in.getEvent(&event_in);

  // repeat for the outer one
  /* Get a new sensor event */
  sensors_event_t event_out;
  accel_out.getEvent(&event_out);
  
  if ( (millis() - accel_update_ts) > (unsigned long) (1000 / float(accel_update_freq)) ) {
    accel_update_ts = millis();

    if (acc_verbose && human_readable_output) {
      /* Display the results (acceleration is measured in m/s^2) */
      Serial.print(F("Innter - X: ")); Serial.print(event_in.acceleration.x); Serial.print(F("  "));
      Serial.print(F("Y: ")); Serial.print(event_in.acceleration.y); Serial.print(F("  "));
      Serial.print(F("Z: ")); Serial.print(event_in.acceleration.z); Serial.print(F("  ")); Serial.print(F("m/s^2      "));
    }


    if (acc_verbose && human_readable_output) {
      /* Display the results (acceleration is measured in m/s^2) */
      Serial.print(F("outer X: ")); Serial.print(event_out.acceleration.x); Serial.print(F("  "));
      Serial.print(F("Y: ")); Serial.print(event_out.acceleration.y); Serial.print(F("  "));
      Serial.print(F("Z: ")); Serial.print(event_out.acceleration.z); Serial.print(F("  ")); Serial.println(F("m/s^2 "));
    }
  }
#endif

  //block -- rc out
  static int rc_out_freq = 100;
  static unsigned long rc_out_ts = millis();
  if ( (millis() - rc_out_ts) > (unsigned long) (1000 / float(rc_out_freq)) ) {
    rc_out_ts = millis();
//    flap.writeMicroseconds(map(rc_in_val[0], 1000, 2000, 260, 1260));
//    esc.writeMicroseconds(rc_in_val[1]);
  }
  // block ----


  //block -- loss of signal
  static int loss_of_signal_freq = 10;
  static unsigned long loss_of_signal_ts = millis();
  if ( (millis() - loss_of_signal_ts) > (unsigned long) (1000 / float(loss_of_signal_freq)) ) {
    loss_of_signal_ts = millis();

    // detect loss of signal (works with FHSS on a channel without fail safe enabled
    unsigned long us_since_last_rc_in = micros() - rc_rising_ts[0];
    if (us_since_last_rc_in > 500000) {
      if (sig_verbose && human_readable_output){
        Serial.println(F("Signal loss"));
      }
      flag_signal_loss = true;
    } else {
      flag_signal_loss = false;
    }
  }
  // block ---

  // data processing and visualization
  kf_predict();

  // acc update
  // WARNING FIXME two acc not aligned, implement same algo used in reader.py
  //float delta_acc = acc_norm(&event_out)-acc_norm(&event_in);
  //if (x[1][0]>2*pi){
    //kf_update_acc(delta_acc);
  //}

  float m_norm = sqrt(myIMU.mx*myIMU.mx + myIMU.my*myIMU.my + myIMU.mz*myIMU.mz);
  // vec_mag dot (0,1,0) / mag_norm
  dot = myIMU.my/m_norm;
  lowfilter.input(dot);
  mag_hist[mag_index] = lowfilter.output();
  mag_index++;

  // magnetic update
  if (mag_hist[(mag_index+3)%4]>mag_hist[(mag_index+2)%4] && mag_hist[(mag_index+2)%4]>mag_hist[(mag_index+1)%4] && mag_hist[(mag_index+1)%4]<mag_hist[(mag_index)%4] && abs(mag_hist[(mag_index+3)%4]-mag_hist[(mag_index+1)%4])>0.1&&!flag_led_on){
      flag_reset_point = true;
      last_mag_update_ts = millis();
      flag_running = true;
      kf_update_mag();

      // servo actuation/buffered phase update
      if (x[1][0]>2*pi*8){
        Serial.println(F("Warning: omega too high"));
        flag_servo_protect = true;
        pending_action_cyclic = NONE;
        setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
        digitalWrite(PIN_LED4,HIGH);
      } else if (x[1][0]<2*pi) {
        Serial.println(F("Omega too low, servo disabled"));
        flag_servo_protect = true;
        pending_action_cyclic = NONE;
        setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
        digitalWrite(PIN_LED4,HIGH);
      } else {
        // suitable omega for control
        if (flag_servo_protect){
          // disable ''failsafe''
          Serial.println(F("Resume Servo Control"));
          flag_servo_protect = false;
          pending_action_cyclic = RISING_NEUTRAL;
        }

        noInterrupts();
        state_buffer_ts = micros();
        state_buffer[0] = x[0][0];
        state_buffer[1] = x[1][0];
        interrupts();
      }

      Serial.println("mag update--");
      // Setup LED to blink when azimuth = ref_heading
      if (!flash_in_progress){
        // time needed to travel 30 degrees, so that light will be on for 30 deg
        on_time_ms = 30.0/180.0*pi/x[1][0]*1000.0;
        // put a lower limit
        on_time_ms = (on_time_ms<20)?20:on_time_ms;
        float blinker_delay_us = (2*pi - ffmod(ref_heading-x[0][0],2*pi))/x[1][0]*1e6;
        if (blinker_delay_us>0){
          flash_in_progress = true;
          pending_action_blinker = LED_ON;
          blinkerTimer.begin(blinker,blinker_delay_us);
          //Serial.println(F("LED on in- "));
          //Serial.println(blinker_delay);
        } else {
          //Serial.print("neg omega - ");
          //Serial.println(blinker_delay);
        }
      } else {

      }
/*
      Serial.print("omega = ");
      Serial.print(x[1][0]);
      Serial.print(" theta = ");
      Serial.print(x[0][0]);
      Serial.print("in ");
      Serial.println((2*pi-(x[0][0]-(2*pi*floor(x[0][0]/2.0/pi))))/x[1][0]*1000.0);
*/
  }
  mag_index%=4;

  // set target phase to pilot input
  // ctrl_phase: phase to START command to max deflection
  //volatile float ctrl_phase;
  // the control space is a 2 by 2 square (roll -1~1, pitch -1~1)
  // the actual output is a circle, inner tangent to the control space, so control 
  // will saturate at around 70% when going diagonally, this may result in undesirable handling characteristics
  
  // first find direction
  roll_normalized = fmap(float(rc_in_val[0]),ROLL_FULL_LEFT,ROLL_FULL_RIGHT,-1.0,1.0);
  pitch_normalized = fmap(float(rc_in_val[1]),PITCH_FULL_PULL,PITCH_FULL_PUSH,-1.0,1.0);
  rudder_normalized = fmap(float(rc_in_val[2]),RUDDER_FULL_LEFT,RUDDER_FULL_RIGHT,-1.0,1.0);
  // target control : at max rudder, heading change 150deg/sec, loop runs 50Hz, so
  // 1.5*pi/100 (unit: rad/loop)
  // apply a 5% deadzone on rudder so it doesn't drift
  if (abs(rudder_normalized)>0.1){
    ref_heading += rudder_normalized * 1.5*pi/100.0;
    ref_heading = ffmod(ref_heading,2*pi);
  }
  // positive direction is the monocopter's spin direction, here its clock-wise
  ctrl_phase = ref_heading + atan2(roll_normalized,pitch_normalized);
  ctrl_magnitude = sqrt(roll_normalized*roll_normalized+pitch_normalized*pitch_normalized)/1.41421;
  ctrl_magnitude = ctrl_magnitude>1.0?1.0:ctrl_magnitude;

  // machine readable output
  if (!human_readable_output){
    //mag_x,y,z,(inner)acc1_x,y,z,(outer)acc2_x,y,z,
    // signify this is a line intended for machine parsing


    Serial.print('#');

    Serial.print(millis()-epoch);
    Serial.print(",");
    Serial.print(myIMU.mx,2);
    Serial.print(",");
    Serial.print(myIMU.my,2);
    Serial.print(",");
    Serial.print(myIMU.mz,2);
    Serial.print(",");
    
#ifdef DUALACC
    Serial.print(event_in.acceleration.x,2);
    Serial.print(",");
    Serial.print(event_in.acceleration.y,2);
    Serial.print(",");
    Serial.print(event_in.acceleration.z,2);
    Serial.print(",");
    Serial.print(event_out.acceleration.x,2);
    Serial.print(",");
    Serial.print(event_out.acceleration.y,2);
    Serial.print(",");
    Serial.print(event_out.acceleration.z,2);
#else
    Serial.print(-1.0,2);
    Serial.print(",");
    Serial.print(-1.0,2);
    Serial.print(",");
    Serial.print(-1.0,2);
    Serial.print(",");
    Serial.print(-1.0,2);
    Serial.print(",");
    Serial.print(-1.0,2);
    Serial.print(",");
    Serial.print(-1.0,2);
#endif

    Serial.print(",");
    // unit: degree
    // may not need
    kf_predict();
    long offset = (long)(x[0][0]/pi*180)/360*360;
    Serial.print(x[0][0]/pi*180-offset);
    Serial.print(",");
    //unit: rev/s
    Serial.println(x[1][0]/2.0/pi);

    
//    Serial.print(" ref: ");
//    Serial.print(ref_heading/pi*180);
//    Serial.print("  ctrl ");
//    Serial.println((ctrl_phase-ref_heading)/pi*180);

    //Serial.println(millis()-loop_ts);
    loop_ts = millis();
  }

  
    flag_reset_point = false;
    // limit transmission rate to 50Hz
    while (millis()-loop_ts < 20){
      delayMicroseconds(100);
    }
      if (millis()-last_mag_update_ts>2000){
        x[1][0] = 0;
        x[0][0] = 0;
        P[0][0] = 0;
        P[1][0] = 0;
        P[0][1] = 0;
        P[1][1] = 0;
        // stationary
        if (flag_running){
          Serial.println("Stopped");
          flag_running = false;
          flag_servo_protect = true;
          pending_action_cyclic = NONE;
          setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
          digitalWrite(PIN_LED4,HIGH);
        }
      }
    
    if (!flag_running){
       //Serial.println(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
       setPulseWidth(fmap(float(rc_in_val[3]),VR_MIN,VR_MAX,MIN_SERVO_PULSEWIDTH,MAX_SERVO_PULSEWIDTH));
    }
}
