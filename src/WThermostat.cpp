#include <Arduino.h>
#include "WNetwork.h"
#include "WClock.h"
#include "WThermostat.h"
#include "WThermostat_BHT_002_GBLW.h"
#include "WThermostat_BAC_002_ALW.h"
#include "WThermostat_ET81W.h"
#include "WThermostat_HY08WE.h"
#include "WThermostat_ME81H.h"
#include "WThermostat_MK70GBH.h"
#include "WThermostat_ME102H.h"
#include "WThermostat_CalypsoW.h"
#include "WThermostat_DLX_LH01.h"

#define APPLICATION "Thermostat"
#define VERSION "1.38.10"
#define FLAG_SETTINGS 0x12
#define DEBUG false

WNetwork* network;
WProperty* thermostatModel;
WThermostat* device;
WClock* wClock;
const unsigned long AP_WATCHDOG_TIMEOUT = 5 * 60000;
unsigned long apModeSince = 0;

void setup() {
	Serial.begin(9600);
	//Wifi and Mqtt connection
	network = new WNetwork(DEBUG, APPLICATION, VERSION, NO_LED, FLAG_SETTINGS, nullptr);
	network->setOnConfigurationFinished([]() {
		//Switch blinking thermostat in normal operating mode back
		device->cancelConfiguration();
	});
	//WClock - time sync
	wClock = new WClock(network, false);
	network->addDevice(wClock);
	//Model
	thermostatModel = network->settings()->setByte("thermostatModel", MODEL_BHT_002_GBLW);
	//Thermostat device
	device = nullptr;
	switch (thermostatModel->asByte()) {
		case MODEL_BHT_002_GBLW :
			device = new WThermostat_BHT_002_GBLW(network, thermostatModel, wClock);
			break;
		case MODEL_BAC_002_ALW :
			device = new WThermostat_BAC_002_ALW(network, thermostatModel, wClock);
			break;
		case MODEL_ET81W :
			device = new WThermostat_ET81W(network, thermostatModel, wClock);
			break;
		case MODEL_HY08WE :
			device = new WThermostat_HY08WE(network, thermostatModel, wClock);
			break;
		case MODEL_ME81H :
		  device = new WThermostat_ME81H(network, thermostatModel, wClock);
			break;
		case MODEL_MK70GBH :
		  device = new WThermostat_MK70GBH(network, thermostatModel, wClock);
			break;
		case MODEL_ME102H :
			device = new WThermostat_ME102H(network, thermostatModel, wClock);
			break;
		case MODEL_CALYPSOW :
			device = new WThermostat_CalypsoW(network, thermostatModel, wClock);
			break;
		case MODEL_DLX_LH01 :
			device = new WThermostat_DLX_LH01(network, thermostatModel, wClock);
			break;
		default :
		  network->error(F("Can't start device. Wrong thermostatModel (%d)"), thermostatModel->asByte());
	}
	if (device != nullptr) {
		device->configureCommandBytes();
		device->initializeProperties();
	}
	network->addDevice(device);
}

void loop() {
	unsigned long now = millis();
	network->loop(now);
	//Access point watchdog: after 3 failed connection attempts (e.g. router not yet up
	//after a power cut) WAdapter stays in config AP mode without retrying. Restart to
	//try again, unless Wifi is not configured, AP is forced or someone is connected to the AP.
	if ((network->isSoftAP()) && (strlen(network->getSsid()) > 0) &&
	    (!network->settings()->forceNetworkAccessPoint()) && (WiFi.softAPgetStationNum() == 0)) {
		if (apModeSince == 0) {
			apModeSince = now;
		} else if (now - apModeSince > AP_WATCHDOG_TIMEOUT) {
			ESP.restart();
		}
	} else {
		apModeSince = 0;
	}
	delay(50);
}
