#include <LiquidCrystal.h>
#include <Servo.h>
#include "TrackerGCS.h"
#include "RSSI_Tracker.h"
#include "GPS_Tracker.h"
#include "GPS_Adapter.h"
#include "HMC5883L.h"
#include "uart.h"
#include "Mikrokopter_Datastructs.h"
#include "AeroQuad_Datastructs.h"

// RSSI tracking
const int horizontalTolerance = 2;
const int thresholdValue = 80;

int trackingCounter = 0;
int calibrate1 = 0;
int calibrate2 = 0;
int i = horizontalMid;
int y = verticalMid;

char horizontalDirection = 0;
char verticalDirection = 0;

#define NUMBER_OF_SAMPLES 10

// GPS tracking
char trackingMode = 0; // 0=RSSI-Tracking, 1=GPS-Tracking
char protocolType = 0;
const int protocolTypeSwitchPin = 8; // Switch to determine protocol type on start-up; LOW=AeroQuad, HIGH=Mikrokopter

float uav_lat = 0;
float uav_lon = 0;
uint8_t uav_satellites_visible = 0;
int16_t uav_alt = 0;

float home_lon = 0;
float home_lat = 0;
float home_dist = 0;
int home_alt = 0;
int home_bearing = 0;

int Bearing = 0;
int Elevation = 0;

bool hasGPSFix = false;
bool isTelemetryOk = false;
long lastPacketReceived = 0;

// General
const int trackingModeSwitchPin = 9; // Switch to determine tracking mode on start-up; LOW=RSSI, HIGH=GPS

LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

int rssiTrack = 0;
int rssiFix = 0;
int rssiTrackOld = 0;

Servo VerticalServo;
Servo HorizontalServo;

// Main loop time variable
unsigned long frameCounter = 0;
unsigned long previousTime = 0;
unsigned long currentTime = 0;
unsigned long deltaTime = 0;

#define TASK_50HZ 2
#define TASK_10HZ 10
#define TASK_5HZ 20
#define TASK_1HZ 100

void setup()
{
	determineTrackingMode();

	lcd.begin(16, 2);
	VerticalServo.attach(10);
	VerticalServo.write(verticalMid);
	HorizontalServo.attach(11);
	HorizontalServo.write(horizontalMid);

	if (trackingMode == 0) {
		lcd.setCursor(0, 0);
		lcd.print("Calibrating...");

		for (int counter = 0; counter < NUMBER_OF_SAMPLES; counter++) {
			calibrate1 = calibrate1 + analogRead(rssi1);
			delay(25);
		}
		calibrate1 = calibrate1 / NUMBER_OF_SAMPLES;

		for (int counter = 0; counter < NUMBER_OF_SAMPLES; counter++) {
			calibrate2 = calibrate2 + analogRead(rssi2);
			delay(25);
		}
		calibrate2 = calibrate2 / NUMBER_OF_SAMPLES;
	}
	else if (trackingMode == 1) {
		determineProtocolType();
		usart0_Init();

		//already done by OSD
		//usart0_request_nc_uart();

		initializeGps();
		setupHMC5883L();
	}
}

void setupHMC5883L(){
	compass = HMC5883L();
	compass.SetScale(1.3); //Set the scale of the compass.
	compass.SetMeasurementMode(Measurement_Continuous); // Set the measurement mode to Continuous
}

void determineTrackingMode() {
        if (digitalRead(trackingModeSwitchPin)  == HIGH) {
                trackingMode = 1;
        }
        else {
                trackingMode = 0;
        }
}

void determineProtocolType() {
        if (digitalRead(protocolTypeSwitchPin)  == HIGH) {
                protocolType = 1;
        }
        else {
                protocolType = 0;
        }
}

void loop()
{
	currentTime = micros();
	deltaTime = currentTime - previousTime;

	// ================================================================
	// 100Hz task loop
	// ================================================================
	if (deltaTime >= 10000) {
		frameCounter++;
		previousTime = currentTime;
	}

	if (frameCounter % TASK_10HZ == 0) {   //   10 Hz tasks
		process10HzTask();
	}

	if (frameCounter % TASK_5HZ == 0) {  //  5 Hz tasks
		process5HzTask();
	}

	if (frameCounter % TASK_1HZ == 0) {  //   1 Hz tasks
		process1HzTask();
	}

	if (frameCounter >= 100) {
		frameCounter = 0;
	}
}

void process10HzTask() {
	if (trackingMode == 1) {
		// request OSD Data from NC every 100ms, already requested by OSD
		//usart0_puts_pgm(PSTR(REQUEST_OSD_DATA));

		if (millis() - lastPacketReceived > 2000) {
			isTelemetryOk = false;
		}

		processUsartData();
	}
}

void process5HzTask() {
	updateGps();
	updateGCSPosition();
	updateGCSHeading();
	processTracking();
}

void process1HzTask() {
	updateLCD();
}

void processTracking() {
	readRSSI();

	if (trackingMode == 0) {
		if (rssiTrack <= thresholdValue) {
			if (trackingCounter < 15) {
				trackingCounter++;
				calculateRSSIDiff();

				if (rssiDiv <= horizontalTolerance) {
					if (rssiTrack <= 45) {
						VerticalServo.write(verticalMid);

						if (i >= horizontalMid) {
							i = i - 30;
							horizontalDirection = 'L';
						}
						else {
							i = i + 30;
							horizontalDirection = 'R';
						}
					}
					else {
						trackVertical();
					}
				}
				else {
					trackHorizontal();
				}
			}
			else {
				//Temporarilly stop tracking if threshold can't be exceeded for more than 3 seconds
				HorizontalServo.write(horizontalMid);
				VerticalServo.write(verticalMid);
			}
		}
		else {
			trackingCounter = 0;
		}
	}
	else if (trackingMode == 1) {
		// Only move servo if GPS has a fix, otherwise standby to last known position
		if (hasGPSFix && isTelemetryOk) {

			int rel_alt = uav_alt - home_alt;
			calculateTrackingVariables(home_lon, home_lat, uav_lon, uav_lat, rel_alt); //calculate tracking bearing/azimuth

			//set current GPS bearing relative to home_bearing
			if (Bearing >= home_bearing) {
				Bearing -= home_bearing;
			}
			else {
				Bearing += 360 - home_bearing;
			}

			if (home_dist > minTrackingDistance) {
				servoPathfinder(Bearing, Elevation);
			}
		}
	}
}

void updateLCD() {
	lcd.clear();
	lcd.setCursor(0, 0);
	lcd.print("RSSI Track ");
	lcd.print(rssiTrack);
	lcd.setCursor(0, 1);
	lcd.print("RSSI Fix   ");
	lcd.print(rssiFix);
}

void readRSSI() {
	rssiTrackOld = rssiTrack;

	rssiTrack = map(analogRead(rssi1), 0, calibrate1, 0, 100);
	rssiFix = map(analogRead(rssi2), 0, calibrate2, 0, 100);

	rssiTrack = constrain(rssiTrack, 0, 100);
	rssiFix = constrain(rssiFix, 0, 100);
}